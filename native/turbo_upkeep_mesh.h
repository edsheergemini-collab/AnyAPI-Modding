#pragma once
// Air filter and intercooler meshes, built at start-up like the valves' (turbo_valve_mesh.h), in the same stock look.
//
//   air filter, small: 1 cell, small pipe; large: 2 x 2 x 2 cells, large pipe. The neck goes down to the gas node
//     (direction 2), then a clamp band, a base cap, a pleated cone element (first colour) and a top cap (second
//     colour) with a bright badge.
//   intercooler: three tiles the game strings together along y like the radiator's (radiator_a_0_<y>_0): tile 0 the
//     bottom cell (bottom tank), tile 1 one cell of core (repeated per extra height), tile 2 the top cell (top tank).
//     Each tile is 3 cells wide (x -1..1, which the game stretches) and one cell deep, modelled around its own cell at
//     y = 0. No ports: the player snaps gas ports on, as on the radiator. Tanks take the first colour, end caps the second.
#include "turbo_valve_mesh.h"

namespace turbo {

static const uint8_t kCoreDark[3] = {30, 30, 32};

// A pleated cone along `axis`: `pleats` folds whose inner edges sit `depth` (fraction of the radius) in.
inline void pleated_cone(MeshBuilder& b, int axis, const double c[3], double r0, double r1, double w0, double w1, int pleats, double depth, const uint8_t col[3]) {
    const int n = pleats * 2;
    auto radius = [&](int k, double r) { return k % 2 ? r * (1 - depth) : r; };
    for (int k = 0; k < n; ++k) {
        double a0 = 2 * MeshBuilder::kPi * k / n, a1 = 2 * MeshBuilder::kPi * (k + 1) / n;
        double p[4][3];
        MeshBuilder::place(axis, c, radius(k, r0) * std::cos(a0), radius(k, r0) * std::sin(a0), w0, p[0]);
        MeshBuilder::place(axis, c, radius(k + 1, r0) * std::cos(a1), radius(k + 1, r0) * std::sin(a1), w0, p[1]);
        MeshBuilder::place(axis, c, radius(k + 1, r1) * std::cos(a1), radius(k + 1, r1) * std::sin(a1), w1, p[2]);
        MeshBuilder::place(axis, c, radius(k, r1) * std::cos(a0), radius(k, r1) * std::sin(a0), w1, p[3]);
        // the face's normal from its edges, turned outwards
        double u[3], v[3], nn[3];
        for (int i = 0; i < 3; ++i) { u[i] = p[1][i] - p[0][i]; v[i] = p[3][i] - p[0][i]; }
        nn[0] = u[1] * v[2] - u[2] * v[1]; nn[1] = u[2] * v[0] - u[0] * v[2]; nn[2] = u[0] * v[1] - u[1] * v[0];
        double mid[3], out[3];
        for (int i = 0; i < 3; ++i) mid[i] = (p[0][i] + p[1][i] + p[2][i] + p[3][i]) / 4;
        MeshBuilder::place(axis, c, 0, 0, (w0 + w1) / 2, out);
        double dot = 0, l = 0;
        for (int i = 0; i < 3; ++i) { dot += nn[i] * (mid[i] - out[i]); l += nn[i] * nn[i]; }
        l = std::sqrt(l) * (dot < 0 ? -1 : 1);
        if (l == 0) continue;
        uint32_t q[4];
        for (int j = 0; j < 4; ++j) q[j] = b.vertex(p[j][0], p[j][1], p[j][2], nn[0] / l, nn[1] / l, nn[2] / l, col);
        if (dot >= 0) { b.tri(q[0], q[3], q[1]); b.tri(q[1], q[3], q[2]); }
        else { b.tri(q[0], q[1], q[3]); b.tri(q[1], q[2], q[3]); }
    }
}

// Builds the small or large cone air filter.
inline std::vector<uint8_t> build_air_filter_mesh(const std::vector<uint8_t>& stock, bool large, const char* new_name) {
    MeshLayout m;
    if (!parse_mesh(stock, m)) return {};
    const double u = large ? kCell : kCell / 2, c0 = large ? kCell / 2 : 0, rp = valve_pipe_radius(large);
    const double centre[3] = {c0, c0, c0};
    MeshBuilder b;
    // port frame under the part and the neck up into the filter
    oct(b, 1, centre, rp * 1.32, rp * 1.32, -u, -u + 0.14 * u, kValveIron);
    oct(b, 1, centre, rp * 0.72, rp * 0.72, -u - 0.002, -u + 0.001, kValveHole);
    oct(b, 1, centre, rp, rp, -u + 0.14 * u, -0.62 * u, kValveMid);
    oct(b, 1, centre, rp * 1.15, rp * 1.15, -0.62 * u, -0.5 * u, kValveAlloy);         // clamp band
    const double rb = 0.8 * u, rt = 0.6 * u;                                             // element: base and top radius
    b.frustum_phase(1, centre, rb * 1.04, rb * 1.04, -0.5 * u, -0.4 * u, kValvePaint1, 24, 0);   // base cap
    pleated_cone(b, 1, centre, rb, rt, -0.4 * u, 0.72 * u, 18, 0.12, kValvePaint0);
    b.frustum_phase(1, centre, rt * 0.95, rt * 0.95, -0.4 * u, 0.72 * u, kCoreDark, 18, 0);   // the dark inside, seen between pleats
    b.frustum_phase(1, centre, rt * 1.05, rt * 0.9, 0.72 * u, 0.86 * u, kValvePaint1, 24, 0);  // top cap
    b.frustum_phase(1, centre, rt * 0.42, rt * 0.42, 0.86 * u, 0.9 * u, kValveAlloy, 24, 0);    // badge
    return encode_valve_mesh(stock, m, b, new_name);
}

// Builds one intercooler tile (0 bottom, 1 middle, 2 top). No ports: gas ports snap onto its faces.
inline std::vector<uint8_t> build_intercooler_tile(const std::vector<uint8_t>& stock, int tile, const char* new_name) {
    MeshLayout m;
    if (!parse_mesh(stock, m)) return {};
    const double h = kCell / 2, xw = 1.5 * kCell;          // the tile's own cell: y -h..h; 3 cells across
    const double td = 0.026, tank_h = 0.03, cd = 0.017;     // tank half depth and height, core half depth
    MeshBuilder b;
    // the core's extent in this tile
    double y0 = tile == 0 ? -h + tank_h : -h, y1 = tile == 2 ? h - tank_h : h;
    { double lo[3] = {-xw + 0.012, y0, -cd}, hi[3] = {xw - 0.012, y1, cd}; b.box(lo, hi, kCoreDark); }
    for (int s = -1; s <= 1; s += 2) {                       // side rails
        double lo[3] = {s < 0 ? -xw + 0.002 : xw - 0.012, y0, -cd - 0.002}, hi[3] = {s < 0 ? -xw + 0.012 : xw - 0.002, y1, cd + 0.002};
        b.box(lo, hi, kValveIron);
    }
    for (int k = 0; k < 4; ++k) {                            // the fins: thin bars across both faces, 4 per cell
        double y = -h + (k + 0.5) * kCell / 4;
        if (y < y0 + 0.003 || y > y1 - 0.003) continue;
        for (int s = -1; s <= 1; s += 2) {
            double lo[3] = {-xw + 0.012, y - 0.0025, s < 0 ? -cd - 0.0015 : cd}, hi[3] = {xw - 0.012, y + 0.0025, s < 0 ? -cd : cd + 0.0015};
            b.box(lo, hi, kValveMid);
        }
    }
    if (tile == 0 || tile == 2) {                            // end tank, its end caps and a weld seam
        double ty0 = tile == 0 ? -h : h - tank_h, ty1 = tile == 0 ? -h + tank_h : h;
        { double lo[3] = {-xw + 0.008, ty0, -td}, hi[3] = {xw - 0.008, ty1, td}; b.box(lo, hi, kValvePaint0); }
        for (int s = -1; s <= 1; s += 2) {
            double lo[3] = {s < 0 ? -xw : xw - 0.008, ty0 - (tile == 0 ? 0 : 0.002), -td - 0.002}, hi[3] = {s < 0 ? -xw + 0.008 : xw, ty1 + (tile == 2 ? 0 : 0.002), td + 0.002};
            b.box(lo, hi, kValvePaint1);
        }
        double seam = tile == 0 ? ty1 : ty0;
        double lo[3] = {-xw + 0.008, seam - 0.0015, -td - 0.001}, hi[3] = {xw - 0.008, seam + 0.0015, td + 0.001};
        b.box(lo, hi, kValveIron);
    }
    return encode_valve_mesh(stock, m, b, new_name);
}

}  // namespace turbo
