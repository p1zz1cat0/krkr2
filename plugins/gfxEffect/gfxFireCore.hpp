// gfxFire core: the fire effect of kaede-software gfxEffect.dll.
//
// This header has no KrKr2 dependency. It follows the algorithm that was
// recovered from the DLL (see .sbectrum/features/0012-gfxeffect-dll-gfxfire).
// A heat map F is warped through a jittered 16x16 spring mesh, cooled by a
// blurred random cool map K, blurred again and mapped to colours.
//
// Port differences (on purpose):
//   - Layer pixels are RGBA in this fork. The built-in colour tables are
//     stored as Windows BGRA words, so they are swizzled when first used.
//   - The Windows SSE2 renderer skips the last 8 rows. This port draws all rows
//     (the scalar behaviour).
//   - Float operations use one statement per operation. The mesh step must
//     not use FMA contraction, because the DLL uses plain SSE2 mulps/addps.

#ifndef YOGHOURT_GFX_FIRE_CORE_HPP
#define YOGHOURT_GFX_FIRE_CORE_HPP

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

namespace gfxfire {

// Truncate like the CRT _ftol2. Out of range gives INT_MIN.
inline std::int32_t ftol32(double v) {
    if(!(v > -2147483649.0 && v < 2147483648.0))
        return INT_MIN;
    return static_cast<std::int32_t>(v);
}

// The DLL uses one global Mersenne Twister (standard MT19937).
inline std::mt19937 &rng() {
    static std::mt19937 engine;
    return engine;
}
inline void reseed(std::uint32_t s) { rng().seed(s); }
// u32 * 2^-32 as a double, like the DLL.
inline double draw01() { return static_cast<double>(rng()()) * 2.3283064365386963e-10; }

struct Plane8 {
    int w = 0, h = 0, stride = 0;
    std::vector<std::uint8_t> d;
    void alloc(int nw, int nh) {
        w = nw;
        h = nh;
        stride = (nw + 15) & ~15;
        d.assign(static_cast<std::size_t>(stride) * nh, 0);
    }
    void free() { w = h = stride = 0; d.clear(); }
    std::uint8_t *row(int y) { return d.data() + static_cast<std::size_t>(y) * stride; }
    const std::uint8_t *row(int y) const { return d.data() + static_cast<std::size_t>(y) * stride; }
};

struct Seed {
    Plane8 plane;
    int x = 0, y = 0;
};

namespace detail {
#include "gfxFireTables.inc"

inline std::uint32_t swapRB(std::uint32_t v) {
    return (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
}
inline const std::uint32_t *builtin(int which) {
    static const auto tables = [] {
        std::array<std::array<std::uint32_t, 256>, 3> t{};
        const std::uint32_t *src[3] = {kRed, kBlue, kWhite};
        for(int k = 0; k < 3; ++k)
            for(int i = 0; i < 256; ++i)
                t[k][i] = swapRB(src[k][i]);
        return t;
    }();
    return tables[which].data();
}
// The custom table is global in the DLL. It is shared by all instances.
inline std::array<std::uint32_t, 256> &customTable() {
    static std::array<std::uint32_t, 256> t{};
    return t;
}
} // namespace detail

enum Palette { kPaletteRed = 0, kPaletteBlue = 1, kPaletteWhite = 2, kPaletteCustom = 3 };

struct Params {
    int forceH = -1, forceV = 0;
    int boundRangeH = 8, boundRangeV = 4;
    int numOfBlurForCoolMap = 20;
    std::uint32_t randomSeed = 0;
    int textureFilterType = 0;
    int coolRange = 1, coolStrength = 160;
    std::uint32_t denominator = 320, numerator = 35;
    bool edgeSmoothing = false;
    float scalingCoeff = 0.0f;
    int palette = kPaletteRed;
    Params() {
        const std::uint32_t bits = 0x37fba882u;
        std::memcpy(&scalingCoeff, &bits, 4);
    }
};

class FireCore {
public:
    Params p;
    int w = 0, h = 0; // target size, multiples of 16
    Plane8 F, W, K;
    std::vector<std::uint8_t> B; // blurred F1 rows, stride w
    std::vector<Seed> seeds;
    // Mesh planes, ncol x nrow floats each.
    int ncol = 0, nrow = 0;
    std::vector<float> Px, Py, Vx, Vy;

    // init(): drop F, W, K and seeds, then set the target.
    void init(int tw, int th, int nSeeds) {
        F.free();
        W.free();
        K.free();
        seeds.assign(static_cast<std::size_t>(nSeeds < 0 ? 0 : nSeeds), Seed());
        setTarget(tw, th);
    }

    void setTarget(int tw, int th) {
        w = tw;
        h = th;
        F.alloc(w, h);
        W.alloc(w, h);
        const int nc = (w >> 4) + 1, nr = (h >> 4) + 1;
        if(nc != ncol || nr != nrow) {
            ncol = nc;
            nrow = nr;
            const std::size_t n = static_cast<std::size_t>(ncol) * nrow;
            Px.assign(n, 0.f);
            Py.assign(n, 0.f);
            Vx.assign(n, 0.f);
            Vy.assign(n, 0.f);
            updateWarp();
        }
        if(K.w != w || K.h != h)
            updateCool();
    }

    bool ready() const { return F.w > 0 && K.w == F.w && ncol > 0; }

    // ---- mesh jitter -------------------------------------------------
    void updateWarp() {
        if(ncol < 2 || nrow < 2)
            return;
        auto at = [&](int y, int x) { return static_cast<std::size_t>(y) * ncol + x; };
        std::fill(Vx.begin(), Vx.end(), 0.f);
        std::fill(Vy.begin(), Vy.end(), 0.f);
        for(int x = 0; x < ncol; ++x) {
            Px[at(0, x)] = static_cast<float>(16 * x);
            Py[at(0, x)] = 0.f;
            Px[at(nrow - 1, x)] = static_cast<float>(16 * x);
            Py[at(nrow - 1, x)] = static_cast<float>(16 * (nrow - 1));
        }
        for(int y = 1; y < nrow - 1; ++y) {
            Px[at(y, 0)] = 0.f;
            Py[at(y, 0)] = static_cast<float>(16 * y);
            Px[at(y, ncol - 1)] = static_cast<float>(16 * (ncol - 1));
            Py[at(y, ncol - 1)] = static_cast<float>(16 * y);
        }
        for(int y = 1; y < nrow - 1; ++y)
            for(int x = 1; x < ncol - 1; ++x) {
                const std::int32_t jx = ftol32(draw01() * p.boundRangeH);
                Px[at(y, x)] = static_cast<float>(16 * x - (p.boundRangeH >> 1) - p.forceH + jx);
                const std::int32_t jy = ftol32(draw01() * p.boundRangeV);
                Py[at(y, x)] = static_cast<float>(16 * y - (p.boundRangeV >> 1) - p.forceV + jy);
            }
    }

    // ---- cool map ----------------------------------------------------
    void updateCool() {
        if(w <= 0 || h <= 0)
            return;
        if(K.w != w || K.h != h)
            K.alloc(w, h);
        else
            std::fill(K.d.begin(), K.d.end(), 0);
        if(p.edgeSmoothing) {
            static const std::uint8_t edge[16] = {0xf5, 200, 0xa0, 0x78, 0x50, 0x3c, 0x28, 0x14,
                                                  10,   8,   6,    4,    10,   8,    6,    4};
            for(int y = 0; y < h; ++y) {
                std::uint8_t *r = K.row(y);
                // Right side first; for w = 16 the left side wins.
                for(int i = 0; i < 16 && i < w; ++i)
                    r[w - 1 - i] = edge[i];
                for(int i = 0; i < 16 && i < w; ++i)
                    r[i] = edge[i];
            }
        }
        const std::uint32_t area = static_cast<std::uint32_t>(w) * static_cast<std::uint32_t>(h);
        const std::uint32_t count = (p.numerator * area) / p.denominator;
        for(std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t pos = static_cast<std::uint32_t>(ftol32(draw01() * static_cast<double>(area)));
            const std::int32_t heat = p.coolStrength + ftol32(draw01() * p.coolRange);
            if(pos < area)
                K.row(static_cast<int>(pos / w))[pos % w] = static_cast<std::uint8_t>(heat);
        }
        if(w >= 24 && h >= 3)
            for(int i = 0; i < p.numOfBlurForCoolMap; ++i)
                blurCool();
    }

    // ---- seeds -------------------------------------------------------
    // Copies the alpha byte (byte 3) of each pixel.
    void setFireSeed(int idx, const std::uint8_t *rgba, int sw, int sh, int pitchBytes) {
        if(idx < 0 || idx >= static_cast<int>(seeds.size()) || sw <= 0 || sh <= 0)
            return;
        Plane8 &pl = seeds[idx].plane;
        if(pl.w != sw || pl.h != sh)
            pl.alloc(sw, sh);
        for(int y = 0; y < sh; ++y) {
            const std::uint8_t *s = rgba + static_cast<std::size_t>(y) * pitchBytes;
            std::uint8_t *d = pl.row(y);
            for(int x = 0; x < sw; ++x)
                d[x] = s[x * 4 + 3];
        }
    }
    void clearFireSeed(int idx) {
        if(idx < 0 || idx >= static_cast<int>(seeds.size()))
            return;
        seeds[idx] = Seed();
    }

    // ---- one animation step -----------------------------------------
    // dst: target layer pixels (RGBA words). Rows h-2 and h-1 are zeroed.
    void cycle(std::uint8_t *dst, int pitchBytes) {
        if(!ready())
            return;
        stageSeed();
        stageMesh();
        buildEdges();
        render();
        colorize(dst, pitchBytes);
    }

    const std::uint32_t *table() const {
        return p.palette == kPaletteCustom ? detail::customTable().data()
                                           : detail::builtin(p.palette);
    }

    struct Rec {
        float x, u, v;
    };
    std::vector<std::vector<Rec>> rows; // edge records per target row

private:
    // H pass K -> W, V pass W -> K. The right neighbour of the last pixel is 0.
    void blurCool() {
        for(int y = 0; y < h; ++y) {
            const std::uint8_t *s = K.row(y);
            std::uint8_t *d = W.row(y);
            for(int x = 0; x < w; ++x) {
                const unsigned l = s[x > 0 ? x - 1 : 0];
                const unsigned r = x + 1 < w ? s[x + 1] : 0u;
                d[x] = static_cast<std::uint8_t>((l + 2u * s[x] + r) >> 2);
            }
        }
        for(int y = 0; y < h; ++y) {
            const std::uint8_t *a = W.row(y > 0 ? y - 1 : 0);
            const std::uint8_t *b = W.row(y);
            const std::uint8_t *c = W.row(y + 1 < h ? y + 1 : h - 1);
            std::uint8_t *d = K.row(y);
            if(y == 0 || y == h - 1) {
                const std::uint8_t *o = W.row(y == 0 ? 1 : h - 2);
                for(int x = 0; x < w; ++x)
                    d[x] = static_cast<std::uint8_t>((3u * b[x] + o[x]) >> 2);
            } else {
                for(int x = 0; x < w; ++x)
                    d[x] = static_cast<std::uint8_t>((a[x] + 2u * b[x] + c[x]) >> 2);
            }
        }
    }

    void stageSeed() {
        for(const Seed &s : seeds) {
            if(s.plane.w <= 0 || s.plane.h <= 0)
                continue;
            const int x0 = std::max(s.x, 0), y0 = std::max(s.y, 0);
            const int x1 = static_cast<int>(std::min<long long>(static_cast<long long>(s.x) + s.plane.w, w));
            const int y1 = static_cast<int>(std::min<long long>(static_cast<long long>(s.y) + s.plane.h, h));
            for(int y = y0; y < y1; ++y) {
                const std::uint8_t *src = s.plane.row(y - s.y);
                std::uint8_t *dst = F.row(y);
                for(int x = x0; x < x1; ++x)
                    dst[x] = static_cast<std::uint8_t>(std::min(255, dst[x] + src[x - s.x]));
            }
        }
    }

    // Spring step: every inner mesh node pushes its 24 neighbours.
    void stageMesh() {
        const float k = p.scalingCoeff;
        static const float rest5[5][5] = {
            {45.254833f, 35.777088f, 32.f, 35.777088f, 45.254833f},
            {35.777088f, 22.627417f, 16.f, 22.627417f, 35.777088f},
            {32.f, 16.f, 0.f, 16.f, 32.f},
            {35.777088f, 22.627417f, 16.f, 22.627417f, 35.777088f},
            {45.254833f, 35.777088f, 32.f, 35.777088f, 45.254833f}};
        for(int y = 2; y < nrow - 2; ++y)
            for(int x = 2; x < ncol - 2; ++x) {
                const std::size_t c = static_cast<std::size_t>(y) * ncol + x;
                for(int j = -2; j <= 2; ++j)
                    for(int i = -2; i <= 2; ++i) {
                        if(i == 0 && j == 0)
                            continue;
                        const std::size_t n = c + static_cast<std::ptrdiff_t>(j) * ncol + i;
                        const float dx = Px[n] - Px[c];
                        const float dy = Py[n] - Py[c];
                        if(i == 2 && j == 2) {
                            // The DLL runs this neighbour with x87 code: the
                            // square sum is rounded once from double, and t
                            // is rounded to float before it scales dx, dy.
                            const float d2 = static_cast<float>(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
                            const float dd = std::sqrt(d2);
                            const float td = static_cast<float>((static_cast<double>(rest5[4][4]) - dd) * static_cast<double>(k));
                            const float tx = td * dx;
                            const float ty = td * dy;
                            Vx[n] = Vx[n] + tx;
                            Vy[n] = Vy[n] + ty;
                            continue;
                        }
                        const float dyy = dy * dy;
                        const float dxx = dx * dx;
                        const float sum = dyy + dxx;
                        const float d = std::sqrt(sum);
                        const float diff = rest5[j + 2][i + 2] - d;
                        const float t = diff * k;
                        const float ax = t * dx;
                        const float ay = t * dy;
                        Vx[n] = Vx[n] + ax;
                        Vy[n] = Vy[n] + ay;
                    }
            }
        // P += V. The SSE2 code of the DLL adds V twice to some columns when
        // the mesh has 9 or more columns: 4 wide blocks cover columns 4 to
        // 4*nb+3, then a scalar tail starts at column 4*nb. This port keeps
        // the double add, because the tail overlaps the last block.
        const int blocks = ncol >= 9 ? (ncol - 5) / 4 : 0;
        for(int y = 1; y < nrow - 1; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * ncol;
            auto add = [&](int x) {
                Px[row + x] = Px[row + x] + Vx[row + x];
                Py[row + x] = Py[row + x] + Vy[row + x];
            };
            if(blocks == 0) {
                for(int x = 1; x < ncol - 1; ++x)
                    add(x);
                continue;
            }
            for(int x = 1; x < 4 + 4 * blocks; ++x)
                add(x);
            for(int x = 4 * blocks; x < ncol - 1; ++x)
                add(x);
        }
    }

    void emit(int r, float x, float u, float v) { rows[r].push_back(Rec{x, u, v}); }

    struct Edge {
        float x0, y0, x1, y1;
        int u0, v0, u1, v1;
    };

    // Scalar edge rasterizer (used for the tail edges). Double precision.
    void rasterEdge(const Edge &e) {
        float dy = e.y1 - e.y0;
        if(dy < 0.01f)
            dy = 0.01f;
        const double ddy = dy;
        const float dxdy = static_cast<float>((static_cast<double>(e.x1) - static_cast<double>(e.x0)) / ddy);
        const float dudy = static_cast<float>(static_cast<double>(e.u1 - e.u0) / ddy);
        const float dvdy = static_cast<float>(static_cast<double>(e.v1 - e.v0) / ddy);
        float x = e.x0, u = static_cast<float>(e.u0), v = static_cast<float>(e.v0);
        if(e.y0 < 0.f) {
            const double t = -static_cast<double>(e.y0);
            x = static_cast<float>(t * dxdy + x);
            u = static_cast<float>(t * dudy + u);
            v = static_cast<float>(t * dvdy + v);
        }
        const int start = e.y0 > 0.f ? ftol32(e.y0) + 1 : 0;
        const int ye = std::min(ftol32(e.y1), h - 1);
        if(start < ye)
            for(int r = start; r <= ye; ++r) {
                emit(r, x, u, v);
                x = x + dxdy;
                u = u + dudy;
                v = v + dvdy;
            }
    }

    // SSE2 edge rasterizer (4 edges per call in the DLL). Single precision.
    // 1/dy uses rcpps and one Newton step in the DLL. Here 1/dy is exact,
    // then the same Newton step runs. The start row is 0 for 0 < y0 < 1.
    void rasterEdgeSse(const Edge &e) {
        float dy = e.y1 - e.y0;
        if(!(dy > 0.01f))
            dy = 0.01f;
        const float r0 = 1.0f / dy;
        const float twice = r0 + r0;
        const float rd = r0 * dy;
        const float rdr = rd * r0;
        const float r = twice - rdr;
        const float dxdy = (e.x1 - e.x0) * r;
        const float dudy = static_cast<float>(e.u1 - e.u0) * r;
        const float dvdy = static_cast<float>(e.v1 - e.v0) * r;
        float x = e.x0, u = static_cast<float>(e.u0), v = static_cast<float>(e.v0);
        if(e.y0 < 0.f) {
            const float t = 0.0f - e.y0;
            const float tx = t * dxdy, tu = t * dudy, tv = t * dvdy;
            x = tx + x;
            u = tu + u;
            v = tv + v;
        }
        const int y0i = ftol32(e.y0);
        const int start = y0i < 1 ? 0 : y0i + 1;
        const int ye = std::min(ftol32(e.y1), h - 1);
        if(start < ye)
            for(int r2 = start; r2 <= ye; ++r2) {
                emit(r2, x, u, v);
                x = x + dxdy;
                u = u + dudy;
                v = v + dvdy;
            }
    }

    Edge horizontalEdge(int j, int x) const {
        const std::size_t a = static_cast<std::size_t>(j) * ncol + x - 1, b = a + 1;
        return Edge{Px[a] - 0.5f, Py[a] - 0.5f, Px[b] - 0.5f, Py[b] - 0.5f, 16 * (x - 1), 16 * j, 16 * x, 16 * j};
    }
    Edge verticalEdge(int j, int x) const {
        const std::size_t a = static_cast<std::size_t>(j - 1) * ncol + x, b = a + ncol;
        return Edge{Px[a] - 0.5f, Py[a] - 0.5f, Px[b] - 0.5f, Py[b] - 0.5f, 16 * x, 16 * (j - 1), 16 * x, 16 * j};
    }

    // The DLL builds edges in groups of 4 (SSE2 path) and the rest one by one.
    // A group starts at j = 1, 5, 9 ... while j < 4 * floor((nrow - 1) / 4).
    void buildEdges() {
        rows.resize(static_cast<std::size_t>(h));
        for(int r = 0; r < h; ++r) {
            rows[r].clear();
            emit(r, -0.5f, 0.f, static_cast<float>(r) + 0.5f);
        }
        const int groupEnd = 4 * ((nrow - 1) / 4);
        for(int x = 1; x < ncol - 1; ++x) {
            int j = 1;
            if(1 < groupEnd)
                do {
                    for(int k = 0; k < 4; ++k)
                        rasterEdgeSse(horizontalEdge(j + k, x));
                    j += 4;
                } while(j < groupEnd);
            for(; j < nrow - 1; ++j)
                rasterEdge(horizontalEdge(j, x));
            j = 1;
            if(1 < groupEnd)
                do {
                    for(int k = 0; k < 4; ++k)
                        rasterEdgeSse(verticalEdge(j + k, x));
                    j += 4;
                } while(j < groupEnd);
            for(; j < nrow; ++j)
                rasterEdge(verticalEdge(j, x));
        }
        const float right = static_cast<float>((ncol - 1) * 16);
        for(int r = 0; r < h; ++r)
            emit(r, right - 0.5f, right, static_cast<float>(r) + 0.5f);
    }

    // Texture mapped spans: W = F sampled through the warped mesh.
    void render() {
        const int fs = F.stride;
        const std::uint32_t limit = static_cast<std::uint32_t>(F.h) * static_cast<std::uint32_t>(fs);
        const std::uint8_t *fp = F.d.data();
        const bool bilinear = p.textureFilterType != 0;
        const auto at = [&](std::uint32_t idx) -> unsigned { return idx < limit ? fp[idx] : 0u; };
        for(int y = 0; y < h; ++y) {
            auto &rec = rows[y];
            std::stable_sort(rec.begin(), rec.end(), [](const Rec &a, const Rec &b) { return a.x < b.x; });
            std::uint8_t *out = W.row(y);
            for(std::size_t k = 0; k + 1 < rec.size(); ++k) {
                float x0 = rec[k].x, x1 = rec[k + 1].x;
                float u0 = rec[k].u, u1 = rec[k + 1].u;
                float v0 = rec[k].v, v1 = rec[k + 1].v;
                float dx = x1 - x0;
                if(dx < 0.01f)
                    dx = 0.01f;
                const float du = static_cast<float>((static_cast<double>(u1) - static_cast<double>(u0)) / dx);
                const float dv = static_cast<float>((static_cast<double>(v1) - static_cast<double>(v0)) / dx);
                if(x0 < 0.f) {
                    u0 = static_cast<float>(static_cast<double>(u0) - static_cast<double>(x0) * du);
                    v0 = static_cast<float>(static_cast<double>(v0) - static_cast<double>(x0) * dv);
                    x0 = 0.f;
                }
                if(x1 > static_cast<float>(w))
                    x1 = static_cast<float>(w);
                std::uint32_t u = static_cast<std::uint32_t>(ftol32(65536.0 * u0));
                std::uint32_t v = static_cast<std::uint32_t>(ftol32(65536.0 * v0));
                const std::uint32_t us = static_cast<std::uint32_t>(ftol32(65536.0 * du));
                const std::uint32_t vs = static_cast<std::uint32_t>(ftol32(65536.0 * dv));
                const int xs = ftol32(x0), xe = ftol32(x1);
                for(int x = xs; x < xe; ++x) {
                    const std::uint32_t idx = static_cast<std::uint32_t>(static_cast<std::int32_t>(v) >> 16) * fs +
                                              static_cast<std::uint32_t>(static_cast<std::int32_t>(u) >> 16);
                    if(!bilinear) {
                        out[x] = static_cast<std::uint8_t>(at(idx));
                    } else {
                        const unsigned fu = u & 0xffff, fv = v & 0xffff;
                        const unsigned top = (at(idx) * (65535u - fu) + at(idx + 1) * fu) >> 16;
                        const unsigned bot = (at(idx + fs) * (65535u - fu) + at(idx + fs + 1) * fu) >> 16;
                        out[x] = static_cast<std::uint8_t>((top * (65535u - fv) + bot * fv) >> 16);
                    }
                    u += us;
                    v += vs;
                }
            }
        }
    }

    static unsigned avg(unsigned a, unsigned b) { return (a + b + 1u) >> 1; }

    // Cool, blur, colour table. Stage 5 does not store into W, so the pixel
    // W[w-1] (never drawn by the renderer) keeps its value from the cool map
    // blur for the whole life of the target.
    void colorize(std::uint8_t *dst, int pitchBytes) {
        B.resize(static_cast<std::size_t>(w) * h);
        std::vector<std::uint8_t> f1(static_cast<std::size_t>(w));
        for(int y = 0; y < h; ++y) {
            const std::uint8_t *wr = W.row(y);
            const std::uint8_t *kr = K.row(y);
            for(int x = 0; x < w; ++x)
                f1[x] = wr[x] > kr[x] ? static_cast<std::uint8_t>(wr[x] - kr[x]) : 0;
            std::uint8_t *b = B.data() + static_cast<std::size_t>(y) * w;
            for(int x = 0; x < w; ++x) {
                const unsigned l = f1[x > 0 ? x - 1 : 0], c = f1[x], r = f1[x + 1 < w ? x + 1 : w - 1];
                b[x] = static_cast<std::uint8_t>(avg(avg(l, c), avg(c, r)));
            }
        }
        for(int y = 2; y < h; ++y) {
            const std::uint8_t *a = B.data() + static_cast<std::size_t>(y - 2) * w;
            const std::uint8_t *b = a + w, *c = b + w;
            std::uint8_t *f = F.row(y - 2);
            for(int x = 0; x < w; ++x)
                f[x] = static_cast<std::uint8_t>(avg(avg(a[x], b[x]), avg(b[x], c[x])));
        }
        std::memset(F.row(h - 2), 0, static_cast<std::size_t>(w));
        std::memset(F.row(h - 1), 0, static_cast<std::size_t>(w));
        const std::uint32_t *t = table();
        for(int y = 0; y < h; ++y) {
            std::uint32_t *o = reinterpret_cast<std::uint32_t *>(dst + static_cast<std::size_t>(y) * pitchBytes);
            const std::uint8_t *f = F.row(y);
            for(int x = 0; x < w; ++x)
                o[x] = y < h - 2 ? t[f[x]] : 0u;
        }
    }
};

} // namespace gfxfire

#endif
