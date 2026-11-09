// HD wheels: the cars' wheels are six-sided prisms (20 triangles) with a painted rim. Each graphics task's display
// lists are scanned as the game submits it; a display list that is such a wheel gets a modelled wheel of the same size
// in its place (its first command becomes a branch to it): a tyre with rounded shoulders, a rim lip and barrel, five
// spokes and a hub, each part a flat color lit by the game's lights, with the original's render modes.
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
    // Built wheels live in the Expansion Pak after the extended matrix buffers (see enhancements.cpp).
    // 0x80000: a race's field has about 20 wheel sizes and sides at 0x51E0 each. When it's full every model is dropped
    // and rebuilt, which takes long enough to stall a frame (it did every second or so in 0x40000).
    constexpr uint32_t pool_start = 0x80480000u;
    constexpr uint32_t pool_end = 0x80500000u;

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

    std::unordered_map<uint64_t, uint32_t> built;        // models by wheel size and side

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

    // The model: built in the wheel's own space (x across, the hub's face toward `outer` on x), from surfaces of
    // revolution and boxes, each part a flat color lit by the game's lights (no textures).
    constexpr double pi = 3.14159265358979323846;

    struct MVtx {
        double x, y, z;
        double nx, ny, nz;
    };

    struct Mesh {
        uint32_t rgba;
        std::vector<std::array<MVtx, 3>> tris;
    };

    struct Model {
        std::vector<Mesh> meshes;
        int front_sign = 1;   // which winding faces the camera (from the original)

        Mesh& mesh(uint32_t rgba) {
            for (Mesh& m : meshes) if (m.rgba == rgba) return m;
            meshes.push_back(Mesh{ rgba, {} });
            return meshes.back();
        }

        // A triangle, wound to face along its vertices' normals.
        void tri(uint32_t rgba, MVtx a, MVtx b, MVtx c) {
            const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
            const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
            const double gx = uy * vz - uz * vy, gy = uz * vx - ux * vz, gz = ux * vy - uy * vx;
            const double d = gx * (a.nx + b.nx + c.nx) + gy * (a.ny + b.ny + c.ny) + gz * (a.nz + b.nz + c.nz);
            if (std::fabs(gx) + std::fabs(gy) + std::fabs(gz) < 1e-9) return;
            if ((d > 0) != (front_sign > 0)) std::swap(b, c);
            mesh(rgba).tris.push_back({ a, b, c });
        }
    };

    // A point at angle `ang` around the axis: x along it, r out from it; the normal (nx along, nr outward).
    MVtx around(double x, double r, double ang, double nx, double nr) {
        const double c = std::cos(ang), s = std::sin(ang);
        const double len = std::hypot(nx, nr);
        return MVtx{ x, r * c, r * s, nx / len, nr / len * c, nr / len * s };
    }

    struct ProfilePoint {
        double x, r, nx, nr;
        bool sharp = false;   // starts a new face (its own normal on each side)
    };

    // A surface of revolution through the profile (smooth between points unless one is sharp).
    void revolve(Model& m, uint32_t rgba, const std::vector<ProfilePoint>& prof, int segs, double a0 = 0, double a1 = 2 * pi) {
        for (int k = 0; k < segs; k++) {
            const double t0 = a0 + (a1 - a0) * k / segs, t1 = a0 + (a1 - a0) * (k + 1) / segs;
            for (size_t i = 0; i + 1 < prof.size(); i++) {
                const ProfilePoint& p = prof[i];
                const ProfilePoint& q = prof[i + 1];
                const MVtx p0 = around(p.x, p.r, t0, p.nx, p.nr), p1 = around(p.x, p.r, t1, p.nx, p.nr);
                const MVtx q0 = around(q.x, q.r, t0, q.nx, q.nr), q1 = around(q.x, q.r, t1, q.nx, q.nr);
                m.tri(rgba, p0, q0, q1);
                m.tri(rgba, p0, q1, p1);
            }
        }
    }

    // A flat disc or ring facing along x (dir +1 or -1).
    void ring(Model& m, uint32_t rgba, double x, double r0, double r1, double dir, int segs) {
        revolve(m, rgba, { { x, r0, dir, 0 }, { x, r1, dir, 0 } }, segs);
    }

    // A quad with its own flat normal (pointing toward `toward`, a direction).
    void quad(Model& m, uint32_t rgba, const double* a, const double* b, const double* c, const double* d, const double* toward) {
        auto v = [&](const double* p) { return MVtx{ p[0], p[1], p[2], toward[0], toward[1], toward[2] }; };
        m.tri(rgba, v(a), v(b), v(c));
        m.tri(rgba, v(a), v(c), v(d));
    }

    constexpr uint32_t rubber = 0x262628FFu;
    constexpr uint32_t rubber_side = 0x2E2E31FFu;
    constexpr uint32_t alloy = 0xB4B8C0FFu;
    constexpr uint32_t alloy_dark = 0x7C8088FFu;
    constexpr uint32_t barrel = 0x2C2E33FFu;
    constexpr uint32_t brake = 0x1C1C1EFFu;
    constexpr uint32_t centre_cap = 0x2A2C30FFu;

    Model make_wheel(double R, double W, double outer, int front_sign) {
        Model m;
        m.front_sign = front_sign;
        const int segs = 32;
        const double o = outer;           // the hub side's sign on x
        const double Rr = R * 0.68;       // rim (where the tyre sits)
        const double sh = std::min(W * 0.4, R * 0.14);
        const double bead = W * 0.9;

        // Tyre: inner bead, inner sidewall (a slight bulge), rounded shoulder, tread, shoulder, outer sidewall, bead.
        std::vector<ProfilePoint> tyre;
        auto add = [&](double u, double r, double nu, double nr) { tyre.push_back({ u * o, r, nu * o, nr }); };
        add(-bead, Rr, -1.0, -0.35);
        add(-W, (Rr + R - sh) / 2, -1.0, 0.05);
        for (int i = 0; i <= 3; i++) {
            const double a = (pi / 2) * i / 3;
            add(-W + sh - sh * std::cos(a), R - sh + sh * std::sin(a), -std::cos(a), std::sin(a));
        }
        for (int i = 3; i >= 0; i--) {
            const double a = (pi / 2) * i / 3;
            add(W - sh + sh * std::cos(a), R - sh + sh * std::sin(a), std::cos(a), std::sin(a));
        }
        add(W, (Rr + R - sh) / 2, 1.0, 0.05);
        add(bead, Rr, 1.0, -0.35);
        revolve(m, rubber, tyre, segs);

        // Inner side: closed off dark.
        ring(m, brake, -bead * o, 0, Rr, -o, segs);

        // Rim: the lip on the outer face, then the barrel inside it back to the dish.
        const double lip_in = Rr * 0.92;
        const double face = bead;                  // outer face of the lip
        const double dish = W * 0.1;               // the dark dish behind the spokes
        ring(m, alloy, face * o, lip_in, Rr, o, segs);
        revolve(m, barrel, { { face * o, lip_in, 0, -1 }, { dish * o, lip_in, 0, -1 } }, segs);
        ring(m, brake, dish * o, 0, lip_in, o, segs);

        // Hub: a short cylinder out to the spokes' face, with a centre cap.
        const double hub_r = R * 0.17, hub_face = W * 0.8;
        revolve(m, alloy, { { dish * o, hub_r, 0, 1 }, { hub_face * o, hub_r, 0, 1 } }, 16);
        ring(m, alloy, hub_face * o, R * 0.08, hub_r, o, 16);
        revolve(m, alloy_dark, { { hub_face * o, R * 0.08, 0, 1 }, { (hub_face + W * 0.06) * o, R * 0.08, 0, 1 } }, 12);
        ring(m, centre_cap, (hub_face + W * 0.06) * o, 0, R * 0.08, o, 12);

        // Five spokes: from the hub to the lip, their faces sloping back from the hub, with tapered sides.
        for (int k = 0; k < 5; k++) {
            const double ang = 2 * pi * k / 5 + pi / 10;
            const double ca = std::cos(ang), sa = std::sin(ang);
            const double ty = -sa, tz = ca;    // across the spoke
            const double r0 = hub_r * 0.9, r1 = lip_in * 1.01;
            const double w0 = R * 0.11, w1 = R * 0.07;      // half widths at the hub and the rim
            const double f0 = hub_face * 0.98, f1 = W * 0.7;    // face height (along x) at the hub and the rim
            const double b0 = dish + W * 0.05;                  // back, just off the dish
            auto P = [&](double r, double wdt, double u, double* out) {
                out[0] = u * o;
                out[1] = ca * r + ty * wdt;
                out[2] = sa * r + tz * wdt;
            };
            double fl0[3], fr0[3], fl1[3], fr1[3], bl0[3], br0[3], bl1[3], br1[3];
            P(r0, -w0, f0, fl0); P(r0, w0, f0, fr0); P(r1, -w1, f1, fl1); P(r1, w1, f1, fr1);
            P(r0, -w0, b0, bl0); P(r0, w0, b0, br0); P(r1, -w1, b0, bl1); P(r1, w1, b0, br1);
            // Face normal: out along x, tipped toward the rim by the slope.
            const double slope = (f0 - f1) / (r1 - r0);
            double nf[3] = { o, ca * slope, sa * slope };
            const double nl = std::sqrt(nf[0] * nf[0] + nf[1] * nf[1] + nf[2] * nf[2]);
            nf[0] /= nl; nf[1] /= nl; nf[2] /= nl;
            const double nside_l[3] = { 0, -ty, -tz }, nside_r[3] = { 0, ty, tz };
            quad(m, alloy, fl0, fr0, fr1, fl1, nf);
            quad(m, alloy_dark, fl0, fl1, bl1, bl0, nside_l);
            quad(m, alloy_dark, fr0, br0, br1, fr1, nside_r);
        }
        return m;
    }

    // F3DEX combine mode: color = PRIM * SHADE, alpha = PRIM (both cycles).
    constexpr uint32_t cc_a = 3, cc_b = 15, cc_c = 4, cc_d = 7, ca_a = 7, ca_b = 7, ca_c = 7, ca_d = 3;
    constexpr uint32_t combine_w0 = 0xFC000000u | (cc_a << 20) | (cc_c << 15) | (ca_a << 12) | (ca_c << 9) | (cc_a << 5) | cc_c;
    constexpr uint32_t combine_w1 = (cc_b << 28) | (cc_b << 24) | (ca_a << 21) | (ca_c << 18) | (cc_d << 15) | (ca_b << 12) |
        (ca_d << 9) | (cc_d << 6) | (ca_b << 3) | ca_d;

    // Writes the model's display list (and vertices) into the pool, keeping the original's render modes; returns its
    // address or 0 when the pool is full.
    uint32_t write_model(const Parsed& p, const Model& m) {
        std::vector<std::pair<uint32_t, uint32_t>> cmds;
        std::vector<MVtx> verts;
        cmds.emplace_back(0xE7000000u, 0);
        for (const auto& c : p.cmds) {   // the original's render and geometry modes, until its first triangles
            const uint32_t op = c.first >> 24;
            if (is_geometry(op)) break;
            // (and texture state: left on, as the original has it. The combiner doesn't read it, but RT64 reads the
            // texture flag for the call before ours when it flushes it, which can be the car's shadow.)
            if (op == 0xBA || op == 0xB9 || op == 0xB6 || op == 0xB7 || op == 0xBB) cmds.push_back(c);
        }
        // The model is written 16 times its size (vertex positions are whole numbers, and the wheel is only about 37
        // units across) under a matrix that scales it back down.
        cmds.emplace_back(0x01040040u, 0xE0000000u);   // gSPMatrix(scale, MODELVIEW | MUL | PUSH): address fixed below
        cmds.emplace_back(0xB7000000u, 0x00000200u);    // smooth shading
        cmds.emplace_back(combine_w0, combine_w1);
        for (const Mesh& mesh : m.meshes) {
            cmds.emplace_back(0xFA000000u, mesh.rgba);
            // Pack triangles into loads of up to 32 vertices.
            size_t i = 0;
            while (i < mesh.tris.size()) {
                std::vector<MVtx> load;
                std::vector<std::array<int, 3>> tris;
                auto index = [&](const MVtx& v) {
                    for (size_t k = 0; k < load.size(); k++) {
                        const MVtx& w = load[k];
                        if (std::fabs(w.x - v.x) + std::fabs(w.y - v.y) + std::fabs(w.z - v.z) < 1e-6 &&
                            std::fabs(w.nx - v.nx) + std::fabs(w.ny - v.ny) + std::fabs(w.nz - v.nz) < 1e-6) return int(k);
                    }
                    return -1;
                };
                for (; i < mesh.tris.size(); i++) {
                    int need = 0;
                    for (const MVtx& v : mesh.tris[i]) if (index(v) < 0) need++;
                    if (load.size() + need > 32) break;
                    std::array<int, 3> t;
                    for (int k = 0; k < 3; k++) {
                        int idx = index(mesh.tris[i][k]);
                        if (idx < 0) { load.push_back(mesh.tris[i][k]); idx = int(load.size()) - 1; }
                        t[k] = idx;
                    }
                    tris.push_back(t);
                }
                const uint32_t base = uint32_t(verts.size());
                verts.insert(verts.end(), load.begin(), load.end());
                cmds.emplace_back(0x04000000u | uint32_t(load.size()), 0xF0000000u | base);
                auto bits = [](const std::array<int, 3>& t) {
                    return (uint32_t(t[0] * 2) << 16) | (uint32_t(t[1] * 2) << 8) | uint32_t(t[2] * 2);
                };
                for (size_t k = 0; k < tris.size(); k += 2) {
                    if (k + 1 < tris.size()) cmds.emplace_back(0xB1000000u | bits(tris[k]), bits(tris[k + 1]));
                    else cmds.emplace_back(0xBF000000u, bits(tris[k]));
                }
            }
        }
        cmds.emplace_back(0xE7000000u, 0);
        cmds.emplace_back(0xBD000000u, 0);              // gSPPopMatrix(MODELVIEW)
        cmds.emplace_back(0xB6000000u, 0x00000200u);
        cmds.emplace_back(0xB8000000u, 0);

        const uint32_t list_bytes = uint32_t(cmds.size() * 8);
        const uint32_t total = (list_bytes + 64 + uint32_t(verts.size()) * 16 + 15) & ~15u;
        if (pool_next + total > pool_end) return 0;
        const uint32_t list = pool_next, mtx = (pool_next + list_bytes + 7) & ~7u, vaddr = mtx + 64;
        pool_next += total;
        // N64 matrix: the integer halves of the 16 entries, then the fractions. Scale 1/16 = 0x0000.1000.
        for (int i = 0; i < 16; i++) {
            const bool diag = (i % 5) == 0;
            wr16(mtx + i * 2, i == 15 ? 1 : 0);
            wr16(mtx + 32 + i * 2, diag && i != 15 ? 0x1000 : 0);
        }
        for (size_t i = 0; i < verts.size(); i++) {
            const MVtx& v = verts[i];
            const uint32_t va = vaddr + uint32_t(i) * 16;
            auto s16 = [](double d) { return uint16_t(int16_t(std::lround(std::clamp(d, -32768.0, 32767.0)))); };
            auto n8 = [](double d) { return uint8_t(int8_t(std::lround(std::clamp(d * 127.0, -127.0, 127.0)))); };
            wr16(va, s16(v.x * 16)); wr16(va + 2, s16(v.y * 16)); wr16(va + 4, s16(v.z * 16)); wr16(va + 6, 0);
            wr16(va + 8, 0); wr16(va + 10, 0);
            wr8(va + 12, n8(v.nx)); wr8(va + 13, n8(v.ny)); wr8(va + 14, n8(v.nz)); wr8(va + 15, 0xFF);
        }
        for (size_t i = 0; i < cmds.size(); i++) {
            uint32_t w0 = cmds[i].first, w1 = cmds[i].second;
            if ((w0 >> 24) == 0x01 && w1 == 0xE0000000u) {
                w1 = mtx & 0x00FFFFFFu;
            }
            if ((w0 >> 24) == 0x04 && (w1 >> 28) == 0xF) {
                const uint32_t count = w0 & 0xFF;   // F3DEX 1.23 gSPVertex(addr, n, 0)
                w0 = 0x04000000u | (count << 10) | (count * 16 - 1);
                w1 = (vaddr + (w1 & 0xFFFFFFF) * 16) & 0x00FFFFFFu;
            }
            wr32(list + uint32_t(i) * 8, w0);
            wr32(list + uint32_t(i) * 8 + 4, w1);
        }
        return list;
    }

    // Which side the hub faces: the original draws its inner side with the tread's texture (in the same run as the
    // tread) and its outer side, the rim, on its own. And which winding faces the camera.
    void wheel_sides(const Parsed& p, const Shape& shape, double& outer, int& front_sign) {
        int plus_alone = 0, minus_alone = 0, wind = 0;
        for (const Run& r : p.runs) {
            bool tread = false;
            int plus = 0, minus = 0;
            for (const auto& t : r.tris) {
                const bool same_x = t[0].x == t[1].x && t[1].x == t[2].x;
                if (!same_x) {
                    tread = true;
                    const double ax = t[1].x - t[0].x, ay = t[1].y - t[0].y, az = t[1].z - t[0].z;
                    const double bx = t[2].x - t[0].x, by = t[2].y - t[0].y, bz = t[2].z - t[0].z;
                    const double cy = az * bx - ax * bz, cz = ax * by - ay * bx;
                    const double oy = (t[0].y + t[1].y + t[2].y) / 3.0 - shape.cy, oz = (t[0].z + t[1].z + t[2].z) / 3.0 - shape.cz;
                    wind += (cy * oy + cz * oz) > 0 ? 1 : -1;
                }
                else if (t[0].x > 0) plus++;
                else minus++;
            }
            if (!tread) { plus_alone += plus; minus_alone += minus; }
        }
        outer = minus_alone > plus_alone ? -1.0 : 1.0;
        front_sign = wind >= 0 ? 1 : -1;
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
        double outer;
        int front_sign;
        wheel_sides(p, shape, outer, front_sign);
        // Wheels of the same size and side share one model.
        const uint64_t hash = (uint64_t(std::lround(shape.radius * 8)) << 32) | (uint64_t(std::lround(shape.half_width * 8)) << 8) |
            (outer > 0 ? 2u : 0u) | (front_sign > 0 ? 1u : 0u);
        auto found = built.find(hash);
        const Model model = found != built.end() ? Model{} : make_wheel(shape.radius, shape.half_width, outer, front_sign);
        uint32_t copy = found != built.end() ? found->second : write_model(p, model);
        if (copy == 0) {
            unpatch_all();  // the pool is full (many models over time): start again
            copy = write_model(p, model);
            if (copy == 0) return;
        }
        built[hash] = copy;
        patched[dl] = Patched{ rd32(dl), rd32(dl + 4), copy };
        wr32(dl, 0x06010000u);
        wr32(dl + 4, copy & 0x00FFFFFFu);
        static const bool log = std::getenv("GT_WHEEL_LOG") != nullptr;
        if (log) {
            std::printf("[wheels] %08X: r %.1f half width %.1f outer %+.0f -> %08X (pool %X used)\n", dl, shape.radius,
                shape.half_width, outer, copy, pool_next - pool_start);
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
extern "C" void gt_car_fog(uint8_t* rdram, uint32_t task);

extern "C" void gt_hd_wheels(uint8_t* rdram, uint32_t task) {
    gt_car_fog(rdram, task);
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
