#include "render/world_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "render/convert.h"
#include "render/iso.h"
#include "theme/palette.h"

namespace render {

namespace {

constexpr float kPingLifetime = 0.5f;
constexpr float kBlastLifetime = 0.45f;
constexpr float kWreckLifetime = 25.0f;
constexpr float kBodyLifetime = 8.0f;

// A circle of radius 1 tile on the ground is drawn as an ellipse with these semi-axes.
constexpr float kCircleRx = iso::kTileWidth * 0.5f * 1.41421356f;
constexpr float kCircleRy = iso::kTileHeight * 0.5f * 1.41421356f;

// How high a unit's body is above its feet, pixels.
float body_lift(const engine::Unit& u) { return engine::unit_type(u.type).vehicle ? 6.0f : 9.0f; }

Color shade(Color c, float k) {
    auto channel = [k](unsigned char v) {
        return static_cast<unsigned char>(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f));
    };
    return {channel(c.r), channel(c.g), channel(c.b), c.a};
}

// raylib skips triangles with the "wrong" winding; accept either.
void fill_triangle(Vector2 a, Vector2 b, Vector2 c, Color color) {
    const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross > 0) std::swap(b, c);
    DrawTriangle(a, b, c, color);
}

void fill_quad(Vector2 a, Vector2 b, Vector2 c, Vector2 d, Color color) {
    fill_triangle(a, b, c, color);
    fill_triangle(a, c, d, color);
}

// Ground point -> iso pixel on the terrain surface, optionally lifted up.
Vector2 on_terrain(const engine::TileMap& map, Vector2 ground, float lift = 0.0f) {
    Vector2 p = iso::project(ground, iso::surface_height(map, ground));
    p.y -= lift;
    return p;
}

// Screen-space offset of a ground-space vector (no height change).
Vector2 iso_offset(Vector2 ground_vec) { return iso::project(ground_vec, 0.0f); }

Vector2 lerp(Vector2 a, Vector2 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

Vector2 unit_facing(const engine::Unit& u) {
    const Vector2 f = to_vector2(u.facing);
    const float len = std::hypot(f.x, f.y);
    return len > 0.0f ? Vector2{f.x / len, f.y / len} : Vector2{1.0f, 0.0f};
}

uint32_t tile_hash(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

void draw_ground_ellipse(Vector2 center, float radius_tiles, Color color) {
    DrawEllipseLines(static_cast<int>(center.x), static_cast<int>(center.y), radius_tiles * kCircleRx,
                     radius_tiles * kCircleRy, color);
}

void fill_ground_ellipse(Vector2 center, float radius_tiles, Color color) {
    DrawEllipse(static_cast<int>(center.x), static_cast<int>(center.y), radius_tiles * kCircleRx,
                radius_tiles * kCircleRy, color);
}

}  // namespace

Vector2 unit_ground_pos(const engine::Unit& unit, float alpha) {
    return lerp(to_vector2(unit.prev_pos), to_vector2(unit.pos), alpha);
}

Vector2 unit_screen_pos(const RtsCamera& camera, const engine::TileMap& map, const engine::Unit& unit,
                        float alpha) {
    return camera.world_to_screen(on_terrain(map, unit_ground_pos(unit, alpha), body_lift(unit)));
}

float unit_pick_radius(const RtsCamera& camera, const engine::Unit& unit) {
    const engine::UnitTypeDef& def = engine::unit_type(unit.type);
    const float px = def.vehicle ? to_float(def.radius) * kCircleRx + 4.0f : 11.0f;
    return px * camera.camera2d().zoom;
}

// --- Effects -----------------------------------------------------------------

void WorldRenderer::add_order_ping(Vector2 ground, bool attack) { pings_.push_back({ground, 0.0f, attack}); }

void WorldRenderer::update(const engine::World& world, float dt) {
    const engine::TileMap& map = world.map();
    if (map.width() != cache_width_ || map.height() != cache_height_) {
        cache_width_ = map.width();
        cache_height_ = map.height();
        corner_heights_.resize(static_cast<size_t>((cache_width_ + 1) * (cache_height_ + 1)));
        for (int cy = 0; cy <= cache_height_; ++cy) {
            for (int cx = 0; cx <= cache_width_; ++cx) {
                corner_heights_[static_cast<size_t>(cy * (cache_width_ + 1) + cx)] = iso::corner_height(map, cx, cy);
            }
        }
    }

    // Shells and rockets that went off since the last frame: where they
    // actually burst, which may be a tree or a soldier in the way.
    for (const engine::Impact& impact : world.recent_impacts()) {
        if (impact.tick < impacts_seen_until_) continue;
        const float splash = to_float(engine::unit_type(impact.shooter_type).weapon.splash_radius);
        blasts_.push_back({to_vector2(impact.pos), 0.0f, std::max(splash, 0.2f)});
    }
    impacts_seen_until_ = world.tick();

    // Units that vanished died (a garrison buried in its house leaves no body to see).
    std::unordered_map<engine::EntityId, Remains> alive;
    for (const engine::Unit& u : world.units()) {
        if (!u.inside) alive[u.id] = {to_vector2(u.pos), 0.0f, engine::unit_type(u.type).vehicle};
    }
    for (const auto& [id, last] : units_seen_) {
        if (alive.contains(id) || world.find_unit(id)) continue;
        remains_.push_back(last);
        if (last.vehicle) blasts_.push_back({last.ground, 0.0f, 0.7f});
    }
    units_seen_ = std::move(alive);

    // Structures that vanished came down: a cloud of dust.
    std::unordered_map<engine::EntityId, Vector2> standing;
    for (const engine::Structure& s : world.structures()) standing[s.id] = to_vector2(s.center);
    for (const auto& [id, center] : structures_seen_) {
        if (!standing.contains(id)) blasts_.push_back({center, 0.0f, 1.6f});
    }
    structures_seen_ = std::move(standing);

    for (Ping& p : pings_) p.age += dt;
    for (Blast& b : blasts_) b.age += dt;
    for (Remains& r : remains_) r.age += dt;
    std::erase_if(pings_, [](const Ping& p) { return p.age >= kPingLifetime; });
    std::erase_if(blasts_, [](const Blast& b) { return b.age >= kBlastLifetime; });
    std::erase_if(remains_, [](const Remains& r) { return r.age >= (r.vehicle ? kWreckLifetime : kBodyLifetime); });
}

// --- Drawing -----------------------------------------------------------------

namespace {

// Calls fn(tx, ty) for every tile that may be on screen, back to front.
// Screen x depends on (x - y), screen y on (x + y) minus the height, so both
// are bounded and only visible tiles are walked, whatever the map size.
template <typename Fn>
void for_each_visible_tile(const engine::TileMap& map, Rectangle view, Fn fn) {
    const int w = map.width();
    const int h = map.height();
    const float half_w = iso::kTileWidth * 0.5f;
    const float half_h = iso::kTileHeight * 0.5f;
    const float max_lift = engine::TileMap::kMaxElevation * iso::kElevationStep + 40.0f;  // + tall scenery
    const int diff_min = static_cast<int>(std::floor(view.x / half_w)) - 1;
    const int diff_max = static_cast<int>(std::ceil((view.x + view.width) / half_w)) + 1;
    const int sum_min = std::max(0, static_cast<int>(std::floor(view.y / half_h)) - 2);
    const int sum_max = std::min(w + h - 2, static_cast<int>(std::ceil((view.y + view.height + max_lift) / half_h)));

    for (int diagonal = sum_min; diagonal <= sum_max; ++diagonal) {
        // x - y = 2x - diagonal must lie in [diff_min, diff_max].
        const int x_lo = std::max({0, diagonal - (h - 1), static_cast<int>(std::floor((diagonal + diff_min) * 0.5f))});
        const int x_hi = std::min({w - 1, diagonal, static_cast<int>(std::ceil((diagonal + diff_max) * 0.5f))});
        for (int tx = x_lo; tx <= x_hi; ++tx) fn(tx, diagonal - tx);
    }
}

// Two trees per forest tile, placed by a hash so they never move.
struct Tree {
    Vector2 ground;
    float size;
    float tint;
};

std::array<Tree, 2> trees_on_tile(int tx, int ty) {
    std::array<Tree, 2> trees{};
    uint32_t h = tile_hash(tx, ty);
    for (Tree& t : trees) {
        const float fx = 0.2f + static_cast<float>(h & 0xFF) / 255.0f * 0.6f;
        const float fy = 0.2f + static_cast<float>((h >> 8) & 0xFF) / 255.0f * 0.6f;
        t = {{static_cast<float>(tx) + fx, static_cast<float>(ty) + fy},
             0.85f + static_cast<float>((h >> 16) & 0xFF) / 255.0f * 0.35f,
             0.85f + static_cast<float>((h >> 24) & 0xFF) / 255.0f * 0.3f};
        h = h * 2654435761u + 0x9E3779B9u;
    }
    return trees;
}

void draw_tree(const engine::TileMap& map, const Tree& t) {
    const Vector2 base = on_terrain(map, t.ground);
    const float s = t.size;
    const Color leaf = shade({46, 94, 44, 255}, t.tint);
    DrawLineEx(base, {base.x, base.y - 7.0f * s}, 2.0f, {72, 54, 38, 255});
    DrawCircleV({base.x, base.y - 13.0f * s}, 8.5f * s, ColorAlpha(shade(leaf, 0.8f), 0.95f));
    DrawCircleV({base.x - 2.0f * s, base.y - 15.0f * s}, 5.5f * s, ColorAlpha(shade(leaf, 1.15f), 0.9f));
}

// A small house on one tile: walls and a hipped roof. Damage darkens it with
// soot and sets it smoking.
void draw_house(const engine::TileMap& map, int tx, int ty, float damage) {
    constexpr float kWall = 13.0f;
    constexpr float kRoof = 9.0f;
    const auto x = static_cast<float>(tx);
    const auto y = static_cast<float>(ty);
    const Vector2 ground[4] = {
        {x + 0.12f, y + 0.12f}, {x + 0.88f, y + 0.12f}, {x + 0.88f, y + 0.88f}, {x + 0.12f, y + 0.88f}};
    Vector2 base[4];
    Vector2 top[4];
    for (int i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, ground[i]);
        top[i] = {base[i].x, base[i].y - kWall};
    }
    const uint32_t h = tile_hash(tx, ty);
    const float soot = 1.0f - 0.45f * damage;
    const Color wall = shade({196, 184, 160, 255}, (0.9f + static_cast<float>(h & 0x3F) / 400.0f) * soot);
    const Color roof = shade((h >> 8) % 3 == 0 ? Color{120, 72, 58, 255} : Color{92, 90, 96, 255}, soot);

    // The walls facing the viewer are the +x (lit) and +y (shaded) sides.
    fill_quad(base[1], base[2], top[2], top[1], wall);
    fill_quad(base[2], base[3], top[3], top[2], shade(wall, 0.72f));
    const Vector2 apex = on_terrain(map, {x + 0.5f, y + 0.5f}, kWall + kRoof);
    fill_triangle(top[0], top[1], apex, shade(roof, 1.1f));
    fill_triangle(top[3], top[0], apex, shade(roof, 0.95f));
    fill_triangle(top[1], top[2], apex, shade(roof, 1.2f));
    fill_triangle(top[2], top[3], apex, shade(roof, 0.8f));

    if (damage > 0.5f && (h & 3) == 0) {  // smoke from some of the tiles of a badly hit house
        const float puff = 5.0f + 5.0f * damage;
        DrawCircleV({apex.x + 2.0f, apex.y - 6.0f}, puff, {70, 68, 66, 150});
        DrawCircleV({apex.x - 3.0f, apex.y - 12.0f}, puff * 0.8f, {90, 88, 86, 110});
    }
}

// Ground rectangle covering a structure's tiles, shrunk by `inset` tiles.
Rectangle footprint(const engine::Structure& s, float inset) {
    int x0 = s.tiles.front().x, x1 = x0, y0 = s.tiles.front().y, y1 = y0;
    for (const engine::TilePos& t : s.tiles) {
        x0 = std::min(x0, t.x);
        x1 = std::max(x1, t.x);
        y0 = std::min(y0, t.y);
        y1 = std::max(y1, t.y);
    }
    return {static_cast<float>(x0) + inset, static_cast<float>(y0) + inset,
            static_cast<float>(x1 - x0 + 1) - 2 * inset, static_cast<float>(y1 - y0 + 1) - 2 * inset};
}

// A player's headquarters: a two-storey block with a flat roof and a flag.
void draw_headquarters(const engine::TileMap& map, const engine::Structure& s) {
    constexpr float kWall = 24.0f;
    const Rectangle r = footprint(s, 0.1f);
    const Vector2 ground[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}};
    Vector2 base[4];
    Vector2 top[4];
    for (int i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, ground[i]);
        top[i] = {base[i].x, base[i].y - kWall};
    }
    const float damage = 1.0f - static_cast<float>(s.hp) / static_cast<float>(engine::structure_type(s.type).max_hp);
    const Color wall = shade({168, 164, 150, 255}, 1.0f - 0.4f * damage);
    fill_quad(base[1], base[2], top[2], top[1], wall);
    fill_quad(base[2], base[3], top[3], top[2], shade(wall, 0.72f));
    fill_quad(top[0], top[1], top[2], top[3], shade(wall, 0.85f));
    for (int i = 0; i < 4; ++i) DrawLineV(top[i], top[(i + 1) % 4], shade(wall, 0.5f));

    // Rows of windows on the two visible walls.
    for (int side = 1; side <= 2; ++side) {
        const Vector2 a = base[side];
        const Vector2 b = base[(side + 1) % 4];
        for (int i = 1; i <= 4; ++i) {
            const float t = static_cast<float>(i) / 5.0f;
            const Vector2 p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            DrawRectangleRec({p.x - 2.0f, p.y - kWall * 0.75f, 4.0f, 5.0f}, {60, 66, 72, 255});
            DrawRectangleRec({p.x - 2.0f, p.y - kWall * 0.4f, 4.0f, 5.0f}, {60, 66, 72, 255});
        }
    }

    const Vector2 pole = top[0];
    DrawLineEx(pole, {pole.x, pole.y - 22.0f}, 2.0f, {50, 50, 50, 255});
    DrawRectangleRec({pole.x, pole.y - 22.0f, 14.0f, 9.0f}, theme::player_color(s.owner));
}

// Grey boulders of a stone outcrop, fewer as it is quarried away.
void draw_rock(const engine::TileMap& map, int tx, int ty, int32_t left) {
    const uint32_t h = tile_hash(tx, ty);
    const int boulders = left > engine::kRockMaterials / 2 ? 3 : 2;
    for (int i = 0; i < boulders; ++i) {
        const float fx = 0.25f + static_cast<float>((h >> (i * 8)) & 0xFF) / 255.0f * 0.5f;
        const float fy = 0.25f + static_cast<float>((h >> (i * 8 + 4)) & 0xFF) / 255.0f * 0.5f;
        const Vector2 p = on_terrain(map, {static_cast<float>(tx) + fx, static_cast<float>(ty) + fy});
        const float size = 7.0f + static_cast<float>((h >> (i * 5)) & 7);
        DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y - size * 0.4f), size, size * 0.7f, {112, 114, 110, 255});
        DrawEllipse(static_cast<int>(p.x - size * 0.25f), static_cast<int>(p.y - size * 0.7f), size * 0.5f,
                    size * 0.3f, {150, 152, 148, 255});
    }
}

// Heaps of brick where a house stood.
void draw_ruins(const engine::TileMap& map, int tx, int ty) {
    const uint32_t h = tile_hash(tx, ty);
    for (int i = 0; i < 3; ++i) {
        const float fx = 0.25f + static_cast<float>((h >> (i * 8)) & 0xFF) / 255.0f * 0.5f;
        const float fy = 0.25f + static_cast<float>((h >> (i * 8 + 4)) & 0xFF) / 255.0f * 0.5f;
        const Vector2 p = on_terrain(map, {static_cast<float>(tx) + fx, static_cast<float>(ty) + fy});
        DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y - 2.0f), 9.0f, 4.5f, {92, 80, 70, 255});
        DrawEllipse(static_cast<int>(p.x - 1.0f), static_cast<int>(p.y - 4.0f), 5.0f, 2.5f, {130, 108, 92, 255});
    }
}

}  // namespace

// Garrison flags and health bars over houses and bridges.
void WorldRenderer::draw_structure_overlays(const engine::World& world, Rectangle view) const {
    const engine::TileMap& map = world.map();
    for (const engine::Structure& s : world.structures()) {
        const Vector2 c = on_terrain(map, to_vector2(s.center), 30.0f);
        if (!CheckCollisionPointRec(c, {view.x - 40, view.y - 40, view.width + 80, view.height + 80})) continue;

        const int32_t max_hp = engine::structure_type(s.type).max_hp;
        if (s.hp < max_hp) {
            const float frac = static_cast<float>(s.hp) / static_cast<float>(max_hp);
            DrawRectangleRec({c.x - 21, c.y - 1, 42, 5}, {0, 0, 0, 170});
            DrawRectangleRec({c.x - 20, c.y, 40 * frac, 3}, frac > 0.5f ? Color{200, 200, 190, 255} : Color{230, 110, 60, 255});
        }
        if (!s.garrison.empty()) {
            const char* count = TextFormat("%d", static_cast<int>(s.garrison.size()));
            const Color flag = theme::player_color(s.owner);
            DrawLineEx({c.x, c.y + 18}, {c.x, c.y - 10}, 1.5f, {40, 40, 40, 255});
            DrawRectangleRec({c.x, c.y - 10, 16, 11}, flag);
            DrawText(count, static_cast<int>(c.x + 4), static_cast<int>(c.y - 9), 10, WHITE);
        }
    }
}

void WorldRenderer::draw(const engine::World& world, const RtsCamera& camera, float alpha,
                         std::span<const engine::EntityId> selection, engine::EntityId selected_structure) const {
    const engine::TileMap& map = world.map();
    const Rectangle view = camera.visible_world_rect();
    auto is_selected = [&](engine::EntityId id) {
        return std::binary_search(selection.begin(), selection.end(), id);
    };

    BeginMode2D(camera.camera2d());

    draw_terrain(map, view);
    draw_remains(map);

    for (const engine::Unit& u : world.units()) {
        if (!is_selected(u.id) || u.inside) continue;
        const float radius = to_float(engine::unit_type(u.type).radius) + 0.1f;
        draw_ground_ellipse(on_terrain(map, unit_ground_pos(u, alpha)), radius, theme::kSelection);
        draw_orders(world, u, alpha);
    }
    draw_pings(map);

    if (const engine::Structure* s = world.find_structure(selected_structure)) {
        const Rectangle r = footprint(*s, -0.15f);
        const Vector2 corners[4] = {on_terrain(map, {r.x, r.y}), on_terrain(map, {r.x + r.width, r.y}),
                                    on_terrain(map, {r.x + r.width, r.y + r.height}),
                                    on_terrain(map, {r.x, r.y + r.height})};
        for (int i = 0; i < 4; ++i) DrawLineEx(corners[i], corners[(i + 1) % 4], 2.0f, theme::kSelection);
    }

    // Units, projectiles, trees and houses back to front, so nearer things
    // cover farther ones (a soldier in a forest hides among the trees).
    struct Drawable {
        float depth;
        const engine::Unit* unit = nullptr;
        const engine::Projectile* projectile = nullptr;
        Tree tree{};
        int house_x = -1;  // a house tile, or ruins / rock as flagged
        int house_y = -1;
        bool ruins = false;
        bool rock = false;
        float damage = 0.0f;  // 0 = intact, 1 = about to collapse
        const engine::Structure* building = nullptr;
    };
    std::vector<Drawable> drawables;
    drawables.reserve(world.units().size() + world.projectiles().size() + 1024);
    for (const engine::Unit& u : world.units()) {
        if (u.inside) continue;  // behind walls
        const Vector2 g = unit_ground_pos(u, alpha);
        const Vector2 p = on_terrain(map, g);
        if (!CheckCollisionPointRec(p, {view.x - 60, view.y - 60, view.width + 120, view.height + 120})) continue;
        drawables.push_back({.depth = g.x + g.y, .unit = &u});
    }
    for (const engine::Projectile& p : world.projectiles()) {
        const Vector2 g = lerp(to_vector2(p.prev_pos), to_vector2(p.pos), alpha);
        drawables.push_back({.depth = g.x + g.y, .projectile = &p});
    }
    for_each_visible_tile(map, view, [&](int tx, int ty) {
        switch (map.terrain(tx, ty)) {
            case engine::Terrain::Forest:
                for (const Tree& t : trees_on_tile(tx, ty)) {
                    drawables.push_back({.depth = t.ground.x + t.ground.y, .tree = t});
                }
                break;
            case engine::Terrain::House: {
                float damage = 0.0f;
                if (const engine::Structure* s = world.structure_at({tx, ty})) {
                    const int32_t max_hp = engine::structure_type(s->type).max_hp;
                    damage = 1.0f - static_cast<float>(s->hp) / static_cast<float>(max_hp);
                }
                drawables.push_back(
                    {.depth = static_cast<float>(tx + ty) + 1.0f, .house_x = tx, .house_y = ty, .damage = damage});
                break;
            }
            case engine::Terrain::Ruins:
                drawables.push_back(
                    {.depth = static_cast<float>(tx + ty) + 1.0f, .house_x = tx, .house_y = ty, .ruins = true});
                break;
            case engine::Terrain::Rock:
                drawables.push_back(
                    {.depth = static_cast<float>(tx + ty) + 1.0f, .house_x = tx, .house_y = ty, .rock = true});
                break;
            default:
                break;
        }
    });
    for (const engine::Structure& s : world.structures()) {
        if (s.type != engine::StructureType::Headquarters) continue;
        const Vector2 c = to_vector2(s.center);
        if (!CheckCollisionPointRec(on_terrain(map, c), {view.x - 150, view.y - 150, view.width + 300, view.height + 300})) {
            continue;
        }
        drawables.push_back({.depth = c.x + c.y + 1.0f, .building = &s});
    }
    std::stable_sort(drawables.begin(), drawables.end(),
                     [](const Drawable& a, const Drawable& b) { return a.depth < b.depth; });
    for (const Drawable& d : drawables) {
        if (d.unit) {
            draw_unit(map, *d.unit, alpha);
        } else if (d.projectile) {
            draw_projectile(*d.projectile, alpha);
        } else if (d.building) {
            draw_headquarters(map, *d.building);
        } else if (d.ruins) {
            draw_ruins(map, d.house_x, d.house_y);
        } else if (d.rock) {
            draw_rock(map, d.house_x, d.house_y, map.resource({d.house_x, d.house_y}));
        } else if (d.house_x >= 0) {
            draw_house(map, d.house_x, d.house_y, d.damage);
        } else {
            draw_tree(map, d.tree);
        }
    }

    draw_shots(world, alpha);
    draw_blasts(map);
    draw_structure_overlays(world, view);
    for (const Drawable& d : drawables) {
        if (d.unit && !d.unit->inside &&
            (is_selected(d.unit->id) || d.unit->hp < engine::unit_type(d.unit->type).max_hp)) {
            draw_health_bar(map, *d.unit, alpha);
        }
    }

    EndMode2D();
}

void WorldRenderer::draw_terrain(const engine::TileMap& map, Rectangle view) const {
    if (map.width() != cache_width_ || map.height() != cache_height_) return;  // update() hasn't seen this map yet

    for_each_visible_tile(map, view, [&](int tx, int ty) {
        const float h00 = corner(tx, ty);
        const float h10 = corner(tx + 1, ty);
        const float h01 = corner(tx, ty + 1);
        const float h11 = corner(tx + 1, ty + 1);
        const auto fx = static_cast<float>(tx);
        const auto fy = static_cast<float>(ty);
        const Vector2 top = iso::project({fx, fy}, h00);
        const Vector2 right = iso::project({fx + 1, fy}, h10);
        const Vector2 bottom = iso::project({fx + 1, fy + 1}, h11);
        const Vector2 left = iso::project({fx, fy + 1}, h01);

        const float min_y = std::min({top.y, right.y, bottom.y, left.y});
        const float max_y = std::max({top.y, right.y, bottom.y, left.y});
        if (right.x < view.x || left.x > view.x + view.width || max_y < view.y || min_y > view.y + view.height) {
            return;
        }

        // Light comes from the upper left of the screen: slopes rising
        // towards +x face it, slopes rising towards +y turn away.
        const float slope_x = (h10 + h11 - h00 - h01) * 0.5f;
        const float slope_y = (h01 + h11 - h00 - h10) * 0.5f;
        const float noise = static_cast<float>(tile_hash(tx, ty) & 0xFF) / 255.0f - 0.5f;
        const float height = (h00 + h10 + h01 + h11) * 0.25f;
        const float light = 1.0f + noise * 0.07f + slope_x * 0.30f - slope_y * 0.18f + height * 0.05f;
        fill_quad(top, right, bottom, left, shade(theme::terrain_color(map.terrain(tx, ty)), light));
    });
}

void WorldRenderer::draw_remains(const engine::TileMap& map) const {
    for (const Remains& r : remains_) {
        const float lifetime = r.vehicle ? kWreckLifetime : kBodyLifetime;
        const float fade = std::clamp((lifetime - r.age) / 3.0f, 0.0f, 1.0f);  // fade out over the last 3 s
        const Vector2 p = on_terrain(map, r.ground);
        if (r.vehicle) {
            fill_ground_ellipse(p, 0.5f, ColorAlpha({30, 28, 26, 255}, 0.85f * fade));
            fill_ground_ellipse({p.x - 3, p.y - 4}, 0.3f, ColorAlpha({55, 50, 45, 255}, 0.9f * fade));
        } else {
            fill_ground_ellipse(p, 0.15f, ColorAlpha({70, 40, 35, 255}, 0.7f * fade));
        }
    }
}

void WorldRenderer::draw_pings(const engine::TileMap& map) const {
    for (const Ping& p : pings_) {
        const float t = p.age / kPingLifetime;
        const Color color = p.attack ? theme::kDanger : theme::kMovePing;
        draw_ground_ellipse(on_terrain(map, p.ground), 0.45f * (1.0f - t) + 0.1f, ColorAlpha(color, 1.0f - t));
    }
}

void WorldRenderer::draw_orders(const engine::World& world, const engine::Unit& u, float alpha) const {
    const engine::TileMap& map = world.map();
    const Vector2 from = on_terrain(map, unit_ground_pos(u, alpha));
    switch (u.order) {
        case engine::Order::Move:
        case engine::Order::AttackMove: {
            const Color color = u.order == engine::Order::Move ? theme::kSelection : theme::kDanger;
            const Vector2 to = on_terrain(map, to_vector2(u.order_point));
            DrawLineV(from, to, ColorAlpha(color, 0.3f));
            DrawCircleV(to, 2.0f, ColorAlpha(color, 0.7f));
            break;
        }
        case engine::Order::Attack:
            if (const engine::Unit* target = world.find_unit(u.order_target)) {
                DrawLineV(from, on_terrain(map, unit_ground_pos(*target, alpha)), ColorAlpha(theme::kDanger, 0.4f));
            }
            break;
        case engine::Order::AttackGround: {
            const Vector2 to = on_terrain(map, to_vector2(u.order_point));
            DrawLineV(from, to, ColorAlpha(theme::kWarning, 0.35f));
            draw_ground_ellipse(to, 0.35f, ColorAlpha(theme::kWarning, 0.8f));
            DrawLineV({to.x - 6, to.y}, {to.x + 6, to.y}, theme::kWarning);
            DrawLineV({to.x, to.y - 3}, {to.x, to.y + 3}, theme::kWarning);
            break;
        }
        case engine::Order::Idle:
            break;
    }
}

void WorldRenderer::draw_unit(const engine::TileMap& map, const engine::Unit& u, float alpha) const {
    const engine::UnitTypeDef& def = engine::unit_type(u.type);
    const Vector2 ground = unit_ground_pos(u, alpha);
    const Vector2 feet = on_terrain(map, ground);
    const Vector2 facing = unit_facing(u);

    fill_ground_ellipse(feet, to_float(def.radius) * 0.9f, {0, 0, 0, 70});  // shadow
    if (def.vehicle) {
        draw_vehicle(map, u, ground, facing);
    } else {
        draw_soldier(u, feet, facing);
    }
}

void WorldRenderer::draw_soldier(const engine::Unit& u, Vector2 feet, Vector2 facing) const {
    const Color color = theme::player_color(u.owner);
    const Color dark = shade(color, 0.55f);

    DrawRectangleRounded({feet.x - 3.0f, feet.y - 13.0f, 6.0f, 12.0f}, 0.6f, 4, color);
    DrawCircleV({feet.x, feet.y - 15.5f}, 3.0f, shade(color, 1.25f));

    // The weapon, pointing where the soldier faces; its shape tells the type.
    const Vector2 hands{feet.x, feet.y - 9.0f};
    float length = 0.22f;
    float thickness = 1.5f;
    Color weapon = {35, 35, 35, 255};
    switch (u.type) {
        case engine::UnitTypeId::MachineGunner:
            length = 0.28f;
            thickness = 2.5f;
            break;
        case engine::UnitTypeId::Grenadier:
            length = 0.27f;
            thickness = 3.0f;
            weapon = {85, 95, 60, 255};
            break;
        case engine::UnitTypeId::Worker:
            length = 0.15f;
            thickness = 1.2f;
            break;
        default:
            break;
    }
    const Vector2 offset = iso_offset({facing.x * length, facing.y * length});
    DrawLineEx(hands, {hands.x + offset.x, hands.y + offset.y}, thickness, weapon);
    if (u.carrying > 0) {
        // A bundle of timber or stone on the back.
        const Vector2 back = iso_offset({-facing.x * 0.12f, -facing.y * 0.12f});
        DrawRectangleRec({hands.x + back.x - 3.5f, hands.y + back.y - 5.0f, 7.0f, 5.0f}, {120, 88, 52, 255});
    }
    DrawRectangleRoundedLines({feet.x - 3.0f, feet.y - 13.0f, 6.0f, 12.0f}, 0.6f, 4, dark);
}

void WorldRenderer::draw_vehicle(const engine::TileMap& map, const engine::Unit& u, Vector2 ground,
                                 Vector2 facing) const {
    const bool tank = u.type == engine::UnitTypeId::Tank;
    const float length = tank ? 0.95f : 0.85f;
    const float width = tank ? 0.58f : 0.5f;
    const float hull = tank ? 6.0f : 8.0f;  // hull height, pixels

    const Vector2 fwd{facing.x * length * 0.5f, facing.y * length * 0.5f};
    const Vector2 side{-facing.y * width * 0.5f, facing.x * width * 0.5f};
    const Vector2 corners_ground[4] = {
        {ground.x + fwd.x + side.x, ground.y + fwd.y + side.y},
        {ground.x + fwd.x - side.x, ground.y + fwd.y - side.y},
        {ground.x - fwd.x - side.x, ground.y - fwd.y - side.y},
        {ground.x - fwd.x + side.x, ground.y - fwd.y + side.y},
    };
    Vector2 base[4];
    Vector2 top[4];
    for (int i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, corners_ground[i]);
        top[i] = {base[i].x, base[i].y - hull};
    }

    const Color color = shade(theme::player_color(u.owner), 0.85f);
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        fill_quad(base[i], base[j], top[j], top[i], shade(color, 0.55f));
    }
    fill_quad(top[0], top[1], top[2], top[3], color);
    for (int i = 0; i < 4; ++i) DrawLineV(top[i], top[(i + 1) % 4], shade(color, 0.45f));

    // Turret and gun.
    const Vector2 center = on_terrain(map, ground, hull + 2.0f);
    const float barrel = tank ? 0.75f : 0.5f;
    const Vector2 muzzle = iso_offset({facing.x * barrel, facing.y * barrel});
    DrawLineEx(center, {center.x + muzzle.x, center.y + muzzle.y}, tank ? 3.0f : 2.0f, shade(color, 0.4f));
    DrawCircleV(center, tank ? 6.0f : 4.0f, shade(color, 1.15f));
    DrawCircleLinesV(center, tank ? 6.0f : 4.0f, shade(color, 0.45f));
}

void WorldRenderer::draw_projectile(const engine::Projectile& p, float alpha) const {
    // Drawn at its real flight height, so a shot from a hill visibly passes
    // over the trees and heads below.
    const Vector2 ground = lerp(to_vector2(p.prev_pos), to_vector2(p.pos), alpha);
    const Vector2 origin = to_vector2(p.origin);
    const Vector2 target = to_vector2(p.target);
    const float total = std::hypot(target.x - origin.x, target.y - origin.y);
    const float t = total > 0.0f ? std::hypot(ground.x - origin.x, ground.y - origin.y) / total : 1.0f;
    const float height = to_float(p.origin_height) + (to_float(p.target_height) - to_float(p.origin_height)) * t;
    const Vector2 pos = iso::project(ground, height);

    Vector2 dir = iso_offset(to_vector2(p.target - p.origin));
    const float len = std::hypot(dir.x, dir.y);
    dir = len > 0.0f ? Vector2{dir.x / len, dir.y / len} : Vector2{1.0f, 0.0f};

    switch (engine::unit_type(p.shooter_type).weapon.damage_type) {
        case engine::DamageType::AntiTank:  // rocket with a smoke trail
            DrawLineEx({pos.x - dir.x * 14.0f, pos.y - dir.y * 14.0f}, pos, 2.5f, {180, 180, 170, 150});
            DrawCircleV(pos, 2.5f, {255, 150, 40, 255});
            break;
        case engine::DamageType::Explosive:  // tank shell
            DrawLineEx({pos.x - dir.x * 6.0f, pos.y - dir.y * 6.0f}, pos, 2.0f, {255, 240, 170, 200});
            DrawCircleV(pos, 2.0f, {255, 250, 210, 255});
            break;
        case engine::DamageType::Bullet:  // autocannon tracer
        default:
            DrawLineEx({pos.x - dir.x * 8.0f, pos.y - dir.y * 8.0f}, pos, 1.5f, {255, 220, 120, 230});
            break;
    }
}

void WorldRenderer::draw_shots(const engine::World& world, float alpha) const {
    const engine::TileMap& map = world.map();
    for (const engine::Unit& u : world.units()) {
        if (u.last_shot_tick == engine::kNeverFired || world.tick() - u.last_shot_tick > 2) continue;
        const engine::UnitTypeDef& def = engine::unit_type(u.type);
        const Vector2 ground = unit_ground_pos(u, alpha);
        const Vector2 facing = unit_facing(u);
        const float reach = def.vehicle ? (u.type == engine::UnitTypeId::Tank ? 0.75f : 0.5f) : 0.25f;
        const Vector2 muzzle = on_terrain(map, {ground.x + facing.x * reach, ground.y + facing.y * reach},
                                          def.vehicle ? 8.0f : 9.0f);

        DrawCircleV(muzzle, def.vehicle ? 4.0f : 2.0f, {255, 230, 140, 220});
        if (def.weapon.projectile_speed.raw == 0) {  // instant hit: draw the tracer
            DrawLineV(muzzle, on_terrain(map, to_vector2(u.last_shot_at), 6.0f), {255, 235, 160, 140});
        }
    }
}

void WorldRenderer::draw_blasts(const engine::TileMap& map) const {
    for (const Blast& b : blasts_) {
        const float t = b.age / kBlastLifetime;
        const Vector2 p = on_terrain(map, b.ground);
        const float radius = b.radius * (0.5f + t);
        fill_ground_ellipse(p, radius, ColorAlpha({255, 170, 60, 255}, 0.55f * (1.0f - t)));
        fill_ground_ellipse({p.x, p.y - 3.0f}, radius * 0.5f, ColorAlpha({255, 240, 190, 255}, 0.8f * (1.0f - t)));
        draw_ground_ellipse(p, radius * 1.1f, ColorAlpha({90, 80, 70, 255}, 0.6f * (1.0f - t)));
    }
}

void WorldRenderer::draw_health_bar(const engine::TileMap& map, const engine::Unit& u, float alpha) const {
    const engine::UnitTypeDef& def = engine::unit_type(u.type);
    const Vector2 feet = on_terrain(map, unit_ground_pos(u, alpha));
    const float width = def.vehicle ? 30.0f : 18.0f;
    const float y = feet.y - (def.vehicle ? 26.0f : 24.0f);
    const float frac = std::clamp(static_cast<float>(u.hp) / static_cast<float>(def.max_hp), 0.0f, 1.0f);

    const Color fill = frac > 0.6f ? Color{90, 210, 90, 255} : frac > 0.3f ? Color{230, 200, 60, 255}
                                                                             : Color{230, 70, 60, 255};
    DrawRectangleRec({feet.x - width * 0.5f - 1, y - 1, width + 2, 5}, {0, 0, 0, 170});
    DrawRectangleRec({feet.x - width * 0.5f, y, width * frac, 3}, fill);
}

}  // namespace render
