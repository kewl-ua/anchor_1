#include "render/plants.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace render::plants {
namespace {

// A cell being painted, pixel by pixel.
struct Cell {
    int w;
    int h;
    std::vector<Color> px;
    Cell(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_ * h_), Color{0, 0, 0, 0}) {}
    bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    Color get(int x, int y) const { return in(x, y) ? px[static_cast<size_t>(y * w + x)] : Color{0, 0, 0, 0}; }
    void set(int x, int y, Color c) {
        if (in(x, y)) px[static_cast<size_t>(y * w + x)] = c;
    }
    void dot(float x, float y, Color c) { set(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), c); }
    void line(float x0, float y0, float x1, float y1, Color c) {
        const float d = std::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
        const int n = std::max(1, static_cast<int>(std::ceil(d)));
        for (int i = 0; i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            dot(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, c);
        }
    }
    void disc(float cx, float cy, float r, Color c) {
        for (int y = static_cast<int>(std::floor(cy - r)) - 1; y <= static_cast<int>(std::ceil(cy + r)) + 1; ++y) {
            for (int x = static_cast<int>(std::floor(cx - r)) - 1; x <= static_cast<int>(std::ceil(cx + r)) + 1; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - cx;
                const float dy = static_cast<float>(y) + 0.5f - cy;
                if (dx * dx + dy * dy <= r * r) set(x, y, c);
            }
        }
    }
    void oval(float cx, float cy, float rx, float ry, Color c) {
        for (int y = static_cast<int>(std::floor(cy - ry)) - 1; y <= static_cast<int>(std::ceil(cy + ry)) + 1; ++y) {
            for (int x = static_cast<int>(std::floor(cx - rx)) - 1; x <= static_cast<int>(std::ceil(cx + rx)) + 1; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f - cx) / rx;
                const float dy = (static_cast<float>(y) + 0.5f - cy) / ry;
                if (dx * dx + dy * dy <= 1.0f) set(x, y, c);
            }
        }
    }
    // A ball lit from the upper left: dark under, the colour, a highlight.
    void ball(float cx, float cy, float r, Color c) {
        disc(cx, cy, r, shade(c, 0.7f));
        disc(cx - 0.25f * r, cy - 0.25f * r, r * 0.78f, c);
        if (r > 1.2f) disc(cx - 0.45f * r, cy - 0.45f * r, r * 0.34f, shade(c, 1.25f));
    }
    // A dark rim round what's there.
    void outline(Color rim) {
        const std::vector<Color> was = px;
        auto filled = [&](int x, int y) { return in(x, y) && was[static_cast<size_t>(y * w + x)].a != 0; };
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (filled(x, y)) continue;
                if (filled(x - 1, y) || filled(x + 1, y) || filled(x, y - 1) || filled(x, y + 1)) px[static_cast<size_t>(y * w + x)] = rim;
            }
        }
    }
    static Color shade(Color c, float k) {
        auto ch = [k](unsigned char v) { return static_cast<unsigned char>(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f)); };
        return {ch(c.r), ch(c.g), ch(c.b), c.a};
    }
};

Color shade(Color c, float k) { return Cell::shade(c, k); }
Color mix(Color a, Color b, float t) {
    auto ch = [t](unsigned char x, unsigned char y) { return static_cast<unsigned char>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t); };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), 255};
}
float hash01(uint32_t a, int i) {
    uint32_t h = a * 0x9E3779B1u ^ static_cast<uint32_t>(i) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return static_cast<float>(h & 0xFFFFu) / 65536.0f;
}

constexpr Color kRim{30, 38, 22, 255};

// --- Sunflowers ---------------------------------------------------------------

constexpr Layout kSunflower{18, 30, 9.0f, 28.0f, 42};

// A leaf out from the stalk at (x, y) to `side`: a heart gone to a point,
// two-tone, drooping.
void leaf(Cell& c, float x, float y, float side, float size, Color green) {
    for (int k = 1; k <= 4; ++k) {
        const float t = static_cast<float>(k) / 4.0f;
        const float r = size * (k == 1 ? 0.9f : k == 2 ? 1.1f : k == 3 ? 0.85f : 0.5f);
        c.disc(x + side * (1.2f + 3.6f * t * size / 1.5f), y + 1.6f * t * t * size, r, shade(green, 0.78f));
    }
    for (int k = 1; k <= 3; ++k) {
        const float t = static_cast<float>(k) / 4.0f;
        c.disc(x + side * (1.0f + 3.4f * t * size / 1.5f), y - 0.4f + 1.4f * t * t * size, size * (k == 2 ? 0.7f : 0.5f), green);
    }
    c.line(x, y, x + side * 4.0f * size / 1.5f, y + 1.4f * size, shade(green, 0.6f));  // the vein
}

void paint_sunflower(Cell& c, int stage, int height, int lean, bool back, uint32_t seed) {
    const float ox = kSunflower.origin_x;
    const float oy = kSunflower.origin_y;
    const float tall = 15.0f + 2.5f * static_cast<float>(height);
    const float lx = (static_cast<float>(lean) - 1.0f) * 2.0f;
    const Color stalk = stage == 0 ? Color{72, 108, 44, 255} : stage == 1 ? Color{104, 114, 54, 255} : Color{130, 106, 64, 255};
    auto along = [&](float t) { return Vector2{ox + lx * t * t, oy - tall * t}; };
    // The leaves behind the stalk first, then it, then those in front.
    const Color green = stage == 0 ? Color{66, 116, 46, 255} : Color{112, 122, 52, 255};
    const Color yellowed{156, 146, 66, 255};
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < 4; ++i) {
            const float side = i % 2 == 0 ? -1.0f : 1.0f;
            if ((side > 0.0f) != (pass == 1)) continue;
            const float t = 0.22f + 0.17f * static_cast<float>(i);
            const Vector2 a = along(t);
            if (stage == 2) {  // shrivelled, hanging
                c.line(a.x, a.y, a.x + side * 2.0f, a.y + 1.0f, {96, 70, 44, 255});
                c.line(a.x + side * 2.0f, a.y + 1.0f, a.x + side * 2.5f, a.y + 4.0f, {80, 58, 38, 255});
                continue;
            }
            const float size = 1.6f - 0.18f * static_cast<float>(i) + 0.2f * hash01(seed, i);
            leaf(c, a.x, a.y, side, size, stage == 1 && i == 0 ? yellowed : green);
        }
        if (pass == 0) {
            for (float t = 0.0f; t <= 1.0f; t += 0.02f) {
                const Vector2 p = along(t);
                c.dot(p.x, p.y, stalk);
                c.dot(p.x + 1.0f, p.y, shade(stalk, 0.72f));
            }
        }
    }
    const Vector2 neck = along(1.0f);
    if (stage == 0 && !back) {  // in bloom, turned to the sun: petals round the brown disc of the seeds
        const Vector2 h{neck.x + 0.5f, neck.y - 1.5f};
        for (int k = 0; k < 12; ++k) {
            const float a = static_cast<float>(k) / 12.0f * 6.2831853f;
            const float r = 3.5f + 0.4f * hash01(seed, 20 + k);
            const bool under = std::sin(a) + 0.4f * std::cos(a) > 0.3f;
            c.disc(h.x + std::cos(a) * r, h.y + std::sin(a) * r * 0.9f, 1.25f, under ? Color{214, 150, 28, 255} : Color{246, 202, 44, 255});
        }
        c.disc(h.x, h.y, 2.4f, {88, 58, 30, 255});
        c.disc(h.x - 0.5f, h.y - 0.5f, 1.4f, {118, 80, 40, 255});
        c.dot(h.x - 1.0f, h.y - 1.0f, {150, 108, 56, 255});
        c.dot(h.x + 0.5f, h.y + 0.6f, {60, 40, 22, 255});
    } else if (stage == 0) {  // in bloom, turned away: the green back, the petals' tips round it
        const Vector2 h{neck.x + 0.5f, neck.y - 1.5f};
        for (int k = 0; k < 10; ++k) {
            const float a = static_cast<float>(k) / 10.0f * 6.2831853f;
            c.disc(h.x + std::cos(a) * 3.4f, h.y + std::sin(a) * 3.1f, 1.1f, {236, 186, 40, 255});
        }
        c.ball(h.x, h.y, 2.9f, {96, 130, 56, 255});
    } else if (stage == 1) {  // ripening: the heavy head hanging, the petals gone brown
        const Vector2 h{neck.x + 2.0f, neck.y + 2.0f};
        c.line(neck.x, neck.y, h.x, h.y - 1.5f, stalk);
        for (int k = 0; k < 7; ++k) {
            const float a = 0.3f + static_cast<float>(k) / 7.0f * 3.1f;
            c.disc(h.x + std::cos(a) * 3.4f, h.y + std::sin(a) * 3.2f, 1.0f, {170, 126, 50, 255});
        }
        c.ball(h.x, h.y, 3.2f, {118, 116, 58, 255});
    } else {  // dry: black, bowed right down
        const Vector2 h{neck.x + 1.5f, neck.y + 3.5f};
        c.line(neck.x, neck.y, h.x, h.y - 2.0f, stalk);
        c.ball(h.x, h.y, 3.0f, {56, 42, 30, 255});
        c.dot(h.x - 1.5f, h.y - 1.5f, {104, 80, 52, 255});
    }
    c.outline(kRim);
}

// Knocked down: the stalk lying along the ground, its head at the end, the leaves crushed about it.
void paint_sunflower_down(Cell& c, int stage, int way, uint32_t seed) {
    const float ox = kSunflower.origin_x;
    const float oy = kSunflower.origin_y - 1.0f;
    const float dir = way == 0 ? -1.0f : 1.0f;
    const Color stalk = stage == 2 ? Color{128, 104, 62, 255} : Color{92, 110, 50, 255};
    const Vector2 end{ox + dir * 5.5f, oy - 2.5f};
    for (int k = 0; k < 4; ++k) {
        const float t = 0.2f + 0.2f * static_cast<float>(k);
        const Color leaf = stage == 2 ? Color{94, 72, 46, 255} : Color{74, 108, 48, 255};
        c.oval(ox + (end.x - ox) * t + (hash01(seed, k) - 0.5f) * 2.0f, oy + (end.y - oy) * t + 1.0f, 1.8f, 0.9f, leaf);
    }
    c.line(ox, oy, end.x, end.y, stalk);
    c.line(ox, oy + 1.0f, end.x, end.y + 1.0f, shade(stalk, 0.7f));
    const Color head = stage == 0 ? Color{236, 190, 40, 255} : stage == 1 ? Color{128, 116, 60, 255} : Color{58, 44, 32, 255};
    c.oval(end.x + dir * 1.5f, end.y + 0.5f, 2.6f, 1.6f, head);
    c.oval(end.x + dir * 1.5f, end.y + 0.3f, 1.4f, 0.9f, stage == 0 ? Color{94, 62, 32, 255} : shade(head, 0.7f));
    c.outline(kRim);
}

// --- Maize --------------------------------------------------------------------

constexpr Layout kMaize{22, 36, 11.0f, 34.0f, 16};

void paint_maize(Cell& c, int stage, int height, int lean, uint32_t seed) {
    const float ox = kMaize.origin_x;
    const float oy = kMaize.origin_y;
    const float tall = 24.0f + 3.0f * static_cast<float>(height);
    const float lx = (static_cast<float>(lean) - 1.0f) * 1.6f;
    const bool dry = stage == 1;
    const Color stalk = dry ? Color{188, 166, 104, 255} : Color{88, 128, 54, 255};
    const Color green = dry ? Color{196, 176, 112, 255} : Color{80, 134, 52, 255};
    auto along = [&](float t) { return Vector2{ox + lx * t * t, oy - tall * t}; };
    // Long leaves arching out from the nodes, drooping at the tips.
    auto blade = [&](float t, float side, float reach) {
        const Vector2 a = along(t);
        const Vector2 m{a.x + side * reach * 0.55f, a.y - 3.0f - reach * 0.1f};
        const Vector2 e{a.x + side * reach, a.y + (dry ? 3.5f : 1.5f)};
        c.line(a.x, a.y, m.x, m.y, green);
        c.line(m.x, m.y, e.x, e.y, shade(green, 0.82f));
        c.line(a.x, a.y + 1.0f, m.x, m.y + 1.0f, shade(green, 0.7f));
        c.dot(m.x - side * 1.0f, m.y - 0.5f, shade(green, 1.25f));
    };
    for (int i = 0; i < 7; ++i) {
        const float side = i % 2 == 0 ? -1.0f : 1.0f;
        if (side > 0.0f) continue;
        blade(0.2f + 0.11f * static_cast<float>(i), side, 6.0f + 3.0f * hash01(seed, i));
    }
    for (float t = 0.0f; t <= 1.0f; t += 0.02f) {
        const Vector2 p = along(t);
        c.dot(p.x, p.y, stalk);
        c.dot(p.x + 1.0f, p.y, shade(stalk, 0.72f));
    }
    for (int n = 1; n < 6; ++n) {  // the nodes
        const Vector2 p = along(static_cast<float>(n) / 6.0f);
        c.dot(p.x, p.y, shade(stalk, 0.6f));
    }
    // The cob in its husk, its silk.
    const Vector2 cob = along(0.42f);
    const Color husk = dry ? Color{214, 194, 132, 255} : Color{154, 184, 92, 255};
    c.oval(cob.x + 2.0f, cob.y - 1.0f, 1.4f, 3.0f, husk);
    c.dot(cob.x + 1.5f, cob.y - 2.5f, shade(husk, 1.2f));
    c.line(cob.x + 2.0f, cob.y - 4.0f, cob.x + 3.0f, cob.y - 5.5f, {156, 94, 50, 255});
    for (int i = 0; i < 7; ++i) {
        const float side = i % 2 == 0 ? -1.0f : 1.0f;
        if (side < 0.0f) continue;
        blade(0.2f + 0.11f * static_cast<float>(i), side, 6.0f + 3.0f * hash01(seed, i));
    }
    // The tassel on top.
    const Vector2 top = along(1.0f);
    const Color tassel{214, 196, 124, 255};
    c.line(top.x, top.y, top.x, top.y - 4.0f, tassel);
    c.line(top.x, top.y - 1.0f, top.x - 2.5f, top.y - 3.5f, shade(tassel, 0.85f));
    c.line(top.x, top.y - 1.0f, top.x + 2.5f, top.y - 3.0f, shade(tassel, 0.9f));
    c.outline(kRim);
}

void paint_maize_down(Cell& c, int stage, int way, uint32_t seed) {
    const float ox = kMaize.origin_x;
    const float oy = kMaize.origin_y - 1.0f;
    const float dir = way == 0 ? -1.0f : 1.0f;
    const bool dry = stage == 1;
    const Color stalk = dry ? Color{176, 154, 96, 255} : Color{90, 120, 54, 255};
    const Vector2 end{ox + dir * 7.5f, oy - 3.5f};
    for (int k = 0; k < 5; ++k) {
        const float t = 0.15f + 0.18f * static_cast<float>(k);
        const float bx = ox + (end.x - ox) * t;
        const float by = oy + (end.y - oy) * t;
        const float side = k % 2 == 0 ? -1.0f : 1.0f;
        c.line(bx, by, bx + 2.0f * dir + (hash01(seed, k) - 0.5f) * 2.0f, by + side * 1.5f, dry ? Color{200, 178, 112, 255} : Color{84, 128, 54, 255});
    }
    c.line(ox, oy, end.x, end.y, stalk);
    c.line(ox, oy + 1.0f, end.x, end.y + 1.0f, shade(stalk, 0.7f));
    c.oval(ox + (end.x - ox) * 0.5f, oy + (end.y - oy) * 0.5f + 1.0f, 2.2f, 1.0f, dry ? Color{214, 194, 132, 255} : Color{154, 184, 92, 255});
    c.outline(kRim);
}

// --- Wheat --------------------------------------------------------------------

constexpr Layout kWheat{16, 16, 8.0f, 14.0f, 20};

void paint_wheat(Cell& c, int stage, int variant, int lean, uint32_t seed) {
    const float ox = kWheat.origin_x;
    const float oy = kWheat.origin_y;
    const bool ripe = stage == 1;
    const Color straw = ripe ? Color{186, 150, 66, 255} : Color{140, 152, 72, 255};
    const Color ear = ripe ? Color{228, 196, 108, 255} : Color{176, 184, 94, 255};
    const float wind = (static_cast<float>(lean) - 1.5f) * 1.3f;
    const int stalks = 7 + variant;
    for (int i = 0; i < stalks; ++i) {
        const float spread = (static_cast<float>(i) / static_cast<float>(stalks - 1) - 0.5f) * 6.0f + (hash01(seed, i) - 0.5f) * 1.2f;
        const float tall = 7.0f + 3.0f * hash01(seed, 10 + i);
        const Vector2 base{ox + spread * 0.5f, oy};
        const Vector2 tip{ox + spread + wind * (0.7f + 0.3f * hash01(seed, 20 + i)), oy - tall};
        c.line(base.x, base.y, tip.x, tip.y + 2.0f, i % 3 == 0 ? shade(straw, 0.8f) : straw);
        // The ear, two pixels thick, and its awns.
        c.line(tip.x, tip.y, tip.x - wind * 0.15f, tip.y + 2.5f, ear);
        c.line(tip.x + 1.0f, tip.y + 0.5f, tip.x + 1.0f - wind * 0.15f, tip.y + 2.5f, shade(ear, 0.78f));
        if (ripe) c.line(tip.x, tip.y, tip.x + wind * 0.4f + 0.5f, tip.y - 2.0f, shade(ear, 1.12f));
    }
}

void paint_wheat_down(Cell& c, int stage, int variant, uint32_t seed) {
    const float ox = kWheat.origin_x;
    const float oy = kWheat.origin_y;
    const bool ripe = stage == 1;
    const Color straw = ripe ? Color{206, 176, 100, 255} : Color{152, 160, 84, 255};
    const float a = variant == 0 ? -0.35f : 3.5f;  // which way it was pressed down
    for (int i = 0; i < 9; ++i) {
        const float x = ox + (hash01(seed, i) - 0.5f) * 10.0f;
        const float y = oy - 1.0f - hash01(seed, 10 + i) * 3.0f;
        const float r = a + (hash01(seed, 20 + i) - 0.5f) * 0.5f;
        c.line(x, y, x + std::cos(r) * 5.0f, y + std::sin(r) * 1.6f, i % 3 == 0 ? shade(straw, 0.78f) : straw);
    }
}

// --- A kitchen garden ---------------------------------------------------------

constexpr Layout kGarden{16, 16, 8.0f, 13.0f, 12};

void paint_garden(Cell& c, int kind, int variant, uint32_t seed) {
    const float ox = kGarden.origin_x;
    const float oy = kGarden.origin_y;
    if (kind == 0) {  // a potato plant: a mound of dark leaves, a few flowers
        const Color leaf{58, 102, 42, 255};
        for (int k = 0; k < 6; ++k) {
            const float a = static_cast<float>(k) / 6.0f * 3.14159f + 3.14159f;
            c.ball(ox + std::cos(a) * 3.0f, oy - 2.0f + std::sin(a) * 2.2f, 1.8f + 0.3f * hash01(seed, k), leaf);
        }
        c.ball(ox, oy - 3.5f, 2.0f, shade(leaf, 1.1f));
        const Color flower = variant == 1 ? Color{176, 128, 196, 255} : Color{238, 236, 226, 255};
        for (int k = 0; k < 1 + variant; ++k) c.dot(ox - 2.0f + 2.0f * static_cast<float>(k), oy - 5.5f + static_cast<float>(k % 2), flower);
    } else if (kind == 1) {  // a cabbage: the outer leaves spread, the pale head
        c.oval(ox, oy - 1.0f, 4.6f, 2.4f, {98, 140, 104, 255});
        c.oval(ox - 0.5f, oy - 1.3f, 3.8f, 1.8f, {120, 164, 124, 255});
        for (int k = 0; k < 4; ++k) {
            const float a = static_cast<float>(k) * 1.57f + 0.4f;
            c.line(ox, oy - 1.0f, ox + std::cos(a) * 4.0f, oy - 1.0f + std::sin(a) * 2.0f, {160, 196, 164, 255});
        }
        c.ball(ox, oy - 3.0f, 2.8f + 0.3f * static_cast<float>(variant), {172, 208, 164, 255});
    } else {  // tomatoes tied to a stake: its leaves, the red fruit
        c.line(ox + 1.0f, oy, ox + 1.0f, oy - 11.0f, {140, 104, 64, 255});
        const Color leaf{70, 116, 48, 255};
        for (int k = 0; k < 5; ++k) c.ball(ox + (k % 2 == 0 ? -1.2f : 2.4f), oy - 2.0f - 1.8f * static_cast<float>(k), 1.6f, leaf);
        for (int k = 0; k < 3 + variant; ++k) {
            c.ball(ox - 1.5f + 3.2f * hash01(seed, k), oy - 3.0f - 6.0f * hash01(seed, 10 + k), 1.4f,
                   k % 4 == 3 ? Color{226, 170, 50, 255} : Color{214, 52, 40, 255});
        }
    }
    c.outline(kRim);
}

void paint_garden_down(Cell& c, int variant, uint32_t seed) {
    const float ox = kGarden.origin_x;
    const float oy = kGarden.origin_y;
    c.oval(ox, oy - 0.5f, 4.5f, 1.8f, {96, 80, 56, 255});
    for (int k = 0; k < 3 + variant; ++k) c.oval(ox - 3.0f + 6.0f * hash01(seed, k), oy - 1.0f + hash01(seed, 5 + k), 1.3f, 0.7f, {70, 100, 50, 255});
}

Image sheet(const Layout& l, const std::function<void(Cell&, int)>& paint) {
    Image img = GenImageColor(l.w * l.frames, l.h, {0, 0, 0, 0});
    auto* out = static_cast<Color*>(img.data);
    for (int f = 0; f < l.frames; ++f) {
        Cell c(l.w, l.h);
        paint(c, f);
        for (int y = 0; y < l.h; ++y) {
            for (int x = 0; x < l.w; ++x) out[static_cast<size_t>(y * l.w * l.frames + f * l.w + x)] = c.px[static_cast<size_t>(y * l.w + x)];
        }
    }
    return img;
}

}  // namespace

Layout layout(Plant p) {
    switch (p) {
        case Plant::Sunflower: return kSunflower;
        case Plant::Maize: return kMaize;
        case Plant::Wheat: return kWheat;
        default: return kGarden;
    }
}

int sunflower(int stage, int height, int lean, bool back) { return back && stage == 0 ? 27 + height * 3 + lean : stage * 9 + height * 3 + lean; }
int sunflower_down(int stage, int way) { return 36 + stage * 2 + way; }
int maize(int stage, int height, int lean) { return stage * 6 + height * 3 + lean; }
int maize_down(int stage, int way) { return 12 + stage * 2 + way; }
int wheat(int stage, int variant, int lean) { return stage * 8 + variant * 4 + lean; }
int wheat_down(int stage, int variant) { return 16 + stage * 2 + variant; }
int garden(int kind, int variant) { return kind * 3 + variant; }
int garden_down(int variant) { return 9 + variant; }

Image bake(Plant p) {
    switch (p) {
        case Plant::Sunflower:
            return sheet(kSunflower, [](Cell& c, int f) {
                const auto seed = static_cast<uint32_t>((f / 3) * 7919 + 13);  // (the same plant swaying: the same leaves)
                if (f >= 36) return paint_sunflower_down(c, (f - 36) / 2, (f - 36) % 2, seed);
                if (f >= 27) return paint_sunflower(c, 0, (f - 27) / 3, (f - 27) % 3, true, seed);
                paint_sunflower(c, f / 9, (f % 9) / 3, f % 3, false, seed);
            });
        case Plant::Maize:
            return sheet(kMaize, [](Cell& c, int f) {
                const auto seed = static_cast<uint32_t>((f / 3) * 7919 + 29);
                if (f >= 12) return paint_maize_down(c, (f - 12) / 2, (f - 12) % 2, seed);
                paint_maize(c, f / 6, (f % 6) / 3, f % 3, seed);
            });
        case Plant::Wheat:
            return sheet(kWheat, [](Cell& c, int f) {
                const auto seed = static_cast<uint32_t>(f * 7919 + 41);
                if (f >= 16) return paint_wheat_down(c, (f - 16) / 2, (f - 16) % 2, seed);
                paint_wheat(c, f / 8, (f % 8) / 4, f % 4, static_cast<uint32_t>((f % 8) / 4 * 17 + 3));
            });
        default:
            return sheet(kGarden, [](Cell& c, int f) {
                const auto seed = static_cast<uint32_t>(f * 7919 + 53);
                if (f >= 9) return paint_garden_down(c, f - 9, seed);
                paint_garden(c, f / 3, f % 3, seed);
            });
    }
}

}  // namespace render::plants
