#include "hud/cursors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace hud {
namespace {

// A cursor is drawn in pixels on a small canvas, a dark outline added round
// it, then shown twice its size: pixel art, like the rest.
constexpr int kSize = 20;
constexpr float kScale = 2.0f;

struct Canvas {
    std::array<Color, kSize * kSize> px{};
    void set(int x, int y, Color c) {
        if (x >= 0 && y >= 0 && x < kSize && y < kSize) px[static_cast<size_t>(y * kSize + x)] = c;
    }
    Color get(int x, int y) const {
        return x >= 0 && y >= 0 && x < kSize && y < kSize ? px[static_cast<size_t>(y * kSize + x)] : Color{0, 0, 0, 0};
    }
    // A line `w` pixels thick.
    void line(float x0, float y0, float x1, float y1, Color c, int w = 1) {
        const float d = std::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
        const int n = std::max(1, static_cast<int>(std::ceil(d * 2.0f)));
        for (int i = 0; i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            const int x = static_cast<int>(std::floor(x0 + (x1 - x0) * t));
            const int y = static_cast<int>(std::floor(y0 + (y1 - y0) * t));
            for (int dy = 0; dy < w; ++dy) {
                for (int dx = 0; dx < w; ++dx) set(x + dx, y + dy, c);
            }
        }
    }
    void disc(float cx, float cy, float r, Color c) {
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - cx;
                const float dy = static_cast<float>(y) + 0.5f - cy;
                if (dx * dx + dy * dy <= r * r) set(x, y, c);
            }
        }
    }
    void ring(float cx, float cy, float r0, float r1, Color c) {
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - cx;
                const float dy = static_cast<float>(y) + 0.5f - cy;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d >= r0 && d <= r1) set(x, y, c);
            }
        }
    }
    void rect(int x0, int y0, int x1, int y1, Color c) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) set(x, y, c);
        }
    }
    // A convex polygon, filled.
    void poly(const std::vector<Vector2>& p, Color c) {
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const Vector2 q{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
                bool in = true;
                float sign = 0.0f;
                for (size_t i = 0; i < p.size() && in; ++i) {
                    const Vector2 a = p[i];
                    const Vector2 b = p[(i + 1) % p.size()];
                    const float cross = (b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x);
                    if (cross != 0.0f) {
                        if (sign == 0.0f) sign = cross;
                        in = (cross > 0.0f) == (sign > 0.0f);
                    }
                }
                if (in) set(x, y, c);
            }
        }
    }
    void outline() {
        const std::array<Color, kSize * kSize> was = px;
        auto filled = [&](int x, int y) { return x >= 0 && y >= 0 && x < kSize && y < kSize && was[static_cast<size_t>(y * kSize + x)].a > 0; };
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                if (filled(x, y)) continue;
                if (filled(x - 1, y) || filled(x + 1, y) || filled(x, y - 1) || filled(x, y + 1)) set(x, y, {18, 16, 14, 255});
            }
        }
    }
};

constexpr Color kWhite{240, 240, 234, 255};
constexpr Color kGrey{176, 180, 184, 255};
constexpr Color kSteel{150, 160, 170, 255};
constexpr Color kShine{226, 234, 240, 255};
constexpr Color kWood{140, 92, 52, 255};
constexpr Color kWoodDark{104, 66, 36, 255};
constexpr Color kGold{232, 190, 70, 255};
constexpr Color kRed{222, 58, 48, 255};
constexpr Color kGreen{110, 126, 70, 255};

struct Baked {
    Texture2D tex{};
    Vector2 hotspot{};  // canvas pixels
};
std::array<Baked, static_cast<size_t>(Cursor::Count)> g_cursors{};

Baked bake(Cursor kind) {
    Canvas c;
    Vector2 hot{1.0f, 1.0f};
    switch (kind) {
        case Cursor::Arrow:
            c.poly({{1.0f, 1.0f}, {1.0f, 15.0f}, {5.0f, 11.5f}, {11.0f, 11.0f}}, kWhite);
            c.poly({{4.0f, 11.0f}, {6.0f, 10.0f}, {9.0f, 16.0f}, {7.0f, 17.0f}}, kWhite);
            c.line(2.0f, 12.0f, 5.0f, 10.5f, kGrey);  // its shade
            c.line(7.0f, 12.0f, 8.0f, 15.5f, kGrey);
            break;
        case Cursor::Attack:  // a sword, its point up at the top left
            c.line(1.0f, 1.0f, 10.5f, 10.5f, kSteel, 2);
            c.line(1.0f, 1.0f, 10.0f, 10.0f, kShine);
            c.line(13.5f, 7.5f, 7.5f, 13.5f, kGold, 2);  // the guard
            c.line(11.5f, 11.5f, 14.5f, 14.5f, kWood, 2);  // the grip
            c.disc(16.0f, 16.0f, 1.8f, kGold);             // the pommel
            break;
        case Cursor::Axe:  // the handle down to the right, the blade flaring out up to the right of its top
            c.line(16.0f, 18.0f, 4.0f, 6.5f, kWood, 2);
            c.poly({{2.4f, 6.4f}, {5.2f, 0.4f}, {12.6f, 6.2f}, {7.0f, 9.6f}}, kSteel);
            c.line(5.6f, 0.8f, 12.0f, 6.0f, kShine);  // the edge
            c.rect(1, 8, 2, 9, kSteel);               // the poll behind
            hot = {6.0f, 4.0f};
            break;
        case Cursor::Pick:  // a pickaxe: the head across the handle's end
            c.line(15.5f, 16.5f, 7.0f, 8.0f, kWood, 2);
            c.line(1.5f, 10.0f, 10.0f, 1.5f, kSteel, 2);
            c.line(2.0f, 9.0f, 9.0f, 2.0f, kShine);
            hot = {2.0f, 10.0f};
            break;
        case Cursor::Build:  // a hammer
            c.line(15.5f, 16.5f, 6.0f, 7.0f, kWood, 2);
            c.poly({{1.0f, 7.0f}, {7.0f, 1.0f}, {10.0f, 4.0f}, {4.0f, 10.0f}}, kSteel);
            c.line(1.0f, 7.0f, 7.0f, 1.0f, kShine);
            hot = {3.0f, 3.0f};
            break;
        case Cursor::Enter:  // a small arrow, a doorway by it, its door ajar
            c.poly({{1.0f, 1.0f}, {1.0f, 10.0f}, {3.5f, 7.8f}, {7.0f, 7.5f}}, kWhite);
            c.rect(8, 6, 17, 18, kWoodDark);
            c.rect(8, 6, 17, 6, kWood);  // the lintel
            c.rect(10, 8, 15, 18, {34, 28, 24, 255});
            c.rect(10, 8, 11, 18, kWood);  // the door, open
            c.rect(9, 18, 16, 18, kGrey);  // the step
            hot = {1.0f, 1.0f};
            break;
        case Cursor::Supply:  // a small arrow, a crate of supplies by it, bands round it
            c.poly({{1.0f, 1.0f}, {1.0f, 10.0f}, {3.5f, 7.8f}, {7.0f, 7.5f}}, kWhite);
            c.rect(7, 8, 17, 17, kGreen);
            c.rect(7, 8, 17, 9, {150, 160, 100, 255});
            c.rect(11, 8, 12, 17, kGold);
            c.rect(7, 12, 17, 13, kGold);
            hot = {1.0f, 1.0f};
            break;
        case Cursor::Target:  // crosshairs
            c.ring(10.0f, 10.0f, 5.0f, 6.6f, kRed);
            c.line(10.0f, 1.0f, 10.0f, 6.0f, kRed);
            c.line(10.0f, 14.0f, 10.0f, 19.0f, kRed);
            c.line(1.0f, 10.0f, 6.0f, 10.0f, kRed);
            c.line(14.0f, 10.0f, 19.0f, 10.0f, kRed);
            c.set(10, 10, kWhite);
            hot = {10.5f, 10.5f};
            break;
        case Cursor::Forbidden:
            c.ring(10.0f, 10.0f, 5.5f, 7.5f, kRed);
            c.line(5.5f, 14.0f, 14.0f, 5.5f, kRed, 2);
            hot = {10.0f, 10.0f};
            break;
        case Cursor::Count: break;
    }
    c.outline();
    Image img = GenImageColor(kSize, kSize, {0, 0, 0, 0});
    auto* out = static_cast<Color*>(img.data);
    std::copy(c.px.begin(), c.px.end(), out);
    Baked b;
    b.tex = LoadTextureFromImage(img);
    SetTextureFilter(b.tex, TEXTURE_FILTER_POINT);
    b.hotspot = hot;
    UnloadImage(img);
    return b;
}

}  // namespace

void draw_cursor(Cursor cursor, Vector2 at) {
    const auto i = static_cast<size_t>(cursor);
    if (i >= g_cursors.size()) return;
    if (g_cursors[i].tex.id == 0) g_cursors[i] = bake(cursor);
    const Baked& b = g_cursors[i];
    // A little bob on the action cursors, as AoE II's animate: it's alive, it'll do something.
    const float bob = cursor == Cursor::Arrow || cursor == Cursor::Target || cursor == Cursor::Forbidden
                          ? 0.0f
                          : std::round(std::sin(static_cast<float>(GetTime()) * 7.0f) * 1.0f);
    const Rectangle dst{std::round(at.x - b.hotspot.x * kScale), std::round(at.y - b.hotspot.y * kScale + bob), kSize * kScale, kSize * kScale};
    DrawTexturePro(b.tex, {0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)}, dst, {0.0f, 0.0f}, 0.0f, WHITE);
}

}  // namespace hud
