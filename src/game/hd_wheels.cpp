// HD wheels: the cars' wheels are six-sided prisms (20 triangles). Each graphics task's display lists are scanned as
// the game submits it; a display list that is such a wheel gets a smooth, many-sided copy built from it, and its first
// command becomes a branch to the copy. The copy keeps the original's commands (render modes, texture loads) and
// replaces each run of its triangles with the new geometry: the tyre a ring with rounded shoulders, each side a fan, with the same
// textures mapped the same way (each part's texture coordinates are a planar projection of its positions, fitted from
// the original) and normals that curve with the tread.
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <algorithm>

#include "gt_config.h"

namespace {
    // Built wheels live at the end of the Expansion Pak's matrix buffers (which keep 0x20000 each, see enhancements.cpp).
    constexpr uint32_t pool_start = 0x804C0000u;
    constexpr uint32_t pool_end = 0x80500000u;
    constexpr int segments = 36;
    constexpr int shoulder_steps = 3;  // the profile: 4 points per shoulder, 8 in all (4 columns per vertex load)

    uint8_t* ram = nullptr;
    uint32_t rd32(uint32_t a) { return *reinterpret_cast<uint32_t*>(ram + ((a & 0x7FFFFFu))); }
    void wr32(uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(ram + (a & 0x7FFFFFu)) = v; }
    int16_t rd16s(uint32_t a) { return int16_t(*reinterpret_cast<uint16_t*>(ram + (((a & 0x7FFFFFu) ^ 2)))); }
    uint8_t rd8(uint32_t a) { return ram[(a & 0x7FFFFFu) ^ 3]; }
    void wr16(uint32_t a, uint16_t v) { *reinterpret_cast<uint16_t*>(ram + (((a & 0x7FFFFFu) ^ 2))) = v; }
    void wr8(uint32_t a, uint8_t v) { ram[(a & 0x7FFFFFu) ^ 3] = v; }

    struct Vtx {
        int x, y, z, s, t;
        int8_t nx, ny, nz;
        uint8_t a;
    };

    struct Patched {
        uint32_t w0, w1;      // the original's first command
        uint32_t copy;        // the built list it branches to
    };
    std::unordered_map<uint32_t, Patched> patched;       // by display list address
    std::unordered_set<uint32_t> not_wheels;             // lists checked (by address and first words) and left alone
    uint32_t pool_next = pool_start;
    uint64_t key(uint32_t dl) {
        return (uint64_t(dl) << 32) ^ rd32(dl + 4) ^ (uint64_t(rd32(dl + 0x20)) << 7) ^ (uint64_t(rd32(dl + 0x40)) << 13);
    }

    // One display list's commands until its end, with the triangles' vertices resolved.
    struct Run {
        size_t first_cmd = 0, end_cmd = 0;   // commands [first, end) are its vertex loads and triangles
        std::vector<std::array<Vtx, 3>> tris;
    };

    struct Parsed {
        std::vector<std::pair<uint32_t, uint32_t>> cmds;
        std::vector<Run> runs;
    };

    std::unordered_map<uint64_t, uint32_t> built;        // copies by the original's content (a model loaded again)

    uint64_t content_hash(const Parsed& p) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        for (const auto& c : p.cmds) { mix(c.first); mix(c.second); }
        for (const Run& r : p.runs) for (const auto& t : r.tris) for (const Vtx& v : t) { mix(uint64_t(v.x) << 32 ^ uint64_t(v.y) << 16 ^ uint64_t(v.z)); mix(uint64_t(v.s) << 16 ^ uint64_t(v.t)); }
        return h;
    }

    bool is_geometry(uint32_t op) {
        return op == 0x04 || op == 0xBF || op == 0xB1;
    }

    bool parse(uint32_t dl, const uint32_t* segs, Parsed& out) {
        Vtx buf[32] = {};
        bool loaded[32] = {};
        Run run;
        bool in_run = false;
        for (int i = 0; i < 400; i++) {
            const uint32_t a = dl + i * 8;
            const uint32_t w0 = rd32(a), w1 = rd32(a + 4);
            const uint32_t op = w0 >> 24;
            out.cmds.emplace_back(w0, w1);
            if (is_geometry(op)) {
                if (!in_run) {
                    run = Run{};
                    run.first_cmd = out.cmds.size() - 1;
                    in_run = true;
                }
                if (op == 0x04) {
                    const int n = (w0 >> 10) & 0x3F, v0 = ((w0 >> 16) & 0xFF) / 2;
                    const uint32_t addr = 0x80000000u | (segs[(w1 >> 24) & 15] + (w1 & 0xFFFFFF));
                    if (v0 + n > 32 || (addr & 0x7FFFFFu) + n * 16 > 0x800000u) {
                        return false;
                    }
                    for (int k = 0; k < n; k++) {
                        const uint32_t va = addr + k * 16;
                        Vtx& v = buf[v0 + k];
                        v.x = rd16s(va); v.y = rd16s(va + 2); v.z = rd16s(va + 4);
                        v.s = rd16s(va + 8); v.t = rd16s(va + 10);
                        v.nx = int8_t(rd8(va + 12)); v.ny = int8_t(rd8(va + 13)); v.nz = int8_t(rd8(va + 14));
                        v.a = rd8(va + 15);
                        loaded[v0 + k] = true;
                    }
                }
                else {
                    auto tri = [&](uint32_t bits) {
                        const int i0 = ((bits >> 16) & 0xFF) / 2, i1 = ((bits >> 8) & 0xFF) / 2, i2 = (bits & 0xFF) / 2;
                        if (i0 >= 32 || i1 >= 32 || i2 >= 32 || !loaded[i0] || !loaded[i1] || !loaded[i2]) {
                            return false;
                        }
                        run.tris.push_back({ buf[i0], buf[i1], buf[i2] });
                        return true;
                    };
                    if (!tri(op == 0xBF ? w1 : w0) || (op == 0xB1 && !tri(w1))) {
                        return false;
                    }
                }
                continue;
            }
            if (in_run) {
                run.end_cmd = out.cmds.size() - 1;
                out.runs.push_back(run);
                in_run = false;
            }
            if (op == 0xB8) {
                return true;
            }
            if (op == 0x06 || op == 0x01 || op == 0xBD) {
                return false;  // calls, branches or matrices: not a plain model
            }
        }
        return false;
    }

    // The wheel's shape: every vertex on one of the two sides (x = +-half width), around a circle in y-z.
    struct Shape {
        double half_width, radius, cy, cz;
    };

    bool wheel_shape(const Parsed& p, Shape& shape) {
        int count = 0;
        double maxx = 0, ymin = 1e9, ymax = -1e9, zmin = 1e9, zmax = -1e9;
        for (const Run& r : p.runs) {
            for (const auto& t : r.tris) {
                for (const Vtx& v : t) {
                    maxx = std::max(maxx, double(std::abs(v.x)));
                    ymin = std::min(ymin, double(v.y)); ymax = std::max(ymax, double(v.y));
                    zmin = std::min(zmin, double(v.z)); zmax = std::max(zmax, double(v.z));
                    count++;
                }
            }
        }
        if (count < 30 || count > 400 || maxx < 2) {
            return false;
        }
        shape.half_width = maxx;
        shape.cy = (ymin + ymax) / 2;
        shape.cz = (zmin + zmax) / 2;
        double rsum = 0, rmin = 1e9, rmax = 0;
        std::vector<std::pair<int, int>> ring;
        for (const Run& r : p.runs) {
            for (const auto& t : r.tris) {
                for (const Vtx& v : t) {
                    if (std::abs(std::abs(v.x) - maxx) > 1.0) {
                        return false;  // not on a side
                    }
                    const double rr = std::hypot(v.y - shape.cy, v.z - shape.cz);
                    rsum += rr;
                    rmin = std::min(rmin, rr);
                    rmax = std::max(rmax, rr);
                    if (std::find(ring.begin(), ring.end(), std::make_pair(v.y, v.z)) == ring.end()) {
                        ring.emplace_back(v.y, v.z);
                    }
                }
            }
        }
        shape.radius = rsum / count;
        // A prism of 5 to 12 sides, wider across than it is thick.
        return ring.size() >= 5 && ring.size() <= 12 && rmin > shape.radius * 0.8 && rmax < shape.radius * 1.2 &&
            shape.radius > maxx * 0.6;
    }

    // Planar texture mapping: s, t = a.x + b.y + c.z + d, least squares (with a little damping for the axes a part is
    // flat along).
    struct Mapping {
        double s[4] = {}, t[4] = {};
        double eval_s(double x, double y, double z) const { return s[0] * x + s[1] * y + s[2] * z + s[3]; }
        double eval_t(double x, double y, double z) const { return t[0] * x + t[1] * y + t[2] * z + t[3]; }
    };

    bool solve4(double m[4][5], double out[4]) {
        for (int c = 0; c < 4; c++) {
            int piv = c;
            for (int r = c + 1; r < 4; r++) if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
            if (std::fabs(m[piv][c]) < 1e-12) return false;
            std::swap(m[c], m[piv]);
            for (int r = 0; r < 4; r++) {
                if (r == c) continue;
                const double f = m[r][c] / m[c][c];
                for (int k = c; k < 5; k++) m[r][k] -= f * m[c][k];
            }
        }
        for (int c = 0; c < 4; c++) out[c] = m[c][4] / m[c][c];
        return true;
    }

    bool fit(const std::vector<Vtx>& vs, Mapping& map) {
        for (int which = 0; which < 2; which++) {
            double m[4][5] = {};
            for (const Vtx& v : vs) {
                const double row[4] = { double(v.x), double(v.y), double(v.z), 1.0 };
                const double val = which == 0 ? v.s : v.t;
                for (int i = 0; i < 4; i++) {
                    for (int j = 0; j < 4; j++) m[i][j] += row[i] * row[j];
                    m[i][4] += row[i] * val;
                }
            }
            for (int i = 0; i < 3; i++) m[i][i] += 1e-3;
            if (!solve4(m, which == 0 ? map.s : map.t)) {
                return false;
            }
        }
        return true;
    }

    enum Part { SidePlus, SideMinus, Tread, PartCount };

    Part part_of(const std::array<Vtx, 3>& t) {
        if (t[0].x == t[1].x && t[1].x == t[2].x) {
            return t[0].x > 0 ? SidePlus : SideMinus;
        }
        return Tread;
    }

    // Writes the new list (and its vertices) into the pool; returns its address or 0.
    uint32_t build(const Parsed& p, const Shape& shape) {
        struct Out {
            std::vector<std::pair<uint32_t, uint32_t>> cmds;
            std::vector<Vtx> verts;   // in load order; VTX commands reference them by index (fixed up below)
        } out;

        // Smooth shading for the curved tread, set back after.
        bool smooth_set = false;
        size_t cmd = 0;
        for (const Run& r : p.runs) {
            for (; cmd < r.first_cmd; cmd++) out.cmds.push_back(p.cmds[cmd]);
            cmd = r.end_cmd;
            if (!smooth_set) {
                out.cmds.emplace_back(0xB7000000u, 0x00000200u);
                smooth_set = true;
            }
            for (int part = 0; part < PartCount; part++) {
                std::vector<Vtx> vs;
                int winding = 0, nx = 0, ny = 0, nz = 0;
                uint8_t alpha = 255;
                for (const auto& t : r.tris) {
                    if (part_of(t) != part) continue;
                    // Which way round the game winds this part (seen from outside).
                    const double ax = t[1].x - t[0].x, ay = t[1].y - t[0].y, az = t[1].z - t[0].z;
                    const double bx = t[2].x - t[0].x, by = t[2].y - t[0].y, bz = t[2].z - t[0].z;
                    const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
                    double ox = 0, oy = 0, oz = 0;
                    if (part == Tread) {
                        oy = (t[0].y + t[1].y + t[2].y) / 3.0 - shape.cy;
                        oz = (t[0].z + t[1].z + t[2].z) / 3.0 - shape.cz;
                    }
                    else {
                        ox = part == SidePlus ? 1 : -1;
                    }
                    winding += (cx * ox + cy * oy + cz * oz) > 0 ? 1 : -1;
                    for (const Vtx& v : t) {
                        vs.push_back(v);
                        nx += v.nx; ny += v.ny; nz += v.nz;
                        alpha = v.a;
                    }
                }
                if (vs.empty()) continue;
                Mapping map;
                if (!fit(vs, map)) return 0;
                const bool outward_ccw = winding >= 0;
                const double hw = shape.half_width;
                auto make = [&](double x, double ang, double rr, int8_t vnx, int8_t vny, int8_t vnz) {
                    Vtx v;
                    const double y = shape.cy + rr * std::cos(ang), z = shape.cz + rr * std::sin(ang);
                    v.x = int(std::lround(x)); v.y = int(std::lround(y)); v.z = int(std::lround(z));
                    // Texture coordinates from the unrounded position, so they stay smooth.
                    v.s = int(std::lround(std::clamp(map.eval_s(x, y, z), -32768.0, 32767.0)));
                    v.t = int(std::lround(std::clamp(map.eval_t(x, y, z), -32768.0, 32767.0)));
                    v.nx = vnx; v.ny = vny; v.nz = vnz; v.a = alpha;
                    return v;
                };
                const int n = int(vs.size());
                const int8_t fnx = int8_t(std::clamp(nx / n, -127, 127)), fny = int8_t(std::clamp(ny / n, -127, 127)),
                    fnz = int8_t(std::clamp(nz / n, -127, 127));
                // Emits triangles from a vertex list loaded in chunks of up to 32.
                auto emit_tris = [&](const std::vector<Vtx>& chunk, const std::vector<std::array<int, 3>>& tris) {
                    const uint32_t base = uint32_t(out.verts.size());
                    out.verts.insert(out.verts.end(), chunk.begin(), chunk.end());
                    // VTX: w1 holds the vertex index for now (fixed to an address once the pool is laid out).
                    out.cmds.emplace_back(0x04000000u | uint32_t(chunk.size()), 0xF0000000u | base);
                    for (size_t i = 0; i < tris.size(); i += 2) {
                        auto bits = [&](const std::array<int, 3>& t) {
                            std::array<int, 3> o = t;
                            if (!outward_ccw) std::swap(o[1], o[2]);
                            return (uint32_t(o[0] * 2) << 16) | (uint32_t(o[1] * 2) << 8) | uint32_t(o[2] * 2);
                        };
                        if (i + 1 < tris.size()) {
                            out.cmds.emplace_back(0xB1000000u | bits(tris[i]), bits(tris[i + 1]));
                        }
                        else {
                            out.cmds.emplace_back(0xBF000000u, bits(tris[i]));
                        }
                    }
                };
                // The tyre's profile across its width: rounded shoulders (a quarter circle of radius `shoulder`, from the
                // side's outer edge to the tread) and a flat tread between them.
                const double shoulder = std::min(hw * 0.35, shape.radius * 0.12);
                struct ProfilePoint { double x, r, nx, nr; };
                std::vector<ProfilePoint> profile;
                for (int i = 0; i <= shoulder_steps; i++) {  // left shoulder: facing -x to facing out
                    const double a = (3.14159265358979323846 / 2) * i / shoulder_steps;
                    profile.push_back({ -hw + shoulder - shoulder * std::cos(a), shape.radius - shoulder + shoulder * std::sin(a),
                        -std::cos(a), std::sin(a) });
                }
                for (int i = shoulder_steps; i >= 0; i--) {  // right shoulder: facing out to facing +x
                    const double a = (3.14159265358979323846 / 2) * i / shoulder_steps;
                    profile.push_back({ hw - shoulder + shoulder * std::cos(a), shape.radius - shoulder + shoulder * std::sin(a),
                        std::cos(a), std::sin(a) });
                }
                if (part == Tread) {
                    // Columns of the profile's vertices around the circle, as many columns as fit a load of 32.
                    const int m = int(profile.size());
                    const int per_load = 32 / m;
                    int col = 0;
                    while (col < segments) {
                        const int cols = std::min(per_load, segments - col + 1);
                        std::vector<Vtx> chunk;
                        std::vector<std::array<int, 3>> tris;
                        for (int c = 0; c < cols; c++) {
                            const double ang = 2.0 * 3.14159265358979323846 * (col + c) / segments;
                            for (const ProfilePoint& pp : profile) {
                                chunk.push_back(make(pp.x, ang, pp.r, int8_t(std::lround(127 * pp.nx)),
                                    int8_t(std::lround(127 * pp.nr * std::cos(ang))), int8_t(std::lround(127 * pp.nr * std::sin(ang)))));
                            }
                            if (c > 0) {
                                for (int i = 0; i + 1 < m; i++) {
                                    const int a0 = (c - 1) * m + i, a1 = a0 + 1, b0 = c * m + i, b1 = b0 + 1;
                                    // Outward with angle increasing from y toward z and the profile going toward +x.
                                    tris.push_back({ a0, b1, b0 });
                                    tris.push_back({ a0, a1, b1 });
                                }
                            }
                        }
                        emit_tris(chunk, tris);
                        col += cols - 1;
                    }
                }
                else {
                    // A fan around the center: 1 + up to 30 ring vertices per load.
                    const double x = part == SidePlus ? hw : -hw;
                    int seg = 0;
                    while (seg < segments) {
                        const int count = std::min(30, segments - seg);
                        std::vector<Vtx> chunk;
                        std::vector<std::array<int, 3>> tris;
                        Vtx center = make(x, 0.0, 0.0, fnx, fny, fnz);
                        chunk.push_back(center);
                        for (int k = 0; k <= count; k++) {
                            const double ang = 2.0 * 3.14159265358979323846 * (seg + k) / segments;
                            chunk.push_back(make(x, ang, shape.radius - shoulder, fnx, fny, fnz));
                            if (k > 0) {
                                // Seen from +x, angle increasing from y toward z is counterclockwise.
                                if (part == SidePlus) tris.push_back({ 0, k, k + 1 });
                                else tris.push_back({ 0, k + 1, k });
                            }
                        }
                        emit_tris(chunk, tris);
                        seg += count;
                    }
                }
            }
        }
        for (; cmd < p.cmds.size(); cmd++) {
            if ((p.cmds[cmd].first >> 24) == 0xB8 && smooth_set) {
                out.cmds.emplace_back(0xB6000000u, 0x00000200u);
            }
            out.cmds.push_back(p.cmds[cmd]);
        }

        const uint32_t list_bytes = uint32_t(out.cmds.size() * 8);
        const uint32_t vert_bytes = uint32_t(out.verts.size() * 16);
        const uint32_t total = (list_bytes + vert_bytes + 15) & ~15u;
        if (pool_next + total > pool_end) {
            return 0;
        }
        const uint32_t list = pool_next, verts = pool_next + list_bytes;
        pool_next += total;
        for (size_t i = 0; i < out.verts.size(); i++) {
            const Vtx& v = out.verts[i];
            const uint32_t va = verts + uint32_t(i) * 16;
            wr16(va, uint16_t(v.x)); wr16(va + 2, uint16_t(v.y)); wr16(va + 4, uint16_t(v.z)); wr16(va + 6, 0);
            wr16(va + 8, uint16_t(v.s)); wr16(va + 10, uint16_t(v.t));
            wr8(va + 12, uint8_t(v.nx)); wr8(va + 13, uint8_t(v.ny)); wr8(va + 14, uint8_t(v.nz)); wr8(va + 15, v.a);
        }
        for (size_t i = 0; i < out.cmds.size(); i++) {
            uint32_t w0 = out.cmds[i].first, w1 = out.cmds[i].second;
            if ((w0 >> 24) == 0x04 && (w1 >> 28) == 0xF) {
                // F3DEX 1.23 gSPVertex(addr, n, 0): 0x04 | n << 10 | (n * 16 - 1).
                const uint32_t count = w0 & 0xFF;
                w0 = 0x04000000u | (count << 10) | (count * 16 - 1);
                w1 = (verts + (w1 & 0xFFFFFFF) * 16) & 0x00FFFFFFu;
            }
            wr32(list + uint32_t(i) * 8, w0);
            wr32(list + uint32_t(i) * 8 + 4, w1);
        }
        return list;
    }

    void unpatch_all() {
        for (auto& [dl, p] : patched) {
            if (rd32(dl) == 0x06010000u && rd32(dl + 4) == (p.copy & 0x00FFFFFFu)) {
                wr32(dl, p.w0);
                wr32(dl + 4, p.w1);
            }
        }
        patched.clear();
        not_wheels.clear();
        built.clear();
        pool_next = pool_start;
    }

    void consider(uint32_t dl, const uint32_t* segs) {
        if (rd32(dl) == 0x06010000u) {
            auto it = patched.find(dl);
            if (it != patched.end() && rd32(dl + 4) == (it->second.copy & 0x00FFFFFFu)) {
                return;  // already ours
            }
        }
        const uint32_t k = uint32_t(key(dl));
        if (not_wheels.size() > 50000) {
            not_wheels.clear();
        }
        if (not_wheels.count(k ^ dl)) {
            return;
        }
        Parsed p;
        Shape shape;
        if (!parse(dl, segs, p) || p.runs.empty() || !wheel_shape(p, shape)) {
            not_wheels.insert(k ^ dl);
            return;
        }
        const uint64_t hash = content_hash(p);
        auto found = built.find(hash);
        uint32_t copy = found != built.end() ? found->second : build(p, shape);
        if (copy == 0) {
            unpatch_all();  // the pool is full (many models over time): start again
            copy = build(p, shape);
            if (copy == 0) return;
        }
        built[hash] = copy;
        patched[dl] = Patched{ rd32(dl), rd32(dl + 4), copy };
        wr32(dl, 0x06010000u);
        wr32(dl + 4, copy & 0x00FFFFFFu);
        static const bool log = std::getenv("GT_WHEEL_LOG") != nullptr;
        if (log) {
            std::printf("[wheels] %08X: r %.1f half width %.1f -> %08X (pool %X used)\n", dl, shape.radius, shape.half_width,
                copy, pool_next - pool_start);
        }
    }

    void walk(uint32_t dl, uint32_t* segs, int depth, int& budget) {
        for (int i = 0; budget > 0; i++, budget--) {
            const uint32_t a = dl + i * 8;
            if ((a & 0x7FFFFFu) + 8 > 0x800000u) return;
            const uint32_t w0 = rd32(a), w1 = rd32(a + 4);
            const uint32_t op = w0 >> 24;
            if (op == 0xB8) return;
            if (op == 0xBC && (w0 & 0xFF) == 6) {
                segs[((w0 >> 8) & 0xFFFF) / 4 & 15] = w1 & 0x00FFFFFFu;
            }
            else if (op == 0x06) {
                const uint32_t target = 0x80000000u | ((segs[(w1 >> 24) & 15] + (w1 & 0xFFFFFF)) & 0x7FFFFFu);
                if (depth < 6) {
                    consider(target, segs);
                    if (rd32(target) != 0x06010000u || patched.find(target) == patched.end()) {
                        walk(target, segs, depth + 1, budget);
                    }
                }
                if ((w0 >> 16) & 0xFF) return;  // branch
            }
        }
    }
}

// Each graphics task, as the game submits it (before the renderer reads it).
extern "C" void gt_hd_wheels(uint8_t* rdram, uint32_t task) {
    ram = rdram;
    if (gt::get_hd_wheels() != gt::HdWheels::On) {
        if (!patched.empty()) unpatch_all();
        return;
    }
    if (rd32(task) != 1) return;  // M_GFXTASK
    uint32_t segs[16] = {};
    int budget = 30000;
    walk(rd32(task + 0x30), segs, 0, budget);
}
