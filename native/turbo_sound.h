#pragma once
// Turbo sounds, synthesized: spool whistle, compressor surge flutter and blow-off whoosh. No game or Windows
// dependencies, so tests/turbo_logic_test.cpp renders them on any platform.
//
// The game plays voice chat by streaming 48 kHz mono 16-bit PCM into a positional voice effect (application.audio
// play_voice + effect_3d.append_audio_data; its Opus decoder writes 960-sample frames). The mod streams these
// sounds the same way, one voice per turbo and per blow-off valve, on the client. The server sends what each
// client needs through the impeller's existing replicated speed factor (see encode_turbo_factor), so every
// player hears them.
#include <cmath>
#include <cstdint>

namespace turbo {

constexpr int kSampleRate = 48000;

// The impeller's replicated speed factor (0-1, sent as one byte, so steps of 1/255) carries a turbo's spool and
// surge: 0-0.5 is spool, 0.51-1 is spool while surging. A blow-off valve sends 0 when shut, else 0.5-1 for how
// hard it vents.
inline double encode_turbo_factor(double spool, bool surging) {
    double s = std::isfinite(spool) ? (spool < 0 ? 0 : spool > 1 ? 1 : spool) : 0;
    return surging ? 0.51 + 0.49 * s : 0.5 * s;
}
inline void decode_turbo_factor(double factor, double& spool, bool& surging) {
    double f = std::isfinite(factor) ? (factor < 0 ? 0 : factor > 1 ? 1 : factor) : 0;
    surging = f > 0.505;
    spool = surging ? (f - 0.51) / 0.49 : f * 2.0;
    spool = spool < 0 ? 0 : spool > 1 ? 1 : spool;
}
inline double encode_blow_off_factor(bool open, double intensity) {
    double i = std::isfinite(intensity) ? (intensity < 0 ? 0 : intensity > 1 ? 1 : intensity) : 0;
    return open ? 0.5 + 0.5 * i : 0.0;
}
inline void decode_blow_off_factor(double factor, bool& open, double& intensity) {
    double f = std::isfinite(factor) ? (factor < 0 ? 0 : factor > 1 ? 1 : factor) : 0;
    open = f > 0.25;
    intensity = open ? (f - 0.5) * 2.0 : 0.0;
    intensity = intensity < 0 ? 0 : intensity > 1 ? 1 : intensity;
}

// RBJ band-pass biquad (constant 0 dB peak gain).
struct BandPass {
    double b0 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void set(double centre_hz, double q) {
        double w = 2.0 * 3.14159265358979 * centre_hz / kSampleRate, alpha = std::sin(w) / (2.0 * q), a0 = 1.0 + alpha;
        b0 = alpha / a0; b2 = -alpha / a0; a1 = -2.0 * std::cos(w) / a0; a2 = (1.0 - alpha) / a0;
    }
    double run(double x) {
        double y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        if (!std::isfinite(y)) { x1 = x2 = y1 = y2 = 0; y = 0; }
        return y;
    }
};

// State-variable band-pass whose centre can move every sample (for the surge chirps).
struct Sweep {
    double ic1 = 0, ic2 = 0;
    double run(double x, double centre_hz, double q) {
        double g = std::tan(3.14159265358979 * centre_hz / kSampleRate), k = 1.0 / q;
        double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1;
        double v1 = a1 * ic1 + a2 * (x - ic2), v2 = ic2 + g * v1;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        if (!std::isfinite(v1)) { ic1 = ic2 = 0; return 0; }
        return v1;
    }
};

enum class Voice { kTurbo, kBlowOff };

struct Synth {
    Voice voice = Voice::kTurbo;
    double size = 3;                   // turbo size 2-5: bigger turbos whistle lower
    // smoothed state
    double spool = 0, pitch = 0, phase1 = 0, phase2 = 0;
    double surge_env = 0, surge_age = 0, surge_clock = 1, pulse_age = 1, pulse_gain = 0, pulse_period = 0.07;
    double whoosh_env = 0;
    bool was_open = false;
    uint32_t rng = 0x12345678u;
    BandPass hiss, click, whoosh, whoosh_low;
    Sweep chirp, chirp_low;
    bool init = false;
    double noise() {                   // white noise, -1..1
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return double(rng) / 2147483648.0 - 1.0;
    }
};

// Renders n samples. Turbo voice: spool 0-1 and surging; blow-off voice: open (true while venting) and boost 0-1
// (how hard it vents, in spool's place).
inline void render(Synth& s, double spool, bool flag, int16_t* out, int n) {
    if (!s.init) {
        s.hiss.set(5200.0, 0.9); s.click.set(6500.0, 0.7); s.whoosh.set(3600.0, 0.7); s.whoosh_low.set(1400.0, 0.8);
        s.init = true;
    }
    double target = std::isfinite(spool) ? (spool < 0 ? 0 : spool > 1 ? 1 : spool) : 0;
    const double dt = 1.0 / kSampleRate, two_pi = 6.283185307179586;
    double base = 1500.0 - 180.0 * (s.size - 3), span = 5600.0 - 500.0 * (s.size - 3);
    for (int i = 0; i < n; ++i) {
        double v = 0;
        if (s.voice == Voice::kTurbo) {
            s.spool += (target - s.spool) * (1.0 - std::exp(-dt / 0.05));
            // Spool whistle: a whine that rises with shaft speed, plus intake hiss.
            double f = base + span * s.spool;
            s.pitch = s.pitch <= 0 ? f : s.pitch + (f - s.pitch) * 0.002;
            // Surge ("stu-stu-stu"): boost slams back through the compressor in quick puffs. Each puff is a burst of
            // air: a sharp "t" then a breathy "u" that falls in pitch, like a short blow-off. The puffs come fast
            // and close together, then slow and fade as the boost runs out. The whistle keeps winding down
            // underneath and ducks a little on each puff.
            s.surge_env += ((flag ? 1.0 : 0.0) - s.surge_env) * (1.0 - std::exp(-dt / (flag ? 0.01 : 0.15)));
            s.surge_age = flag ? s.surge_age + dt : 0.0;
            s.surge_clock += dt;
            if (s.surge_env > 0.05 && s.surge_clock >= s.pulse_period) {
                s.surge_clock = 0;
                s.pulse_age = 0;
                double fade = std::exp(-s.surge_age / 0.9);
                s.pulse_gain = s.surge_env * (0.35 + 0.65 * fade) * (0.85 + 0.3 * (0.5 + 0.5 * s.noise()));
                double rate = 9.0 + 9.0 * fade;                                  // 18 a second, slowing to 9
                s.pulse_period = (1.0 / rate) * (0.9 + 0.2 * (0.5 + 0.5 * s.noise()));
            }
            s.pulse_age += dt;
            double pa = s.pulse_age;
            double body = pa < 0.004 ? pa / 0.004 : std::exp(-(pa - 0.004) / 0.028);   // "u": breath of air
            double tick = std::exp(-pa / 0.0025);                                       // "t": the slam
            double centre = 900.0 + 2600.0 * std::exp(-pa / 0.02);                      // falls 3.5 kHz -> 0.9 kHz
            double nz = s.noise();
            double puff = 0.9 * s.chirp.run(nz, centre, 2.2) + 0.5 * s.chirp_low.run(nz, centre * 0.5, 1.4);
            double puffs = s.pulse_gain * (body * puff + 0.35 * tick * s.click.run(s.noise()));
            s.phase1 += two_pi * s.pitch * dt;
            s.phase2 += two_pi * s.pitch * 1.5 * dt;
            if (s.phase1 > two_pi) s.phase1 -= two_pi;
            if (s.phase2 > two_pi) s.phase2 -= two_pi;
            double loud = std::pow(s.spool, 1.6) * (1.0 - 0.35 * s.surge_env) * (1.0 - 0.4 * s.pulse_gain * body);
            v += loud * (0.30 * std::sin(s.phase1) + 0.08 * std::sin(s.phase2));
            v += 0.16 * s.spool * s.spool * s.hiss.run(s.noise());
            v += 1.2 * puffs;
        } else {
            // Blow-off whoosh: a burst of air ("pssh") when the valve opens, held while it vents, then a tail.
            if (flag && !s.was_open) s.whoosh_env = 1.0;
            s.was_open = flag;
            double hold = flag ? 0.55 + 0.45 * target : 0.0;
            if (s.whoosh_env < hold) s.whoosh_env += (hold - s.whoosh_env) * (1.0 - std::exp(-dt / 0.01));
            else s.whoosh_env *= std::exp(-dt / (flag ? 0.6 : 0.28));
            double nz = s.noise();
            v += s.whoosh_env * (0.55 * s.whoosh.run(nz) + 0.30 * s.whoosh_low.run(nz));
        }
        v = std::isfinite(v) ? v : 0;
        out[i] = int16_t(std::lround(std::tanh(v) * 30000.0));   // soft clip: loud puffs round off, never crack
    }
}

}  // namespace turbo
