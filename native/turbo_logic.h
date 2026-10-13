#pragma once
// Turbo pure logic: sizes, the part definitions, the data channels and the per-tick turbo model.
// No game or Windows dependencies, so tests/turbo_logic_test.cpp runs it on any platform.
//
// Game facts this model is built on (Anymaker 0.1.24, read from game.gcl):
//   - Gas pressure is content volume / capacity, so 1.0 is ambient. The game's part descriptions call 1.0
//     "9.8 kPa" (impeller: 0.1 per rps = "1 kPa per rps"), and this mod uses the same scale for its outputs.
//   - Engine power multiplier = remap(clamp(average air manifold pressure, 1, 10), 1..10 -> 1..2), so full
//     boost is an intake pressure of 10 ("98 kPa"). Exhaust pressure above 4 ("39 kPa") cuts power and
//     the engine stops at 20 ("196 kPa").
//   - Engines burn max_watts = 400 W per volume unit; air use is about 0.36 L/s per unit at 1x, and the
//     exhaust is the air plus the burnt fuel, at 300 C.
//   - The stock impeller moves min(content, 0.4 * net pressure * dt) litres per tick from inlet to outlet,
//     with head 0.1 per rps. A turbo here is the same pump with its head set by the spool, plus a turbine
//     that lets exhaust through its hot side.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace turbo {

constexpr int kMinSize = 2, kMaxSize = 5;
constexpr const char* kIdPrefix = "turbocharger_";
constexpr double kKpaPerUnit = 9.8;       // the game's own scale (see above)
constexpr double kMaxHead = 9.0;          // ambient 1 + 9 = 10: the engine's full 2x boost
constexpr double kSpoolStart = 0.25;      // exhaust flow (fraction of rated) where the shaft starts to build boost
constexpr double kAmbientC = 20.0;

// One turbo size: N x N face, 2 cells thick (cold side cell x = 0, hot side cell x = 1).
struct Spec {
    int size = 0;
    double rated_flow = 0;        // exhaust L/s through the hot side for full boost
    double turbine_coeff = 0;     // L/s per unit of pressure drop across the hot side
    double compressor_coeff = 0;  // L/s per unit of net head on the cold side
    double spool_up_s = 0;        // time constant while building boost (turbo lag)
    double spool_down_s = 0;      // time constant while boost falls
    double max_rpm = 0;           // shown on the rpm output at full spool
    bool large_pipe = false;
};
// Rated flow scales with the face area; lag with size. At rated flow the hot side drops 2 units (about
// 20 kPa) of pressure, well under the 4 units (39 kPa) where engines lose power, so a matched turbo never
// chokes its engine; an undersized one does, as a real turbo would.
inline Spec spec(int n) {
    n = std::clamp(n, kMinSize, kMaxSize);
    static constexpr double rated[] = {10, 22, 40, 65};
    static constexpr double lag[] = {0.5, 0.9, 1.5, 2.4};
    static constexpr double rpm[] = {220000, 160000, 120000, 90000};
    Spec s;
    s.size = n;
    s.rated_flow = rated[n - kMinSize];
    s.turbine_coeff = s.rated_flow / 2.0;
    s.compressor_coeff = s.rated_flow * 1.2;
    s.spool_up_s = lag[n - kMinSize];
    s.spool_down_s = s.spool_up_s * 0.5;
    s.max_rpm = rpm[n - kMinSize];
    s.large_pipe = n >= 4;
    return s;
}
inline std::string definition_id(int n) { return std::string(kIdPrefix) + char('0' + n); }
// Returns 2-5 for a turbo definition id, else 0.
inline int size_from_id(const char* id, size_t length) {
    size_t p = std::char_traits<char>::length(kIdPrefix);
    if (!id || length != p + 1 || std::char_traits<char>::compare(id, kIdPrefix, p) != 0) return 0;
    int n = id[p] - '0';
    return n >= kMinSize && n <= kMaxSize ? n : 0;
}

// Gas logic nodes, in definition order. 0 and 1 are the stock impeller's own (inlet, outlet); 2 and 3 are
// the hot side the mod adds at runtime.
enum Node { kColdIn, kColdOut, kHotIn, kHotOut, kNodeCount };

// Data channels.
enum Channel { kIntakePressure, kExhaustPressure, kPowerMultiplier, kSpool, kRpm, kExhaustFlow, kIntakeFlow, kExhaustTemperature, kBoostLimit, kChannelCount };
constexpr int kFirstInput = kBoostLimit;
struct ChannelInfo { const char* name; bool input; };
inline constexpr ChannelInfo kChannels[kChannelCount] = {
    {"intake_pressure", false},     // compressor outlet, kPa on the game's scale (98 = full boost)
    {"exhaust_pressure", false},    // turbine inlet, same scale (39 starts to cut engine power)
    {"power_multiplier", false},    // engine power factor the intake pressure gives, 1-2
    {"spool", false},               // 0-1
    {"rpm", false},
    {"exhaust_flow", false},        // L/s through the hot side
    {"intake_flow", false},         // L/s through the cold side
    {"exhaust_temperature", false}, // C
    {"boost_limit", true},          // input: cap intake pressure (kPa, game scale); 0 = no cap
};
inline int channel_index(const char* name, size_t length) {
    for (int i = 0; i < kChannelCount; ++i) {
        const char* n = kChannels[i].name;
        if (std::char_traits<char>::length(n) == length && std::char_traits<char>::compare(n, name, length) == 0) return i;
    }
    return -1;
}

// Where the ports sit, in cells. Axial ports use the cell at the centre of the face (rounded down on even
// sizes, as the stock large impeller does); tangential ports sit at the top far corner (cold outlet, +z) and
// bottom near corner (hot inlet, -z), where the scaled housings put their outlet stubs.
struct Port { int x, y, z, dir; };
inline Port port(int n, Node node) {
    int c = (n - 1) / 2, top = n - 1;
    switch (node) {
    case kColdIn: return {0, c, c, 0};      // x_neg: front, axial
    case kColdOut: return {0, top, top, 5}; // z_pos
    case kHotIn: return {1, 0, 0, 4};       // z_neg
    default: return {1, c, c, 1};           // x_pos: back, axial
    }
}

inline std::string fmt(double v) {
    char b[32]; std::snprintf(b, sizeof b, "%g", v); return b;
}
inline std::string description(int n) {
    Spec s = spec(n);
    return "Exhaust-driven turbocharger, " + std::to_string(n) + "x" + std::to_string(n) + "x2. Front: compressor intake (centre) and "
        "compressor outlet (top) - feed an air manifold. Back: turbine inlet (bottom) from the exhaust manifold and turbine "
        "exhaust out (centre) - vent it. Full boost (98 kPa, double engine power) needs about " + fmt(s.rated_flow) +
        " L/s of exhaust; boost starts near " + fmt(s.rated_flow * kSpoolStart) + " L/s. Bigger turbos move more air with less back pressure but have more lag. " +
        (s.large_pipe ? "Large pipe." : "Small pipe.");
}

// One part definition: a resized stock impeller (class gas_pump_centrifugal) with a hot side.
inline std::string definition_entry(int n, const std::string& mesh_path) {
    Spec s = spec(n);
    int top = n - 1;
    std::string pipe = s.large_pipe ? ", \"size\": 1" : "";
    auto node = [&](Node which) {
        Port p = port(n, which);
        return "{\"pos\": [" + std::to_string(p.x) + ", " + std::to_string(p.y) + ", " + std::to_string(p.z) + "], \"direction\": " + std::to_string(p.dir) + ", \"type\": \"gas\"" + pipe + "}";
    };
    // Mounting faces for a data port: every front and back face cell except the axial pipe cell.
    std::string surfaces = "{\"id\": \"torque\", \"pos\": [0, 0, " + std::to_string(top) + "], \"dir\": 2, \"type\": \"torque\"}";
    Port axial = port(n, kColdIn);
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < n; ++y)
            for (int z = 0; z < n; ++z) {
                if (y == axial.y && z == axial.z) continue;
                surfaces += ",\n        {\"pos\": [" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + "], \"dir\": " + std::to_string(x) + ", \"gender\": 2, \"type\": \"port\"}";
            }
    std::string descriptors;
    for (int i = 0; i < kChannelCount; ++i)
        descriptors += std::string(i ? ",\n        " : "") + "{\"type\": \"" + (kChannels[i].input ? "type_f64_input" : "type_f64_output") + "\", \"name\": \"" + kChannels[i].name + "\"}";
    std::string box = "[1, " + std::to_string(top) + ", " + std::to_string(top) + "]";
    return std::string("    {\n") +
        "      \"id\": \"" + definition_id(n) + "\",\n" +
        "      \"name\": \"Turbo " + std::to_string(n) + "x" + std::to_string(n) + "\",\n" +
        "      \"description\": \"" + description(n) + "\",\n" +
        "      \"class\": \"gas_pump_centrifugal\",\n" +
        "      \"zones\": [{\"bounds_min\": [0, 0, 0], \"bounds_max\": " + box + "}],\n" +
        "      \"interaction_zones\": [],\n" +
        "      \"surfaces\": [\n        " + surfaces + "\n      ],\n" +
        "      \"logic_nodes\": [\n        " + node(kColdIn) + ",\n        " + node(kColdOut) + ",\n        " + node(kHotIn) + ",\n        " + node(kHotOut) + "\n      ],\n" +
        "      \"mesh_static\": {\"mesh_path\": \"" + mesh_path + "\"},\n" +
        "      \"meshes_dynamic\": [],\n" +
        "      \"category\": \"gas\",\n" +
        "      \"tags\": \"torque\",\n" +
        "      \"tags_generated\": \"torque,port,\",\n" +
        "      \"inspection_geometry\": [{\"p0\": [0.0, 0.0, 0.0], \"p1\": [1.0, " + std::to_string(top) + ".0, " + std::to_string(top) + ".0]}],\n" +
        "      \"density\": 800.0,\n" +
        "      \"mateable_port_types\": 2,\n" +
        "      \"data_descriptors\": [\n        " + descriptors + "\n      ],\n" +
        "      \"tech_tier\": 3,\n" +
        "      \"spawn_probability\": 0.1,\n" +
        "      \"loot_group\": \"mechanical\",\n" +
        "      \"constraint_orientations\": [[1, 0, 0, 0, 1, 0, 0, 0, 1]],\n" +
        "      \"force_emitter_element_rotor\": {\"blades\": []}\n" +
        "    }";
}
inline std::string mesh_path(int n) { return "meshes/components/turbo_" + definition_id(n) + ".mesh"; }
constexpr const char* kStockMeshPath = "meshes/components/turbo_a.mesh";
// The definitions file: all four sizes. built[n - kMinSize] says whether that size's mesh was written; a size
// whose mesh could not be built falls back to the stock small impeller's look.
inline std::string definitions_json(const bool built[kMaxSize - kMinSize + 1]) {
    std::string out = "{\n  \"definitions\": [\n";
    for (int n = kMinSize; n <= kMaxSize; ++n)
        out += definition_entry(n, built[n - kMinSize] ? mesh_path(n) : std::string(kStockMeshPath)) + (n < kMaxSize ? ",\n" : "\n");
    return out + "  ]\n}\n";
}

// ------------------------------------------------------------------------------------------- model
struct Inputs {
    double pressure[kNodeCount]{};   // content volume / capacity of each node's content
    double volume[kNodeCount]{};     // litres currently in each node's content
    double exhaust_temperature = kAmbientC;
    double dt = 0;
};
struct Step {
    double turbine_move = 0;     // litres to move this tick, hot in -> hot out (negative: back)
    double compressor_move = 0;  // litres to move this tick, cold in -> cold out (negative: back)
    double head = 0;             // pressure the compressor adds
    double power = 0;            // compressor power, for the stock tooltip (L/s x pressure)
};
struct State {
    double spool = 0;
    double exhaust_flow = 0;     // smoothed L/s
    double intake_flow = 0;      // smoothed L/s
    double slots[kChannelCount]{};
};

inline double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline double finite_or(double v, double d) { return std::isfinite(v) ? v : d; }
// Move up to `want` litres from a content holding `from_volume` (want < 0 moves back, capped by `back_volume`).
inline double capped(double want, double from_volume, double back_volume) {
    return want >= 0 ? std::min(want, std::max(0.0, from_volume)) : -std::min(-want, std::max(0.0, back_volume));
}
inline double power_multiplier(double intake_pressure) {
    return 1.0 + (std::clamp(intake_pressure, 1.0, 10.0) - 1.0) / 9.0;
}

inline Step step(State& st, const Spec& s, const Inputs& in) {
    Step out;
    double dt = std::clamp(finite_or(in.dt, 0.0), 0.0, 0.25);
    if (dt <= 0) return out;
    double p[kNodeCount];
    for (int i = 0; i < kNodeCount; ++i) p[i] = std::max(0.0, finite_or(in.pressure[i], 1.0));
    // Hot side: exhaust passes the turbine in proportion to the pressure drop across it.
    out.turbine_move = capped(s.turbine_coeff * (p[kHotIn] - p[kHotOut]) * dt, in.volume[kHotIn], in.volume[kHotOut]);
    double exhaust_lps = std::abs(out.turbine_move) / dt;
    double k = std::min(1.0, dt / 0.15);
    st.exhaust_flow += (exhaust_lps - st.exhaust_flow) * k;
    // Shaft: the spool chases the exhaust flow, slowly (turbo lag), faster when it falls.
    double target = clamp01((st.exhaust_flow / s.rated_flow - kSpoolStart) / (1.0 - kSpoolStart));
    double tau = target > st.spool ? s.spool_up_s : s.spool_down_s;
    st.spool += (target - st.spool) * (1.0 - std::exp(-dt / tau));
    st.spool = clamp01(st.spool);
    // Cold side: a centrifugal compressor with the spool's head, like the stock impeller but far more flow.
    out.head = kMaxHead * st.spool;
    double limit = finite_or(st.slots[kBoostLimit], 0.0);
    if (limit > 0) out.head = std::min(out.head, std::max(0.0, limit / kKpaPerUnit - p[kColdIn]));
    double net = out.head - (p[kColdOut] - p[kColdIn]);
    out.compressor_move = capped(s.compressor_coeff * net * dt, in.volume[kColdIn], in.volume[kColdOut]);
    double intake_lps = std::abs(out.compressor_move) / dt;
    st.intake_flow += (intake_lps - st.intake_flow) * k;
    out.power = intake_lps * out.head;
    // Outputs (inputs keep whatever the data link wrote).
    st.slots[kIntakePressure] = p[kColdOut] * kKpaPerUnit;
    st.slots[kExhaustPressure] = p[kHotIn] * kKpaPerUnit;
    st.slots[kPowerMultiplier] = power_multiplier(p[kColdOut]);
    st.slots[kSpool] = st.spool;
    st.slots[kRpm] = st.spool * s.max_rpm;
    st.slots[kExhaustFlow] = st.exhaust_flow;
    st.slots[kIntakeFlow] = st.intake_flow;
    st.slots[kExhaustTemperature] = finite_or(in.exhaust_temperature, kAmbientC);
    return out;
}

}  // namespace turbo
