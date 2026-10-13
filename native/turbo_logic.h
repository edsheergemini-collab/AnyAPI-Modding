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
constexpr double kSurgeSpool = 0.15;      // below this spool a backflow is just boost bleeding off, not surge
constexpr double kSurgeTau = 0.4;         // surge stalls the wheel: extra spool decay time constant, seconds

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
enum Channel { kIntakePressure, kExhaustPressure, kPowerMultiplier, kSpool, kRpm, kExhaustFlow, kIntakeFlow, kExhaustTemperature, kSurge, kBoostLimit, kChannelCount };
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
    {"surge", false},               // 1 while the compressor surges (boost flowing back, no blow-off valve), else 0
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
// ------------------------------------------------------------------------------------------- model
struct Inputs {
    double pressure[kNodeCount]{};   // content volume / capacity of each node's content
    double volume[kNodeCount]{};     // litres currently in each node's content
    double exhaust_temperature = kAmbientC;
    double dt = 0;
    bool bov_venting = false;        // a blow-off valve on the boost pipe is venting this tick
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
    bool surging = false;
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

// The compressor's head for the current spool and boost limit (pressure units over its inlet).
inline double head_for(const State& st, double inlet_pressure) {
    double head = kMaxHead * st.spool;
    double limit = finite_or(st.slots[kBoostLimit], 0.0);
    if (limit > 0) head = std::min(head, std::max(0.0, limit / kKpaPerUnit - inlet_pressure));
    return head;
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
    out.head = head_for(st, p[kColdIn]);
    double net = out.head - (p[kColdOut] - p[kColdIn]);
    out.compressor_move = capped(s.compressor_coeff * net * dt, in.volume[kColdIn], in.volume[kColdOut]);
    // Surge: boost pushing back through a spinning compressor (throttle off, no blow-off valve). The flow stalls and
    // reverses in pulses, which slows the wheel hard. A venting blow-off valve takes that boost instead.
    st.surging = false;
    if (out.compressor_move < 0 && in.bov_venting) out.compressor_move = 0;
    else if (out.compressor_move < 0 && st.spool > kSurgeSpool) {
        st.surging = true;
        st.spool *= std::exp(-dt / kSurgeTau);
    }
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
    st.slots[kSurge] = st.surging ? 1.0 : 0.0;
    return out;
}

// ------------------------------------------------------------------------------------------- valves
// Two companion parts, also of the impeller's class (two gas nodes: in at the bottom, out at the top, one cell):
//   Blow-off valve: sits on the boost pipe. When boost in the pipe is higher than the turbo can hold (throttle
//     off), and the excess passes its spring setting (Properties Tool, kPa), it vents the pipe to the air. That
//     stops the compressor surging. It sits inline: turbo -> inlet, outlet ("Air") -> the air manifold; the gas
//     passes straight through. Small (small pipe) and large (large pipe) versions.
//   Wastegate: sits between the exhaust manifold and the downpipe or a vent. Its "open" input (0-1, from a data
//     link or microcontroller) sets how much exhaust bypasses the turbine, which caps boost.
enum Valve { kNoValve, kBlowOff, kWastegate, kBlowOffLarge };
constexpr const char* kBlowOffId = "turbo_blow_off_valve";
constexpr const char* kBlowOffLargeId = "turbo_blow_off_valve_large";
constexpr const char* kWastegateId = "turbo_wastegate";
constexpr double kBlowOffCoeff = 60.0;          // L/s per unit of excess boost while open (small; large x2.5)
constexpr double kBlowOffLargeFactor = 2.5;
constexpr double kWastegateCoeff = 40.0;        // L/s per unit of pressure drop, fully open
constexpr double kBlowOffDefaultKpa = 5.0;      // spring setting: excess boost (kPa) before it opens
constexpr double kBlowOffMaxKpa = 50.0;
inline Valve valve_from_id(const char* id, size_t length) {
    auto is = [&](const char* want) { return std::char_traits<char>::length(want) == length && std::char_traits<char>::compare(want, id, length) == 0; };
    return !id ? kNoValve : is(kBlowOffId) ? kBlowOff : is(kBlowOffLargeId) ? kBlowOffLarge : is(kWastegateId) ? kWastegate : kNoValve;
}

enum ValveChannel { kValveOpen, kValveFlow, kValvePressure, kValveChannelCount };
inline constexpr const char* kValveChannelNames[kValveChannelCount] = {"open", "flow", "pressure"};
inline int valve_channel_index(const char* name, size_t length) {
    for (int i = 0; i < kValveChannelCount; ++i)
        if (std::char_traits<char>::length(kValveChannelNames[i]) == length && std::char_traits<char>::compare(kValveChannelNames[i], name, length) == 0) return i;
    return -1;
}
struct ValveState {
    double setting_kpa = kBlowOffDefaultKpa;   // blow-off spring (Properties Tool)
    double flow = 0;                          // smoothed L/s
    double venting_until = 0;                 // blow-off: seconds of game time it stays open after a lift
    double slots[kValveChannelCount]{};       // open (wastegate: input; blow-off: output 0/1), flow L/s, pressure kPa
};

// Blow-off: litres to vent this tick from the boost pipe, given the boost above what the turbo holds (pressure
// units), its spring setting and the litres in its inlet. 0 while the excess is under the spring.
inline double blow_off_vent(double excess, double setting_kpa, double inlet_volume, double dt, bool large = false) {
    if (!(dt > 0) || !(excess * kKpaPerUnit > std::max(0.0, finite_or(setting_kpa, kBlowOffDefaultKpa)))) return 0;
    return std::min(kBlowOffCoeff * (large ? kBlowOffLargeFactor : 1.0) * excess * dt, std::max(0.0, inlet_volume));
}
// Wastegate: litres to pass this tick, inlet -> outlet (negative: back), for an opening 0-1.
inline double wastegate_move(double open, double p_in, double p_out, double v_in, double v_out, double dt) {
    double o = clamp01(finite_or(open, 0.0));
    if (!(dt > 0) || o <= 0) return 0;
    return capped(kWastegateCoeff * o * (finite_or(p_in, 1.0) - finite_or(p_out, 1.0)) * dt, v_in, v_out);
}

inline std::string blow_off_entry(bool large, const std::string& mesh) {
    std::string pipe = large ? ", \"size\": 1" : "";
    std::string last = large ? "2" : "1", top = large ? "1" : "0";
    // Data ports on both sides of every cell; the torque node (unused) underneath.
    std::string surfaces = "{\"id\": \"torque\", \"dir\": 2, \"type\": \"torque\"}";
    for (int z = 0; z <= (large ? 2 : 1); ++z)
        for (int y = 0; y <= (large ? 1 : 0); ++y) {
            surfaces += ", {\"pos\": [0, " + std::to_string(y) + ", " + std::to_string(z) + "], \"dir\": 0, \"gender\": 2, \"type\": \"port\"}";
            surfaces += ", {\"pos\": [" + top + ", " + std::to_string(y) + ", " + std::to_string(z) + "], \"dir\": 1, \"gender\": 2, \"type\": \"port\"}";
        }
    std::string box = large ? "[1, 1, 2]" : "[0, 0, 1]";
    std::string description = std::string("Inline blow-off valve for a Turbo, ") + (large ? "large pipe (4x4 and 5x5 turbos)" : "small pipe (2x2 and 3x3 turbos)") +
        ". Pipe the turbo's compressor outlet into its steel end and its blue Air end into the air manifold; the arrows show the "
        "flow. When you lift off the throttle it vents the trapped boost through the red cap so the turbo does not surge and keeps "
        "its spin. Properties Tool: Stiffness (kPa of extra boost before it opens) and Sound effect (0 whoosh, 1 flutter, 2 chirp, "
        "3 dump). Outputs open (0/1), flow (L/s) and pressure (kPa).";
    return std::string("    {\n") +
        "      \"id\": \"" + (large ? kBlowOffLargeId : kBlowOffId) + "\",\n" +
        "      \"name\": \"" + (large ? "Turbo Blow-Off Valve (Large)" : "Turbo Blow-Off Valve (Small)") + "\",\n" +
        "      \"description\": \"" + description + "\",\n" +
        "      \"class\": \"gas_pump_centrifugal\",\n" +
        "      \"zones\": [{\"bounds_min\": [0, 0, 0], \"bounds_max\": " + box + "}],\n" +
        "      \"interaction_zones\": [],\n" +
        "      \"surfaces\": [" + surfaces + "],\n" +
        "      \"logic_nodes\": [{\"pos\": [0, 0, 0], \"direction\": 4, \"type\": \"gas\"" + pipe + "}, {\"id\": \"air\", \"pos\": [0, 0, " + last + "], \"direction\": 5, \"type\": \"gas\"" + pipe + "}],\n" +
        "      \"mesh_static\": {\"mesh_path\": \"" + mesh + "\"},\n" +
        "      \"meshes_dynamic\": [],\n" +
        "      \"category\": \"gas\",\n" +
        "      \"tags_generated\": \"torque,port,\",\n" +
        "      \"inspection_geometry\": [{\"p0\": [0.0, 0.0, 0.0], \"p1\": " + (large ? "[1.0, 1.0, 2.0]" : "[0.0, 0.0, 1.0]") + "}],\n" +
        "      \"density\": 800.0,\n" +
        "      \"mateable_port_types\": 2,\n" +
        "      \"data_descriptors\": [{\"type\": \"type_f64_output\", \"name\": \"open\"}, {\"type\": \"type_f64_output\", \"name\": \"flow\"}, {\"type\": \"type_f64_output\", \"name\": \"pressure\"}],\n" +
        "      \"tech_tier\": 3,\n" +
        "      \"spawn_probability\": 0.1,\n" +
        "      \"loot_group\": \"mechanical\",\n" +
        "      \"constraint_orientations\": [[1, 0, 0, 0, 1, 0, 0, 0, 1]],\n" +
        "      \"force_emitter_element_rotor\": {\"blades\": []}\n" +
        "    }";
}
inline std::string blow_off_mesh_path(bool large) { return std::string("meshes/components/turbo_") + (large ? kBlowOffLargeId : kBlowOffId) + ".mesh"; }

inline std::string wastegate_entry(const std::string& mesh) {
    std::string descriptors = "{\"type\": \"type_f64_input\", \"name\": \"open\"}, {\"type\": \"type_f64_output\", \"name\": \"flow\"}, {\"type\": \"type_f64_output\", \"name\": \"pressure\"}";
    std::string description = "Electronic wastegate for a Turbo. Put it between the exhaust manifold (bottom port) and a vent or the turbo's exhaust out "
        "(top port). Its open input (0 to 1, from a data link or microcontroller) lets exhaust bypass the turbine, which caps boost. "
        "Outputs flow (L/s) and pressure (kPa).";
    return std::string("    {\n") +
        "      \"id\": \"" + kWastegateId + "\",\n" +
        "      \"name\": \"Turbo Wastegate\",\n" +
        "      \"description\": \"" + description + "\",\n" +
        "      \"class\": \"gas_pump_centrifugal\",\n" +
        "      \"zones\": [{}],\n" +
        "      \"interaction_zones\": [],\n" +
        "      \"surfaces\": [{\"id\": \"torque\", \"dir\": 0, \"type\": \"torque\"}, {\"dir\": 1, \"gender\": 2, \"type\": \"port\"}, {\"dir\": 4, \"gender\": 2, \"type\": \"port\"}, {\"dir\": 5, \"gender\": 2, \"type\": \"port\"}],\n" +
        "      \"logic_nodes\": [{\"direction\": 2, \"type\": \"gas\"}, {\"direction\": 3, \"type\": \"gas\"}],\n" +
        "      \"mesh_static\": {\"mesh_path\": \"" + mesh + "\"},\n" +
        "      \"meshes_dynamic\": [],\n" +
        "      \"category\": \"gas\",\n" +
        "      \"tags_generated\": \"torque,port,\",\n" +
        "      \"inspection_geometry\": [{\"p0\": [0.0, -0.5, -0.5], \"p1\": [0.0, 0.5, 0.5]}],\n" +
        "      \"density\": 800.0,\n" +
        "      \"mateable_port_types\": 2,\n" +
        "      \"data_descriptors\": [" + descriptors + "],\n" +
        "      \"tech_tier\": 3,\n" +
        "      \"spawn_probability\": 0.1,\n" +
        "      \"loot_group\": \"mechanical\",\n" +
        "      \"constraint_orientations\": [[1, 0, 0, 0, 1, 0, 0, 0, 1]],\n" +
        "      \"force_emitter_element_rotor\": {\"blades\": []}\n" +
        "    }";
}
constexpr const char* kValveMeshPath = "meshes/components/gas_port_a.mesh";

// The definitions file: all four sizes, the two blow-off valves and the wastegate. built[n - kMinSize] says whether
// that size's mesh was written (else the stock small impeller's look); bov_built[large] the same for the valves (else
// the gas vent's look).
inline std::string definitions_json(const bool built[kMaxSize - kMinSize + 1], const bool bov_built[2] = nullptr) {
    std::string out = "{\n  \"definitions\": [\n";
    for (int n = kMinSize; n <= kMaxSize; ++n)
        out += definition_entry(n, built[n - kMinSize] ? mesh_path(n) : std::string(kStockMeshPath)) + ",\n";
    for (int large = 0; large < 2; ++large)
        out += blow_off_entry(large, bov_built && bov_built[large] ? blow_off_mesh_path(large) : std::string(kValveMeshPath)) + ",\n";
    out += wastegate_entry(kValveMeshPath) + "\n";
    return out + "  ]\n}\n";
}

}  // namespace turbo
