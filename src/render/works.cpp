#include "render/works.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace render::works {
namespace {

constexpr float kPi = 3.14159265f;
// A tile is this many pixels in the model's own space: the ground's x and
// y times this, and height in pixels, make a space the right way up.
constexpr float kTilePx = 32.0f;

// --- Numbers from positions -------------------------------------------------

uint32_t mixh(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
float h01(uint32_t h) { return static_cast<float>(mixh(h) & 0xFFFFFFu) / 16777216.0f; }
float rnd(uint32_t seed, int i) { return h01(seed * 2654435761u + static_cast<uint32_t>(i) * 40503u + 17u); }
float lattice(int x, int y, uint32_t salt) {
    return h01(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ salt * 83492791u);
}
// Smooth value noise, 0..1, on the map's ground (the same across tiles).
float noise(Vector2 g, float freq, uint32_t salt) {
    const float x = g.x * freq;
    const float y = g.y * freq;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const float sx = fx * fx * (3.0f - 2.0f * fx);
    const float sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = lattice(x0, y0, salt) + (lattice(x0 + 1, y0, salt) - lattice(x0, y0, salt)) * sx;
    const float b = lattice(x0, y0 + 1, salt) + (lattice(x0 + 1, y0 + 1, salt) - lattice(x0, y0 + 1, salt)) * sx;
    return a + (b - a) * sy;
}

Vector2 add(Vector2 a, Vector2 b) { return {a.x + b.x, a.y + b.y}; }
Vector2 sub(Vector2 a, Vector2 b) { return {a.x - b.x, a.y - b.y}; }
Vector2 mul(Vector2 a, float k) { return {a.x * k, a.y * k}; }
float dot(Vector2 a, Vector2 b) { return a.x * b.x + a.y * b.y; }
float len(Vector2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
Vector2 unit(Vector2 a) {
    const float l = len(a);
    return l > 1e-6f ? Vector2{a.x / l, a.y / l} : Vector2{1.0f, 0.0f};
}
Vector2 perp(Vector2 a) { return {-a.y, a.x}; }  // to its left
float clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }
float smooth(float x) {
    x = clamp01(x);
    return x * x * (3.0f - 2.0f * x);
}

// --- Colours --------------------------------------------------------------------

enum class Mat : uint8_t { None, Spoil, Clay, Planks, Wattle, Floor, Duck, Bags, Turf, Door };

Color scaled(Color c, float k) {
    auto ch = [k](unsigned char v) { return static_cast<unsigned char>(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f)); };
    return {ch(c.r), ch(c.g), ch(c.b), 255};
}
// The light as pixel art has it: a few steps, not a gradient.
float stepped(float b) {
    static constexpr float kLevels[] = {0.5f, 0.62f, 0.74f, 0.86f, 0.97f, 1.08f, 1.2f};
    float best = kLevels[0];
    for (const float l : kLevels) {
        if (std::fabs(l - b) < std::fabs(best - b)) best = l;
    }
    return best;
}
// Light from over the viewer's right (the houses' lit faces are their +x ones).
constexpr float kLx = 0.62f;
constexpr float kLy = -0.18f;
constexpr float kLz = 0.76f;
float light(float nx, float ny, float nz) {
    const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
    const float d = l > 0.0f ? (nx * kLx + ny * kLy + nz * kLz) / l : 1.0f;
    return 0.52f + 0.62f * std::max(d, -0.1f);
}

constexpr Color kSpoil{120, 97, 68, 255};
constexpr Color kClods{94, 74, 53, 255};
constexpr Color kLoess{150, 124, 86, 255};
constexpr Color kClay{142, 110, 72, 255};
constexpr Color kPlank{130, 113, 88, 255};
constexpr Color kWattle{124, 100, 64, 255};
constexpr Color kFloor{72, 60, 45, 255};
constexpr Color kSlat{126, 101, 70, 255};
constexpr Color kBag{188, 172, 130, 255};
constexpr Color kPolyBag{206, 204, 192, 255};
constexpr Color kGrass{98, 114, 62, 255};
constexpr Color kDark{26, 22, 18, 255};
constexpr Color kLog{136, 101, 66, 255};
constexpr Color kLogEnd{204, 172, 124, 255};
constexpr Color kWood{112, 86, 57, 255};
constexpr Color kSteel{78, 74, 70, 255};
constexpr Color kRust{124, 80, 52, 255};
constexpr Color kCrate{88, 102, 64, 255};
constexpr Color kWire{168, 168, 162, 255};
constexpr Color kInk{44, 36, 27, 255};  // the outline round what's put in: dark earth, not black

// --- What's dug, what's thrown up -------------------------------------------------

// How a tile's works look: the bank thrown up round what's dug, its walls, its floor.
struct Look {
    float bank_front = 3.0f;
    float bank_rear = 3.0f;
    bool horseshoe = false;  // no bank at the back
    bool bags = false;       // sandbags on the front bank
    bool parapet = false;    // the bank higher to the front
    Vector2 facing{1, 0};
    Mat wall = Mat::Clay;
    Mat floor = Mat::Floor;
    uint32_t seed = 0;
};

struct Seg {
    Vector2 a;  // tile-local
    Vector2 b;
    float half;
    float depth;
    bool steps = false;  // up from `a` to the ground at `b` (a dugout's way in)
    float start = 0.0f;  // along the trench, where `a` is
};

// A tile's works, on the tile (0..1).
struct Parts {
    Spec spec;
    Look look;
    std::vector<Seg> segs;
    bool round = false;  // a round pit
    Vector2 rc{};
    float rr = 0.0f;
    float rdepth = 0.0f;
    bool rect = false;  // a long pit along `kd`, its ramp at the back
    Vector2 kc{};
    Vector2 kd{1, 0};
    float klen = 0.0f;
    float kwid = 0.0f;
    float kdepth = 0.0f;
    float kramp = 0.0f;
    bool mound = false;  // a dugout's roof
    Vector2 mc{};
    Vector2 md{1, 0};
    float mlen = 0.0f;
    float mwid = 0.0f;
    float mh = 0.0f;
    float top = 8.0f;    // the most anything rises (pixels)
    float deep = 10.0f;  // the deepest it goes
    float margin = 0.2f;
};

Vector2 dir_of_link(int bit) {
    switch (bit) {
        case 0: return {1, 0};
        case 1: return {-1, 0};
        case 2: return {0, 1};
        default: return {0, -1};
    }
}

Parts parts_of(const Spec& spec) {
    Parts sc;
    sc.spec = spec;
    sc.spec.facing = unit(spec.facing);
    Look& look = sc.look;
    look.facing = sc.spec.facing;
    look.parapet = spec.parapet;
    look.seed = spec.seed;
    const Vector2 c{0.5f, 0.5f};
    const Vector2 f = sc.spec.facing;
    const uint32_t h = spec.seed;
    // Passages on to the trenches next to it, from the middle out past the edge.
    auto passages = [&](float half, float depth) {
        float start = 0.0f;
        for (int bit = 0; bit < 4; ++bit) {
            if (!(spec.links & (1u << bit))) continue;
            const Vector2 d = dir_of_link(bit);
            sc.segs.push_back({c, add(c, mul(d, 0.5f)), half, depth, false, start});
            start += 0.5f;
        }
    };
    switch (spec.kind) {
        case Kind::Trench: {
            passages(0.14f, 9.0f);
            if (sc.segs.empty()) sc.segs.push_back({add(c, mul(perp(f), -0.22f)), add(c, mul(perp(f), 0.22f)), 0.14f, 9.0f});
            look.bank_front = spec.parapet ? 5.5f : 3.0f;
            look.bank_rear = spec.parapet ? 2.4f : 3.0f;
            look.bags = spec.parapet;
            const float pick = rnd(h, 1);
            look.wall = pick < 0.45f ? Mat::Planks : pick < 0.8f ? Mat::Wattle : Mat::Clay;
            look.floor = Mat::Duck;
            sc.top = 7.0f;
            // What's built into it: a pit off the ditch for it, its way
            // (half dug while it's being made).
            const uint8_t built = spec.fit != kNoFit ? spec.fit : spec.fitting <= kMortarPost ? spec.fitting : kNoFit;
            const float dug = spec.fit != kNoFit ? 1.0f : 0.45f;
            if (built != kNoFit) sc.round = true;
            switch (built) {
                case kCell: sc.rc = add(c, mul(f, 0.17f)), sc.rr = 0.085f, sc.rdepth = 8.0f * dug; break;
                case kNest: sc.rc = add(c, mul(f, 0.11f)), sc.rr = 0.19f, sc.rdepth = 7.0f * dug; break;
                case kAtPost: sc.rc = add(c, mul(f, 0.15f)), sc.rr = 0.13f, sc.rdepth = 8.0f * dug; break;
                case kMortarPost: sc.rc = add(c, mul(f, -0.2f)), sc.rr = 0.21f, sc.rdepth = 6.0f * dug; break;
                default: break;
            }
            break;
        }
        case Kind::Foxhole: {
            passages(0.12f, 8.0f);
            sc.round = true;
            sc.rc = c;
            sc.rr = 0.19f;
            sc.rdepth = 9.0f;
            look.bank_front = spec.parapet ? 5.0f : 3.2f;
            look.bank_rear = spec.parapet ? 2.2f : 3.2f;
            look.bags = spec.parapet;
            look.wall = rnd(h, 1) < 0.5f ? Mat::Clay : Mat::Wattle;
            sc.top = 7.0f;
            sc.margin = 0.3f;
            break;
        }
        case Kind::Dugout: {
            // The way in: from a trench next to it if there's one (the
            // nearest side first), or down steps from the open ground.
            Vector2 way{0, 1};
            bool joined = false;
            for (const int bit : {2, 0, 1, 3}) {
                if (spec.links & (1u << bit)) {
                    way = dir_of_link(bit);
                    joined = true;
                    break;
                }
            }
            sc.mound = true;
            sc.md = perp(way);
            sc.mc = add(c, mul(way, -0.08f));
            sc.mlen = 0.34f;
            sc.mwid = 0.24f;
            sc.mh = 8.5f;
            const Vector2 door = add(sc.mc, mul(way, 0.16f));
            sc.segs.push_back({door, add(c, mul(way, joined ? 0.5f : 0.44f)), 0.1f, 9.0f, !joined});
            if (joined) passages(0.13f, 9.0f);
            look.bank_front = 2.0f;
            look.bank_rear = 2.0f;
            look.wall = Mat::Planks;
            sc.top = 16.0f;
            sc.margin = 0.25f;
            break;
        }
        case Kind::Parapet: {
            // A scrape behind a bank of earth, sandbags along its top.
            const Vector2 across = perp(f);
            const Vector2 m = add(c, mul(f, -0.02f));
            sc.segs.push_back({add(m, mul(across, -0.3f)), add(m, mul(across, 0.3f)), 0.1f, 3.0f});
            look.parapet = true;
            look.bank_front = 6.0f;
            look.bank_rear = 0.8f;
            look.bags = true;
            sc.top = 8.0f;
            sc.deep = 4.0f;
            sc.margin = 0.3f;
            break;
        }
        case Kind::MortarPit: {
            sc.round = true;
            sc.rc = c;
            sc.rr = 0.23f;
            sc.rdepth = 6.0f;
            // The crew's slit off to the back and the side.
            const Vector2 back = unit(add(mul(f, -1.0f), mul(perp(f), rnd(h, 2) < 0.5f ? 0.7f : -0.7f)));
            sc.segs.push_back({add(c, mul(back, 0.2f)), add(c, mul(back, 0.44f)), 0.07f, 8.0f});
            passages(0.12f, 8.0f);
            look.bank_front = 4.2f;
            look.bank_rear = 3.2f;
            look.parapet = true;
            sc.top = 7.0f;
            sc.margin = 0.35f;
            break;
        }
        case Kind::GunPit: {
            sc.round = true;
            sc.rc = add(c, mul(f, 0.02f));
            sc.rr = 0.43f;
            sc.rdepth = 2.5f;
            const Vector2 side = perp(f);
            const float s = rnd(h, 2) < 0.5f ? 1.0f : -1.0f;
            sc.segs.push_back({add(c, add(mul(f, -0.3f), mul(side, 0.42f * s))), add(c, add(mul(f, -0.52f), mul(side, 0.16f * s))), 0.065f, 8.0f});
            look.bank_front = 5.8f;
            look.bank_rear = 4.6f;
            look.horseshoe = true;
            look.parapet = true;
            sc.top = 8.0f;
            sc.margin = 0.6f;
            break;
        }
        case Kind::Caponier: {
            sc.rect = true;
            sc.kc = add(c, mul(f, -0.02f));
            sc.kd = f;
            sc.klen = 0.54f;
            sc.kwid = 0.24f;
            sc.kdepth = 7.0f;
            sc.kramp = 0.34f;
            look.bank_front = 6.5f;
            look.bank_rear = 4.6f;
            look.horseshoe = true;
            look.parapet = true;
            sc.top = 9.0f;
            sc.margin = 0.65f;
            break;
        }
        case Kind::Wire:
        case Kind::Hedgehogs:
            sc.top = 16.0f;
            sc.deep = 1.0f;
            sc.margin = 0.2f;
            break;
    }
    if (spec.damage >= 2) {  // shelled: caved in in places
        for (Seg& sg : sc.segs) sg.depth *= spec.damage >= 3 ? 0.45f : 0.75f;
        sc.rdepth *= spec.damage >= 3 ? 0.5f : 0.8f;
        sc.kdepth *= spec.damage >= 3 ? 0.55f : 0.85f;
        sc.mh *= spec.damage >= 3 ? 0.5f : 0.85f;
        look.bags = look.bags && spec.damage < 3;
    }
    return sc;
}

struct Hit {
    float h = 0.0f;      // pixels above the ground (below: negative)
    Mat top = Mat::None;
    Mat wall = Mat::Clay;
    Vector2 in{};        // across a wall, into what's dug
    float along = 0.0f;  // along the wall (tiles)
    float ref = -1e9f;   // how near (x + y) what stands in it is: nearer than that, it's in front of him
    bool mound = false;
};

// What's dug on the map round the tile: its own and its neighbours' as far
// as they reach it (a ray down to it passes over their banks; their banks
// spill onto it).
enum class Shape : uint8_t { Seg, Circle, Box };
struct Feature {
    Shape shape = Shape::Seg;
    Vector2 a{};  // a segment's end; a circle's, a box's middle
    Vector2 b{};  // a segment's other end; a box's way
    float half = 0.0f;  // a segment's half width, a circle's radius, a box's half length
    float wid = 0.0f;   // a box's half width
    float depth = 0.0f;
    float ramp = 0.0f;
    bool steps = false;
    float start = 0.0f;
    int look = 0;
};
struct Mound {
    Vector2 c;
    Vector2 d;
    float len;
    float wid;
    float h;
};

struct Scene {
    int tx = 0;
    int ty = 0;
    Parts own;  // the tile's own: what's built into it goes by it
    std::vector<Look> looks;
    std::vector<Feature> feats;
    std::vector<Mound> mounds;
    float top = 0.0f;
    float deep = 0.0f;

    Hit eval(Vector2 p) const {
        Hit hit;
        float e = 1e9f;
        float depth = 0.0f;
        Mat floor_here = Mat::Floor;
        const Feature* near = nullptr;
        Vector2 ref{};
        for (const Feature& ft : feats) {
            const Look& lk = looks[static_cast<size_t>(ft.look)];
            switch (ft.shape) {
                case Shape::Seg: {
                    const Vector2 ab = sub(ft.b, ft.a);
                    const float l2 = std::max(dot(ab, ab), 1e-8f);
                    const float t = clamp01(dot(sub(p, ft.a), ab) / l2);
                    const Vector2 q = add(ft.a, mul(ab, t));
                    const float d = len(sub(p, q));
                    const float ee = d - ft.half;
                    if (ee < e) {
                        e = ee;
                        near = &ft;
                        hit.in = d > 1e-5f ? mul(sub(q, p), 1.0f / d) : unit(perp(ab));
                        depth = ft.steps ? ft.depth * std::floor((1.0f - t) * 4.0f + 0.999f) / 4.0f : ft.depth;
                        hit.along = ft.start + t * std::sqrt(l2);
                        hit.wall = lk.wall;
                        floor_here = ft.steps ? Mat::Floor : lk.floor;
                        ref = q;
                    }
                    break;
                }
                case Shape::Circle: {
                    const float d = len(sub(p, ft.a));
                    const float ee = d - ft.half;
                    if (ee < e) {
                        e = ee;
                        near = &ft;
                        hit.in = d > 1e-5f ? mul(sub(ft.a, p), 1.0f / d) : Vector2{1, 0};
                        depth = ft.depth;
                        hit.along = (std::atan2(p.y - ft.a.y, p.x - ft.a.x) + kPi) * ft.half;
                        hit.wall = lk.wall;
                        floor_here = Mat::Floor;
                        ref = ft.a;
                    }
                    break;
                }
                case Shape::Box: {
                    const Vector2 side = perp(ft.b);
                    const float a = dot(sub(p, ft.a), ft.b);
                    const float c = dot(sub(p, ft.a), side);
                    const float qx = std::fabs(a) - ft.half;
                    const float qy = std::fabs(c) - ft.wid;
                    const float ee = len({std::max(qx, 0.0f), std::max(qy, 0.0f)}) + std::min(std::max(qx, qy), 0.0f);
                    if (ee < e) {
                        e = ee;
                        near = &ft;
                        Vector2 grad;
                        if (qx > 0.0f && qy > 0.0f) {
                            grad = unit({(a > 0 ? 1.0f : -1.0f) * qx, (c > 0 ? 1.0f : -1.0f) * qy});
                        } else if (qx > qy) {
                            grad = {a > 0 ? 1.0f : -1.0f, 0.0f};
                        } else {
                            grad = {0.0f, c > 0 ? 1.0f : -1.0f};
                        }
                        hit.in = mul(add(mul(ft.b, grad.x), mul(side, grad.y)), -1.0f);
                        depth = ft.depth * clamp01((a + ft.half) / std::max(ft.ramp, 0.01f));
                        hit.along = std::fabs(grad.x) > 0.5f ? c : a;
                        hit.wall = grad.x > 0.5f ? Mat::Planks : Mat::Clay;  // the front wall shored up with logs
                        floor_here = Mat::Floor;
                        ref = ft.a;
                    }
                    break;
                }
            }
        }
        if (near) hit.ref = ref.x + ref.y;
        if (near && e < 0.0f) {
            hit.h = -depth;
            hit.top = floor_here;
        } else if (near) {
            // The bank: dug earth thrown up a little way off the edge; the
            // breastwork to the front higher and broader; lumpy, clods
            // scattered past it.
            const Look& lk = looks[static_cast<size_t>(near->look)];
            const Vector2 out = mul(hit.in, -1.0f);
            const float ahead = dot(out, lk.facing);
            const float f = lk.parapet ? smooth((ahead + 0.3f) / 0.9f) : 0.5f;
            float hb = lk.bank_rear + (lk.bank_front - lk.bank_rear) * f;
            if (lk.horseshoe) hb *= clamp01((0.72f + ahead) / 0.5f);
            hb *= 0.8f + 0.4f * noise(p, 6.0f, 11u);
            const float ec = 0.075f + 0.045f * f + (noise(p, 4.0f, 23u) - 0.5f) * 0.03f;
            const float hw = 0.07f + 0.05f * f;
            const float x = (e - ec) / hw;
            float h = std::fabs(x) < 1.0f ? hb * std::pow(std::cos(x * kPi * 0.5f), 2.0f) : 0.0f;
            if (h < 0.6f && e < ec + hw * 2.2f && noise(p, 22.0f, 37u) > 0.74f) h = std::max(h, 0.8f);  // clods
            hit.h = h;
            hit.top = h > 0.0f ? Mat::Spoil : Mat::None;

        }
        for (const Mound& m : mounds) {
            // A dugout's roof: logs under earth, a low mound with a flat top.
            const Vector2 side = perp(m.d);
            const float a = dot(sub(p, m.c), m.d);
            const float c = dot(sub(p, m.c), side);
            const float qx = std::fabs(a) - m.len;
            const float qy = std::fabs(c) - m.wid;
            const float dm = len({std::max(qx, 0.0f), std::max(qy, 0.0f)}) + std::min(std::max(qx, qy), 0.0f);
            const float hm = m.h * smooth((0.03f - dm) / 0.13f) * (0.92f + 0.16f * noise(p, 9.0f, 51u));
            const bool dug = near && e < 0.0f;
            if (hm > hit.h && !dug) {
                hit.h = hm;
                hit.top = Mat::Turf;
                hit.mound = true;
                hit.ref = -1e9f;  // it all stands up in front of whoever's behind it
            }
            if (!dug && dm < 0.02f) hit.wall = Mat::Door;  // its face where the way in cuts into it
        }
        return hit;
    }
};

// The tile's works and whatever of its neighbours' comes near it.
Scene scene_of(const Area& area, int tx, int ty) {
    Scene sc;
    sc.tx = tx;
    sc.ty = ty;
    const float x0 = static_cast<float>(tx);
    const float y0 = static_cast<float>(ty);
    auto off_tile = [&](Vector2 g) {  // how far from the tile
        const float dx = std::max({x0 - g.x, 0.0f, g.x - x0 - 1.0f});
        const float dy = std::max({y0 - g.y, 0.0f, g.y - y0 - 1.0f});
        return std::sqrt(dx * dx + dy * dy);
    };
    constexpr float kReach = 0.6f;  // a bank's reach, a ray's way down over the ones in front
    for (int k = 0; k < 9; ++k) {
        if (!area.has[static_cast<size_t>(k)]) continue;
        const Parts pt = parts_of(area.spec[static_cast<size_t>(k)]);
        const Vector2 o{x0 + static_cast<float>(k % 3 - 1), y0 + static_cast<float>(k / 3 - 1)};
        if (k == 4) sc.own = pt;
        sc.top = std::max(sc.top, pt.top);
        sc.deep = std::max(sc.deep, pt.deep);
        const int li = static_cast<int>(sc.looks.size());
        sc.looks.push_back(pt.look);
        for (const Seg& sg : pt.segs) {
            const Vector2 a = add(o, sg.a);
            const Vector2 b = add(o, sg.b);
            float best = 1e9f;
            for (int i = 0; i <= 6; ++i) best = std::min(best, off_tile(add(a, mul(sub(b, a), static_cast<float>(i) / 6.0f))));
            if (best - sg.half > kReach) continue;
            Feature ft;
            ft.shape = Shape::Seg;
            ft.a = a;
            ft.b = b;
            ft.half = sg.half;
            ft.depth = sg.depth;
            ft.steps = sg.steps;
            ft.start = sg.start;
            ft.look = li;
            sc.feats.push_back(ft);
        }
        if (pt.round && off_tile(add(o, pt.rc)) - pt.rr <= kReach) {
            Feature ft;
            ft.shape = Shape::Circle;
            ft.a = add(o, pt.rc);
            ft.half = pt.rr;
            ft.depth = pt.rdepth;
            ft.look = li;
            sc.feats.push_back(ft);
        }
        if (pt.rect && off_tile(add(o, pt.kc)) - pt.klen <= kReach) {
            Feature ft;
            ft.shape = Shape::Box;
            ft.a = add(o, pt.kc);
            ft.b = pt.kd;
            ft.half = pt.klen;
            ft.wid = pt.kwid;
            ft.depth = pt.kdepth;
            ft.ramp = pt.kramp;
            ft.look = li;
            sc.feats.push_back(ft);
        }
        if (pt.mound && off_tile(add(o, pt.mc)) - pt.mlen <= kReach) sc.mounds.push_back({add(o, pt.mc), pt.md, pt.mlen, pt.mwid, pt.mh});
    }
    return sc;
}

// --- The pixels ---------------------------------------------------------------

struct Canvas {
    const Scene& sc;
    std::function<float(Vector2)> ground;  // world pixels up, at a ground point
    float x0 = 0.0f;
    float y0 = 0.0f;
    int w = 0;
    int h = 0;
    std::vector<Color> back;
    std::vector<Color> front;
    std::vector<float> z;        // how near (x + y) what's shown at each pixel is
    std::vector<uint8_t> thing;  // put in over the ground (a sandbag, a log): gets an outline
    std::vector<uint8_t> kind;   // 0 nothing, 1 a top, 2 a wall

    Vector2 screen(Vector2 g, float lift) const {
        return {(g.x - g.y) * 32.0f - x0, (g.x + g.y) * 16.0f - ground(g) - lift - y0};
    }
    // A point of something put in: shown if it's nearer than what's there.
    uint8_t mark = 1;  // what the next splats are: 1 outlined, 2 not (wire)
    void splat(Vector2 g, float lift, Color c, bool in_front) {
        const Vector2 p = screen(g, lift);
        const int ix = static_cast<int>(std::floor(p.x));
        const int iy = static_cast<int>(std::floor(p.y));
        if (ix < 0 || iy < 0 || ix >= w || iy >= h) return;
        const size_t i = static_cast<size_t>(iy * w + ix);
        const float s = g.x + g.y + lift * 0.0005f;
        if (s <= z[i]) return;
        z[i] = s;
        back[i] = c;
        front[i] = in_front ? c : Color{0, 0, 0, 0};
        thing[i] = mark;
    }
};

// A point in the model's space (x, y along the ground in pixels, z up) back to the ground.
struct P3 {
    float x;
    float y;
    float z;
};
P3 p3(Vector2 g, float lift) { return {g.x * kTilePx, g.y * kTilePx, lift}; }
P3 operator+(P3 a, P3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
P3 operator*(P3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot3(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
P3 cross3(P3 a, P3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
P3 norm3(P3 a) {
    const float l = std::sqrt(dot3(a, a));
    return l > 1e-6f ? a * (1.0f / l) : P3{0, 0, 1};
}
float lit3(P3 n) { return light(n.x, n.y, n.z); }

void put(Canvas& cv, P3 p, Color c, float b, bool in_front) {
    cv.splat({p.x / kTilePx, p.y / kTilePx}, p.z, scaled(c, stepped(b)), in_front);
}

// A log (or a pipe, a post, a steel bar) from a to b, r pixels thick; its cut ends pale.
void cylinder(Canvas& cv, P3 a, P3 b, float r, Color c, bool in_front, bool ends = false, Color end = kLogEnd) {
    const P3 ax{b.x - a.x, b.y - a.y, b.z - a.z};
    const float l = std::sqrt(dot3(ax, ax));
    if (l < 1e-4f) return;
    const P3 d = ax * (1.0f / l);
    const P3 u = norm3(std::fabs(d.z) < 0.9f ? cross3(d, P3{0, 0, 1}) : cross3(d, P3{1, 0, 0}));
    const P3 v = cross3(d, u);
    const int around = std::max(8, static_cast<int>(r * 10.0f));
    for (float t = 0.0f; t <= l; t += 0.4f) {
        for (int k = 0; k < around; ++k) {
            const float an = static_cast<float>(k) / static_cast<float>(around) * 2.0f * kPi;
            const P3 n = u * std::cos(an) + v * std::sin(an);
            const float bark = (static_cast<int>(t * 0.9f + an * 1.7f) % 3 == 0) ? 0.86f : 1.0f;
            put(cv, a + d * t + n * r, c, lit3(n) * bark, in_front);
        }
    }
    if (ends) {
        for (const float t : {0.0f, l}) {
            const P3 n = d * (t == 0.0f ? -1.0f : 1.0f);
            for (float rr = 0.0f; rr <= r; rr += 0.35f) {
                for (int k = 0; k < around; ++k) {
                    const float an = static_cast<float>(k) / static_cast<float>(around) * 2.0f * kPi;
                    const bool ring = std::fabs(rr - r * 0.55f) < 0.3f;
                    put(cv, a + d * t + (u * std::cos(an) + v * std::sin(an)) * rr, ring ? scaled(end, 0.85f) : end, lit3(n) + 0.2f, in_front);
                }
            }
        }
    }
}

// A box along `d` on the ground, from z0 up to z1: its sides and top, lit
// by the way they face; lines across its top every `slat` pixels (planks).
void box(Canvas& cv, Vector2 g, Vector2 d, float half_len, float half_wid, float z0, float z1, Color c, bool in_front,
         float slat = 0.0f) {
    const Vector2 s = perp(d);
    const P3 o = p3(g, z0);
    const P3 D{d.x, d.y, 0};
    const P3 S{s.x, s.y, 0};
    const float L = half_len * kTilePx;
    const float W = half_wid * kTilePx;
    const float H = z1 - z0;
    for (float a = -L; a <= L; a += 0.35f) {
        for (float b = -W; b <= W; b += 0.35f) {
            const bool line = slat > 0.0f && std::fmod(a + L, slat) < 0.4f;
            put(cv, o + D * a + S * b + P3{0, 0, H}, line ? scaled(c, 0.8f) : c, lit3({0, 0, 1}), in_front);
        }
    }
    for (const float sgn : {-1.0f, 1.0f}) {
        for (float a = -L; a <= L; a += 0.35f) {
            for (float z = 0.0f; z <= H; z += 0.35f) put(cv, o + D * a + S * (W * sgn) + P3{0, 0, z}, c, lit3(S * sgn), in_front);
        }
        for (float b = -W; b <= W; b += 0.35f) {
            for (float z = 0.0f; z <= H; z += 0.35f) put(cv, o + D * (L * sgn) + S * b + P3{0, 0, z}, c, lit3(D * sgn), in_front);
        }
    }
}

// A sandbag: a fat pillow along `d`, lying at height z.
void sandbag(Canvas& cv, Vector2 g, float z, Vector2 d, Color c, bool in_front) {
    const Vector2 s = perp(d);
    const P3 o = p3(g, z);
    const P3 D{d.x, d.y, 0};
    const P3 S{s.x, s.y, 0};
    for (float th = 0.0f; th < 2.0f * kPi; th += 0.2f) {
        for (float ph = -1.45f; ph <= 1.45f; ph += 0.2f) {
            const P3 n{std::cos(ph) * std::cos(th), std::cos(ph) * std::sin(th), std::sin(ph)};
            const P3 at = o + D * (n.x * 2.7f) + S * (n.y * 1.7f) + P3{0, 0, 1.0f + n.z * 1.0f};
            const P3 nn = norm3(D * (n.x / 2.7f) + S * (n.y / 1.7f) + P3{0, 0, n.z / 1.0f});
            put(cv, at, c, lit3(nn), in_front);
        }
    }
}

// Sandbags in their colours: sacking, white plastic, green.
Color bag_colour(uint32_t seed, int i) {
    const float k = rnd(seed + 5u, i);
    return k < 0.45f ? kBag : k < 0.8f ? kPolyBag : Color{112, 118, 78, 255};
}

// Sandbags along the crest of the bank in front of a stretch of ditch
// from a to b (tile-local): a row, a few more on top of it.
void bags_along(Canvas& cv, Vector2 a, Vector2 b, float half, Vector2 facing, uint32_t seed) {
    const Vector2 base{static_cast<float>(cv.sc.tx), static_cast<float>(cv.sc.ty)};
    const Vector2 d = unit(sub(b, a));
    Vector2 n = perp(d);
    if (dot(n, facing) < 0.0f) n = mul(n, -1.0f);
    if (dot(n, facing) < 0.3f) return;  // (a stretch running towards the front)
    const float l = len(sub(b, a));
    int i = 0;
    for (float t = 0.05f; t < l; t += 0.1f, ++i) {
        const Vector2 m = add(a, mul(d, t));
        const Vector2 at = add(base, add(m, mul(n, half + 0.1f)));
        const float z = std::max(0.0f, cv.sc.eval(at).h - 1.4f);
        const bool near = at.x + at.y > base.x + base.y + m.x + m.y;
        sandbag(cv, at, z, d, bag_colour(seed, i), near);
        if (rnd(seed, i + 40) < 0.6f && t + 0.05f < l) {
            sandbag(cv, add(at, mul(d, 0.05f)), z + 1.8f, d, bag_colour(seed, i + 100), near);
        }
    }
}

// The same round the front of a round pit.
// `rows`: 0 a row and a few more on it; else that many rows, whole.
void bags_round(Canvas& cv, Vector2 c, float r, Vector2 facing, float spread, uint32_t seed, int rows = 0) {
    const Vector2 base{static_cast<float>(cv.sc.tx), static_cast<float>(cv.sc.ty)};
    const float a0 = std::atan2(facing.y, facing.x);
    const float step = 0.1f / (r + 0.1f);
    int i = 0;
    for (float a = a0 - spread; a <= a0 + spread + 1e-3f; a += step, ++i) {
        const Vector2 dir{std::cos(a), std::sin(a)};
        const Vector2 at = add(base, add(c, mul(dir, r + 0.1f)));
        const float z = std::max(0.0f, cv.sc.eval(at).h - 1.4f);
        const bool near = at.x + at.y > base.x + base.y + c.x + c.y;
        sandbag(cv, at, z, perp(dir), bag_colour(seed, i), near);
        if (rows == 0 && rnd(seed, i + 40) < 0.5f) sandbag(cv, at, z + 1.6f, perp(dir), bag_colour(seed, i + 100), near);
        for (int row = 1; row < rows; ++row) {  // (each row set back a little, half a bag along)
            const float a2 = a + step * 0.5f * static_cast<float>(row % 2);
            const Vector2 d2{std::cos(a2), std::sin(a2)};
            const Vector2 at2 = add(base, add(c, mul(d2, r + 0.1f - 0.015f * static_cast<float>(row))));
            sandbag(cv, at2, z + 1.7f * static_cast<float>(row), perp(d2), bag_colour(seed, i + 100 * row), near);
        }
    }
}

// Sandbags stacked up, `high` of them, lying along `d`.
void bag_stack(Canvas& cv, Vector2 g, Vector2 d, int high, uint32_t seed, bool in_front) {
    const float z = std::max(0.0f, cv.sc.eval(g).h - 1.2f);
    for (int k = 0; k < high; ++k) sandbag(cv, add(g, mul(d, 0.02f * static_cast<float>(k % 2))), z + 1.7f * static_cast<float>(k), d, bag_colour(seed, k), in_front);
}

void stake(Canvas& cv, Vector2 g, float tall, bool in_front) {
    cylinder(cv, p3(g, -1.0f), p3(g, tall), 0.7f, kWood, in_front);
}

// What's built into it, over the dug ground.
void furnish(Canvas& cv) {
    const Parts& sc = cv.sc.own;
    const Spec& spec = sc.spec;
    const Vector2 base{static_cast<float>(cv.sc.tx), static_cast<float>(cv.sc.ty)};
    auto at = [&](Vector2 l) { return add(base, l); };
    const Vector2 c{0.5f, 0.5f};
    const Vector2 f = spec.facing;
    const Vector2 side = perp(f);
    const uint32_t h = spec.seed;
    switch (spec.kind) {
        case Kind::Trench:
        case Kind::Foxhole: {
            if (spec.parapet && spec.damage < 3 && (spec.kind != Kind::Trench || spec.fit == kNoFit || spec.fit == kMortarPost)) {
                if (spec.kind == Kind::Trench) {
                    for (const Seg& sg : sc.segs) bags_along(cv, sg.a, sg.b, sg.half, f, h + static_cast<uint32_t>(sg.start * 100.0f));
                } else {
                    bags_round(cv, sc.rc, sc.rr, f, 1.1f, h);
                }
            }
            // Now and then a green ammunition box on the floor by the wall.
            if (spec.kind == Kind::Trench && rnd(h, 5) < 0.3f && !sc.segs.empty()) {
                const Seg& s = sc.segs[static_cast<size_t>(h % sc.segs.size())];
                const Vector2 d = unit(sub(s.b, s.a));
                const Vector2 p = add(add(s.a, mul(sub(s.b, s.a), 0.55f)), mul(perp(d), 0.06f));
                box(cv, at(p), d, 0.06f, 0.035f, -9.0f, -6.0f, kCrate, false);
            }
            if (spec.kind == Kind::Trench && spec.damage < 3) {
                switch (spec.fit) {
                    case kCell: {  // a loophole: two stacks of bags either side of the niche, a plank over them; cartridges on its step
                        const Vector2 lip = add(sc.rc, mul(f, sc.rr + 0.08f));
                        const bool near = at(lip).x + at(lip).y > at(sc.rc).x + at(sc.rc).y;
                        for (const float k : {-1.0f, 1.0f}) bag_stack(cv, at(add(lip, mul(side, 0.1f * k))), f, 3, h + 7u + static_cast<uint32_t>(k + 1.0f), near);
                        const float top = std::max(0.0f, cv.sc.eval(at(lip)).h - 1.2f) + 5.3f;
                        box(cv, at(lip), side, 0.15f, 0.04f, top, top + 1.0f, kWood, near, 2.5f);
                        sandbag(cv, at(add(lip, mul(side, 0.04f))), top + 1.0f, side, bag_colour(h, 70), near);
                        bags_along(cv, add(c, mul(side, -0.5f)), add(lip, mul(side, -0.2f)), 0.14f, f, h + 3u);
                        bags_along(cv, add(lip, mul(side, 0.2f)), add(c, mul(side, 0.5f)), 0.14f, f, h + 5u);
                        box(cv, at(sub(sc.rc, mul(f, 0.02f))), side, 0.045f, 0.03f, -3.2f, -1.4f, kCrate, false);
                        break;
                    }
                    case kNest:  // sandbagged round three high, open at the back; a plank rest for the gun, a belt box
                        bags_round(cv, sc.rc, sc.rr, f, 2.2f, h + 9u, 3);
                        box(cv, at(add(sc.rc, mul(f, sc.rr - 0.03f))), side, 0.09f, 0.025f, -1.2f, 0.4f, kWood, true);
                        box(cv, at(add(sub(sc.rc, mul(f, 0.06f)), mul(side, 0.09f))), f, 0.035f, 0.025f, -7.0f, -5.2f, kCrate, false);
                        break;
                    case kAtPost: {  // bags on its flanks two high, the rockets' long box on the lip, rounds by it; nothing behind
                        for (const float k : {-1.0f, 1.0f}) {
                            const Vector2 flank = add(sc.rc, mul(side, (sc.rr + 0.1f) * k));
                            const bool near = at(flank).x + at(flank).y > at(sc.rc).x + at(sc.rc).y;
                            bag_stack(cv, at(add(flank, mul(f, 0.05f))), f, 2, h + 11u + static_cast<uint32_t>(k + 1.0f), near);
                            bag_stack(cv, at(sub(flank, mul(f, 0.06f))), f, 2, h + 15u + static_cast<uint32_t>(k + 1.0f), near);
                        }
                        const Vector2 lip = add(sc.rc, mul(f, sc.rr + 0.06f));
                        const float z = std::max(0.0f, cv.sc.eval(at(lip)).h - 1.0f);
                        box(cv, at(lip), side, 0.09f, 0.03f, z, z + 2.2f, kCrate, true, 2.0f);
                        for (int i = 0; i < 3; ++i) {  // RPG rounds leaning on it: a tube, its fat head
                            const Vector2 foot = add(sub(lip, mul(f, 0.05f)), mul(side, -0.06f + 0.06f * static_cast<float>(i)));
                            const P3 a0 = p3(at(foot), z);
                            const P3 a1 = p3(at(add(foot, mul(f, 0.03f))), z + 5.0f);
                            cylinder(cv, a0, a1, 0.6f, {86, 92, 64, 255}, true);
                            cylinder(cv, a1, p3(at(add(foot, mul(f, 0.04f))), z + 7.0f), 1.1f, {70, 76, 52, 255}, true);
                        }
                        break;
                    }
                    case kMortarPost:  // bags round its back, the bombs' boxes in niches
                        bags_round(cv, sc.rc, sc.rr, mul(f, -1.0f), 1.2f, h + 13u);
                        box(cv, at(add(sc.rc, mul(side, 0.15f))), f, 0.05f, 0.035f, -6.0f, -3.4f, kCrate, false, 2.0f);
                        box(cv, at(sub(sc.rc, mul(side, 0.15f))), f, 0.05f, 0.035f, -6.0f, -3.4f, kCrate, false, 2.0f);
                        break;
                    default: break;
                }
                if (spec.fitting != kNoFit && spec.fitting != kDugoutFit) {  // being made: bags waiting on the bank, a spade stuck in
                    const Vector2 pile = add(c, add(mul(f, spec.fitting == kParapetFit ? 0.33f : -0.33f), mul(side, 0.16f)));
                    for (int i = 0; i < 4; ++i) {
                        const Vector2 p = add(pile, mul(side, 0.07f * static_cast<float>(i % 2)));
                        sandbag(cv, at(p), std::max(0.0f, cv.sc.eval(at(p)).h - 1.0f) + 1.8f * static_cast<float>(i / 2), side, bag_colour(h, 50 + i), true);
                    }
                    const Vector2 spade = add(c, add(mul(f, -0.3f), mul(side, -0.14f)));
                    cylinder(cv, p3(at(spade), -1.0f), p3(at(spade), 7.0f), 0.6f, kWood, true);
                    box(cv, at(spade), side, 0.03f, 0.008f, -1.5f, 1.5f, kSteel, true);
                }
            }
            if (spec.upgrading) {  // being made a dugout: logs laid over, more stacked by it
                for (int i = 0; i < 3; ++i) {
                    const float k = -0.12f + 0.12f * static_cast<float>(i);
                    const Vector2 m = add(c, mul(side, k));
                    cylinder(cv, p3(at(add(m, mul(f, -0.27f))), 0.8f), p3(at(add(m, mul(f, 0.27f))), 0.8f), 1.4f, kLog, true, true);
                }
                const Vector2 pile = add(c, add(mul(side, 0.36f), mul(f, -0.2f)));
                for (int i = 0; i < 5; ++i) {
                    const float row = i < 3 ? 0.0f : 1.0f;
                    const float k = (i < 3 ? static_cast<float>(i) - 1.0f : static_cast<float>(i - 3) - 0.5f) * 0.075f;
                    const Vector2 m = add(pile, mul(side, k));
                    cylinder(cv, p3(at(add(m, mul(f, -0.22f))), 1.3f + row * 2.4f), p3(at(add(m, mul(f, 0.22f))), 1.3f + row * 2.4f), 1.3f,
                             kLog, true, true);
                }
            }
            break;
        }
        case Kind::Dugout: {
            // The door's frame of logs where the steps go in, its dark
            // doorway; the logs' ends under the earth either side; the
            // stove's pipe up through the roof.
            const Seg& way = sc.segs.front();
            const Vector2 in = unit(sub(way.b, way.a));
            const Vector2 across = perp(in);
            const Vector2 door = add(way.a, mul(in, 0.02f));
            for (const float s : {-1.0f, 1.0f}) {
                const Vector2 post = add(door, mul(across, 0.105f * s));
                cylinder(cv, p3(at(post), -9.0f), p3(at(post), 4.0f), 1.1f, kLog, true);
            }
            cylinder(cv, p3(at(add(door, mul(across, -0.15f))), 4.4f), p3(at(add(door, mul(across, 0.15f))), 4.4f), 1.3f, kLog, true, true);
            for (int i = 0; i < 8; ++i) {  // the roof's logs, their ends showing under the earth
                const float k = -0.32f + 0.09f * static_cast<float>(i);
                if (std::fabs(k) < 0.14f) continue;
                const Vector2 e = add(add(sc.mc, mul(in, sc.mwid + 0.03f)), mul(across, k));
                cylinder(cv, p3(at(add(e, mul(in, -0.06f))), 3.4f), p3(at(e), 3.4f), 1.6f, kLog, true, true);
            }
            if (spec.damage < 2) {  // sandbags along the roof's front edge
                int i = 0;
                for (float k = -0.3f; k <= 0.3f; k += 0.13f, ++i) {
                    if (std::fabs(k) < 0.12f) continue;
                    const Vector2 p = add(add(sc.mc, mul(in, sc.mwid - 0.05f)), mul(across, k));
                    sandbag(cv, at(p), std::max(0.0f, cv.sc.eval(at(p)).h - 1.2f), across, bag_colour(h, i), true);
                }
            }
            if (spec.damage < 3) {
                const Vector2 pipe = add(sc.mc, add(mul(in, -0.08f), mul(across, 0.12f)));
                cylinder(cv, p3(at(pipe), 5.0f), p3(at(pipe), 13.5f), 0.9f, kSteel, true);
                cylinder(cv, p3(at(pipe), 13.5f), p3(at(pipe), 14.5f), 1.4f, kRust, true);
            }
            break;
        }
        case Kind::Parapet:
            if (spec.damage < 3) bags_along(cv, sc.segs.front().a, sc.segs.front().b, sc.segs.front().half, f, h);
            break;
        case Kind::MortarPit: {
            if (spec.damage < 3) bags_round(cv, sc.rc, sc.rr, f, 0.9f, h);
            // Bombs in their boxes in a niche either side.
            for (const float s : {-1.0f, 1.0f}) {
                const Vector2 n = add(sc.rc, add(mul(side, 0.17f * s), mul(f, -0.07f)));
                box(cv, at(n), f, 0.05f, 0.035f, -6.0f, -3.4f, kCrate, false, 2.0f);
                box(cv, at(add(n, mul(f, 0.08f))), f, 0.05f, 0.035f, -6.0f, -3.4f, kCrate, false, 2.0f);
            }
            break;
        }
        case Kind::GunPit: {
            // The shells in their boxes in the bank's niches, at the back.
            for (const float s : {-1.0f, 1.0f}) {
                const Vector2 n = add(sc.rc, add(mul(side, 0.38f * s), mul(f, -0.18f)));
                for (int i = 0; i < 3; ++i) {
                    box(cv, at(add(n, mul(f, 0.075f * static_cast<float>(i)))), side, 0.07f, 0.032f, -2.5f, 0.3f + (i == 1 ? 2.6f : 0.0f), kCrate,
                        false, 2.0f);
                }
            }
            break;
        }
        case Kind::Caponier: {
            // Logs across the front wall; the tracks down the ramp.
            const Vector2 fw = add(sc.kc, mul(f, sc.klen + 0.02f));
            for (int i = 0; i < 3; ++i) {
                const float z = -6.0f + 2.4f * static_cast<float>(i);
                cylinder(cv, p3(at(add(fw, mul(side, -sc.kwid))), z), p3(at(add(fw, mul(side, sc.kwid))), z), 1.2f, kLog, false, true);
            }
            break;
        }
        case Kind::Wire: {
            // A coil of barbed wire along the line (to the wire next to it,
            // straight or corner to corner), on stakes.
            Vector2 d = perp(f);
            float reach = 0.5f;
            for (int bit = 0; bit < 8; ++bit) {
                if (spec.links & (1u << bit)) {
                    d = bit < 4 ? dir_of_link(bit) : unit(bit == 4 ? Vector2{1, 1} : bit == 5 ? Vector2{-1, -1} : bit == 6 ? Vector2{1, -1} : Vector2{-1, 1});
                    reach = bit < 4 ? 0.5f : 0.7071f;
                    break;
                }
            }
            const Vector2 a = add(c, mul(d, -reach));
            const Vector2 across = perp(d);
            for (int i = 0; i < 2; ++i) {  // stakes both sides of the coil, a strand along their tops
                for (const float k : {-0.13f, 0.13f}) {
                    stake(cv, at(add(add(a, mul(d, reach * (0.5f + static_cast<float>(i)))), mul(across, k))), 8.5f, true);
                }
            }
            const P3 D{d.x, d.y, 0};
            const P3 S{across.x, across.y, 0};
            cv.mark = 2;
            for (const float k : {-0.13f, 0.13f}) {
                for (float t = 0.0f; t <= 2.0f * reach; t += 0.01f) {
                    const float sag = 1.0f - 4.0f * std::pow(std::fmod(t / (2.0f * reach) + 0.25f, 0.5f) / 0.5f - 0.5f, 2.0f);
                    put(cv, p3(at(add(add(a, mul(d, t)), mul(across, k))), 7.8f - sag * 0.8f), kWire, 1.0f, true);
                }
            }
            for (float t = 0.02f; t < 2.0f * reach; t += 0.085f) {  // the coil, loop after loop leaning along it
                const P3 m = p3(at(add(a, mul(d, t))), 4.0f);
                int k = 0;
                for (float an = 0.0f; an < 2.0f * kPi; an += 0.07f, ++k) {
                    const P3 n = S * std::cos(an) + P3{0, 0, std::sin(an)};
                    const P3 p = m + n * 4.0f + D * (std::sin(an) * 1.4f);
                    put(cv, p, k % 9 == 0 ? kSteel : kWire, lit3(n) + 0.15f, true);  // (a barb now and then)
                }
            }
            cv.mark = 1;
            break;
        }
        case Kind::Hedgehogs: {
            // Czech hedgehogs: three steel bars through one another, standing on three ends.
            for (int i = 0; i < 2; ++i) {
                const Vector2 m = add(c, mul(unit({1.0f, -1.0f}), (i == 0 ? -0.2f : 0.2f)));
                const float turn = rnd(h, 10 + i) * 2.0f * kPi;
                const P3 mid = p3(at(m), 5.2f);
                // Three bars at right angles, the whole turned so their diagonal stands up.
                const P3 e[3] = {{0.8165f, 0.0f, 0.5774f}, {-0.4082f, 0.7071f, 0.5774f}, {-0.4082f, -0.7071f, 0.5774f}};
                for (const P3& v : e) {
                    const P3 r{v.x * std::cos(turn) - v.y * std::sin(turn), v.x * std::sin(turn) + v.y * std::cos(turn), -v.z};
                    // (tipped over so the bar's other end is up: each bar runs down to the ground and up past the middle)
                    const P3 down{r.x * 9.0f, r.y * 9.0f, r.z * 9.0f};
                    cylinder(cv, mid + down, mid + down * -1.0f, 0.9f, i == 0 ? kSteel : kRust, true);
                }
            }
            break;
        }
    }
}

}  // namespace

Sprite bake(const Area& area, int tx, int ty, const std::array<float, 4>& corner) {
    const Scene sc = scene_of(area, tx, ty);
    const Spec& spec = sc.own.spec;
    auto ground = [&](Vector2 g) {
        const float u = g.x - static_cast<float>(tx);
        const float v = g.y - static_cast<float>(ty);
        return corner[0] * (1.0f - u) * (1.0f - v) + corner[1] * u * (1.0f - v) + corner[2] * u * v + corner[3] * (1.0f - u) * v;
    };
    // Where it lies on the screen: the tile and round it, up as high as anything rises.
    float x0 = 1e9f;
    float x1 = -1e9f;
    float y0 = 1e9f;
    float y1 = -1e9f;
    const float m = sc.own.margin;
    for (const Vector2 l : {Vector2{-m, -m}, Vector2{1 + m, -m}, Vector2{1 + m, 1 + m}, Vector2{-m, 1 + m}}) {
        const Vector2 g{static_cast<float>(tx) + l.x, static_cast<float>(ty) + l.y};
        const float X = (g.x - g.y) * 32.0f;
        const float Y = (g.x + g.y) * 16.0f - ground(g);
        x0 = std::min(x0, X);
        x1 = std::max(x1, X);
        y0 = std::min(y0, Y);
        y1 = std::max(y1, Y);
    }
    x0 = std::floor(x0);
    y0 = std::floor(y0 - sc.top - 2.0f);
    const int w = static_cast<int>(std::ceil(x1 - x0)) + 1;
    const int hgt = static_cast<int>(std::ceil(y1 + 2.0f - y0)) + 1;
    Canvas cv{sc, ground, x0, y0, w, hgt};
    const size_t n = static_cast<size_t>(w * hgt);
    cv.back.assign(n, Color{0, 0, 0, 0});
    cv.front.assign(n, Color{0, 0, 0, 0});
    cv.z.assign(n, -1e9f);
    cv.thing.assign(n, 0);
    cv.kind.assign(n, 0);

    const bool dug = spec.kind != Kind::Wire && spec.kind != Kind::Hedgehogs;
    for (int j = 0; dug && j < hgt; ++j) {
        for (int i = 0; i < w; ++i) {
            const float X = x0 + static_cast<float>(i) + 0.5f;
            const float Y = y0 + static_cast<float>(j) + 0.5f;
            const float dxy = X / 32.0f;
            // Down the ray: s = x + y falls as it goes away and down.
            auto ground_at = [&](float s) { return Vector2{(s + dxy) * 0.5f, (s - dxy) * 0.5f}; };
            const float hc = ground({static_cast<float>(tx) + 0.5f, static_cast<float>(ty) + 0.5f});
            float s = (Y + sc.top + 1.0f + hc) / 16.0f;
            for (int k = 0; k < 3; ++k) s = (Y + sc.top + 1.0f + ground(ground_at(s))) / 16.0f;
            auto lift_at = [&](float ss) { return 16.0f * ss - ground(ground_at(ss)) - Y; };
            const float step = 1.0f / 16.0f;
            float hi = s;
            float lo = s;
            bool found = false;
            for (int k = 0; k < 64; ++k) {
                lo = hi - step;
                const float lift = lift_at(lo);
                if (lift <= sc.eval(ground_at(lo)).h) {
                    found = true;
                    break;
                }
                if (lift < -sc.deep - 2.0f) break;
                hi = lo;
            }
            if (!found) continue;
            const size_t at_idx = static_cast<size_t>(j * w + i);
            {
                const Vector2 g0 = ground_at(lo);
                const Hit rough = sc.eval(g0);
                if (rough.top == Mat::None && rough.h == 0.0f) {  // the plain ground: nothing to draw
                    cv.z[at_idx] = g0.x + g0.y;
                    continue;
                }
            }
            for (int k = 0; k < 6; ++k) {
                const float mid = (hi + lo) * 0.5f;
                if (lift_at(mid) <= sc.eval(ground_at(mid)).h) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            const Vector2 g = ground_at(lo);
            const size_t idx = static_cast<size_t>(j * w + i);
            cv.z[idx] = g.x + g.y;
            // This tile's pixels: what's on it, or what spills onto a tile with
            // no works of its own (the tile with works draws its own).
            const int nx = static_cast<int>(std::floor(g.x)) - tx + 1;
            const int ny = static_cast<int>(std::floor(g.y)) - ty + 1;
            if ((nx != 1 || ny != 1) && nx >= 0 && nx <= 2 && ny >= 0 && ny <= 2 && area.has[static_cast<size_t>(ny * 3 + nx)]) continue;
            const Hit hit = sc.eval(g);
            const float lift = lift_at(lo);
            const bool wall = hit.h - lift_at(hi) > 0.5f;
            Color c{0, 0, 0, 0};
            if (wall) {
                // The far wall of what's dug, seen across it: lit by the way it faces, darker down in it.
                cv.kind[idx] = 2;
                const float down = clamp01(-lift / 9.0f);
                float b = light(hit.in.x, hit.in.y, 0.0f) * (1.0f - 0.3f * down);
                const float u = hit.along * kTilePx;
                switch (hit.wall) {
                    case Mat::Planks: {  // slabs stood on end, a stake between every few
                        const int board = static_cast<int>(std::floor(u / 3.0f));
                        const bool seam = std::fmod(u + 300.0f, 3.0f) < 0.6f;
                        const bool post = board % 4 == 0 && !seam;
                        c = scaled(post ? kWood : kPlank, stepped(b * (seam ? 0.68f : 0.92f + 0.12f * rnd(spec.seed + 7u, board))));
                        break;
                    }
                    case Mat::Wattle: {  // brushwood woven round stakes
                        const float row = std::floor((-lift + 20.0f) / 1.6f);
                        const bool over = (static_cast<int>(std::floor(u / 2.5f)) + static_cast<int>(row)) % 2 == 0;
                        c = scaled(kWattle, stepped(b * (over ? 1.0f : 0.78f)));
                        break;
                    }
                    case Mat::Door:
                        c = kDark;
                        break;
                    default: {  // bare clay, the spade's cuts in it
                        const float grain = noise({u * 0.2f, lift * 0.4f}, 1.0f, 61u);
                        c = scaled(lift > -2.0f ? kSpoil : kClay, stepped(b * (0.9f + 0.18f * grain)));
                        break;
                    }
                }
            } else {
                // Its top: the dug earth, the floor, sandbags, the turf on a dugout's roof.
                cv.kind[idx] = 1;
                const float e = 0.012f;
                const float hx = (sc.eval({g.x + e, g.y}).h - sc.eval({g.x - e, g.y}).h) / (2.0f * e * kTilePx);
                const float hy = (sc.eval({g.x, g.y + e}).h - sc.eval({g.x, g.y - e}).h) / (2.0f * e * kTilePx);
                float b = light(-hx, -hy, 1.0f);
                switch (hit.top) {
                    case Mat::None: break;
                    case Mat::Spoil: {
                        if (hit.h < 0.45f && ((i + j) % 2 != 0 || hit.h < 0.15f)) break;  // its thin edge, dithered into the grass
                        const float k = noise(g, 28.0f, 71u);
                        const Color soil = k > 0.72f ? kLoess : k < 0.3f ? kClods : kSpoil;
                        c = scaled(soil, stepped(b));
                        break;
                    }
                    case Mat::Bags: {  // rows of sandbags, the seams between them dark
                        const float row = std::floor(hit.h / 1.8f);
                        const float u = hit.along * kTilePx / 5.0f + (static_cast<int>(row) % 2 ? 0.5f : 0.0f);
                        const bool seam = std::fmod(u + 100.0f, 1.0f) < 0.18f || std::fmod(hit.h, 1.8f) < 0.35f;
                        const int bag = static_cast<int>(std::floor(u)) * 7 + static_cast<int>(row);
                        const Color kind_of = rnd(spec.seed + 3u, bag) < 0.35f ? kPolyBag : kBag;
                        c = scaled(kind_of, stepped(b * (seam ? 0.7f : 0.95f + 0.1f * rnd(spec.seed, bag))));
                        break;
                    }
                    case Mat::Turf: {
                        const float k = noise(g, 18.0f, 83u);
                        const bool net = noise(g, 3.0f, spec.seed) > 0.5f && hit.h > 3.0f;
                        if (net) {  // a camouflage net over part of it: cloth in greens and browns, the mesh
                            static constexpr Color kCloth[3] = {{64, 80, 46, 255}, {96, 104, 64, 255}, {110, 94, 64, 255}};
                            const bool mesh = (i + j) % 3 == 0;
                            c = scaled(kCloth[static_cast<int>(noise(g, 30.0f, 101u) * 2.99f)], stepped(b * (mesh ? 0.8f : 1.0f)));
                        } else {
                            c = scaled(k > 0.45f ? kGrass : kSpoil, stepped(b * (0.92f + 0.16f * noise(g, 40.0f, 89u))));
                        }
                        break;
                    }
                    case Mat::Duck: {  // duckboards: slats across the floor
                        const bool gap = std::fmod(hit.along * kTilePx + 100.0f, 2.5f) < 0.8f;
                        c = scaled(gap ? kFloor : kSlat, stepped(0.8f * b));
                        break;
                    }
                    default:
                        c = scaled(kFloor, stepped(0.85f * b * (0.9f + 0.2f * noise(g, 30.0f, 97u))));
                        break;
                }
            }
            if (c.a == 0) continue;
            cv.back[idx] = c;
            if (!wall && hit.h > 0.4f && g.x + g.y > hit.ref) cv.front[idx] = c;
        }
    }
    furnish(cv);

    // As pixel art: a lit edge where a bank's top shows against what's behind
    // it, a dark outline round what's put in.
    std::vector<Color> back = cv.back;
    std::vector<Color> front = cv.front;
    for (int j = 1; j < hgt - 1; ++j) {
        for (int i = 1; i < w - 1; ++i) {
            const size_t idx = static_cast<size_t>(j * w + i);
            const size_t up = idx - static_cast<size_t>(w);
            if (cv.back[idx].a == 0) {
                for (const size_t nb : {idx - 1, idx + 1, up, idx + static_cast<size_t>(w)}) {
                    if (cv.thing[nb] == 1 && cv.back[nb].a) {
                        back[idx] = kInk;
                        if (cv.front[nb].a) front[idx] = kInk;
                        break;
                    }
                }
                continue;
            }
            if (cv.thing[idx] == 2) continue;
            if (cv.thing[idx]) {
                bool edge = false;
                for (const size_t nb : {idx - 1, idx + 1, up, idx + static_cast<size_t>(w)}) {
                    if (!cv.thing[nb] && cv.z[nb] < cv.z[idx] - 0.03f) edge = true;
                }
                if (edge) {
                    back[idx] = kInk;
                    if (cv.front[idx].a) front[idx] = kInk;
                }
                continue;
            }
            if (cv.kind[idx] == 1 && cv.z[up] < cv.z[idx] - 0.05f && cv.back[up].a != 0 && cv.kind[up] != 0) {
                back[idx] = scaled(cv.back[idx], 1.18f);
                if (front[idx].a) front[idx] = back[idx];
            }
        }
    }
    Sprite out;
    out.at = {x0, y0};
    out.back = GenImageColor(w, hgt, {0, 0, 0, 0});
    out.front = GenImageColor(w, hgt, {0, 0, 0, 0});
    std::copy(back.begin(), back.end(), static_cast<Color*>(out.back.data));
    std::copy(front.begin(), front.end(), static_cast<Color*>(out.front.data));
    return out;
}

Place place(const Spec& spec, Vector2 local, bool vehicle) {
    const Parts sc = parts_of(spec);
    auto pull = [&](Vector2 c, float r) {
        const Vector2 d = sub(local, c);
        const float l = len(d);
        return l <= r ? local : add(c, mul(d, r / l));
    };
    switch (spec.kind) {
        case Kind::Trench: {
            if (spec.fit != kNoFit && sc.round && len(sub(local, sc.rc)) < 0.26f) {  // at the position built into it
                return {pull(sc.rc, 0.03f), vehicle ? 0.0f : spec.fit == kMortarPost ? 5.0f : 8.0f};
            }
            Vector2 best = local;
            float best_d = 1e9f;
            for (const Seg& s : sc.segs) {
                const Vector2 ab = sub(s.b, s.a);
                const float t = clamp01(dot(sub(local, s.a), ab) / std::max(dot(ab, ab), 1e-8f));
                const Vector2 q = add(s.a, mul(ab, t));
                if (len(sub(local, q)) < best_d) {
                    best_d = len(sub(local, q));
                    best = q;
                }
            }
            return {best, vehicle ? 0.0f : 8.0f};
        }
        case Kind::Foxhole: return {pull(sc.rc, 0.08f), vehicle ? 0.0f : 8.0f};
        case Kind::MortarPit: return {pull(sc.rc, 0.1f), vehicle ? 0.0f : 5.0f};
        case Kind::GunPit: return {pull(sc.rc, vehicle ? 0.05f : 0.2f), 2.0f};
        case Kind::Caponier: return {vehicle ? add(sc.kc, mul(sc.kd, 0.08f)) : local, vehicle ? 6.5f : 0.0f};
        case Kind::Parapet: {
            const Seg& s = sc.segs.front();
            const Vector2 ab = sub(s.b, s.a);
            const float t = clamp01(dot(sub(local, s.a), ab) / std::max(dot(ab, ab), 1e-8f));
            return {vehicle ? local : add(s.a, mul(ab, t)), vehicle ? 0.0f : 2.5f};
        }
        default: return {local, 0.0f};
    }
}

}  // namespace render::works
