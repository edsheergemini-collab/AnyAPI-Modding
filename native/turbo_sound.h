#pragma once
// Turbo sounds, synthesized: spool whistle, compressor surge puffs and four blow-off valve sounds. No game or Windows
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
// Blow-off sounds the valve can make, picked in the Properties Tool ("Sound effect").
enum BlowOffSound { kWhoosh = 0, kFlutter = 1, kChirp = 2, kDump = 3, kBlowOffSoundCount = 4 };
// A blow-off valve sends 0 when shut. Open, it sends one byte b = 51 * (sound + 1) + sub, where sub carries the
// size of the turbo venting through it and how hard (10 levels): sub = 12 * (size - 2) + 1 + level. The spare
// step either side keeps it readable if the game rounds the byte one way or the other.
inline double encode_blow_off_factor(bool open, double intensity, int sound = kWhoosh, int size = 3) {
    if (!open) return 0.0;
    double i = std::isfinite(intensity) ? (intensity < 0 ? 0 : intensity > 1 ? 1 : intensity) : 0;
    int k = sound < 0 || sound >= kBlowOffSoundCount ? 0 : sound;
    int z = size < 2 ? 2 : size > 5 ? 5 : size;
    int level = int(i * 9.0 + 0.5);
    return (51.0 * (k + 1) + 12 * (z - 2) + 1 + level) / 255.0;
}
inline void decode_blow_off_factor(double factor, bool& open, double& intensity, int& sound, int& size) {
    double f = std::isfinite(factor) ? (factor < 0 ? 0 : factor > 1 ? 1 : factor) : 0;
    int b = int(f * 255.0 + 0.5);
    open = b >= 26;
    if (!open) { intensity = 0; return; }                 // shut keeps the last sound and size for the tail
    int k = b / 51 - 1, sub = b % 51;
    sound = k < 0 ? 0 : k >= kBlowOffSoundCount ? kBlowOffSoundCount - 1 : k;
    int z = 2 + sub / 12;
    size = z > 5 ? 5 : z;
    int level = sub % 12 - 1;
    intensity = (level < 0 ? 0 : level > 9 ? 9 : level) / 9.0;
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

// Each turbo size has its own voice: the 2x2 screams high and puffs fast, the 5x5 whooshes deep and slow.
struct Character {
    double base, span;                 // whistle pitch at rest and the rise to full spool (Hz)
    double overtone, overtone_gain;    // second whistle partial (ratio of the pitch)
    double air_gain;                   // breathy whoosh that follows the whistle (big turbos move more air)
    double hiss_hz, hiss_gain;         // intake hiss
    double puff_pitch;                 // surge puff pitch (x the 3x3's)
    double puff_fast, puff_slow;       // surge puffs a second, at the start and as boost runs out
    double puff_decay, puff_gain;      // surge puff length (s) and loudness
};
inline Character character(int size) {
    switch (size) {
    case 2: return {2400, 7400, 2.0, 0.10, 0.05, 6500, 0.14, 1.30, 22, 11, 0.022, 1.00};
    // Nate's ears (2026-10-13): the big ones were too deep. The 4x4 sits halfway between the 3x3 and the old
    // 4x4, and the 5x5 just past the old 4x4.
    case 4: return {1275, 5000, 1.5, 0.10, 0.13, 4700, 0.17, 0.90, 16.5, 8.25, 0.032, 1.28};
    case 5: return { 945, 4015, 1.5, 0.127, 0.19, 3920, 0.19, 0.74, 14, 7, 0.039, 1.40};
    default: return {1500, 5600, 1.5, 0.08, 0.10, 5200, 0.16, 1.00, 18, 9, 0.028, 1.20};
    }
}

struct Synth {
    Voice voice = Voice::kTurbo;
    int size = 3;                      // turbo size 2-5 (see character())
    int sound = kWhoosh;               // blow-off valve sound (BlowOffSound)
    int tuned_size = 0;                // size the filters were set for
    // smoothed state
    double spool = 0, pitch = 0, phase1 = 0, phase2 = 0;
    double surge_env = 0, surge_age = 0, surge_clock = 1, pulse_age = 1, pulse_gain = 0, pulse_period = 0.07;
    double whoosh_env = 0;
    double flutter_phase = 0, flutter_rate = 32, flutter_depth = 0.6, pop_age = 1;
    bool was_open = false;
    uint32_t rng = 0x12345678u;
    BandPass hiss, click, whoosh, whoosh_low, rumble;
    Sweep chirp, chirp_low, air, bov_chirp;
    bool init = false;
    double noise() {                   // white noise, -1..1
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return double(rng) / 2147483648.0 - 1.0;
    }
};

// Renders n samples. Turbo voice: level = spool 0-1, flag = surging. Blow-off voice: flag = venting, level = how
// hard (0-1), in the synth's sound, voiced for the turbo size venting through it.
inline void render(Synth& s, double level, bool flag, int16_t* out, int n) {
    const Character c = character(s.size);
    // A blow-off valve on a bigger turbo dumps more air: lower, longer and, for the flutter, slower.
    const double bov_pitch = s.size <= 2 ? 1.25 : s.size == 4 ? 0.91 : s.size >= 5 ? 0.77 : 1.0;
    const double bov_length = s.size <= 2 ? 0.75 : s.size == 4 ? 1.12 : s.size >= 5 ? 1.34 : 1.0;
    if (!s.init || s.tuned_size != s.size) {
        s.hiss.set(c.hiss_hz, 0.9); s.click.set(6500.0, 0.7);
        s.whoosh.set(3600.0 * bov_pitch, 0.7); s.whoosh_low.set(1400.0 * bov_pitch, 0.8); s.rumble.set(500.0 * bov_pitch, 0.8);
        s.init = true; s.tuned_size = s.size;
    }
    double target = std::isfinite(level) ? (level < 0 ? 0 : level > 1 ? 1 : level) : 0;
    const double dt = 1.0 / kSampleRate, two_pi = 6.283185307179586;
    for (int i = 0; i < n; ++i) {
        double v = 0;
        if (s.voice == Voice::kTurbo) {
            s.spool += (target - s.spool) * (1.0 - std::exp(-dt / 0.05));
            // Spool whistle: a whine that rises with shaft speed, the air rushing through, and intake hiss.
            double f = c.base + c.span * s.spool;
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
                double rate = c.puff_slow + (c.puff_fast - c.puff_slow) * fade;
                s.pulse_period = (1.0 / rate) * (0.9 + 0.2 * (0.5 + 0.5 * s.noise()));
            }
            s.pulse_age += dt;
            double pa = s.pulse_age;
            double body = pa < 0.004 ? pa / 0.004 : std::exp(-(pa - 0.004) / c.puff_decay);   // "u": breath of air
            double tick = std::exp(-pa / 0.0025);                                             // "t": the slam
            double centre = c.puff_pitch * (900.0 + 2600.0 * std::exp(-pa / (0.7 * c.puff_decay)));   // falls
            double nz = s.noise();
            double puff = 0.9 * s.chirp.run(nz, centre, 2.2) + 0.5 * s.chirp_low.run(nz, centre * 0.5, 1.4);
            double puffs = s.pulse_gain * (body * puff + 0.35 * tick * s.click.run(s.noise()));
            s.phase1 += two_pi * s.pitch * dt;
            s.phase2 += two_pi * s.pitch * c.overtone * dt;
            if (s.phase1 > two_pi) s.phase1 -= two_pi;
            if (s.phase2 > two_pi) s.phase2 -= two_pi;
            double loud = std::pow(s.spool, 1.6) * (1.0 - 0.35 * s.surge_env) * (1.0 - 0.4 * s.pulse_gain * body);
            v += loud * (0.30 * std::sin(s.phase1) + c.overtone_gain * std::sin(s.phase2));
            v += loud * c.air_gain * 4.0 * s.air.run(s.noise(), std::min(s.pitch * 0.85, 16000.0), 5.0);
            v += c.hiss_gain * s.spool * s.spool * s.hiss.run(s.noise());
            v += c.puff_gain * puffs;
        } else {
            // Blow-off: a burst of air when the valve opens, held while it vents, then a tail.
            //   Whoosh:  the classic "pssshh".
            //   Flutter: the same air chopped by the valve chattering on its seat (25-45 Hz, wandering): "brrrrsh".
            //   Chirp:   a sharp falling "tsu" as it cracks open, then the air.
            //   Dump:    a big, deep, longer "whoomph".
            if (flag && !s.was_open) { s.whoosh_env = 1.0; s.pop_age = 0; }
            s.was_open = flag;
            bool dump = s.sound == kDump;
            double hold = flag ? 0.55 + 0.45 * target : 0.0;
            if (s.whoosh_env < hold) s.whoosh_env += (hold - s.whoosh_env) * (1.0 - std::exp(-dt / 0.01));
            else s.whoosh_env *= std::exp(-dt / (bov_length * (flag ? (dump ? 0.9 : 0.6) : (dump ? 0.45 : 0.28))));
            double nz = s.noise();
            double air = dump ? 0.35 * s.whoosh.run(nz) + 0.55 * s.whoosh_low.run(nz) + 0.6 * s.rumble.run(nz)
                              : 0.55 * s.whoosh.run(nz) + 0.30 * s.whoosh_low.run(nz);
            double gain = 1.0;
            if (s.sound == kFlutter) {
                s.flutter_phase += s.flutter_rate * dt;
                if (s.flutter_phase >= 1.0) {
                    s.flutter_phase -= 1.0;
                    s.flutter_rate = (30.0 + 12.0 * s.whoosh_env + 6.0 * s.noise()) / bov_length;
                    s.flutter_depth = 0.55 + 0.35 * (0.5 + 0.5 * s.noise());
                }
                gain = 1.25 * (1.0 - s.flutter_depth * (0.5 + 0.5 * std::cos(two_pi * s.flutter_phase)));
            }
            v += s.whoosh_env * gain * air;
            s.pop_age += dt;
            if (s.sound == kChirp && s.pop_age < 0.2) {
                double centre = bov_pitch * (1700.0 + 3300.0 * std::exp(-s.pop_age / 0.035));
                double env = s.pop_age < 0.003 ? s.pop_age / 0.003 : std::exp(-(s.pop_age - 0.003) / 0.05);
                v += 0.5 * env * s.bov_chirp.run(s.noise(), centre, 9.0);   // band-pass peak gain is its Q
            }
        }
        v = std::isfinite(v) ? v : 0;
        out[i] = int16_t(std::lround(std::tanh(v) * 30000.0));   // soft clip: loud puffs round off, never crack
    }
}

}  // namespace turbo
