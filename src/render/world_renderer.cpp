#include "render/world_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
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

// Brightness of what is being drawn: 1 in view, lower for remembered
// scenery under the fog of war. Set around each drawable, back to 1 after.
float g_light = 1.0f;
constexpr float kFogLight = 0.5f;

// How high a unit's body is above its feet, pixels.
float body_lift(const engine::Unit& u) { return engine::unit_type(u.type).vehicle ? 6.0f : 9.0f; }

Color shade(Color c, float k) {
    auto channel = [k](unsigned char v) {
        return static_cast<unsigned char>(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f));
    };
    return {channel(c.r), channel(c.g), channel(c.b), c.a};
}

Color lit(Color c) { return g_light >= 1.0f ? c : shade(c, g_light); }

// raylib skips triangles with the "wrong" winding; accept either.
void fill_triangle(Vector2 a, Vector2 b, Vector2 c, Color color) {
    const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross > 0) std::swap(b, c);
    DrawTriangle(a, b, c, lit(color));
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
                     radius_tiles * kCircleRy, lit(color));
}

void fill_ground_ellipse(Vector2 center, float radius_tiles, Color color) {
    DrawEllipse(static_cast<int>(center.x), static_cast<int>(center.y), radius_tiles * kCircleRx,
                radius_tiles * kCircleRy, lit(color));
}

// A box standing on the terrain, `lift` pixels up, turned along `facing`:
// hulls, truck cabs, railway cars. Returns the top face's corners.
std::array<Vector2, 4> draw_box(const engine::TileMap& map, Vector2 ground, Vector2 facing, float length, float width,
                                float height, Color color, float lift = 0.0f) {
    const Vector2 fwd{facing.x * length * 0.5f, facing.y * length * 0.5f};
    const Vector2 side{-facing.y * width * 0.5f, facing.x * width * 0.5f};
    const Vector2 corners_ground[4] = {
        {ground.x + fwd.x + side.x, ground.y + fwd.y + side.y},
        {ground.x + fwd.x - side.x, ground.y + fwd.y - side.y},
        {ground.x - fwd.x - side.x, ground.y - fwd.y - side.y},
        {ground.x - fwd.x + side.x, ground.y - fwd.y + side.y},
    };
    std::array<Vector2, 4> base;
    std::array<Vector2, 4> top;
    for (size_t i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, corners_ground[i], lift);
        top[i] = {base[i].x, base[i].y - height};
    }
    for (size_t i = 0; i < 4; ++i) {
        const size_t j = (i + 1) % 4;
        fill_quad(base[i], base[j], top[j], top[i], shade(color, 0.55f));
    }
    fill_quad(top[0], top[1], top[2], top[3], color);
    for (size_t i = 0; i < 4; ++i) DrawLineV(top[i], top[(i + 1) % 4], lit(shade(color, 0.45f)));
    return top;
}

// What the freight looks like: food under tarpaulin, ammunition crates, fuel.
Color cargo_color(engine::Resource r) {
    switch (r) {
        case engine::Resource::Food: return {150, 138, 100, 255};
        case engine::Resource::Ammo: return {98, 106, 70, 255};
        case engine::Resource::Fuel: return {170, 172, 168, 255};
        default: return {120, 88, 52, 255};
    }
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

void WorldRenderer::set_viewer(engine::PlayerId viewer, bool reveal) {
    viewer_ = viewer;
    reveal_ = reveal;
}

int WorldRenderer::fog(const engine::World& world, int tx, int ty) const {
    if (reveal_) return kInView;
    if (world.visible(viewer_, {tx, ty})) return kInView;
    return world.explored(viewer_, {tx, ty}) ? kRemembered : kUnexplored;
}

bool WorldRenderer::in_view(const engine::World& world, Vector2 ground) const {
    return fog(world, static_cast<int>(std::floor(ground.x)), static_cast<int>(std::floor(ground.y))) == kInView;
}

bool WorldRenderer::shows(const engine::World& world, const engine::Unit& u) const {
    return reveal_ || world.sees(viewer_, u);
}

// What the fog hides is drawn as it was last seen: the ground (a forest
// since cut down, a house since collapsed) and other players' buildings.
void WorldRenderer::remember(const engine::World& world) {
    const engine::TileMap& map = world.map();
    const size_t tiles = static_cast<size_t>(map.width() * map.height());
    const bool fresh = seen_terrain_.size() != tiles;
    if (!fresh && world.vision_revision() == remembered_revision_ && !reveal_) return;
    remembered_revision_ = world.vision_revision();
    if (fresh) seen_terrain_.assign(tiles, engine::Terrain::Grass);
    for (int ty = 0; ty < map.height(); ++ty) {
        for (int tx = 0; tx < map.width(); ++tx) {
            if (fresh || fog(world, tx, ty) == kInView) {
                seen_terrain_[static_cast<size_t>(ty * map.width() + tx)] = map.terrain(tx, ty);
            }
        }
    }
    for (const engine::Structure& s : world.structures()) {
        if (s.owner == viewer_ || s.type == engine::StructureType::House || s.type == engine::StructureType::Bridge) continue;
        if ((engine::is_fieldwork(s.type) || engine::is_obstacle(s.type)) && !s.parapet) continue;  // the ground shows it
        if (reveal_ || world.sees(viewer_, s)) remembered_[s.id] = s;
    }
    // Gone, and we have seen the empty spot: forget it.
    std::erase_if(remembered_, [&](const auto& entry) {
        if (world.find_structure(entry.first)) return false;
        return std::any_of(entry.second.tiles.begin(), entry.second.tiles.end(), [&](const engine::TilePos& t) {
            return fog(world, t.x, t.y) == kInView;
        });
    });
}

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

    remember(world);

    // Shells and rockets that went off since the last frame: where they
    // actually burst, which may be a tree or a soldier in the way.
    for (const engine::Impact& impact : world.recent_impacts()) {
        if (impact.tick < impacts_seen_until_ || !in_view(world, to_vector2(impact.pos))) continue;
        const float splash = to_float(impact.splash);
        blasts_.push_back({to_vector2(impact.pos), 0.0f, std::max(splash, 0.2f)});
    }
    impacts_seen_until_ = world.tick();

    // Units that vanished died (a garrison buried in its house leaves no body
    // to see, and nobody sees who dies in the fog).
    std::unordered_map<engine::EntityId, Remains> alive;
    for (const engine::Unit& u : world.units()) {
        if (!u.inside && shows(world, u)) alive[u.id] = {to_vector2(u.pos), 0.0f, engine::unit_type(u.type).vehicle};
    }
    for (const auto& [id, last] : units_seen_) {
        if (alive.contains(id) || world.find_unit(id)) continue;
        remains_.push_back(last);
        if (last.vehicle) blasts_.push_back({last.ground, 0.0f, 0.7f});
    }
    units_seen_ = std::move(alive);

    // Structures that vanished came down: a cloud of dust, or a fuel fire.
    std::unordered_map<engine::EntityId, StructureSeen> standing;
    for (const engine::Structure& s : world.structures()) standing[s.id] = {to_vector2(s.center), s.type};
    for (const auto& [id, seen] : structures_seen_) {
        if (standing.contains(id) || !in_view(world, seen.center)) continue;
        const bool fuel = seen.type == engine::StructureType::FuelDepot;
        blasts_.push_back({seen.center, 0.0f, fuel ? 3.0f : 1.6f});
        if (fuel) blasts_.push_back({seen.center, -0.3f, 2.2f});  // a second, later burst
    }
    structures_seen_ = std::move(standing);

    // The track only changes with the map.
    if (map.revision() != routes_revision_ || rail_routes_.empty()) {
        routes_revision_ = map.revision();
        rail_routes_.clear();
        for (const engine::Structure& s : world.structures()) {
            if (s.type == engine::StructureType::Station) rail_routes_.emplace_back(s.id, rail_route(world, s));
        }
    }

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
    DrawLineEx(base, {base.x, base.y - 7.0f * s}, 2.0f, lit({72, 54, 38, 255}));
    DrawCircleV({base.x, base.y - 13.0f * s}, 8.5f * s, lit(ColorAlpha(shade(leaf, 0.8f), 0.95f)));
    DrawCircleV({base.x - 2.0f * s, base.y - 15.0f * s}, 5.5f * s, lit(ColorAlpha(shade(leaf, 1.15f), 0.9f)));
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
        DrawCircleV({apex.x + 2.0f, apex.y - 6.0f}, puff, lit({70, 68, 66, 150}));
        DrawCircleV({apex.x - 3.0f, apex.y - 12.0f}, puff * 0.8f, lit({90, 88, 86, 110}));
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

// How each kind of player building looks (placeholders until sprites).
struct BuildingStyle {
    float wall;  // height, pixels
    Color walls;
    Color roof;
    int window_rows;
    bool big_doors = false;  // a tank hangar, a warehouse
    bool tanks = false;      // fuel tanks on the pad
};

BuildingStyle style_of(engine::StructureType type) {
    switch (type) {
        case engine::StructureType::InfantryBarracks: return {16.0f, {150, 146, 118, 255}, {92, 104, 76, 255}, 1};
        case engine::StructureType::ArmorBarracks: return {22.0f, {128, 132, 124, 255}, {86, 92, 84, 255}, 0, true};
        case engine::StructureType::Warehouse: return {13.0f, {142, 112, 80, 255}, {110, 84, 60, 255}, 0, true};
        case engine::StructureType::Station: return {15.0f, {156, 98, 78, 255}, {98, 76, 66, 255}, 1};
        case engine::StructureType::ReconBarracks: return {12.0f, {118, 126, 96, 255}, {80, 94, 66, 255}, 1};
        // Half dug in, under earth: only a low front shows.
        case engine::StructureType::AmmoDepot: return {8.0f, {112, 118, 90, 255}, {96, 104, 72, 255}, 0, true};
        case engine::StructureType::FuelDepot: return {5.0f, {128, 126, 118, 255}, {110, 108, 100, 255}, 0, false, true};
        default: return {24.0f, {168, 164, 150, 255}, {142, 140, 128, 255}, 2};  // headquarters
    }
}

// A player's building: a block with a flat roof and a flag. While under
// construction it rises with the work done, inside a frame of scaffolding.
// A pillbox: a low mound of logs and earth, a dark slit on the side it faces.
void draw_pillbox(const engine::TileMap& map, const engine::Structure& s) {
    const Vector2 c{static_cast<float>(s.tiles.front().x) + 0.5f, static_cast<float>(s.tiles.front().y) + 0.5f};
    const float done = s.built ? 1.0f : 0.4f;
    fill_ground_ellipse(on_terrain(map, c), 0.5f, {110, 94, 70, 255});
    fill_ground_ellipse(on_terrain(map, c, 5.0f * done), 0.4f, {96, 84, 62, 255});
    fill_ground_ellipse(on_terrain(map, c, 8.0f * done), 0.28f, {84, 74, 56, 255});
    if (!s.built) return;
    Vector2 f = to_vector2(s.facing);
    const float len = std::hypot(f.x, f.y);
    f = len > 0.0f ? Vector2{f.x / len, f.y / len} : Vector2{1.0f, 0.0f};
    const Vector2 mid{c.x + f.x * 0.36f, c.y + f.y * 0.36f};
    const Vector2 a{mid.x - f.y * 0.18f, mid.y + f.x * 0.18f};
    const Vector2 b{mid.x + f.y * 0.18f, mid.y - f.x * 0.18f};
    DrawLineEx(on_terrain(map, a, 4.0f), on_terrain(map, b, 4.0f), 2.5f, lit({24, 22, 20, 255}));
}

void draw_building(const engine::TileMap& map, const engine::Structure& s) {
    if (s.type == engine::StructureType::Pillbox) return draw_pillbox(map, s);
    const engine::StructureDef& def = engine::structure_type(s.type);
    const BuildingStyle style = style_of(s.type);
    const float done = s.built ? 1.0f
                               : std::max(0.08f, static_cast<float>(s.build_progress) / static_cast<float>(def.build_time));
    const float wall = style.wall * done;

    const Rectangle r = footprint(s, 0.1f);
    const Vector2 ground[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}};
    Vector2 base[4];
    Vector2 top[4];
    for (int i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, ground[i]);
        top[i] = {base[i].x, base[i].y - wall};
    }
    const float damage = 1.0f - static_cast<float>(s.hp) / static_cast<float>(def.max_hp);
    const float soot = s.built ? 1.0f - 0.4f * damage : 1.0f;
    const Color walls = shade(style.walls, soot);
    fill_quad(base[1], base[2], top[2], top[1], walls);
    fill_quad(base[2], base[3], top[3], top[2], shade(walls, 0.72f));
    fill_quad(top[0], top[1], top[2], top[3], shade(style.roof, soot));
    for (int i = 0; i < 4; ++i) DrawLineV(top[i], top[(i + 1) % 4], lit(shade(walls, 0.5f)));

    if (!s.built) {
        // Scaffolding up to the full height.
        const Color pole{120, 96, 64, 255};
        for (int i = 0; i < 4; ++i) {
            const Vector2 full{base[i].x, base[i].y - style.wall};
            DrawLineEx(base[i], full, 1.5f, lit(pole));
            DrawLineEx(full, {base[(i + 1) % 4].x, base[(i + 1) % 4].y - style.wall}, 1.0f, lit(pole));
        }
        return;
    }

    for (int side = 1; side <= 2; ++side) {
        const Vector2 a = base[side];
        const Vector2 b = base[(side + 1) % 4];
        // Rows of windows...
        for (int row = 0; row < style.window_rows; ++row) {
            for (int i = 1; i <= 4; ++i) {
                const float t = static_cast<float>(i) / 5.0f;
                const Vector2 p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                const float lift = style.wall * (row == 0 ? 0.4f : 0.75f);
                DrawRectangleRec({p.x - 2.0f, p.y - lift, 4.0f, 5.0f}, lit({60, 66, 72, 255}));
            }
        }
        // ...or wide gates.
        if (style.big_doors && side == 1) {
            const Vector2 d0{a.x + (b.x - a.x) * 0.3f, a.y + (b.y - a.y) * 0.3f};
            const Vector2 d1{a.x + (b.x - a.x) * 0.7f, a.y + (b.y - a.y) * 0.7f};
            fill_quad(d0, d1, {d1.x, d1.y - style.wall * 0.7f}, {d0.x, d0.y - style.wall * 0.7f}, {48, 50, 48, 255});
        }
    }

    if (style.tanks) {
        // Two upright tanks on the pad.
        for (const float t : {0.3f, 0.7f}) {
            const Vector2 g{r.x + r.width * t, r.y + r.height * (1.0f - t)};
            const Vector2 bottom = on_terrain(map, g, style.wall);
            const Vector2 cap{bottom.x, bottom.y - 12.0f};
            const float rx = 0.32f * kCircleRx;
            const Color steel = shade({176, 178, 172, 255}, soot);
            fill_ground_ellipse(bottom, 0.32f, shade(steel, 0.6f));
            DrawRectangleRec({bottom.x - rx, cap.y, 2.0f * rx, bottom.y - cap.y}, lit(shade(steel, 0.8f)));
            fill_ground_ellipse(cap, 0.32f, steel);
            draw_ground_ellipse(cap, 0.32f, shade(steel, 0.5f));
        }
    }

    const Vector2 pole = top[0];
    DrawLineEx(pole, {pole.x, pole.y - 22.0f}, 2.0f, lit({50, 50, 50, 255}));
    DrawRectangleRec({pole.x, pole.y - 22.0f, 14.0f, 9.0f}, lit(theme::player_color(s.owner)));
}

// The foundation that follows the cursor while placing a building.
void draw_ghost(const engine::TileMap& map, const BuildGhost& ghost) {
    const engine::StructureDef& def = engine::structure_type(ghost.type);
    const auto x = static_cast<float>(ghost.origin.x);
    const auto y = static_cast<float>(ghost.origin.y);
    const auto w = static_cast<float>(def.width);
    const auto h = static_cast<float>(def.height);
    const Vector2 c[4] = {on_terrain(map, {x, y}), on_terrain(map, {x + w, y}), on_terrain(map, {x + w, y + h}),
                          on_terrain(map, {x, y + h})};
    const Color color = ghost.valid ? theme::kSelection : theme::kDanger;
    fill_quad(c[0], c[1], c[2], c[3], ColorAlpha(color, 0.3f));
    for (int i = 0; i < 4; ++i) DrawLineEx(c[i], c[(i + 1) % 4], 2.0f, color);
}

// Sleepers and two rails across a railway tile, along x or along y.
void draw_rail(const engine::TileMap& map, int tx, int ty, bool along_x) {
    const auto x = static_cast<float>(tx);
    const auto y = static_cast<float>(ty);
    // (u, v) inside the tile: u along the track, v across it.
    auto at = [&](float u, float v) { return on_terrain(map, along_x ? Vector2{x + u, y + v} : Vector2{x + v, y + u}); };
    for (int i = 0; i < 4; ++i) {
        const float u = 0.125f + 0.25f * static_cast<float>(i);
        DrawLineEx(at(u, 0.26f), at(u, 0.74f), 2.0f, lit({84, 64, 46, 255}));
    }
    for (const float v : {0.38f, 0.62f}) DrawLineEx(at(0.0f, v), at(1.0f, v), 1.2f, lit({178, 180, 184, 255}));
}

// Field works on a tile: a trench is a dark ditch joining its neighbours,
// a foxhole a pit with a rim of spoil, a dugout a mound roofed with logs.
template <typename Linked>
void draw_works(const engine::TileMap& map, int tx, int ty, engine::Terrain terrain, Linked linked) {
    const Vector2 c{static_cast<float>(tx) + 0.5f, static_cast<float>(ty) + 0.5f};
    const Color ditch = lit({52, 44, 34, 255});
    const Color spoil = lit({138, 116, 84, 255});
    switch (terrain) {
        case engine::Terrain::Trench: {
            constexpr int kSteps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            bool joined = false;
            for (const auto& d : kSteps) {
                if (!linked(tx + d[0], ty + d[1])) continue;
                joined = true;
                const Vector2 edge{c.x + 0.5f * static_cast<float>(d[0]), c.y + 0.5f * static_cast<float>(d[1])};
                DrawLineEx(on_terrain(map, c), on_terrain(map, edge), 7.0f, spoil);
                DrawLineEx(on_terrain(map, c), on_terrain(map, edge), 4.0f, ditch);
            }
            fill_ground_ellipse(on_terrain(map, c), joined ? 0.12f : 0.25f, {52, 44, 34, 255});
            break;
        }
        case engine::Terrain::Foxhole:
            fill_ground_ellipse(on_terrain(map, c), 0.34f, {138, 116, 84, 255});
            fill_ground_ellipse(on_terrain(map, c), 0.22f, {52, 44, 34, 255});
            break;
        case engine::Terrain::Wire: {
            // Stakes and a zigzag of wire between them.
            const Color wire = lit({150, 150, 146, 255});
            Vector2 prev = on_terrain(map, {c.x - 0.45f, c.y - 0.45f}, 3.0f);
            for (int i = 1; i <= 6; ++i) {
                const float k = -0.45f + 0.9f * static_cast<float>(i) / 6.0f;
                const Vector2 p = on_terrain(map, {c.x + k, c.y + k}, (i % 2) ? 6.0f : 2.0f);
                DrawLineV(prev, p, wire);
                prev = p;
            }
            for (const float k : {-0.35f, 0.0f, 0.35f}) {
                const Vector2 foot = on_terrain(map, {c.x + k, c.y + k});
                DrawLineEx(foot, {foot.x, foot.y - 7.0f}, 1.5f, lit({92, 72, 50, 255}));
            }
            break;
        }
        case engine::Terrain::Hedgehogs: {
            // Two steel crosses.
            for (const float k : {-0.22f, 0.22f}) {
                const Vector2 foot = on_terrain(map, {c.x + k, c.y - k});
                const Color steel = lit({70, 72, 70, 255});
                DrawLineEx({foot.x - 5.0f, foot.y}, {foot.x + 5.0f, foot.y - 9.0f}, 2.0f, steel);
                DrawLineEx({foot.x + 5.0f, foot.y}, {foot.x - 5.0f, foot.y - 9.0f}, 2.0f, steel);
                DrawLineEx({foot.x, foot.y + 1.0f}, {foot.x, foot.y - 10.0f}, 2.0f, steel);
            }
            break;
        }
        case engine::Terrain::GunPit: {
            // A wide pit with a horseshoe of spoil around it.
            fill_ground_ellipse(on_terrain(map, c), 0.46f, {138, 116, 84, 255});
            fill_ground_ellipse(on_terrain(map, c), 0.34f, {70, 60, 46, 255});
            break;
        }
        case engine::Terrain::Dugout: {
            const Vector2 top = on_terrain(map, c, 4.0f);
            fill_ground_ellipse(on_terrain(map, c), 0.46f, {118, 100, 74, 255});
            fill_ground_ellipse(top, 0.36f, {104, 88, 64, 255});
            for (const float k : {-0.2f, 0.0f, 0.2f}) {  // logs of the roof
                DrawLineEx(on_terrain(map, {c.x - 0.25f, c.y + k}, 5.0f), on_terrain(map, {c.x + 0.25f, c.y + k}, 5.0f),
                           2.0f, lit({92, 70, 48, 255}));
            }
            const Vector2 door = on_terrain(map, {c.x + 0.3f, c.y + 0.3f});
            DrawRectangleRec({door.x - 3.0f, door.y - 4.0f, 6.0f, 4.0f}, lit({30, 26, 22, 255}));
            break;
        }
        default:
            break;
    }
}

// A parapet: a bank of earth across the tile on its front side.
void draw_parapet(const engine::TileMap& map, const engine::Structure& s, float light) {
    if (s.tiles.empty()) return;
    g_light = light;
    const Vector2 c{static_cast<float>(s.tiles.front().x) + 0.5f, static_cast<float>(s.tiles.front().y) + 0.5f};
    Vector2 f = to_vector2(s.facing);
    const float len = std::hypot(f.x, f.y);
    f = len > 0.0f ? Vector2{f.x / len, f.y / len} : Vector2{1.0f, 0.0f};
    const Vector2 mid{c.x + f.x * 0.36f, c.y + f.y * 0.36f};
    const Vector2 a{mid.x - f.y * 0.45f, mid.y + f.x * 0.45f};
    const Vector2 b{mid.x + f.y * 0.45f, mid.y - f.x * 0.45f};
    DrawLineEx(on_terrain(map, a, 2.0f), on_terrain(map, b, 2.0f), 8.0f, lit({112, 94, 68, 255}));
    DrawLineEx(on_terrain(map, a, 4.0f), on_terrain(map, b, 4.0f), 4.0f, lit({146, 124, 90, 255}));
    g_light = 1.0f;
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
        DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y - size * 0.4f), size, size * 0.7f,
                    lit({112, 114, 110, 255}));
        DrawEllipse(static_cast<int>(p.x - size * 0.25f), static_cast<int>(p.y - size * 0.7f), size * 0.5f,
                    size * 0.3f, lit({150, 152, 148, 255}));
    }
}

// Heaps of brick where a house stood.
void draw_ruins(const engine::TileMap& map, int tx, int ty) {
    const uint32_t h = tile_hash(tx, ty);
    for (int i = 0; i < 3; ++i) {
        const float fx = 0.25f + static_cast<float>((h >> (i * 8)) & 0xFF) / 255.0f * 0.5f;
        const float fy = 0.25f + static_cast<float>((h >> (i * 8 + 4)) & 0xFF) / 255.0f * 0.5f;
        const Vector2 p = on_terrain(map, {static_cast<float>(tx) + fx, static_cast<float>(ty) + fy});
        DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y - 2.0f), 9.0f, 4.5f, lit({92, 80, 70, 255}));
        DrawEllipse(static_cast<int>(p.x - 1.0f), static_cast<int>(p.y - 4.0f), 5.0f, 2.5f, lit({130, 108, 92, 255}));
    }
}

constexpr float kCarLength = 1.1f;  // tiles, coupling included
constexpr int kTrainCars = 5;

// Point `d` tiles along a polyline, and the direction of travel there.
std::pair<Vector2, Vector2> along(const std::vector<Vector2>& line, float d) {
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const Vector2 a = line[i];
        const Vector2 b = line[i + 1];
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        if (len <= 0.0f) continue;
        const Vector2 dir{(b.x - a.x) / len, (b.y - a.y) / len};
        if (d <= len || i + 2 == line.size()) return {{a.x + dir.x * d, a.y + dir.y * d}, dir};
        d -= len;
    }
    return {line.front(), {1.0f, 0.0f}};
}

void draw_train_car(const engine::TileMap& map, const TrainCar& car) {
    static constexpr Color kColors[] = {{58, 66, 60, 255}, {122, 78, 56, 255}, {104, 110, 76, 255},
                                        {166, 168, 164, 255}, {122, 78, 56, 255}};
    const Color color = kColors[static_cast<size_t>(car.kind) % std::size(kColors)];
    const float height = car.kind == 0 ? 12.0f : 9.0f;
    fill_ground_ellipse(on_terrain(map, car.ground), 0.45f, {0, 0, 0, 60});
    draw_box(map, car.ground, car.facing, kCarLength - 0.15f, 0.5f, height, color, 2.0f);
    if (car.kind == 0) {  // the cab and the stack
        const Vector2 cab{car.ground.x - car.facing.x * 0.28f, car.ground.y - car.facing.y * 0.28f};
        draw_box(map, cab, car.facing, 0.34f, 0.46f, 6.0f, shade(color, 1.2f), 14.0f);
        const Vector2 stack = on_terrain(map, {car.ground.x + car.facing.x * 0.3f, car.ground.y + car.facing.y * 0.3f},
                                         14.0f);
        DrawRectangleRec({stack.x - 1.5f, stack.y - 5.0f, 3.0f, 5.0f}, {40, 40, 40, 255});
    }
}

}  // namespace

// Follows the railway from the tile next to the station out to the end of
// the line. Starts at the station's wall so the train stops right at it.
std::vector<Vector2> WorldRenderer::rail_route(const engine::World& world, const engine::Structure& station) {
    const engine::TileMap& map = world.map();
    auto is_rail = [&](engine::TilePos t) { return map.contains(t) && map.terrain(t) == engine::Terrain::Rail; };
    constexpr engine::TilePos kSteps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

    std::optional<engine::TilePos> start;
    engine::TilePos inward{};
    for (const engine::TilePos& t : station.tiles) {
        for (const engine::TilePos& d : kSteps) {
            const engine::TilePos n{t.x + d.x, t.y + d.y};
            if (!start && is_rail(n)) {
                start = n;
                inward = {-d.x, -d.y};
            }
        }
    }
    if (!start) return {};

    std::vector<Vector2> route;
    const Vector2 first{static_cast<float>(start->x) + 0.5f, static_cast<float>(start->y) + 0.5f};
    route.push_back({first.x + static_cast<float>(inward.x) * 0.5f, first.y + static_cast<float>(inward.y) * 0.5f});
    std::vector<bool> visited(static_cast<size_t>(map.width() * map.height()), false);
    engine::TilePos cur = *start;
    for (;;) {
        visited[static_cast<size_t>(cur.y * map.width() + cur.x)] = true;
        route.push_back({static_cast<float>(cur.x) + 0.5f, static_cast<float>(cur.y) + 0.5f});
        std::optional<engine::TilePos> next;
        for (const engine::TilePos& d : kSteps) {
            const engine::TilePos n{cur.x + d.x, cur.y + d.y};
            if (is_rail(n) && !visited[static_cast<size_t>(n.y * map.width() + n.x)]) next = n;
        }
        if (!next) break;
        cur = *next;
    }
    return route;
}

void WorldRenderer::collect_trains(const engine::World& world, float alpha, std::vector<TrainCar>& cars) const {
    for (const auto& [station_id, route] : rail_routes_) {
        const engine::Structure* station = world.find_structure(station_id);
        if (!station || route.size() < 2) continue;
        const bool ours = station->owner == viewer_;
        float line = 0.0f;
        for (size_t i = 0; i + 1 < route.size(); ++i) {
            line += std::hypot(route[i + 1].x - route[i].x, route[i + 1].y - route[i].y);
        }
        const float train = kTrainCars * kCarLength;

        // The schedule: it slows down coming in, stands, speeds up going out.
        const float now = static_cast<float>(world.tick()) + alpha;
        const auto interval = static_cast<float>(engine::kTrainInterval);
        const auto approach = static_cast<float>(engine::kTrainApproachTicks);
        const auto stay = static_cast<float>(engine::kTrainStayTicks);
        const auto next = static_cast<float>(station->next_train);
        const float last = next - interval;
        constexpr float kStop = 0.2f;  // head of the train this far from the station
        float head = -1.0f;
        if (next - now < approach) {
            const float t = std::max(0.0f, next - now) / approach;
            head = kStop + (line + train) * t * t;
        } else if (last > 0.0f && now - last < stay) {
            head = kStop;
        } else if (last > 0.0f && now - last < stay + approach) {
            const float t = (now - last - stay) / approach;
            head = kStop + (line + train) * t * t;
        }
        if (head < 0.0f) continue;

        for (int i = 0; i < kTrainCars; ++i) {
            const float d = head + kCarLength * (static_cast<float>(i) + 0.5f);
            if (d > line) break;  // still beyond the map edge
            const auto [ground, dir] = along(route, d);
            if (!ours && !in_view(world, ground)) continue;  // an enemy train is seen only where we look
            cars.push_back({ground, {-dir.x, -dir.y}, i});
        }
    }
}

// Garrison flags and health bars over houses and bridges.
void WorldRenderer::draw_structure_overlays(const engine::World& world, Rectangle view) const {
    const engine::TileMap& map = world.map();
    for (const engine::Structure& s : world.structures()) {
        const Vector2 c = on_terrain(map, to_vector2(s.center), 30.0f);
        if (!CheckCollisionPointRec(c, {view.x - 40, view.y - 40, view.width + 80, view.height + 80})) continue;
        if (!reveal_ && !world.sees(viewer_, s)) continue;
        // Who holds a house shows only once one of them is seen.
        const bool garrison_seen = std::any_of(s.garrison.begin(), s.garrison.end(), [&](engine::EntityId id) {
            const engine::Unit* u = world.find_unit(id);
            return u && shows(world, *u);
        });

        const int32_t max_hp = engine::structure_type(s.type).max_hp;
        if (s.hp < max_hp) {
            const float frac = static_cast<float>(s.hp) / static_cast<float>(max_hp);
            DrawRectangleRec({c.x - 21, c.y - 1, 42, 5}, {0, 0, 0, 170});
            DrawRectangleRec({c.x - 20, c.y, 40 * frac, 3}, frac > 0.5f ? Color{200, 200, 190, 255} : Color{230, 110, 60, 255});
        }
        if (garrison_seen) {
            const char* count = TextFormat("%d", static_cast<int>(s.garrison.size()));
            const Color flag = theme::player_color(s.owner);
            DrawLineEx({c.x, c.y + 18}, {c.x, c.y - 10}, 1.5f, {40, 40, 40, 255});
            DrawRectangleRec({c.x, c.y - 10, 16, 11}, flag);
            DrawText(count, static_cast<int>(c.x + 4), static_cast<int>(c.y - 9), 10, WHITE);
        }
    }
}

void WorldRenderer::draw(const engine::World& world, const RtsCamera& camera, float alpha,
                         std::span<const engine::EntityId> selection, engine::EntityId selected_structure,
                         const BuildGhost* ghost, std::span<const engine::TilePos> trench) const {
    const engine::TileMap& map = world.map();
    const Rectangle view = camera.visible_world_rect();
    auto is_selected = [&](engine::EntityId id) {
        return std::binary_search(selection.begin(), selection.end(), id);
    };

    BeginMode2D(camera.camera2d());

    draw_terrain(world, view);
    draw_remains(map);

    // Mines we know of: ours, and the enemy's our sappers found. Charges ticking.
    for (const engine::Mine& m : world.mines()) {
        if (!reveal_ && (!world.knows(viewer_, m) || fog(world, m.tile.x, m.tile.y) == kUnexplored)) continue;
        const Vector2 p = on_terrain(map, to_vector2(engine::tile_center(m.tile)));
        const float r = m.anti_tank ? 0.2f : 0.12f;
        fill_ground_ellipse(p, r, m.anti_tank ? Color{90, 96, 60, 255} : Color{60, 62, 58, 255});
        draw_ground_ellipse(p, r, m.owner == viewer_ ? theme::player_color(m.owner) : theme::kDanger);
    }
    for (const engine::Charge& c : world.charges()) {
        if (!reveal_ && c.owner != viewer_ && !in_view(world, to_vector2(c.pos))) continue;
        const Vector2 p = on_terrain(map, to_vector2(c.pos), 3.0f);
        const bool blink = (world.tick() / 5) % 2 == 0;
        DrawRectangleRec({p.x - 3.0f, p.y - 3.0f, 6.0f, 6.0f}, blink ? Color{220, 40, 30, 255} : Color{120, 30, 24, 255});
    }

    // Parapets: ours as they are, others' as last seen.
    for (const engine::Structure& s : world.structures()) {
        if (s.parapet && s.owner == viewer_) draw_parapet(map, s, 1.0f);
    }
    for (const auto& [id, s] : remembered_) {
        if (s.parapet) draw_parapet(map, s, reveal_ || world.sees(viewer_, s) ? 1.0f : kFogLight);
    }

    for (const engine::Unit& u : world.units()) {
        if (!is_selected(u.id) || u.inside) continue;
        const float radius = to_float(engine::unit_type(u.type).radius) + 0.1f;
        draw_ground_ellipse(on_terrain(map, unit_ground_pos(u, alpha)), radius, theme::kSelection);
        draw_orders(world, u, alpha);
    }
    draw_pings(map);

    if (ghost) draw_ghost(map, *ghost);
    for (const engine::TilePos& t : trench) {
        const auto x = static_cast<float>(t.x);
        const auto y = static_cast<float>(t.y);
        const Color color = world.diggable(t) ? theme::kSelection : theme::kDanger;
        fill_quad(on_terrain(map, {x, y}), on_terrain(map, {x + 1, y}), on_terrain(map, {x + 1, y + 1}),
                  on_terrain(map, {x, y + 1}), ColorAlpha(color, 0.3f));
    }
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
        const TrainCar* car = nullptr;
        float light = 1.0f;  // dimmer under the fog of war
    };
    std::vector<Drawable> drawables;
    drawables.reserve(world.units().size() + world.projectiles().size() + 1024);
    for (const engine::Unit& u : world.units()) {
        if (u.inside || !shows(world, u)) continue;  // behind walls, or unseen
        const Vector2 g = unit_ground_pos(u, alpha);
        const Vector2 p = on_terrain(map, g);
        if (!CheckCollisionPointRec(p, {view.x - 60, view.y - 60, view.width + 120, view.height + 120})) continue;
        drawables.push_back({.depth = g.x + g.y, .unit = &u});
    }
    for (const engine::Projectile& p : world.projectiles()) {
        const Vector2 g = lerp(to_vector2(p.prev_pos), to_vector2(p.pos), alpha);
        if (in_view(world, g)) drawables.push_back({.depth = g.x + g.y, .projectile = &p});
    }
    // Scenery comes from the remembered ground: what the fog hides stays as it was.
    for_each_visible_tile(map, view, [&](int tx, int ty) {
        const int state = fog(world, tx, ty);
        if (state == kUnexplored) return;
        const float light = state == kInView ? 1.0f : kFogLight;
        switch (seen_terrain_[static_cast<size_t>(ty * map.width() + tx)]) {
            case engine::Terrain::Forest:
                for (const Tree& t : trees_on_tile(tx, ty)) {
                    drawables.push_back({.depth = t.ground.x + t.ground.y, .tree = t, .light = light});
                }
                break;
            case engine::Terrain::House: {
                float damage = 0.0f;
                if (const engine::Structure* s = world.structure_at({tx, ty}); s && state == kInView) {
                    const int32_t max_hp = engine::structure_type(s->type).max_hp;
                    damage = 1.0f - static_cast<float>(s->hp) / static_cast<float>(max_hp);
                }
                drawables.push_back({.depth = static_cast<float>(tx + ty) + 1.0f, .house_x = tx, .house_y = ty,
                                     .damage = damage, .light = light});
                break;
            }
            case engine::Terrain::Ruins:
                drawables.push_back({.depth = static_cast<float>(tx + ty) + 1.0f, .house_x = tx, .house_y = ty,
                                     .ruins = true, .light = light});
                break;
            case engine::Terrain::Rock:
                drawables.push_back({.depth = static_cast<float>(tx + ty) + 1.0f, .house_x = tx, .house_y = ty,
                                     .rock = true, .light = light});
                break;
            default:
                break;
        }
    });
    // Our buildings as they are; others' as we last saw them.
    auto add_building = [&](const engine::Structure& s, float light) {
        const Vector2 c = to_vector2(s.center);
        if (!CheckCollisionPointRec(on_terrain(map, c), {view.x - 150, view.y - 150, view.width + 300, view.height + 300})) {
            return;
        }
        drawables.push_back({.depth = c.x + c.y + 1.0f, .building = &s, .light = light});
    };
    // Field works and dugouts are holes in the ground, drawn with it.
    auto is_building = [](const engine::Structure& s) {
        return s.type != engine::StructureType::House && s.type != engine::StructureType::Bridge &&
               !engine::is_fieldwork(s.type) && !engine::is_obstacle(s.type) && s.type != engine::StructureType::Dugout;
    };
    for (const engine::Structure& s : world.structures()) {
        if (is_building(s) && s.owner == viewer_) add_building(s, 1.0f);
    }
    for (const auto& [id, s] : remembered_) {
        if (is_building(s)) add_building(s, reveal_ || world.sees(viewer_, s) ? 1.0f : kFogLight);
    }
    std::vector<TrainCar> cars;
    collect_trains(world, alpha, cars);
    for (const TrainCar& c : cars) drawables.push_back({.depth = c.ground.x + c.ground.y, .car = &c});
    std::stable_sort(drawables.begin(), drawables.end(),
                     [](const Drawable& a, const Drawable& b) { return a.depth < b.depth; });
    for (const Drawable& d : drawables) {
        g_light = d.light;
        if (d.unit) {
            draw_unit(map, *d.unit, alpha);
        } else if (d.projectile) {
            draw_projectile(*d.projectile, alpha);
        } else if (d.building) {
            draw_building(map, *d.building);
        } else if (d.car) {
            draw_train_car(map, *d.car);
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
    g_light = 1.0f;

    draw_shots(world, alpha);
    draw_blasts(map);
    draw_structure_overlays(world, view);
    for (const Drawable& d : drawables) {
        if (d.unit && !d.unit->inside &&
            (is_selected(d.unit->id) || d.unit->hp < engine::unit_type(d.unit->type).max_hp)) {
            draw_health_bar(map, *d.unit, alpha);
        }
        if (d.unit && !d.unit->inside && d.unit->owner == viewer_) draw_supply_warning(map, *d.unit, alpha);
    }

    EndMode2D();
}

void WorldRenderer::draw_terrain(const engine::World& world, Rectangle view) const {
    const engine::TileMap& map = world.map();
    if (map.width() != cache_width_ || map.height() != cache_height_) return;  // update() hasn't seen this map yet
    if (seen_terrain_.size() != static_cast<size_t>(map.width() * map.height())) return;

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
        const int state = fog(world, tx, ty);
        if (state == kUnexplored) return;  // black, like the background
        const engine::Terrain terrain = seen_terrain_[static_cast<size_t>(ty * map.width() + tx)];
        g_light = state == kInView ? 1.0f : kFogLight;
        fill_quad(top, right, bottom, left, shade(theme::terrain_color(terrain), light));
        if (terrain == engine::Terrain::Trench || terrain == engine::Terrain::Foxhole ||
            terrain == engine::Terrain::Dugout || terrain == engine::Terrain::GunPit ||
            terrain == engine::Terrain::Wire || terrain == engine::Terrain::Hedgehogs) {
            draw_works(map, tx, ty, terrain, [&](int x, int y) {
                if (!map.contains_tile(x, y)) return false;
                const engine::Terrain t = seen_terrain_[static_cast<size_t>(y * map.width() + x)];
                return t == engine::Terrain::Trench || t == engine::Terrain::Foxhole || t == engine::Terrain::Dugout;
            });
        }
        if (terrain == engine::Terrain::Rail) {
            auto track = [&](int x, int y) {
                return map.contains_tile(x, y) &&
                       (map.terrain(x, y) == engine::Terrain::Rail || map.terrain(x, y) == engine::Terrain::Building);
            };
            draw_rail(map, tx, ty, track(tx - 1, ty) || track(tx + 1, ty));
        }
    });
    g_light = 1.0f;
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
        case engine::Order::Observe: {
            // The sector: 90 degrees towards the point, as far as the post sees.
            const engine::UnitTypeDef& def = engine::unit_type(u.type);
            const Vector2 ground = unit_ground_pos(u, alpha);
            const Vector2 target = to_vector2(u.order_point);
            const float heading = std::atan2(target.y - ground.y, target.x - ground.x);
            const float reach = to_float(def.sector_range) +
                                static_cast<float>(map.elevation_at(u.pos));
            constexpr float kHalf = 0.785398f;  // 45 degrees
            constexpr int kArc = 16;
            Vector2 prev = from;
            const Color color = ColorAlpha(theme::kSelection, 0.35f);
            for (int i = 0; i <= kArc; ++i) {
                const float a = heading - kHalf + 2.0f * kHalf * static_cast<float>(i) / kArc;
                const Vector2 p = on_terrain(map, {ground.x + std::cos(a) * reach, ground.y + std::sin(a) * reach});
                if (i == 0 || i == kArc) DrawLineV(from, p, color);
                if (i > 0) DrawLineV(prev, p, color);
                prev = p;
            }
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

    if (u.type == engine::UnitTypeId::Mortar) {
        // The tube on its base plate in front of the crewman: up when set up, on his back when not.
        const Vector2 plate = {feet.x + iso_offset(facing).x * 0.25f, feet.y + iso_offset(facing).y * 0.25f};
        if (u.deployed) {
            DrawEllipse(static_cast<int>(plate.x), static_cast<int>(plate.y), 4.0f, 2.0f, {50, 52, 48, 255});
            const Vector2 muzzle{plate.x + iso_offset(facing).x * 0.12f, plate.y - 10.0f};
            DrawLineEx(plate, muzzle, 3.0f, {60, 66, 56, 255});
        }
    }

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
        case engine::UnitTypeId::Mortar:
            length = u.deployed ? 0.1f : 0.2f;
            thickness = u.deployed ? 1.2f : 3.0f;
            weapon = {60, 66, 56, 255};
            break;
        case engine::UnitTypeId::Ags:
            length = 0.3f;
            thickness = 4.0f;
            weapon = {58, 62, 54, 255};
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
    const Color color = shade(theme::player_color(u.owner), 0.85f);
    if (u.type == engine::UnitTypeId::Truck) {
        // Load bed behind, cab in front, the freight on the bed.
        auto at = [&](float k) { return Vector2{ground.x + facing.x * k, ground.y + facing.y * k}; };
        draw_box(map, at(-0.12f), facing, 0.56f, 0.44f, 4.0f, shade(color, 0.7f));
        if (u.carrying > 0) {
            const float load = 3.0f + 5.0f * static_cast<float>(u.carrying) / static_cast<float>(engine::kTruckCapacity);
            draw_box(map, at(-0.12f), facing, 0.5f, 0.38f, load, cargo_color(u.carrying_type), 4.0f);
        }
        draw_box(map, at(0.3f), facing, 0.26f, 0.42f, 10.0f, color);
        return;
    }
    if (u.type == engine::UnitTypeId::FuelTanker || u.type == engine::UnitTypeId::AmmoTruck) {
        // A cab and behind it a silver tank, or a covered bed of olive crates.
        auto at = [&](float k) { return Vector2{ground.x + facing.x * k, ground.y + facing.y * k}; };
        draw_box(map, at(-0.12f), facing, 0.56f, 0.44f, 3.0f, shade(color, 0.7f));
        const bool fuel = u.type == engine::UnitTypeId::FuelTanker;
        const Color body = fuel ? Color{176, 178, 172, 255} : Color{98, 106, 70, 255};
        draw_box(map, at(-0.12f), facing, 0.52f, fuel ? 0.34f : 0.4f, fuel ? 7.0f : 6.0f, body, 3.0f);
        draw_box(map, at(0.3f), facing, 0.26f, 0.42f, 10.0f, color);
        return;
    }

    if (u.type == engine::UnitTypeId::Mlrs) {
        // A truck with a pack of launch tubes: raised when set up.
        auto at = [&](float k) { return Vector2{ground.x + facing.x * k, ground.y + facing.y * k}; };
        draw_box(map, at(-0.12f), facing, 0.56f, 0.44f, 4.0f, shade(color, 0.7f));
        const float raise = u.deployed ? 8.0f : 4.0f;
        const auto top = draw_box(map, at(-0.14f), facing, 0.46f, 0.38f, u.deployed ? 7.0f : 4.0f, {74, 82, 60, 255}, raise);
        for (int i = 0; i < 3; ++i) {  // the tube ends
            const float k = static_cast<float>(i + 1) / 4.0f;
            DrawLineV(lerp(top[0], top[1], k), lerp(top[3], top[2], k), {40, 44, 34, 255});
        }
        draw_box(map, at(0.3f), facing, 0.26f, 0.42f, 10.0f, color);
        return;
    }

    if (u.type == engine::UnitTypeId::Howitzer) {
        // A carriage on two wheels and a long barrel: set up, the trails
        // spread behind and the barrel points up; packed, it trails behind
        // for towing.
        const Color steel = shade(color, 0.6f);
        draw_box(map, ground, facing, 0.34f, 0.46f, 5.0f, shade(color, 0.8f));
        const Vector2 hub = on_terrain(map, ground, 6.0f);
        if (u.deployed) {
            const Vector2 side{-facing.y, facing.x};
            for (const float k : {-0.35f, 0.35f}) {
                const Vector2 tail{ground.x - facing.x * 0.55f + side.x * k, ground.y - facing.y * 0.55f + side.y * k};
                DrawLineEx(on_terrain(map, ground, 2.0f), on_terrain(map, tail), 2.0f, steel);
            }
            const Vector2 tip = on_terrain(map, {ground.x + facing.x * 0.45f, ground.y + facing.y * 0.45f}, 20.0f);
            DrawLineEx(hub, tip, 3.0f, steel);
            if (u.camouflaged) {  // nets and branches over it
                fill_ground_ellipse(on_terrain(map, ground, 8.0f), 0.55f, {64, 88, 52, 170});
                draw_ground_ellipse(on_terrain(map, ground, 8.0f), 0.55f, {46, 66, 38, 200});
            }
        } else {
            const Vector2 tip = on_terrain(map, {ground.x - facing.x * 0.6f, ground.y - facing.y * 0.6f}, 7.0f);
            DrawLineEx(hub, tip, 3.0f, steel);
        }
        return;
    }

    const bool tank = u.type == engine::UnitTypeId::Tank;
    const float length = tank ? 0.95f : 0.85f;
    const float width = tank ? 0.58f : 0.5f;
    const float hull = tank ? 6.0f : 8.0f;  // hull height, pixels
    draw_box(map, ground, facing, length, width, hull, color);

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
    float height = to_float(p.origin_height) + (to_float(p.target_height) - to_float(p.origin_height)) * t;
    if (p.lobbed) height += 4.0f * t * (1.0f - t) * std::max(1.0f, total * 0.3f);  // an arc
    const Vector2 pos = iso::project(ground, height);

    Vector2 dir = iso_offset(to_vector2(p.target - p.origin));
    const float len = std::hypot(dir.x, dir.y);
    dir = len > 0.0f ? Vector2{dir.x / len, dir.y / len} : Vector2{1.0f, 0.0f};

    if (p.lobbed) {
        if (p.weapon.indirect) {
            // A shell high on its arc, with a faint trail just behind it.
            const float back = std::max(0.0f, t - 0.04f);
            const Vector2 g0{origin.x + (target.x - origin.x) * back, origin.y + (target.y - origin.y) * back};
            float h0 = to_float(p.origin_height) + (to_float(p.target_height) - to_float(p.origin_height)) * back;
            h0 += 4.0f * back * (1.0f - back) * std::max(1.0f, total * 0.3f);
            DrawLineEx(iso::project(g0, h0), pos, 2.0f, {200, 200, 190, 110});
            DrawCircleV(pos, 3.0f, {40, 42, 38, 255});
            return;
        }
        DrawCircleV(pos, 2.5f, {60, 64, 50, 255});  // a grenade tumbling through the air
        return;
    }
    switch (p.weapon.damage_type) {
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
        if (!shows(world, u)) continue;
        const engine::UnitTypeDef& def = engine::unit_type(u.type);
        const Vector2 ground = unit_ground_pos(u, alpha);
        const Vector2 facing = unit_facing(u);
        const float reach = def.vehicle ? (u.type == engine::UnitTypeId::Tank ? 0.75f : 0.5f) : 0.25f;
        const Vector2 muzzle = on_terrain(map, {ground.x + facing.x * reach, ground.y + facing.y * reach},
                                          def.vehicle ? 8.0f : 9.0f);

        DrawCircleV(muzzle, def.vehicle ? 4.0f : 2.0f, {255, 230, 140, 220});
        const bool sweep = u.order == engine::Order::Ability && u.order_ability == engine::AbilityId::MgSweep;
        if (engine::weapon_of(u).projectile_speed.raw == 0 || sweep) {  // instant hit: draw the tracer
            DrawLineV(muzzle, on_terrain(map, to_vector2(u.last_shot_at), 6.0f), {255, 235, 160, 140});
        }
    }
}

void WorldRenderer::draw_blasts(const engine::TileMap& map) const {
    for (const Blast& b : blasts_) {
        if (b.age < 0.0f) continue;  // not yet
        const float t = b.age / kBlastLifetime;
        const Vector2 p = on_terrain(map, b.ground);
        const float radius = b.radius * (0.5f + t);
        fill_ground_ellipse(p, radius, ColorAlpha({255, 170, 60, 255}, 0.55f * (1.0f - t)));
        fill_ground_ellipse({p.x, p.y - 3.0f}, radius * 0.5f, ColorAlpha({255, 240, 190, 255}, 0.8f * (1.0f - t)));
        draw_ground_ellipse(p, radius * 1.1f, ColorAlpha({90, 80, 70, 255}, 0.6f * (1.0f - t)));
    }
}

// Our vehicles running low: an F for fuel, an A for rounds; amber when
// low, red when out.
void WorldRenderer::draw_supply_warning(const engine::TileMap& map, const engine::Unit& u, float alpha) const {
    const engine::UnitTypeDef& def = engine::unit_type(u.type);
    auto level = [](float left) { return left <= 0.0f ? 2 : left < 0.25f ? 1 : 0; };
    const int fuel = def.fuel_capacity.raw > 0 ? level(to_float(u.fuel) / to_float(def.fuel_capacity)) : 0;
    const int rounds = def.rounds_capacity > 0
                           ? level(static_cast<float>(u.rounds) / static_cast<float>(def.rounds_capacity))
                           : 0;
    if (fuel == 0 && rounds == 0) return;
    const Vector2 feet = on_terrain(map, unit_ground_pos(u, alpha));
    float x = feet.x + 17.0f;
    const float y = feet.y - 30.0f;
    for (const auto& [letter, state] : {std::pair{"F", fuel}, std::pair{"A", rounds}}) {
        if (state == 0) continue;
        DrawRectangleRec({x, y, 10, 11}, state == 2 ? Color{200, 50, 40, 230} : Color{220, 160, 40, 230});
        DrawText(letter, static_cast<int>(x + 2), static_cast<int>(y + 1), 10, WHITE);
        x += 12.0f;
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
