#pragma once
// Turbo part meshes, built at start-up from the game's own small impeller mesh (rom/meshes/components/turbo_a.mesh),
// so no game asset ships with the mod.
//
// turbo_a.mesh is the 1 x 3 x 3 cell impeller: x from -0.04 to 0.05 m (the axial intake faces -x, the hub +x),
// y and z from -0.12 to 0.12 m, the outlet stub on the top (+y) edge leaving +z. One cell is 0.08 m.
// A turbo of size N (N x N face, 2 cells thick) is:
//   cold side, cell x = 0: the impeller scaled by N/3 across its face, recentred on the N x N face;
//   hot side,  cell x = 1: the same impeller mirrored front to back, turned half a turn about its axis (outlet
//                          stub at the bottom leaving -z: the turbine inlet) and painted heat-rust.
// Mesh file layout (Anymaker 0.1.24; the same reader as GpsPart, checked by parse):
//   "mesh" u32 version(5) u32 (1) u32 name_length, name bytes
//   32-byte attribute header, 56 bytes (zero), 6 x f64 bounds (min xyz, max xyz)
//   u32 vertex_bytes, vertices of 36 bytes: f32 position[3], u8 rgba[4], f32 uv[2], f32 normal[3]
//   u32 index_bytes, u32 indices (triangle list), 8-byte trailer
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace turbo {

constexpr double kCell = 0.08;

struct MeshVertex { float pos[3]; uint8_t rgba[4]; float uv[2]; float normal[3]; };
static_assert(sizeof(MeshVertex) == 36, "mesh vertex");

struct MeshLayout { size_t bounds = 0, vertex_bytes = 0, index_bytes = 0, trailer = 0; uint32_t vertices = 0, indices = 0; };

inline bool read_u32(const std::vector<uint8_t>& b, size_t at, uint32_t& out) {
    if (at + 4 > b.size()) return false;
    std::memcpy(&out, b.data() + at, 4); return true;
}
inline bool parse_mesh(const std::vector<uint8_t>& b, MeshLayout& m) {
    uint32_t version = 0, name = 0, vbytes = 0, ibytes = 0;
    if (b.size() < 16 || std::memcmp(b.data(), "mesh", 4) != 0 || !read_u32(b, 4, version) || version != 5 || !read_u32(b, 12, name) || name > 256) return false;
    m.bounds = 16 + size_t(name) + 32 + 56;
    m.vertex_bytes = m.bounds + 48;
    if (!read_u32(b, m.vertex_bytes, vbytes) || vbytes % sizeof(MeshVertex) || vbytes == 0) return false;
    m.index_bytes = m.vertex_bytes + 4 + vbytes;
    if (!read_u32(b, m.index_bytes, ibytes) || ibytes % 12) return false;
    m.trailer = m.index_bytes + 4 + ibytes;
    if (m.trailer + 8 != b.size()) return false;
    m.vertices = vbytes / sizeof(MeshVertex); m.indices = ibytes / 4;
    for (uint32_t i = 0; i < m.indices; ++i) { uint32_t v = 0; read_u32(b, m.index_bytes + 4 + size_t(i) * 4, v); if (v >= m.vertices) return false; }
    return true;
}

// The stock impeller's red paint becomes heat-rust on the turbine housing; its greys stay.
inline void hot_paint(uint8_t rgba[4]) {
    if (rgba[0] > 120 && rgba[1] < 40 && rgba[2] < 40) { rgba[0] = 156; rgba[1] = 74; rgba[2] = 32; }
}

// Builds the size-n turbo mesh from the stock impeller. Returns an empty vector when the input layout is not
// recognised or is not the 3 x 3 impeller.
inline std::vector<uint8_t> build_turbo_mesh(const std::vector<uint8_t>& stock, int n, const char* new_name) {
    MeshLayout m;
    if (!parse_mesh(stock, m) || n < 2 || n > 5) return {};
    std::vector<MeshVertex> src(m.vertices);
    std::memcpy(src.data(), stock.data() + m.vertex_bytes + 4, m.vertices * sizeof(MeshVertex));
    std::vector<uint32_t> idx(m.indices);
    std::memcpy(idx.data(), stock.data() + m.index_bytes + 4, m.indices * 4);
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (auto& v : src) for (int j = 0; j < 3; ++j) { lo[j] = v.pos[j] < lo[j] ? v.pos[j] : lo[j]; hi[j] = v.pos[j] > hi[j] ? v.pos[j] : hi[j]; }
    // Expect the 3 x 3 impeller (face 0.24 m square, centred, about one cell thick).
    if (hi[1] - lo[1] < 0.2f || hi[1] - lo[1] > 0.28f || hi[2] - lo[2] < 0.2f || hi[2] - lo[2] > 0.28f || hi[0] - lo[0] > 0.12f) return {};

    const float s = float(n) / 3.0f;                    // face scale
    const float c = float((n - 1) * kCell * 0.5);       // centre of the N x N face, cells 0..n-1
    std::vector<MeshVertex> verts;
    verts.reserve(src.size() * 2);
    std::vector<uint32_t> out_idx;
    out_idx.reserve(idx.size() * 2);
    // Normals scale by the inverse of the position scale, then renormalise.
    auto normal = [&](const float in[3], float sx, float sy, float sz, float out[3]) {
        float x = in[0] / sx, y = in[1] / sy, z = in[2] / sz, l = std::sqrt(x * x + y * y + z * z);
        if (l < 1e-6f) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return; }
        out[0] = x / l; out[1] = y / l; out[2] = z / l;
    };
    for (int side = 0; side < 2; ++side) {
        uint32_t base = uint32_t(verts.size());
        for (const auto& v : src) {
            MeshVertex o = v;
            float x = v.pos[0], y = v.pos[1], z = v.pos[2];
            float nx = v.normal[0], ny = v.normal[1], nz = v.normal[2];
            if (side) {                                 // mirror front to back, then half a turn about the axis
                x = float(kCell) - x; y = -y; z = -z;
                nx = -nx; ny = -ny; nz = -nz;
                hot_paint(o.rgba);
            }
            o.pos[0] = x; o.pos[1] = y * s + c; o.pos[2] = z * s + c;
            const float nin[3] = {nx, ny, nz};
            normal(nin, 1.0f, s, s, o.normal);
            verts.push_back(o);
        }
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            // The mirror flips handedness; the half turn does not. Keep faces outward on the hot side.
            if (side) { out_idx.push_back(base + idx[t]); out_idx.push_back(base + idx[t + 2]); out_idx.push_back(base + idx[t + 1]); }
            else { out_idx.push_back(base + idx[t]); out_idx.push_back(base + idx[t + 1]); out_idx.push_back(base + idx[t + 2]); }
        }
    }
    double bounds[6] = {1e9, 1e9, 1e9, -1e9, -1e9, -1e9};
    for (auto& v : verts) for (int j = 0; j < 3; ++j) { bounds[j] = v.pos[j] < bounds[j] ? v.pos[j] : bounds[j]; bounds[3 + j] = v.pos[j] > bounds[3 + j] ? v.pos[j] : bounds[3 + j]; }

    std::string name(new_name);
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t k) { auto* b = static_cast<const uint8_t*>(p); out.insert(out.end(), b, b + k); };
    uint32_t u;
    put(stock.data(), 12);                                   // magic, version, (1)
    u = uint32_t(name.size()); put(&u, 4); put(name.data(), name.size());
    put(stock.data() + m.bounds - 88, 88);                   // attribute header + reserved bytes
    put(bounds, sizeof bounds);
    u = uint32_t(verts.size() * sizeof(MeshVertex)); put(&u, 4); put(verts.data(), verts.size() * sizeof(MeshVertex));
    u = uint32_t(out_idx.size() * 4); put(&u, 4); put(out_idx.data(), out_idx.size() * 4);
    put(stock.data() + m.trailer, 8);
    return out;
}

}  // namespace turbo
