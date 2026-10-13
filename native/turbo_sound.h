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

enum class Voice { kTurbo, kBlowOff };

struct Synth {
    Voice voice = Voice::kTurbo;
    double size = 3;                   // turbo size 2-5: bigger turbos whistle lower
    // smoothed state
    double spool = 0, pitch = 0, phase1 = 0, phase2 = 0;
    double surge_env = 0, surge_clock = 0, pulse_env = 0;
    double whoosh_env = 0;
    bool was_open = false;
    uint32_t rng = 0x12345678u;
    BandPass hiss, chuff, whoosh, whoosh_low;
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
        s.hiss.set(5200.0, 0.9); s.chuff.set(850.0, 1.4); s.whoosh.set(3600.0, 0.7); s.whoosh_low.set(1400.0, 0.8);
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
            // Surge: 12-16 pulses a second ("stu-stu-stu"); each pulse is a chuff of air pushed back through the
            // compressor, and it drags the whistle down for a moment.
            s.surge_env += ((flag ? 1.0 : 0.0) - s.surge_env) * (1.0 - std::exp(-dt / (flag ? 0.02 : 0.12)));
            double rate = 12.0 + 4.0 * s.spool;
            s.surge_clock += rate * dt;
            if (s.surge_clock >= 1.0) { s.surge_clock -= 1.0; s.pulse_env = 1.0; }
            s.pulse_env *= std::exp(-dt / 0.03);
            double wobble = 1.0 - 0.3 * s.surge_env * s.pulse_env;
            s.phase1 += two_pi * s.pitch * wobble * dt;
            s.phase2 += two_pi * s.pitch * 1.5 * wobble * dt;
            if (s.phase1 > two_pi) s.phase1 -= two_pi;
            if (s.phase2 > two_pi) s.phase2 -= two_pi;
            // While surging the whistle chops in time with the pulses.
            double loud = std::pow(s.spool, 1.6) * (1.0 - 0.6 * s.surge_env * (1.0 - s.pulse_env));
            v += loud * (0.30 * std::sin(s.phase1) + 0.08 * std::sin(s.phase2));
            v += 0.16 * s.spool * s.spool * s.hiss.run(s.noise());
            v += 1.5 * s.surge_env * s.pulse_env * (0.7 * s.chuff.run(s.noise()) + 0.25 * std::sin(two_pi * 170.0 * s.surge_clock / rate));
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
        double clipped = v > 1 ? 1 : v < -1 ? -1 : v;
        out[i] = int16_t(std::lround(clipped * 30000.0));
    }
}

}  // namespace turbo
