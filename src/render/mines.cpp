#include "render/mines.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "render/iso.h"

namespace render::mines {
namespace {

// A tile on the ground in pixels, as the figures are drawn.
constexpr float kPxPerTile = iso::kTileWidth * 0.5f * 1.41421356f;

constexpr int kW = 28;
constexpr int kH = 24;
constexpr float kOx = 14.0f;  // its point on the ground in the image
constexpr float kOy = 16.0f;
constexpr float kStep = 0.25f;

Color scaled(Color c, float k) {
    auto ch = [k](unsigned char v) { return static_cast<unsigned char>(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f)); };
    return {ch(c.r), ch(c.g), ch(c.b), 255};
}
// The light as pixel art has it (as the works'): a few steps, from over the viewer's right.
float stepped(float b) {
    static constexpr float kLevels[] = {0.5f, 0.62f, 0.74f, 0.86f, 0.97f, 1.08f, 1.2f};
    float best = kLevels[0];
    for (const float l : kLevels) {
        if (std::fabs(l - b) < std::fabs(best - b)) best = l;
    }
    return best;
}
constexpr float kLx = 0.62f;
constexpr float kLy = -0.18f;
constexpr float kLz = 0.76f;
float light(float nx, float ny, float nz) {
    const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
    const float d = l > 0.0f ? (nx * kLx + ny * kLy + nz * kLz) / l : 1.0f;
    return 0.52f + 0.62f * std::max(d, -0.1f);
}

uint32_t hash(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
float noise01(uint32_t a, uint32_t b) { return static_cast<float>(hash(a, b) & 0xFFFFu) / 65535.0f; }

constexpr Color kInk{40, 34, 26, 255};
// The earth dug over a mine: Donbas black earth, turned, fresh.
constexpr Color kSpoil{118, 94, 64, 255};
constexpr Color kClods{92, 72, 50, 255};
constexpr Color kLoose{140, 114, 78, 255};
constexpr Color kEdge{66, 52, 36, 255};  // where the earth meets it: the gap round it in shadow
constexpr Color kShadow{22, 20, 16, 90};

enum Mark : uint8_t { kEmpty, kBody, kEarth, kThin, kShade };

// The image as it's drawn: each pixel the nearest thing to the eye there.
struct Canvas {
    std::array<Color, kW * kH> px{};
    std::array<float, kW * kH> depth{};
    std::array<uint8_t, kW * kH> mark{};
    Vector2 f{1.0f, 0.0f};  // the way it faces, on the ground (tiles)
    Vector2 l{0.0f, 1.0f};  // across it
    bool shadows = false;   // up on its legs: its shadow on the ground under it

    Canvas() { depth.fill(-1e9f); }

    // Where a point `ahead` and `side` pixels along the ground from its middle, `up` over it, is in the image.
    Vector2 at(float ahead, float side, float up, float* d = nullptr) const {
        const float gx = (f.x * ahead + l.x * side) / kPxPerTile;
        const float gy = (f.y * ahead + l.y * side) / kPxPerTile;
        if (d) *d = (gx + gy) * iso::kTileHeight * 0.5f + up;
        return {(gx - gy) * iso::kTileWidth * 0.5f + kOx, (gx + gy) * iso::kTileHeight * 0.5f - up + kOy};
    }
    void splat(float ahead, float side, float up, Color c, uint8_t m) {
        float d = 0.0f;
        const Vector2 s = at(ahead, side, up, &d);
        const int x = static_cast<int>(std::floor(s.x));
        const int y = static_cast<int>(std::floor(s.y));
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        const auto i = static_cast<size_t>(y * kW + x);
        if (d < depth[i]) return;
        depth[i] = d;
        px[i] = c;
        mark[i] = m;
        if (shadows && up > 0.3f && m != kShade) shade(ahead, side, up);
    }
    // Its shadow: the point cast down onto the ground away from the light.
    void shade(float ahead, float side, float up) {
        const float la = kLx * f.x + kLy * f.y;
        const float ls = kLx * l.x + kLy * l.y;
        const Vector2 s = at(ahead - la / kLz * up, side - ls / kLz * up, 0.0f);
        const int x = static_cast<int>(std::floor(s.x));
        const int y = static_cast<int>(std::floor(s.y));
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        const auto i = static_cast<size_t>(y * kW + x);
        if (mark[i] != kEmpty) return;
        px[i] = kShadow;
        mark[i] = kShade;
    }
    // Lit by its normal: along `f`, along `l`, up.
    void lit(float ahead, float side, float up, float nf, float nl, float nz, Color base, uint8_t m = kBody) {
        const float nx = f.x * nf + l.x * nl;
        const float ny = f.y * nf + l.y * nl;
        splat(ahead, side, up, scaled(base, stepped(light(nx, ny, nz))), m);
    }
};

// A round one standing at (a0, s0): its top and its side over the ground (the rest is in it).
void cylinder(Canvas& cv, float a0, float s0, float r, float z0, float z1, Color c) {
    for (float u = -r; u <= r; u += kStep) {
        for (float v = -r; v <= r; v += kStep) {
            if (u * u + v * v <= r * r) cv.lit(a0 + u, s0 + v, z1, 0.0f, 0.0f, 1.0f, c);
        }
    }
    const int n = std::max(16, static_cast<int>(r * 6.2831853f / kStep));
    for (int k = 0; k < n; ++k) {
        const float t = static_cast<float>(k) * 6.2831853f / static_cast<float>(n);
        const float cu = std::cos(t);
        const float sv = std::sin(t);
        for (float z = std::max(z0, 0.0f); z <= z1; z += kStep) cv.lit(a0 + r * cu, s0 + r * sv, z, cu, sv, 0.0f, c);
    }
}

// A rim rolled round a top, `r0` to `r1` out, at `z`: it catches the light on its near side.
void rim(Canvas& cv, float r0, float r1, float z, Color c) {
    for (float u = -r1; u <= r1; u += kStep) {
        for (float v = -r1; v <= r1; v += kStep) {
            const float d = std::sqrt(u * u + v * v);
            if (d < r0 || d > r1) continue;
            cv.lit(u, v, z, u / d * 0.8f, v / d * 0.8f, 1.0f, c);
        }
    }
}

// A MON's, a Claymore's body: `hf` deep and `hl` wide either side of its
// middle, from `z0` up to `z1`, its ends bent back by `bend`.
void curved_box(Canvas& cv, float hf, float hl, float z0, float z1, float bend, Color c) {
    auto back = [&](float s) { return -bend * (s / hl) * (s / hl); };
    for (float s = -hl; s <= hl; s += kStep) {
        const float b = back(s);
        const float k = -2.0f * bend * s / (hl * hl);  // how its face turns across it
        const float n = std::sqrt(1.0f + k * k);
        for (float z = z0; z <= z1; z += kStep) {
            cv.lit(hf + b, s, z, 1.0f / n, -k / n, 0.0f, c);
            cv.lit(-hf + b, s, z, -1.0f / n, k / n, 0.0f, c);
        }
        for (float a = -hf; a <= hf; a += kStep) cv.lit(a + b, s, z1, 0.0f, 0.0f, 1.0f, c);
    }
    for (float a = -hf; a <= hf; a += kStep) {
        for (float z = z0; z <= z1; z += kStep) {
            cv.lit(a + back(hl), hl, z, 0.0f, 1.0f, 0.0f, c);
            cv.lit(a + back(-hl), -hl, z, 0.0f, -1.0f, 0.0f, c);
        }
    }
}

// A leg, a pixel thick.
void rod(Canvas& cv, float a0, float s0, float z0, float a1, float s1, float z1, Color c) {
    for (int k = 0; k <= 24; ++k) {
        const float t = static_cast<float>(k) / 24.0f;
        cv.splat(a0 + (a1 - a0) * t, s0 + (s1 - s0) * t, z0 + (z1 - z0) * t, c, kThin);
    }
}

// The earth dug over it, `r` out: a low mound of it, loose, clods thrown about.
void dug_earth(Canvas& cv, float r, uint32_t seed) {
    for (float u = -r - 1.0f; u <= r + 1.0f; u += 0.5f) {
        for (float v = -r - 1.0f; v <= r + 1.0f; v += 0.5f) {
            const float d = std::sqrt(u * u + v * v);
            const auto cell = static_cast<uint32_t>(static_cast<int>(std::floor(u)) + 64) * 131u +
                              static_cast<uint32_t>(static_cast<int>(std::floor(v)) + 64);
            const float edge = r * (0.74f + 0.34f * noise01(seed, cell));
            if (d > edge) continue;
            const float up = 0.6f * (1.0f - d / edge);
            const float pick = noise01(seed ^ 0x51u, cell);
            const Color c = pick < 0.3f ? kClods : pick < 0.85f ? kSpoil : kLoose;
            const float out = d > 0.01f ? 0.35f / d : 0.0f;
            cv.lit(u, v, up, u * out, v * out, 1.0f, c, kEarth);
        }
    }
    for (uint32_t k = 0; k < 6; ++k) {
        const float t = noise01(seed, 900u + k) * 6.2831853f;
        const float d = r * (1.0f + 0.45f * noise01(seed, 950u + k));
        cv.lit(std::cos(t) * d, std::sin(t) * d, 0.3f, 0.0f, 0.0f, 1.0f, k % 2 ? kClods : kLoose, kEarth);
    }
}

// Earth thrown back over its top, `r` across, `z` up (not all of it shows).
void earth_over(Canvas& cv, float r, float z, uint32_t seed) {
    for (uint32_t k = 0; k < 4; ++k) {
        const float t = noise01(seed, 300u + k) * 6.2831853f;
        const float d = r * std::sqrt(noise01(seed, 350u + k));
        cv.lit(std::cos(t) * d, std::sin(t) * d, z + 0.3f, 0.0f, 0.0f, 1.0f, k % 2 ? kSpoil : kLoose);  // (on it: no gap round it)
    }
}

// Round the body, over the ground: the ink line.
void outline(Canvas& cv) {
    std::array<uint8_t, kW * kH> ink{};  // 1: against the air, 2: against the earth
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const auto i = static_cast<size_t>(y * kW + x);
            if (cv.mark[i] == kBody || cv.mark[i] == kThin) continue;
            static constexpr int kNear[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& n : kNear) {
                const int nx = x + n[0];
                const int ny = y + n[1];
                if (nx >= 0 && ny >= 0 && nx < kW && ny < kH && cv.mark[static_cast<size_t>(ny * kW + nx)] == kBody) {
                    ink[i] = cv.mark[i] == kEarth ? 2 : 1;
                }
            }
        }
    }
    for (size_t i = 0; i < ink.size(); ++i) {
        if (!ink[i]) continue;
        cv.px[i] = ink[i] == 2 ? kEdge : kInk;
        cv.mark[i] = kThin;
    }
}

}  // namespace

Model model_of(engine::MineKind kind, engine::Axis axis) {
    const bool ours = axis == engine::Axis::Democratic;
    switch (kind) {
        case engine::MineKind::AntiTank: return Model::Tm62m;
        case engine::MineKind::AntiPersonnel: return ours ? Model::M14 : Model::Pmn2;
        case engine::MineKind::Directional: return ours ? Model::Claymore : Model::Mon50;
    }
    return Model::Tm62m;
}

const char* name_of(Model model) {
    switch (model) {
        case Model::Tm62m: return "TM-62M";
        case Model::Pmn2: return "PMN-2";
        case Model::M14: return "M14";
        case Model::Mon50: return "MON-50";
        case Model::Claymore: return "M18A1 Claymore";
        case Model::Count: break;
    }
    return "";
}

int dir_of(Vector2 facing) {
    const float a = std::atan2(facing.y, facing.x);
    const int d = static_cast<int>(std::lround(a / (3.14159265f / 4.0f)));
    return ((d % kDirs) + kDirs) % kDirs;
}

Sprite bake(Model model, int dir, uint32_t seed) {
    Canvas cv;
    // A pressure mine lies any way round; a directional one faces its way.
    const float a = model == Model::Mon50 || model == Model::Claymore ? static_cast<float>(dir) * 3.14159265f / 4.0f
                                                                      : noise01(seed, 7u) * 6.2831853f;
    cv.f = {std::cos(a), std::sin(a)};
    cv.l = {-std::sin(a), std::cos(a)};
    Vector2 fuze{};
    switch (model) {
        case Model::Tm62m: {
            // A steel disc 32 cm across, dug in to its rim; the MVCh-62 fuze
            // screwed into the middle, its cap.
            constexpr Color kSteel{100, 116, 68, 255};
            dug_earth(cv, 7.6f, seed);
            cylinder(cv, 0.0f, 0.0f, 4.6f, -2.0f, 1.2f, kSteel);
            rim(cv, 3.8f, 4.6f, 1.5f, kSteel);
            cylinder(cv, 0.0f, 0.0f, 1.5f, 1.2f, 2.5f, {112, 100, 72, 255});
            cylinder(cv, 0.0f, 0.0f, 0.7f, 2.5f, 2.8f, {58, 52, 42, 255});
            earth_over(cv, 3.6f, 1.2f, seed);
            break;
        }
        case Model::Pmn2: {
            // Green plastic, the black rubber cap over its pressure plate, a cross raised on it.
            dug_earth(cv, 5.4f, seed);
            cylinder(cv, 0.0f, 0.0f, 3.0f, -1.0f, 0.9f, {86, 108, 64, 255});
            cylinder(cv, 0.0f, 0.0f, 1.9f, 0.9f, 1.2f, {50, 50, 46, 255});
            for (float u = -1.8f; u <= 1.8f; u += kStep) {
                for (float v = -1.8f; v <= 1.8f; v += kStep) {
                    if (u * u + v * v <= 3.2f && (std::fabs(u - v) < 0.55f || std::fabs(u + v) < 0.55f)) {
                        cv.lit(u, v, 1.45f, 0.0f, 0.0f, 1.0f, {82, 82, 76, 255});
                    }
                }
            }
            break;
        }
        case Model::M14: {
            // A little olive plastic one, its pressure plate a lighter disc, the arming mark on it.
            dug_earth(cv, 4.6f, seed);
            cylinder(cv, 0.0f, 0.0f, 2.3f, -1.0f, 0.8f, {122, 114, 68, 255});
            cylinder(cv, 0.0f, 0.0f, 1.6f, 0.8f, 1.1f, {144, 136, 84, 255});
            cv.lit(0.0f, 0.0f, 1.3f, 0.0f, 0.0f, 1.0f, {196, 164, 64, 255});
            earth_over(cv, 2.0f, 0.8f, seed);
            break;
        }
        case Model::Mon50: {
            // A curved green box 22 cm by 15 on its folding legs, its convex face
            // to the enemy; the MUV fuze in its well on top, the tripwire off it.
            cv.shadows = true;
            constexpr Color kLeg{56, 58, 50, 255};
            rod(cv, 0.2f, -0.6f, 3.2f, 2.0f, -1.2f, 0.0f, kLeg);
            rod(cv, -0.2f, 0.6f, 3.2f, -2.0f, 1.2f, 0.0f, kLeg);
            curved_box(cv, 1.0f, 3.6f, 3.2f, 7.8f, 1.2f, {94, 116, 68, 255});
            cylinder(cv, -0.4f, 1.5f, 0.6f, 7.8f, 9.3f, {104, 92, 66, 255});
            fuze = cv.at(-0.4f, 1.5f, 9.3f);
            break;
        }
        case Model::Claymore: {
            // M18A1: olive drab, lower and wider, on two pairs of scissor legs;
            // its detonator well on top.
            cv.shadows = true;
            constexpr Color kLeg{58, 58, 50, 255};
            for (const float s : {-2.7f, 2.7f}) {
                rod(cv, 0.0f, s, 2.3f, 1.3f, s * 1.1f, 0.0f, kLeg);
                rod(cv, 0.0f, s, 2.3f, -1.3f, s * 1.1f, 0.0f, kLeg);
            }
            curved_box(cv, 0.7f, 3.7f, 2.2f, 5.0f, 1.0f, {124, 118, 72, 255});
            cylinder(cv, -0.2f, 1.8f, 0.5f, 5.0f, 6.0f, {70, 68, 48, 255});
            fuze = cv.at(-0.2f, 1.8f, 6.0f);
            break;
        }
        case Model::Count: break;
    }
    outline(cv);
    Sprite out;
    out.img = GenImageColor(kW, kH, {0, 0, 0, 0});
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const auto i = static_cast<size_t>(y * kW + x);
            if (cv.mark[i] != kEmpty) ImageDrawPixel(&out.img, x, y, cv.px[i]);
        }
    }
    out.origin = {kOx, kOy};
    out.fuze = {fuze.x == 0.0f && fuze.y == 0.0f ? 0.0f : fuze.x - kOx + 0.5f, fuze.x == 0.0f && fuze.y == 0.0f ? 0.0f : fuze.y - kOy + 0.5f};
    return out;
}

}  // namespace render::mines
