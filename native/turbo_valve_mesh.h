#pragma once
// Blow-off valve meshes, built from simple shapes at start-up (the file header is copied from the stock impeller
// mesh, like the turbos'). An inline valve: a straight pipe along +z with the valve body on top.
//
//   small: 1 x 1 x 2 cells, small pipe.  large: 2 x 2 x 3 cells, large pipe.
//   inlet end (-z, from the turbo): bare steel flange.  outlet end (+z, to the air manifold): blue flange, and a
//   white arrow on the pipe pointing from the turbo to the engine.  Body: polished housing, red anodised cap,
//   dark vent horn on its side.
#include "turbo_mesh.h"

namespace turbo {

struct MeshBuilder {
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idx;
    static constexpr double kPi = 3.14159265358979;
    static constexpr double kOrigin[3] = {0, 0, 0};

    uint32_t vertex(double x, double y, double z, double nx, double ny, double nz, const uint8_t c[3]) {
        MeshVertex v{};
        v.pos[0] = float(x); v.pos[1] = float(y); v.pos[2] = float(z);
        v.rgba[0] = c[0]; v.rgba[1] = c[1]; v.rgba[2] = c[2]; v.rgba[3] = 255;
        v.normal[0] = float(nx); v.normal[1] = float(ny); v.normal[2] = float(nz);
        verts.push_back(v);
        return uint32_t(verts.size() - 1);
    }
    void tri(uint32_t a, uint32_t b, uint32_t c) { idx.push_back(a); idx.push_back(b); idx.push_back(c); }
    // Maps (u, v, w) in the shape's frame (w along the axis) to x, y, z.
    static void place(int axis, const double centre[3], double u, double v, double w, double out[3]) {
        double l[3];
        if (axis == 0) { l[0] = w; l[1] = u; l[2] = v; }
        else if (axis == 1) { l[0] = v; l[1] = w; l[2] = u; }
        else { l[0] = u; l[1] = v; l[2] = w; }
        for (int i = 0; i < 3; ++i) out[i] = centre[i] + l[i];
    }
    // A closed frustum along `axis` from w0 to w1 (relative to centre), radii r0 and r1, outward faces.
    void frustum(int axis, const double centre[3], double r0, double r1, double w0, double w1, const uint8_t c[3], int seg = 24) {
        double slope = (r0 - r1) / (w1 - w0);
        uint32_t ring0 = uint32_t(verts.size());
        for (int i = 0; i <= seg; ++i) {
            double a = 2 * kPi * i / seg, cu = std::cos(a), sv = std::sin(a), p[3], l = std::sqrt(1 + slope * slope);
            double nn[3]; place(axis, kOrigin, cu / l, sv / l, slope / l, nn);
            place(axis, centre, r0 * cu, r0 * sv, w0, p); vertex(p[0], p[1], p[2], nn[0], nn[1], nn[2], c);
            place(axis, centre, r1 * cu, r1 * sv, w1, p); vertex(p[0], p[1], p[2], nn[0], nn[1], nn[2], c);
        }
        for (int i = 0; i < seg; ++i) {
            uint32_t a = ring0 + 2 * i, b = a + 1, d = a + 2, e = a + 3;
            tri(a, d, b); tri(b, d, e);
        }
        disc(axis, centre, r0, w0, false, c, seg);
        disc(axis, centre, r1, w1, true, c, seg);
    }
    void cylinder(int axis, const double centre[3], double r, double w0, double w1, const uint8_t c[3], int seg = 24) { frustum(axis, centre, r, r, w0, w1, c, seg); }
    void disc(int axis, const double centre[3], double r, double w, bool facing_up, const uint8_t c[3], int seg) {
        double n[3]; place(axis, kOrigin, 0, 0, facing_up ? 1 : -1, n);
        double p[3]; place(axis, centre, 0, 0, w, p);
        uint32_t mid = vertex(p[0], p[1], p[2], n[0], n[1], n[2], c), first = uint32_t(verts.size());
        for (int i = 0; i <= seg; ++i) {
            double a = 2 * kPi * i / seg;
            place(axis, centre, r * std::cos(a), r * std::sin(a), w, p);
            vertex(p[0], p[1], p[2], n[0], n[1], n[2], c);
        }
        for (int i = 0; i < seg; ++i) {
            if (facing_up) tri(mid, first + i, first + i + 1);
            else tri(mid, first + i + 1, first + i);
        }
    }
    // A flat arrow lying on the plane y = top, pointing +z, from z0 to z1, half width hw.
    void arrow(double x, double top, double z0, double z1, double hw, const uint8_t c[3]) {
        double shaft = z0 + (z1 - z0) * 0.55;
        uint32_t a = vertex(x - hw * 0.45, top, z0, 0, 1, 0, c), b = vertex(x + hw * 0.45, top, z0, 0, 1, 0, c);
        uint32_t d = vertex(x + hw * 0.45, top, shaft, 0, 1, 0, c), e = vertex(x - hw * 0.45, top, shaft, 0, 1, 0, c);
        tri(a, e, d); tri(a, d, b);
        uint32_t f = vertex(x - hw, top, shaft, 0, 1, 0, c), g = vertex(x + hw, top, shaft, 0, 1, 0, c), h = vertex(x, top, z1, 0, 1, 0, c);
        tri(f, h, g);
    }
};

// Builds the small (large = false) or large inline blow-off valve. `stock` is any version-5 game mesh; only its
// header and trailer are reused. Returns an empty vector if the stock mesh is not readable.
inline std::vector<uint8_t> build_blow_off_mesh(const std::vector<uint8_t>& stock, bool large, const char* new_name) {
    MeshLayout m;
    if (!parse_mesh(stock, m)) return {};
    static const uint8_t steel[3] = {150, 154, 160}, pipe_grey[3] = {112, 116, 122}, polished[3] = {196, 200, 206};
    static const uint8_t cap_red[3] = {190, 28, 32}, air_blue[3] = {40, 110, 210}, white[3] = {235, 235, 235}, dark[3] = {40, 42, 46};
    const double h = kCell / 2;                                 // half a cell
    const int cells = large ? 3 : 2;
    const double cx = large ? h : 0, cy = large ? h : 0;        // pipe axis: centre of the 1x1 or 2x2 face
    const double r = large ? 0.046 : 0.019, flange = large ? 0.058 : 0.026, lip = large ? 0.012 : 0.007;
    const double z0 = -h, z1 = (cells - 1) * kCell + h;
    const double bz = (z0 + z1) / 2;                            // valve body over the middle of the pipe
    const double top = large ? 3 * h : h;                       // top of the part's cells
    MeshBuilder b;
    const double axis[3] = {cx, cy, 0};
    b.cylinder(2, axis, r, z0 + lip, z1 - lip, pipe_grey);
    b.cylinder(2, axis, flange, z0, z0 + lip, steel);           // inlet, from the turbo
    b.cylinder(2, axis, flange, z1 - lip, z1, air_blue);        // outlet, to the air manifold
    // valve body: a short neck, the housing and the cap
    const double body_r = large ? 0.036 : 0.016, cap_r = large ? 0.043 : 0.0195;
    const double body_base[3] = {cx, cy, bz};
    double y_neck = r * 0.8, y_body = cy + r + (top - cy - r) * 0.25, y_cap = top + (large ? 0.012 : 0.004);   // the cap may rise a little past the cells
    b.cylinder(1, body_base, body_r * 0.7, y_neck, y_body - cy, steel);
    b.cylinder(1, body_base, body_r, y_body - cy - (large ? 0.012 : 0.006), y_cap - cy - (large ? 0.012 : 0.006), polished);
    b.frustum(1, body_base, cap_r, cap_r * 0.8, y_cap - cy - (large ? 0.014 : 0.007), y_cap - cy, cap_red);
    // vent horn on the +x side of the housing
    const double horn_at[3] = {cx, cy + (y_body - cy) * 1.0, bz};
    b.frustum(0, horn_at, body_r * 0.45, body_r * 0.75, body_r * 0.6, body_r + (large ? 0.026 : 0.012), dark, 16);
    // flow arrows on the pipe top (both sides of the body), pointing from the turbo to the engine
    double arrow_y = cy + r + 0.002, hw = r * 0.55;
    b.arrow(cx, arrow_y, z0 + lip + 0.004, bz - body_r - 0.004, hw, white);
    b.arrow(cx, arrow_y, bz + body_r + 0.004, z1 - lip - 0.004, hw, white);

    double bounds[6] = {1e9, 1e9, 1e9, -1e9, -1e9, -1e9};
    for (auto& v : b.verts) for (int j = 0; j < 3; ++j) { bounds[j] = std::fmin(bounds[j], v.pos[j]); bounds[3 + j] = std::fmax(bounds[3 + j], v.pos[j]); }
    std::string name(new_name);
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t k) { auto* q = static_cast<const uint8_t*>(p); out.insert(out.end(), q, q + k); };
    uint32_t u;
    put(stock.data(), 12);
    u = uint32_t(name.size()); put(&u, 4); put(name.data(), name.size());
    put(stock.data() + m.bounds - 88, 88);
    put(bounds, sizeof bounds);
    u = uint32_t(b.verts.size() * sizeof(MeshVertex)); put(&u, 4); put(b.verts.data(), b.verts.size() * sizeof(MeshVertex));
    u = uint32_t(b.idx.size() * 4); put(&u, 4); put(b.idx.data(), b.idx.size() * 4);
    put(stock.data() + m.trailer, 8);
    return out;
}

}  // namespace turbo
