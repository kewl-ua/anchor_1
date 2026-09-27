#include "render/world_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

#include <rlgl.h>

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

// The grain of what is being drawn, like a painted texture: fine grain,
// blotches, specks of gravel (0 = none, about 1 = strong). Surfaces take it
// to the shader in their vertex normals; raylib's own normal, (0, 0, 1),
// means no grain.
struct Grain {
    float fine = 0.0f;
    float mottle = 0.0f;
    float speck = 0.0f;
};
Vector3 g_grain{0.0f, 0.0f, 1.0f};
// The view's zoom this frame: far out, crowns are drawn with fewer clumps.
float g_zoom = 1.0f;
void set_grain(Grain g) {
    g_grain = {g.fine, g.mottle, 1.0f - g.speck};
    rlNormal3f(g_grain.x, g_grain.y, g_grain.z);  // for raylib's shapes that don't set their own
}
// Buildings, trees, rocks, wagons: painted surfaces.
constexpr Grain kObjectGrain{0.6f, 0.55f, 0.2f};

// The grain itself, in world pixels (so it stays put on the ground as the
// view scrolls): value noise at a pixel and a half and at three and a half
// for the grain, at 11 and 27 for the blotches (a little warmer and cooler in
// places too), bright and dark specks of gravel two pixels across. Grains
// finer than a screen pixel would shimmer, so they fade out as the view
// zooms out.
constexpr const char* kGrainVertex = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragGrain;
out vec2 fragWorld;
void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragGrain = vertexNormal;
    fragWorld = vertexPosition.xy;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";
constexpr const char* kGrainFragment = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragGrain;
in vec2 fragWorld;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float zoom;
out vec4 finalColor;
float hash(vec2 p) {
    uvec2 q = uvec2(ivec2(floor(p)));
    uint h = (q.x * 1597334677u) ^ (q.y * 3812015801u);
    h ^= h >> 16u;
    h *= 2246822519u;
    h ^= h >> 13u;
    h *= 3266489917u;
    h ^= h >> 16u;
    return float(h) * (1.0 / 4294967295.0);
}
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}
void main() {
    vec4 c = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    float fine = fragGrain.x;
    float mottle = fragGrain.y;
    float speck = 1.0 - fragGrain.z;
    if (fine + mottle + speck > 0.001) {
        vec2 w = fragWorld;
        float sharp = smoothstep(0.3, 0.8, zoom);
        // Each layer turned its own way, so no grid shows through.
        vec2 a = mat2(0.80, 0.60, -0.60, 0.80) * w;
        vec2 b = mat2(0.28, -0.96, 0.96, 0.28) * w;
        float grain = (noise(a / 2.2) - 0.5) * sharp + (noise(b / 5.0) - 0.5) * 0.7;
        float blotch = (noise(b / 11.0) - 0.5) + (noise(a / 27.0) - 0.5) * 0.8;
        float k = 1.0 + fine * grain * 0.5 + mottle * blotch * 0.4;
        // Pebbles: a cell of three pixels may hold a stone, pale or dark,
        // round with a soft edge, lit on its upper left.
        vec2 cell = floor(w / 3.0);
        vec2 f = fract(w / 3.0);
        float h1 = hash(cell + vec2(71.0, 13.0));
        float h2 = hash(cell + vec2(3.0, 157.0));
        vec2 centre = vec2(0.3 + 0.4 * h2, 0.3 + 0.4 * fract(h2 * 7.13));
        vec2 d = (f - centre) / (0.26 + 0.14 * fract(h1 * 11.7));
        float stone = (1.0 - smoothstep(0.7, 1.0, length(d))) * sharp;
        float lit = clamp(-(d.x + d.y) * 0.35, -0.35, 0.35);
        float pale = step(1.0 - 0.06 * speck, h1);
        float dark = step(h1, 0.08 * speck);
        k *= 1.0 + stone * (pale * (0.3 + lit) + dark * (lit * 0.5 - 0.45));
        vec3 tint = vec3(0.12, 0.03, -0.1) * mottle * (noise(w / 19.0 + vec2(5.0, 9.0)) - 0.5);
        c.rgb = clamp(c.rgb * k * (1.0 + tint), 0.0, 1.0);
    }
    finalColor = c;
}
)";

// Electronic warfare: relays' reach, and bearings of our direction finders.
constexpr Color kRadioColor = {120, 170, 255, 255};
constexpr Color kBearingColor = {255, 160, 60, 255};

// Aircraft in the air are drawn this high above their shadow, pixels.
constexpr float kFlightLift =
    static_cast<float>(engine::kFlightHeight.raw) / static_cast<float>(engine::Fixed::kOneRaw) * iso::kElevationStep;

// How high a unit's body is above its feet, pixels.
float body_lift(const engine::Unit& u) {
    if (u.airborne) return kFlightLift;
    return engine::unit_type(u.type).vehicle ? 6.0f : 9.0f;
}

Color shade(Color c, float k) {
    auto channel = [k](unsigned char v) {
        return static_cast<unsigned char>(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f));
    };
    return {channel(c.r), channel(c.g), channel(c.b), c.a};
}

Color lit(Color c) { return g_light >= 1.0f ? c : shade(c, g_light); }

void gradient_triangle(Vector2 a, Vector2 b, Vector2 c, Color ca, Color cb, Color cc, const Vector3* grain = nullptr);

// A triangle in one colour, with the grain of what is being drawn; either winding.
void fill_triangle(Vector2 a, Vector2 b, Vector2 c, Color color) {
    const Color k = lit(color);
    gradient_triangle(a, b, c, k, k, k);
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

namespace {

float drawn_height(const engine::Structure& s);  // with the buildings' drawing, below

// Ground tiles under points straight below a screen point, `lift` world
// pixels down: what stands on them and is drawn that high covers the point.
template <typename Fn>
auto probe_below(const RtsCamera& camera, const engine::World& world, Vector2 screen, float reach, Fn fn)
    -> decltype(fn(engine::TilePos{}, 0.0f)) {
    const float zoom = camera.camera2d().zoom;
    for (float lift = 0.0f; lift <= reach; lift += 3.0f) {
        const Vector2 ground = iso::pick_ground(world.map(), camera.screen_to_world({screen.x, screen.y + lift * zoom}));
        const engine::TilePos t{static_cast<int32_t>(std::floor(ground.x)), static_cast<int32_t>(std::floor(ground.y))};
        if (auto found = fn(t, lift)) return found;
    }
    return {};
}

}  // namespace

const engine::Structure* structure_on_screen(const RtsCamera& camera, const engine::World& world, Vector2 screen) {
    return probe_below(camera, world, screen, 40.0f, [&](engine::TilePos t, float lift) -> const engine::Structure* {
        const engine::Structure* s = world.structure_at(t);
        return s && lift <= drawn_height(*s) ? s : nullptr;
    });
}

std::optional<engine::TilePos> resource_on_screen(const RtsCamera& camera, const engine::World& world, Vector2 screen) {
    return probe_below(camera, world, screen, 36.0f, [&](engine::TilePos t, float lift) -> std::optional<engine::TilePos> {
        if (!world.map().contains(t) || world.map().resource(t) <= 0) return std::nullopt;
        const engine::Terrain terrain = world.map().terrain(t);
        const float height = terrain == engine::Terrain::Forest ? 34.0f : terrain == engine::Terrain::Rock ? 14.0f : -1.0f;
        return lift <= height ? std::optional<engine::TilePos>(t) : std::nullopt;
    });
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
        // A spoil tip's top: a corner among rock, higher than every corner round it.
        spoil_peaks_.clear();
        for (int cy = 1; cy < cache_height_; ++cy) {
            for (int cx = 1; cx < cache_width_; ++cx) {
                bool rock = true;
                for (int ty = cy - 1; ty <= cy; ++ty) {
                    for (int tx = cx - 1; tx <= cx; ++tx) rock = rock && map.terrain(tx, ty) == engine::Terrain::Slag;
                }
                const float here = corner(cx, cy);
                bool top = rock && here >= 4.0f;
                for (int dy = -1; dy <= 1 && top; ++dy) {
                    for (int dx = -1; dx <= 1 && top; ++dx) top = (dx == 0 && dy == 0) || corner(cx + dx, cy + dy) < here;
                }
                if (top) spoil_peaks_.push_back({static_cast<float>(cx), static_cast<float>(cy)});
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

// A filled circle with as few sides as its size needs (a forest is many of them).
void disc(Vector2 centre, float radius, Color color) {
    const int sides = std::clamp(static_cast<int>(radius * 1.6f), 6, 18);
    DrawCircleSector(centre, radius, 0.0f, 360.0f, sides, lit(color));
}

float hash_unit(uint32_t h) { return static_cast<float>(h & 0xFFFF) / 65536.0f; }

// Trees of a forest tile, placed by a hash so they never move: pine stands,
// broadleaf and birch groves in patches, poplars along a tree line, an old
// oak standing alone. Each its own height, girth and bow of the trunk. As the
// wood is cut they come down one by one, leaving stumps.
enum class TreeKind : uint8_t { Broadleaf, Oak, Beech, Birch, Pine, Poplar, Apple };

struct Tree {
    Vector2 ground;
    float size;
    float tint;
    TreeKind kind = TreeKind::Broadleaf;
    bool stump = false;
    float height = 1.0f;  // of the trunk, a share of the kind's usual
    float girth = 1.0f;
    float bend = 0.0f;    // sideways bow of the trunk, pixels at size 1 (+: to the right)
    uint32_t seed = 0;    // for the lumps of its crown
};

// `line`: 0 a forest tile, 1 or 2 a tree line along x or y, 3 a tile standing
// alone. `left`: the share of the wood still standing (0..1).
std::array<Tree, 3> trees_on_tile(int tx, int ty, int line, float left, size_t& count) {
    std::array<Tree, 3> trees{};
    const uint32_t base = tile_hash(tx, ty);
    // The stand: pines here, broadleaf there, birch groves, a little of each everywhere.
    auto stand_at = [&](float x, float y) {
        auto value = [](int ix, int iy) { return hash_unit(tile_hash(ix * 5 + 17, iy * 3 - 41)); };
        const float gx = x / 9.0f;
        const float gy = y / 9.0f;
        const int x0 = static_cast<int>(std::floor(gx));
        const int y0 = static_cast<int>(std::floor(gy));
        const float fx = gx - static_cast<float>(x0);
        const float fy = gy - static_cast<float>(y0);
        const float a = value(x0, y0) + (value(x0 + 1, y0) - value(x0, y0)) * fx;
        const float b = value(x0, y0 + 1) + (value(x0 + 1, y0 + 1) - value(x0, y0 + 1)) * fx;
        return a + (b - a) * fy;
    };
    const float stand = stand_at(static_cast<float>(tx) + 0.5f, static_cast<float>(ty) + 0.5f);
    count = line == 3 ? 1 : line ? 2 : 3;
    static constexpr Vector2 kSpots[3] = {{0.27f, 0.3f}, {0.73f, 0.36f}, {0.46f, 0.76f}};
    const size_t standing = static_cast<size_t>(std::ceil(static_cast<float>(count) * left - 0.001f));
    uint32_t h = base;
    auto next = [&h] {
        h = h * 2654435761u + 0x9E3779B9u;
        return hash_unit(h >> 7);
    };
    for (size_t i = 0; i < count; ++i) {
        Tree& t = trees[i];
        const float jx = (next() - 0.5f) * 0.22f;
        const float jy = (next() - 0.5f) * 0.22f;
        Vector2 spot = kSpots[i];
        if (line == 1) spot = {0.25f + 0.5f * static_cast<float>(i), 0.5f};
        if (line == 2) spot = {0.5f, 0.25f + 0.5f * static_cast<float>(i)};
        if (line == 3) spot = {0.5f, 0.5f};
        t.ground = {static_cast<float>(tx) + spot.x + jx, static_cast<float>(ty) + spot.y + jy};
        t.size = 0.78f + next() * 0.42f;
        t.tint = 0.88f + next() * 0.24f;
        const float pick = next();
        if (line == 3) {
            t.kind = TreeKind::Oak;  // an old oak on its own in the field
            t.size = 1.3f + 0.25f * next();
        } else if (line) {
            t.kind = pick < 0.6f ? TreeKind::Poplar : (pick < 0.8f ? TreeKind::Broadleaf : TreeKind::Oak);
        } else if (stand < 0.36f) {
            t.kind = pick < 0.85f ? TreeKind::Pine : TreeKind::Birch;
        } else if (stand > 0.68f) {
            t.kind = pick < 0.6f ? TreeKind::Birch : (pick < 0.8f ? TreeKind::Beech : TreeKind::Broadleaf);
        } else {
            t.kind = pick < 0.3f   ? TreeKind::Oak
                     : pick < 0.52f ? TreeKind::Beech
                     : pick < 0.8f  ? TreeKind::Broadleaf
                     : pick < 0.9f  ? TreeKind::Pine
                                    : TreeKind::Birch;
        }
        t.height = 0.75f + 0.55f * next();
        t.girth = 0.8f + 0.45f * next();
        t.bend = (next() - 0.5f) * (t.kind == TreeKind::Birch ? 7.0f : t.kind == TreeKind::Poplar ? 1.0f : 4.0f);
        t.seed = h;
        t.stump = i >= standing;
    }
    return trees;
}

// A trunk from `base` up `height` pixels, bowed sideways by `bend` at its
// middle, tapering from `w0` to `w1`, flaring at the roots; lit on the left.
// Returns its top, where the crown sits.
Vector2 draw_trunk(Vector2 base, float height, float w0, float w1, float bend, Color bark) {
    constexpr int kSteps = 5;
    Vector2 p[kSteps + 1];
    float w[kSteps + 1];
    for (int i = 0; i <= kSteps; ++i) {
        const float t = static_cast<float>(i) / kSteps;
        p[i] = {base.x + bend * 4.0f * t * (1.0f - t) + bend * 0.3f * t, base.y - height * t};
        w[i] = w0 + (w1 - w0) * t;
    }
    w[0] *= 1.45f;  // the roots
    const Color light = shade(bark, 1.18f);
    const Color dark = shade(bark, 0.72f);
    for (int i = 0; i < kSteps; ++i) {
        const Vector2 a{p[i].x - w[i] * 0.5f, p[i].y};
        const Vector2 d{p[i + 1].x - w[i + 1] * 0.5f, p[i + 1].y};
        const Vector2 b{p[i].x + w[i] * 0.5f, p[i].y};
        const Vector2 c{p[i + 1].x + w[i + 1] * 0.5f, p[i + 1].y};
        fill_quad(a, p[i], p[i + 1], d, light);
        fill_quad(p[i], b, c, p[i + 1], dark);
    }
    return p[kSteps];
}

// A fluffy crown in a `w` x `h` oval about `c`, like Postal's trees: a dark
// mass inside, tufts of leaves poking out all round its edge, and over it
// clumps of leaves, each shaded on its lower right and catching the light on
// its upper left; the clumps high on the left are the brightest, the ones
// low on the right sit in shade. Each tree tufted its own way.
void draw_crown(Vector2 c, float w, float h, float lump, Color leaf, float tint, uint32_t seed) {
    auto rnd = [seed](int i, int salt) {
        return hash_unit(tile_hash(static_cast<int>(seed >> 3) + i * 7 + salt * 131, i * 13 - salt * 17));
    };
    const bool far = g_zoom < 0.6f;
    DrawEllipse(static_cast<int>(c.x + w * 0.05f), static_cast<int>(c.y + h * 0.06f), w * 0.45f, h * 0.45f,
                lit(shade(leaf, 0.46f * tint)));
    const int tufts = far ? 8 : 16;
    for (int i = 0; i < tufts; ++i) {
        const float a = (static_cast<float>(i) + rnd(i, 5)) * 6.2831853f / static_cast<float>(tufts);
        const float reach = 0.9f + 0.16f * rnd(i, 9);
        const Vector2 at{c.x + std::cos(a) * w * 0.5f * reach, c.y + std::sin(a) * h * 0.5f * reach};
        const float k = 0.78f - 0.24f * (std::cos(a) + std::sin(a));
        disc(at, lump * (0.26f + 0.14f * rnd(i, 11)), shade(leaf, k * tint));
    }
    struct Clump {
        Vector2 at;
        float r;
        float k;
    };
    constexpr int kMaxClumps = 28;
    std::array<Clump, kMaxClumps> clumps{};
    int count = std::clamp(static_cast<int>(w * h / (lump * lump) * 2.4f), 12, kMaxClumps);
    if (far) count /= 2;
    for (int i = 0; i < count; ++i) {
        const float a = rnd(i, 1) * 6.2831853f;
        const float r = std::sqrt(rnd(i, 2)) * 0.8f;
        const Vector2 at{c.x + std::cos(a) * r * w * 0.5f, c.y + std::sin(a) * r * h * 0.5f};
        const float k = 0.8f + 0.55f * ((c.x - at.x) / w + (c.y - at.y) / h) + 0.12f * (rnd(i, 3) - 0.5f);
        clumps[static_cast<size_t>(i)] = {at, lump * (0.34f + 0.2f * rnd(i, 4)), k};
    }
    std::sort(clumps.begin(), clumps.begin() + count, [](const Clump& a, const Clump& b) { return a.k < b.k; });
    for (int i = 0; i < count; ++i) {
        const Clump& l = clumps[static_cast<size_t>(i)];
        disc({l.at.x + l.r * 0.25f, l.at.y + l.r * 0.3f}, l.r, shade(leaf, l.k * 0.72f * tint));
        disc(l.at, l.r * 0.8f, shade(leaf, l.k * tint));
        if (!far) disc({l.at.x - l.r * 0.3f, l.at.y - l.r * 0.34f}, l.r * 0.38f, shade(leaf, l.k * 1.25f * tint));
    }
}

void draw_tree(const engine::TileMap& map, const Tree& t) {
    const Vector2 b = on_terrain(map, t.ground);
    const float s = t.size;
    if (t.stump) {
        DrawEllipse(static_cast<int>(b.x), static_cast<int>(b.y), 2.6f * s * t.girth, 1.3f * s, lit({70, 52, 36, 255}));
        DrawEllipse(static_cast<int>(b.x), static_cast<int>(b.y - 1.5f), 2.2f * s * t.girth, 1.1f * s, lit({176, 150, 110, 255}));
        return;
    }
    const float spread = t.kind == TreeKind::Oak ? 1.5f : t.kind == TreeKind::Poplar ? 0.6f : t.kind == TreeKind::Apple ? 0.8f : 1.0f;
    // Its shadow on the ground, away from the light (upper left).
    DrawEllipse(static_cast<int>(b.x + 5.0f * s), static_cast<int>(b.y + 1.5f * s), 9.0f * s * spread, 3.4f * s,
                lit({16, 24, 12, 70}));
    const float bend = t.bend * s;
    switch (t.kind) {
        case TreeKind::Oak: {
            // Old and thick, short and gnarled, limbs reaching into a wide lumpy crown.
            const Color bark{78, 62, 46, 255};
            const float height = 10.0f * s * t.height;
            const Vector2 top = draw_trunk(b, height, 6.8f * s * t.girth, 3.8f * s * t.girth, bend, bark);
            // Burls on the bark.
            for (const float k : {0.35f, 0.62f}) {
                const float x = b.x + bend * 4.0f * k * (1.0f - k) + bend * 0.3f * k;
                DrawEllipse(static_cast<int>(x + 0.8f * s), static_cast<int>(b.y - height * k), 1.3f * s, 1.0f * s,
                            lit(shade(bark, 0.6f)));
            }
            // Thick limbs forking out, then the wide crown above them.
            const float w = 28.0f * s;
            const Vector2 c{top.x, top.y - 11.0f * s};
            for (const float side : {-1.0f, 1.0f, 0.2f}) {
                const Vector2 elbow{top.x + side * w * 0.18f, top.y - 4.0f * s};
                DrawLineEx(top, elbow, 2.6f * s * t.girth, lit(shade(bark, 0.95f)));
                DrawLineEx(elbow, {c.x + side * w * 0.34f, c.y + 1.0f * s}, 1.8f * s * t.girth, lit(shade(bark, 0.85f)));
            }
            draw_crown(c, w, 14.0f * s, 6.5f * s, {54, 92, 44, 255}, t.tint, t.seed);
            break;
        }
        case TreeKind::Beech: {
            // Tall, slender and smooth grey, an upright crown.
            const Vector2 top = draw_trunk(b, 14.0f * s * t.height, 3.6f * s * t.girth, 2.2f * s, bend, {146, 142, 134, 255});
            draw_crown({top.x, top.y - 8.0f * s}, 16.0f * s, 20.0f * s, 5.5f * s, {70, 112, 50, 255}, t.tint, t.seed);
            break;
        }
        case TreeKind::Broadleaf: {
            // Linden, maple: a brown trunk, a round crown.
            const Vector2 top = draw_trunk(b, 9.5f * s * t.height, 4.0f * s * t.girth, 2.4f * s, bend, {72, 54, 38, 255});
            draw_crown({top.x, top.y - 7.0f * s}, 20.0f * s, 17.0f * s, 6.0f * s, {50, 96, 44, 255}, t.tint, t.seed);
            break;
        }
        case TreeKind::Birch: {
            // Thin and often bowed, white with black marks, a light crown hanging a little.
            const float height = 12.0f * s * t.height;
            const Vector2 top = draw_trunk(b, height, 2.6f * s * t.girth, 1.6f * s, bend, {222, 220, 208, 255});
            for (const float k : {0.25f, 0.5f, 0.72f}) {
                const float x = b.x + bend * 4.0f * k * (1.0f - k) + bend * 0.3f * k;
                const float y = b.y - height * k;
                DrawLineEx({x - 1.0f * s, y}, {x + 0.8f * s, y - 0.4f * s}, 1.0f, lit({40, 40, 38, 255}));
            }
            draw_crown({top.x + 0.5f * s, top.y - 4.0f * s}, 14.0f * s, 14.0f * s, 4.6f * s, {98, 138, 60, 255}, t.tint, t.seed);
            break;
        }
        case TreeKind::Pine: {
            // Old pines: a tall bare reddish trunk, the needles up top; young ones in tiers near the ground.
            const bool old = t.height > 1.0f;
            const float bare = (old ? 11.0f : 4.0f) * s * t.height;
            const Vector2 top = draw_trunk(b, bare + 6.0f * s, 3.0f * s * t.girth, 1.8f * s, bend * 0.5f,
                                           old ? Color{146, 92, 62, 255} : Color{64, 46, 34, 255});
            // Each tier a dark cone under drooping boughs of needle tufts,
            // rows of them widening downwards, lit on the left.
            const Color needles = shade({38, 74, 52, 255}, t.tint);
            const int tiers = old ? 2 : 3;
            for (int k = 0; k < tiers; ++k) {
                const float wid = ((old ? 7.5f : 8.5f) - 2.2f * static_cast<float>(k)) * s;
                const float y0 = b.y - bare - 6.0f * static_cast<float>(k) * s + (old ? 2.0f * s : 0.0f);
                const float apex_y = y0 - (10.0f - static_cast<float>(k)) * s;
                const float x = top.x;
                fill_triangle({x - wid, y0 + 1.0f * s}, {x + wid, y0 + 1.0f * s}, {x, apex_y}, shade(needles, 0.5f));
                constexpr int kRows = 4;
                for (int row = 0; row < kRows; ++row) {
                    const float f = static_cast<float>(row + 1) / kRows;
                    const float y = apex_y + (y0 - apex_y) * f;
                    const int n = row + 2;
                    for (int i = 0; i < n; ++i) {
                        const float u = static_cast<float>(i) / static_cast<float>(n - 1) * 2.0f - 1.0f;  // -1 left .. 1 right
                        const float jitter = (hash_unit(tile_hash(static_cast<int>(t.seed >> 5) + k * 17 + row * 5 + i, i)) - 0.5f) * s;
                        const Vector2 at{x + u * wid * f * 0.85f + jitter, y + std::fabs(u) * 1.4f * s - 1.0f * s};
                        const float r = (1.4f + 0.5f * static_cast<float>(row)) * s * 0.8f;
                        const float light = 1.15f - 0.4f * (u + 1.0f) * 0.5f;
                        disc({at.x + r * 0.25f, at.y + r * 0.3f}, r, shade(needles, light * 0.7f));
                        disc(at, r * 0.75f, shade(needles, light));
                        if (u < 0.2f && !(g_zoom < 0.6f)) disc({at.x - r * 0.3f, at.y - r * 0.3f}, r * 0.35f, shade(needles, light * 1.3f));
                    }
                }
            }
            break;
        }
        case TreeKind::Apple: {
            // Short, the trunk whitewashed a hand high, a round crown with apples.
            const float height = 5.5f * s * t.height;
            const Vector2 top = draw_trunk(b, height, 2.8f * s * t.girth, 1.9f * s, bend * 0.3f, {92, 70, 50, 255});
            DrawLineEx(b, {b.x + bend * 0.2f, b.y - height * 0.45f}, 2.8f * s * t.girth, lit({234, 232, 224, 255}));
            const Vector2 c{top.x, top.y - 5.0f * s};
            draw_crown(c, 14.0f * s, 11.0f * s, 4.4f * s, {72, 114, 50, 255}, t.tint, t.seed);
            for (int k = 0; k < 6; ++k) {
                const float a = hash_unit(tile_hash(static_cast<int>(t.seed >> 2) + k, k * 7)) * 6.2831853f;
                const float r = 0.35f + 0.55f * hash_unit(tile_hash(k * 3, static_cast<int>(t.seed >> 6)));
                disc({c.x + std::cos(a) * r * 6.5f * s, c.y + std::sin(a) * r * 5.0f * s}, 1.1f * s,
                     k % 3 == 0 ? Color{220, 176, 60, 255} : Color{196, 40, 36, 255});
            }
            break;
        }
        case TreeKind::Poplar: {
            // Tall and narrow, along a road or a field.
            const Vector2 top = draw_trunk(b, 5.0f * s, 2.4f * s * t.girth, 1.8f * s, bend, {72, 56, 40, 255});
            // A dark column, clumps of leaves up it, lit on the left.
            const Color leaf = shade({58, 100, 48, 255}, t.tint);
            const float tall = t.height;
            const float half = 13.0f * s * tall;
            const float mid = top.y - half;
            DrawEllipse(static_cast<int>(top.x + 0.6f * s), static_cast<int>(mid), 5.0f * s, half, lit(shade(leaf, 0.5f)));
            constexpr int kClumps = 11;
            for (int i = 0; i < kClumps; ++i) {
                const float f = static_cast<float>(i) / (kClumps - 1);  // 0 at the bottom
                const float y = mid + half * 0.86f - f * half * 1.72f;
                const float along = (y - mid) / half;
                const float wide = 5.0f * s * std::sqrt(std::max(0.08f, 1.0f - along * along));
                const float u = (i % 2 == 0 ? -0.45f : 0.4f) + (hash_unit(tile_hash(static_cast<int>(t.seed >> 4) + i, i * 3)) - 0.5f) * 0.3f;
                const Vector2 at{top.x + u * wide, y};
                const float r = wide * 0.62f + 0.6f * s;
                const float light = u < 0.0f ? 1.1f : 0.78f;
                disc({at.x + r * 0.25f, at.y + r * 0.3f}, r, shade(leaf, light * 0.72f));
                disc(at, r * 0.78f, shade(leaf, light));
                if (u < 0.0f && !(g_zoom < 0.6f)) disc({at.x - r * 0.3f, at.y - r * 0.32f}, r * 0.36f, shade(leaf, 1.35f));
            }
            break;
        }
    }
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

// A barn or a machine shed: one long building with a gable roof and big doors.
void draw_barn(const engine::TileMap& map, const engine::Structure& s, float damage);

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
    bool cross = false;      // a red cross on the roof
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
        case engine::StructureType::Quarters: return {11.0f, {156, 140, 112, 255}, {104, 90, 74, 255}, 1};
        case engine::StructureType::Workshop: return {15.0f, {120, 128, 134, 255}, {84, 90, 96, 255}, 0, true};
        case engine::StructureType::Hospital: return {12.0f, {214, 212, 200, 255}, {180, 178, 168, 255}, 1, false, false, true};
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

void draw_barn(const engine::TileMap& map, const engine::Structure& s, float damage) {
    constexpr float kWall = 15.0f;
    constexpr float kRoof = 12.0f;
    const Rectangle r = footprint(s, 0.1f);
    const Vector2 ground[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}};
    Vector2 base[4];
    Vector2 top[4];
    for (int i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, ground[i]);
        top[i] = {base[i].x, base[i].y - kWall};
    }
    const float soot = 1.0f - 0.45f * damage;
    const Color wall = shade({168, 150, 124, 255}, soot);
    const Color roof = shade({118, 122, 124, 255}, soot);  // corrugated iron
    fill_quad(base[1], base[2], top[2], top[1], wall);
    fill_quad(base[2], base[3], top[3], top[2], shade(wall, 0.72f));
    // The ridge runs along the long side.
    const bool along_x = r.width >= r.height;
    const Vector2 r0 = along_x ? Vector2{r.x, r.y + r.height * 0.5f} : Vector2{r.x + r.width * 0.5f, r.y};
    const Vector2 r1 = along_x ? Vector2{r.x + r.width, r.y + r.height * 0.5f} : Vector2{r.x + r.width * 0.5f, r.y + r.height};
    const Vector2 ridge0 = on_terrain(map, r0, kWall + kRoof);
    const Vector2 ridge1 = on_terrain(map, r1, kWall + kRoof);
    if (along_x) {
        fill_quad(top[0], top[1], ridge1, ridge0, shade(roof, 1.1f));
        fill_triangle(top[1], top[2], ridge1, shade(wall, 0.9f));  // the gable end
        fill_quad(ridge0, ridge1, top[2], top[3], shade(roof, 0.85f));
    } else {
        fill_quad(top[3], top[0], ridge0, ridge1, shade(roof, 1.1f));
        fill_quad(ridge0, top[1], top[2], ridge1, shade(roof, 0.95f));
        fill_triangle(top[2], top[3], ridge1, shade(wall, 0.8f));
    }
    DrawLineV(ridge0, ridge1, lit(shade(roof, 0.6f)));
    // Wide doors in the long wall facing the viewer.
    const Vector2 a = along_x ? base[2] : base[1];
    const Vector2 b = along_x ? base[3] : base[2];
    const Vector2 d0 = lerp(a, b, 0.35f);
    const Vector2 d1 = lerp(a, b, 0.65f);
    fill_quad(d0, d1, {d1.x, d1.y - kWall * 0.8f}, {d0.x, d0.y - kWall * 0.8f}, {60, 54, 46, 255});
}

Color mix(Color a, Color b, float t);  // below, with the ground's colours

// A wooden fence round a rectangle of ground: posts and two rails.
void draw_fence(const engine::TileMap& map, Rectangle r, Color wood) {
    const Vector2 corners[5] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}, {r.x, r.y}};
    for (int side = 0; side < 4; ++side) {
        const Vector2 a = corners[side];
        const Vector2 b = corners[side + 1];
        const float len = std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y));
        const int posts = std::max(1, static_cast<int>(len * 2.0f));
        for (int k = 0; k <= posts; ++k) {
            const Vector2 g{a.x + (b.x - a.x) * static_cast<float>(k) / static_cast<float>(posts),
                            a.y + (b.y - a.y) * static_cast<float>(k) / static_cast<float>(posts)};
            const Vector2 foot = on_terrain(map, g);
            DrawLineEx(foot, {foot.x, foot.y - 6.0f}, 1.2f, lit(shade(wood, 0.8f)));
        }
        for (const float lift : {2.5f, 5.0f}) DrawLineV(on_terrain(map, a, lift), on_terrain(map, b, lift), lit(wood));
    }
}

// A haystack: a golden mound, darker where it's been rained on.
void draw_haystack(const engine::TileMap& map, Vector2 ground, float size) {
    const Vector2 b = on_terrain(map, ground);
    DrawEllipse(static_cast<int>(b.x + 3.0f * size), static_cast<int>(b.y + 1.0f), 7.0f * size, 2.6f * size, lit({20, 24, 12, 60}));
    DrawEllipse(static_cast<int>(b.x), static_cast<int>(b.y - 3.0f * size), 6.0f * size, 4.5f * size, lit({196, 164, 86, 255}));
    DrawEllipse(static_cast<int>(b.x + 1.2f * size), static_cast<int>(b.y - 2.0f * size), 4.0f * size, 3.2f * size, lit({164, 134, 70, 255}));
    DrawEllipse(static_cast<int>(b.x - 1.5f * size), static_cast<int>(b.y - 5.0f * size), 3.0f * size, 2.0f * size, lit({226, 196, 116, 255}));
}

// A water tower (a Rozhnovsky tower): a rusty steel tank on a tall column,
// a ladder up it, a railing round the top; it stands over the whole farm.
void draw_water_tower(const engine::TileMap& map, Vector2 ground) {
    constexpr float kColumn = 52.0f;
    constexpr float kTank = 15.0f;
    const Vector2 b = on_terrain(map, ground);
    DrawEllipse(static_cast<int>(b.x + 18.0f), static_cast<int>(b.y + 4.0f), 20.0f, 3.5f, lit({20, 24, 12, 60}));
    const Color steel{118, 104, 92, 255};
    DrawRectangleRec({b.x - 3.0f, b.y - kColumn, 6.0f, kColumn}, lit(shade(steel, 0.75f)));
    DrawRectangleRec({b.x - 3.0f, b.y - kColumn, 2.5f, kColumn}, lit(steel));
    for (float y = b.y - 4.0f; y > b.y - kColumn; y -= 3.0f) DrawLineV({b.x + 3.0f, y}, {b.x + 5.0f, y}, lit(shade(steel, 0.6f)));
    DrawLineV({b.x + 5.0f, b.y}, {b.x + 5.0f, b.y - kColumn}, lit(shade(steel, 0.6f)));
    const Color tank{134, 76, 56, 255};
    const float top = b.y - kColumn - kTank;
    DrawEllipse(static_cast<int>(b.x), static_cast<int>(b.y - kColumn), 9.0f, 3.0f, lit(shade(tank, 0.7f)));
    DrawRectangleRec({b.x - 9.0f, top, 18.0f, kTank}, lit(shade(tank, 0.85f)));
    DrawRectangleRec({b.x - 9.0f, top, 6.0f, kTank}, lit(shade(tank, 1.15f)));
    DrawLineV({b.x - 9.0f, top + 5.0f}, {b.x + 9.0f, top + 5.0f}, lit(shade(tank, 0.65f)));
    DrawTriangle({b.x - 9.5f, top}, {b.x + 9.5f, top}, {b.x, top - 6.0f}, lit(shade(tank, 0.7f)));
    DrawTriangle({b.x - 9.5f, top}, {b.x, top}, {b.x, top - 6.0f}, lit(shade(tank, 0.95f)));
    DrawLineV({b.x - 10.5f, top - 1.5f}, {b.x + 10.5f, top - 1.5f}, lit(shade(steel, 0.5f)));
}

// A cow: a red steppe cow or a black-and-white one, grazing or looking about.
void draw_cow(const engine::TileMap& map, Vector2 ground, uint32_t h) {
    const Vector2 b = on_terrain(map, ground);
    const float d = (h & 1u) ? 1.0f : -1.0f;  // which way she faces
    const bool pied = (h >> 1) % 3 == 0;
    const bool grazing = (h >> 3) % 2 == 0;
    const Color coat = pied ? Color{36, 34, 34, 255} : Color{132, 66, 40, 255};
    DrawEllipse(static_cast<int>(b.x), static_cast<int>(b.y + 0.5f), 6.0f, 1.8f, lit({20, 24, 12, 70}));
    for (const float x : {-3.6f, -2.2f, 2.2f, 3.6f}) {
        DrawLineEx({b.x + x * d, b.y - 4.0f}, {b.x + x * d, b.y}, 1.1f, lit(shade(coat, x < 0 ? 0.7f : 0.85f)));
    }
    DrawEllipse(static_cast<int>(b.x), static_cast<int>(b.y - 5.5f), 5.0f, 2.6f, lit(coat));
    if (pied) {
        DrawEllipse(static_cast<int>(b.x - 1.8f * d), static_cast<int>(b.y - 6.0f), 1.8f, 1.4f, lit({232, 230, 222, 255}));
        DrawEllipse(static_cast<int>(b.x + 1.6f * d), static_cast<int>(b.y - 4.8f), 1.3f, 1.0f, lit({232, 230, 222, 255}));
    } else {
        DrawEllipse(static_cast<int>(b.x - 0.6f * d), static_cast<int>(b.y - 6.6f), 3.2f, 1.0f, lit(shade(coat, 1.25f)));
    }
    DrawLineEx({b.x - 4.8f * d, b.y - 6.0f}, {b.x - 5.6f * d, b.y - 3.0f}, 0.8f, lit(shade(coat, 0.7f)));  // the tail
    const Vector2 head = grazing ? Vector2{b.x + 6.2f * d, b.y - 2.2f} : Vector2{b.x + 6.4f * d, b.y - 7.2f};
    DrawLineEx({b.x + 4.0f * d, b.y - 6.0f}, head, 2.4f, lit(coat));
    DrawEllipse(static_cast<int>(head.x), static_cast<int>(head.y), 1.8f, 1.3f, lit(shade(coat, 0.9f)));
    DrawCircleV({head.x + 1.2f * d, head.y + 0.4f}, 0.8f, lit(pied ? Color{214, 190, 180, 255} : Color{196, 150, 130, 255}));
    DrawLineV({head.x - 0.6f, head.y - 1.2f}, {head.x - 1.4f, head.y - 2.4f}, lit({226, 220, 200, 255}));
}

// A cowshed: whitewashed brick under a slate roof, a row of small windows,
// ventilation cowls along the ridge of a long one; its yard fenced, haystacks
// in it, cows out in front. A farm's water tower stands between its sheds.
void draw_cowshed(const engine::TileMap& map, const engine::Structure& s, float damage) {
    const Rectangle r = footprint(s, 0.1f);
    const uint32_t h = tile_hash(s.tiles.front().x, s.tiles.front().y);
    const bool long_shed = s.tiles.size() >= engine::kSpaciousTiles;
    const float yard = long_shed ? 0.8f : 0.5f;
    draw_fence(map, {r.x - yard, r.y - yard, r.width + 2 * yard, r.height + 2 * yard}, {126, 100, 70, 255});
    const float kWall = long_shed ? 11.0f : 8.0f;
    const float kRoof = long_shed ? 8.0f : 6.0f;
    const Vector2 ground[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}};
    Vector2 base[4];
    Vector2 top[4];
    for (int i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, ground[i]);
        top[i] = {base[i].x, base[i].y - kWall};
    }
    const float soot = 1.0f - 0.45f * damage;
    const Color wall = shade({216, 212, 198, 255}, soot);
    const Color roof = shade({120, 126, 132, 255}, soot);  // slate
    fill_quad(base[1], base[2], top[2], top[1], wall);
    fill_quad(base[2], base[3], top[3], top[2], shade(wall, 0.74f));
    const bool along_x = r.width >= r.height;
    const Vector2 r0 = along_x ? Vector2{r.x, r.y + r.height * 0.5f} : Vector2{r.x + r.width * 0.5f, r.y};
    const Vector2 r1 = along_x ? Vector2{r.x + r.width, r.y + r.height * 0.5f} : Vector2{r.x + r.width * 0.5f, r.y + r.height};
    const Vector2 ridge0 = on_terrain(map, r0, kWall + kRoof);
    const Vector2 ridge1 = on_terrain(map, r1, kWall + kRoof);
    if (along_x) {
        fill_quad(top[0], top[1], ridge1, ridge0, shade(roof, 1.1f));
        fill_triangle(top[1], top[2], ridge1, shade(wall, 0.9f));
        fill_quad(ridge0, ridge1, top[2], top[3], shade(roof, 0.85f));
    } else {
        fill_quad(top[3], top[0], ridge0, ridge1, shade(roof, 1.1f));
        fill_quad(ridge0, top[1], top[2], ridge1, shade(roof, 0.95f));
        fill_triangle(top[2], top[3], ridge1, shade(wall, 0.8f));
    }
    // Slate ribs down the roof, cowls on the ridge.
    const float length = along_x ? r.width : r.height;
    const int ribs = std::max(3, static_cast<int>(length + 1.0f));
    for (int k = 1; k < ribs; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(ribs);
        DrawLineV(lerp(ridge0, ridge1, t), lerp(along_x ? top[3] : top[1], top[2], t), lit(shade(roof, 0.7f)));
    }
    if (long_shed) {
        for (const float t : {0.3f, 0.7f}) {
            const Vector2 c = lerp(ridge0, ridge1, t);
            DrawRectangleRec({c.x - 2.0f, c.y - 4.0f, 4.0f, 4.0f}, lit(shade(roof, 0.8f)));
            DrawRectangleRec({c.x - 3.0f, c.y - 5.0f, 6.0f, 1.5f}, lit(shade(roof, 0.6f)));
        }
    }
    // Small windows along the long wall facing the viewer, a door in a small shed.
    const Vector2 a = along_x ? base[2] : base[1];
    const Vector2 b = along_x ? base[3] : base[2];
    const int windows = long_shed ? 7 : 1;
    for (int k = 1; k <= windows; ++k) {
        const Vector2 p = lerp(a, b, static_cast<float>(k) / static_cast<float>(windows + 1));
        DrawRectangleRec({p.x - 1.5f, p.y - kWall * 0.7f, 3.0f, 2.5f}, lit({70, 76, 82, 255}));
    }
    if (!long_shed) {
        const Vector2 door = lerp(a, b, 0.25f);
        DrawRectangleRec({door.x - 1.5f, door.y - 6.0f, 3.5f, 6.0f}, lit({86, 64, 44, 255}));
    }
    // The yard: haystacks by the fence, the cows out in front.
    draw_haystack(map, {r.x + r.width + yard * 0.4f, r.y - yard * 0.4f}, (long_shed ? 1.0f : 0.8f) + 0.2f * static_cast<float>(h & 1));
    if (long_shed) draw_haystack(map, {r.x - 0.35f, r.y + r.height + 0.2f}, 0.9f);
    const int cows = long_shed ? 4 : 1 + static_cast<int>((h >> 5) & 1u);
    for (int k = 0; k < cows; ++k) {
        const uint32_t hk = tile_hash(static_cast<int>(h >> 8) + k, k * 13);
        const float along = (static_cast<float>(k) + 0.3f + 0.4f * hash_unit(hk)) / static_cast<float>(cows);
        const float out = 0.25f + (yard - 0.4f) * hash_unit(hk >> 8);
        const Vector2 g = along_x ? Vector2{r.x + r.width * along, r.y + r.height + out}
                                  : Vector2{r.x + r.width + out, r.y + r.height * along};
        draw_cow(map, g, hk);
    }
    // A farm's water tower: by the shed that has another one in front of it.
    if (long_shed) {
        const Rectangle whole = footprint(s, 0.0f);
        const int fx = static_cast<int>(whole.x + whole.width * 0.5f);
        const int fy = static_cast<int>(whole.y + whole.height) + 2;
        if (map.contains_tile(fx, fy) && (map.terrain(fx, fy) == engine::Terrain::House || map.terrain(fx, fy) == engine::Terrain::Ruins)) {
            draw_water_tower(map, {r.x + r.width + 0.6f, r.y + r.height + 1.0f});
        }
    }
}

// A chicken coop: a little plank shed under a lean-to roof, a run of wire
// netting beside it with the hens pecking about.
void draw_coop(const engine::TileMap& map, const engine::Structure& s, float damage) {
    const Rectangle r = footprint(s, 0.15f);
    const uint32_t h = tile_hash(s.tiles.front().x, s.tiles.front().y);
    // The run, in front.
    const Rectangle run{r.x, r.y + r.height + 0.1f, r.width, 0.9f};
    for (int k = 0; k < 7; ++k) {
        const Vector2 p = on_terrain(map, {run.x + 0.1f + (run.width - 0.2f) * hash_unit(h >> k), run.y + 0.1f + 0.7f * hash_unit(h >> (k + 9))});
        DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y - 1.5f), 1.8f, 1.2f, lit(k % 3 == 0 ? Color{150, 96, 50, 255} : Color{236, 232, 222, 255}));
        DrawCircleV({p.x + 1.4f, p.y - 2.4f}, 0.6f, lit({200, 40, 36, 255}));
    }
    draw_fence(map, run, {176, 176, 170, 255});
    constexpr float kWall = 7.0f;
    const Vector2 ground[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}};
    Vector2 base[4];
    Vector2 top[4];
    for (int i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, ground[i]);
        // The lean-to: the back wall higher than the front.
        top[i] = {base[i].x, base[i].y - kWall - (i < 2 ? 3.0f : 0.0f)};
    }
    const float soot = 1.0f - 0.45f * damage;
    const Color planks = shade({152, 128, 94, 255}, soot);
    fill_quad(base[1], base[2], top[2], top[1], planks);
    fill_quad(base[2], base[3], top[3], top[2], shade(planks, 0.74f));
    fill_quad(top[0], top[1], top[2], top[3], shade({92, 86, 80, 255}, soot));
    const Vector2 door = lerp(base[2], base[3], 0.5f);
    DrawRectangleRec({door.x - 2.0f, door.y - 5.0f, 3.0f, 5.0f}, lit({70, 54, 40, 255}));
}

// Roads, fields, bogs and craters: marks on the ground of a tile whose
// corners on screen are top, right, bottom, left.
float field(Vector2 p, int seed);  // with the ground's colours, below
float smooth01(float t);

float noise_at(Vector2 p, float scale, int seed);

// How far along the fields are, in patches, the same for the ground and for
// what grows on it. Sunflowers: 1 in bloom, down to 0 dried black on their
// stalks (fields left standing). Wheat: 1 where the combine has been (in
// straight strips along the field, where the harvest has got to), 0 standing;
// unripe (greener) in patches; flattened by rain and wind in others.
float sunflower_bloom(Vector2 p) { return smooth01((field(p, 41) - 0.2f) / 0.2f); }
float wheat_cut(Vector2 p) {
    const int strip = static_cast<int>(std::floor(p.y)) % 9;
    return strip < 3 && noise_at(p, 20.0f, 47) > 0.55f ? 1.0f : 0.0f;
}
float wheat_unripe(Vector2 p) { return smooth01((field(p, 23) - 0.6f) / 0.2f); }
bool wheat_lodged(Vector2 p) { return field(p, 53) > 0.7f; }

// A sunflower from its foot, `tall` pixels up to the head, `head` its radius.
// `stage`: over 0.6 in bloom, the head up, all yellow petals; down to 0.3
// ripening, the head heavy and turned down, the petals going, the lowest
// leaf yellowing; below that dried black on its stalk, the leaves shrivelled.
// Some show the back of the head. `far`: just the stalk and the head.
void draw_sunflower(Vector2 base, float tall, float head, float stage, float lean, bool back, bool far) {
    const bool bloom = stage > 0.6f;
    const bool dry = stage < 0.3f;
    const Color stalk = dry ? Color{104, 82, 50, 255} : bloom ? Color{66, 96, 38, 255} : Color{92, 104, 46, 255};
    const float droop = bloom ? 0.0f : dry ? 1.0f : 0.55f;  // how far the head hangs
    const Vector2 neck{base.x + lean, base.y - tall};
    const Vector2 hc{neck.x + head * 0.9f * droop, neck.y + head * 1.3f * droop};
    DrawLineEx(base, neck, 1.2f, lit(stalk));
    if (droop > 0.0f) DrawLineEx(neck, hc, 1.0f, lit(stalk));
    if (!far) {
        for (int i = 0; i < 3; ++i) {
            const float side = i % 2 == 0 ? -1.0f : 1.0f;
            const float k = 0.3f + 0.2f * static_cast<float>(i);
            const Vector2 a{base.x + lean * k, base.y - tall * k};
            if (dry) {
                DrawLineEx(a, {a.x + side * 1.6f, a.y + 3.2f}, 1.2f, lit({80, 60, 38, 255}));  // shrivelled, hanging
                continue;
            }
            const Color leaf = i == 0 && !bloom ? Color{150, 144, 62, 255} : Color{70, 104, 40, 255};
            const Vector2 tip{a.x + side * 4.2f, a.y + 1.6f};
            fill_triangle(a, {a.x + side * 2.0f, a.y - 1.2f}, tip, shade(leaf, 1.15f));
            fill_triangle(a, tip, {a.x + side * 2.2f, a.y + 1.5f}, shade(leaf, 0.78f));
        }
    }
    if (dry) {
        disc(hc, head * 0.85f, {46, 34, 24, 255});
        if (!far) disc({hc.x - head * 0.3f, hc.y - head * 0.3f}, head * 0.35f, {72, 54, 36, 255});
        return;
    }
    const Color petal = bloom ? Color{240, 194, 36, 255} : Color{200, 158, 58, 255};
    if (back) {
        // Turned away: the green back of the head, the petals' tips round it.
        if (!far) {
            for (int k = 0; k < 5; ++k) {
                const float a = -2.6f + 0.55f * static_cast<float>(k);
                disc({hc.x + std::cos(a) * head, hc.y + std::sin(a) * head * 0.8f}, head * 0.36f, petal);
            }
        }
        disc(hc, head * 0.85f, {88, 112, 44, 255});
        return;
    }
    disc(hc, head * (bloom ? 0.95f : 0.8f), petal);
    if (!far) {
        const int petals = bloom ? 7 : 4;
        for (int k = 0; k < petals; ++k) {
            const float a = static_cast<float>(k) * 6.2831853f / static_cast<float>(petals) + head;
            disc({hc.x + std::cos(a) * head * 0.95f, hc.y + std::sin(a) * head * 0.8f}, head * 0.38f,
                 k % 3 == 0 ? shade(petal, 0.88f) : petal);
        }
    }
    disc(hc, head * 0.58f, bloom ? Color{96, 62, 30, 255} : Color{78, 62, 36, 255});
    if (!far) disc({hc.x + head * 0.15f, hc.y + head * 0.15f}, head * 0.32f, {58, 38, 20, 255});
}

// A round bale of straw lying on its side: the lit end, the rolled side.
void draw_bale(Vector2 b) {
    DrawEllipse(static_cast<int>(b.x + 4.0f), static_cast<int>(b.y + 0.5f), 8.0f, 2.2f, lit({20, 24, 12, 70}));
    DrawRectangleRec({b.x - 3.5f, b.y - 7.4f, 9.0f, 7.4f}, lit({172, 142, 74, 255}));
    DrawLineEx({b.x - 3.5f, b.y - 6.6f}, {b.x + 5.5f, b.y - 6.6f}, 1.4f, lit({214, 188, 112, 255}));
    DrawLineV({b.x - 3.5f, b.y - 0.5f}, {b.x + 5.5f, b.y - 0.5f}, lit({128, 104, 56, 255}));
    DrawEllipse(static_cast<int>(b.x - 3.5f), static_cast<int>(b.y - 3.7f), 2.6f, 3.7f, lit({224, 198, 126, 255}));
    DrawEllipseLines(static_cast<int>(b.x - 3.5f), static_cast<int>(b.y - 3.7f), 1.5f, 2.2f, lit({176, 146, 78, 255}));
    DrawEllipseLines(static_cast<int>(b.x - 3.5f), static_cast<int>(b.y - 3.7f), 0.6f, 0.9f, lit({176, 146, 78, 255}));
}

void draw_ground_detail(const engine::TileMap& map, int tx, int ty, engine::Terrain terrain, Vector2 top, Vector2 right,
                        Vector2 bottom, Vector2 left) {
    const auto fx = static_cast<float>(tx);
    const auto fy = static_cast<float>(ty);
    const uint32_t h = tile_hash(tx, ty);
    auto at = [&](float u, float v, float lift = 0.0f) { return on_terrain(map, {fx + u, fy + v}, lift); };
    auto same = [&](int x, int y) {
        return map.contains_tile(x, y) && (map.terrain(x, y) == terrain ||
                                           (terrain == engine::Terrain::Road && map.terrain(x, y) == engine::Terrain::Bridge));
    };
    const bool along_x = same(tx - 1, ty) || same(tx + 1, ty);
    switch (terrain) {
        case engine::Terrain::Road: {
            // Concrete slabs: seams across, a pale edge.
            const Color seam = lit({70, 72, 74, 255});
            if (along_x) {
                DrawLineV(lerp(top, right, 0.5f), lerp(left, bottom, 0.5f), seam);
            } else {
                DrawLineV(lerp(top, left, 0.5f), lerp(right, bottom, 0.5f), seam);
            }
            break;
        }
        case engine::Terrain::DirtRoad: {
            // Two ruts along the road.
            const Color rut = lit({104, 84, 58, 255});
            for (const float k : {0.35f, 0.65f}) {
                if (along_x) {
                    DrawLineEx(at(0.0f, k), at(1.0f, k), 1.5f, rut);
                } else {
                    DrawLineEx(at(k, 0.0f), at(k, 1.0f), 1.5f, rut);
                }
            }
            break;
        }
        case engine::Terrain::Plowed: {
            // Furrows.
            const Color furrow = lit({84, 70, 50, 255});
            for (int i = 1; i <= 4; ++i) {
                const float k = static_cast<float>(i) / 5.0f;
                DrawLineV(at(k, 0.0f), at(k, 1.0f), furrow);
            }
            break;
        }
        case engine::Terrain::Crops: {
            // Rows of sunflowers, each its own height and head, swaying a
            // little; in bloom, ripening or dried black, field patch by patch.
            // Now and then one missing from the row.
            const auto t = static_cast<float>(GetTime());
            const bool far = g_zoom < 0.6f;
            const float taller = 1.0f + 0.25f * (field({fx, fy}, 43) - 0.5f);  // some fields grew taller
            for (int row = 0; row < 3; ++row) {
                for (int k = 0; k < 3; ++k) {
                    const uint32_t hk = tile_hash(tx * 3 + k, ty * 5 + row);
                    if (hk % 23 == 0) continue;
                    const float u = 0.18f + 0.32f * static_cast<float>(k) + (hash_unit(hk) - 0.5f) * 0.12f;
                    const float v = 0.18f + 0.32f * static_cast<float>(row) + (hash_unit(hk >> 4) - 0.5f) * 0.06f;
                    const Vector2 g = along_x ? Vector2{fx + u, fy + v} : Vector2{fx + v, fy + u};
                    const float stage = sunflower_bloom(g) + (hash_unit(hk >> 8) - 0.5f) * 0.25f;
                    const float tall = (8.0f + 3.5f * hash_unit(hk >> 12)) * taller;
                    const float head = 2.1f + 0.9f * hash_unit(hk >> 16);
                    const float sway = stage > 0.3f ? 0.5f * std::sin(t * 1.1f + (g.x + g.y) * 0.45f) : 0.0f;
                    const float lean = 0.8f + (hash_unit(hk >> 20) - 0.5f) * 1.4f + sway;
                    draw_sunflower(at(g.x - fx, g.y - fy), tall, head, stage, lean, (hk >> 24) % 7 == 0, far);
                }
            }
            break;
        }
        case engine::Terrain::Wheat: {
            const Vector2 mid{fx + 0.5f, fy + 0.5f};
            auto spot = [&](float u, float v) { return along_x ? at(u, v) : at(v, u); };
            const bool far = g_zoom < 0.6f;
            if (wheat_cut(mid) > 0.5f) {
                // Cut: stubble in rows, a swath of straw along every other
                // row, a round bale here and there.
                for (int row = 0; row < 5 && !far; ++row) {
                    for (int k = 0; k < 6; ++k) {
                        const Vector2 b = at(0.08f + 0.17f * static_cast<float>(k) + hash_unit(h >> (row + k)) * 0.04f,
                                             0.1f + 0.2f * static_cast<float>(row));
                        DrawLineEx(b, {b.x, b.y - 2.0f}, 1.2f, lit({148, 126, 74, 255}));
                        DrawLineV({b.x + 0.5f, b.y - 2.0f}, {b.x + 0.5f, b.y - 1.0f}, lit({214, 196, 136, 255}));
                    }
                }
                if (ty % 2 == 0) {
                    // The swath the combine left, along the strip.
                    const float wob = (hash_unit(h >> 3) - 0.5f) * 0.1f;
                    DrawLineEx(at(0.0f, 0.5f + wob, 0.5f), at(0.5f, 0.52f - wob, 0.5f), 4.0f, lit({170, 142, 80, 255}));
                    DrawLineEx(at(0.5f, 0.52f - wob, 0.5f), at(1.0f, 0.5f + wob, 0.5f), 4.0f, lit({170, 142, 80, 255}));
                    DrawLineEx(at(0.0f, 0.48f + wob, 1.5f), at(0.5f, 0.5f - wob, 1.5f), 2.2f, lit({224, 200, 128, 255}));
                    DrawLineEx(at(0.5f, 0.5f - wob, 1.5f), at(1.0f, 0.48f + wob, 1.5f), 2.2f, lit({224, 200, 128, 255}));
                } else if (h % 5 == 0) {
                    draw_bale(at(0.3f + 0.4f * hash_unit(h >> 5), 0.35f + 0.3f * hash_unit(h >> 9)));
                }
                break;
            }
            // Standing: every fourth row of tiles the tramlines the tractor
            // left; ears on their stalks with their awns, the wind running over
            // them in waves; greener where it's unripe; flattened in patches.
            if ((along_x ? ty : tx) % 4 == 0) {
                for (const float v : {0.38f, 0.62f}) DrawLineEx(spot(0.0f, v), spot(1.0f, v), 1.6f, lit({156, 124, 58, 255}));
            }
            const float green = wheat_unripe(mid);
            const Color ripe = mix({188, 152, 66, 255}, {170, 162, 84, 255}, green);
            const Color light = mix({242, 216, 132, 255}, {204, 206, 128, 255}, green);
            const auto t = static_cast<float>(GetTime());
            if (wheat_lodged(mid)) {
                const float swirl = field(mid, 59) * 3.0f;
                for (int k = 0; k < 9; ++k) {
                    const Vector2 b = spot(0.1f + 0.8f * hash_unit(h >> k), 0.1f + 0.8f * hash_unit(h >> (k + 9)));
                    const float a = swirl + (hash_unit(h >> (k + 3)) - 0.5f) * 0.6f;
                    DrawLineEx(b, {b.x + std::cos(a) * 4.5f, b.y + std::sin(a) * 1.8f}, 1.5f, lit(shade(ripe, 0.9f)));
                }
            }
            const int rows = wheat_lodged(mid) ? 2 : 5;
            for (int row = 0; row < rows; ++row) {
                const float v = 0.1f + (rows == 5 ? 0.2f : 0.45f) * static_cast<float>(row);
                for (int k = 0; k < (far ? 3 : 5); ++k) {
                    const uint32_t hk = tile_hash(tx * 5 + k, ty * 7 + row);
                    const float u = 0.1f + (far ? 0.3f : 0.2f) * static_cast<float>(k) + hash_unit(hk) * 0.08f;
                    const float wave = 0.5f + 0.5f * std::sin(t * 1.4f - (fx + u + fy + v) * 0.55f);
                    const Color ear = mix(ripe, light, wave * 0.8f + 0.2f * hash_unit(hk >> 5));
                    const Vector2 base = spot(u, v);
                    const float lean = (wave - 0.5f) * 1.6f + (hash_unit(hk >> 9) - 0.5f) * 0.8f;
                    const float tall = 3.4f + 1.4f * hash_unit(hk >> 13);
                    const Vector2 tip{base.x + lean, base.y - tall};
                    DrawLineV(base, tip, lit(shade(ear, 0.78f)));
                    DrawLineEx({tip.x - lean * 0.1f, tip.y + 1.8f}, tip, 1.9f, lit(ear));
                    if (!far) DrawLineV(tip, {tip.x + lean * 0.4f + 0.3f, tip.y - 1.6f}, lit(shade(light, 1.05f)));
                }
            }
            // Poppies, cornflowers and camomile, thick along the edge of the field.
            const bool edge = !same(tx - 1, ty) || !same(tx + 1, ty) || !same(tx, ty - 1) || !same(tx, ty + 1);
            const int flowers = far ? 0 : edge ? 3 : (h >> 7) % 5 == 0 ? 1 : 0;
            for (int k = 0; k < flowers; ++k) {
                const uint32_t hk = tile_hash(tx * 11 + k, ty * 3 - k);
                const Vector2 f = at(0.1f + 0.8f * hash_unit(hk), 0.1f + 0.8f * hash_unit(hk >> 8), 3.5f);
                const int kind = static_cast<int>((hk >> 16) % 4);
                if (kind <= 1) {
                    disc(f, 1.5f, {208, 36, 28, 255});
                    DrawCircleV({f.x + 0.3f, f.y + 0.2f}, 0.5f, lit({40, 20, 20, 255}));
                } else if (kind == 2) {
                    disc(f, 1.3f, {72, 112, 206, 255});
                } else {
                    disc(f, 1.3f, {238, 236, 226, 255});
                    DrawCircleV(f, 0.5f, lit({230, 190, 50, 255}));
                }
            }
            break;
        }
        case engine::Terrain::Orchard: {
            // The grass mown between the rows of trees, a windfall apple or two.
            DrawLineEx(at(0.0f, 0.02f), at(1.0f, 0.02f), 5.0f, lit({112, 136, 72, 255}));
            for (int k = 0; k < 2; ++k) {
                if ((h >> (k * 5)) % 3 != 0) continue;
                const Vector2 p = at(0.1f + 0.8f * static_cast<float>((h >> (k * 7 + 3)) & 15) / 15.0f, 0.3f + 0.4f * static_cast<float>(k));
                DrawCircleV(p, 1.0f, lit({186, 44, 38, 255}));
            }
            break;
        }
        case engine::Terrain::Garden: {
            // Beds of potatoes in rows, a row of cabbages.
            for (int row = 0; row < 4; ++row) {
                const float v = 0.14f + 0.24f * static_cast<float>(row);
                const bool cabbage = row == static_cast<int>(h % 4);
                for (int k = 0; k < 4; ++k) {
                    const float u = 0.14f + 0.24f * static_cast<float>(k);
                    const Vector2 p = along_x ? at(u, v) : at(v, u);
                    if (cabbage) {
                        DrawCircleV({p.x, p.y - 1.2f}, 2.0f, lit({150, 186, 120, 255}));
                        DrawCircleV({p.x - 0.4f, p.y - 1.6f}, 0.9f, lit({190, 214, 160, 255}));
                    } else {
                        DrawCircleV({p.x, p.y - 1.4f}, 1.9f, lit({62, 104, 44, 255}));
                        DrawCircleV({p.x - 0.5f, p.y - 2.0f}, 0.9f, lit({92, 136, 60, 255}));
                    }
                }
            }
            break;
        }
        case engine::Terrain::Swamp: {
            // Standing water and reeds.
            fill_ground_ellipse(at(0.35f + 0.1f * static_cast<float>(h & 3) / 3.0f, 0.4f), 0.18f, {52, 74, 82, 220});
            fill_ground_ellipse(at(0.7f, 0.7f - 0.1f * static_cast<float>((h >> 2) & 3) / 3.0f), 0.12f, {52, 74, 82, 200});
            for (int i = 0; i < 4; ++i) {
                const Vector2 base = at(0.2f + 0.2f * static_cast<float>(i), 0.8f - 0.15f * static_cast<float>((h >> i) & 3));
                DrawLineV(base, {base.x + 1.0f, base.y - 6.0f}, lit({92, 116, 60, 255}));
            }
            break;
        }
        case engine::Terrain::Riverbed: {
            // Pebbles on the sand.
            for (int i = 0; i < 5; ++i) {
                const float u = 0.1f + 0.8f * static_cast<float>((h >> (i * 3)) & 7) / 7.0f;
                const float v = 0.1f + 0.8f * static_cast<float>((h >> (i * 3 + 16)) & 7) / 7.0f;
                DrawCircleV(at(u, v), 1.2f, lit({118, 108, 92, 255}));
            }
            break;
        }
        case engine::Terrain::Crater: {
            fill_ground_ellipse(at(0.5f, 0.5f), 0.32f, {120, 108, 88, 255});  // thrown-up earth
            fill_ground_ellipse(at(0.5f, 0.5f), 0.2f, {58, 50, 42, 255});    // the hole
            break;
        }
        default:
            break;
    }
    (void)top;
    (void)right;
    (void)bottom;
    (void)left;
}

// --- Ground variety, drawn like AoE II's ---------------------------------------

float hash01(uint32_t h) { return static_cast<float>(h & 0xFFFF) / 65536.0f; }

float smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Smooth value noise over the map, in [0, 1): patches about `scale` tiles across.
float noise_at(Vector2 p, float scale, int seed) {
    const float x = p.x / scale;
    const float y = p.y / scale;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = smooth01(x - static_cast<float>(x0));
    const float fy = smooth01(y - static_cast<float>(y0));
    auto v = [seed](int ix, int iy) { return hash01(tile_hash(ix + seed * 1013, iy - seed * 7919)); };
    const float a = v(x0, y0) + (v(x0 + 1, y0) - v(x0, y0)) * fx;
    const float b = v(x0, y0 + 1) + (v(x0 + 1, y0 + 1) - v(x0, y0 + 1)) * fx;
    return a + (b - a) * fy;
}

// Big patches with smaller ones in them.
float field(Vector2 p, int seed) { return 0.7f * noise_at(p, 7.0f, seed) + 0.3f * noise_at(p, 2.5f, seed + 1); }

Color mix(Color a, Color b, float t) {
    auto channel = [t](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t);
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a)};
}

bool is_grass(engine::Terrain t) {
    return t == engine::Terrain::Grass || t == engine::Terrain::Wire || t == engine::Terrain::Hedgehogs;
}

// The ground's own colour at a point: grass goes from dark green through lush
// to dry and yellowing in patches a few tiles across; the rest varies a little.
constexpr Color kBareEarth{126, 106, 74, 255};
constexpr float kBareFrom = 0.64f;  // where the trodden patches begin in their field

Color ground_colour(engine::Terrain t, Vector2 p) {
    const float tone = field(p, 1);
    if (is_grass(t)) {
        constexpr Color kDark{68, 102, 52, 255};
        constexpr Color kLush{86, 118, 62, 255};
        constexpr Color kDry{124, 126, 72, 255};
        const Color grass = tone < 0.45f ? mix(kDark, kLush, smooth01(tone / 0.45f))
                                         : mix(kLush, kDry, smooth01((tone - 0.55f) / 0.3f));
        // Bare trodden earth in patches of their own.
        return mix(grass, kBareEarth, 0.85f * smooth01((field(p, 9) - kBareFrom) / 0.1f));
    }
    if (t == engine::Terrain::Crops) {
        // Under the sunflowers, their broad leaves; brown where they dried on the stalk.
        const Color leaves = mix(shade({92, 110, 48, 255}, 0.9f + 0.2f * tone), {128, 128, 60, 255},
                                 0.35f * smooth01((field(p, 29) - 0.55f) / 0.2f));
        return mix({96, 78, 50, 255}, leaves, sunflower_bloom(p));
    }
    if (t == engine::Terrain::Wheat) {
        // Ripe gold, paler and greener in patches; the pale stubble where it's cut.
        const Color standing = mix(shade(theme::terrain_color(t), 0.92f + 0.16f * tone), {170, 164, 86, 255}, 0.4f * wheat_unripe(p));
        return mix(standing, shade({184, 168, 112, 255}, 0.92f + 0.16f * tone), wheat_cut(p));
    }
    if (t == engine::Terrain::Slag) {
        // Black rock (burnt rusty red up the top: see paint_ground).
        return shade(theme::terrain_color(t), 0.9f + 0.2f * tone);
    }
    if (t == engine::Terrain::Chalk) {
        // White, greyer where it's washed down, grass taking hold here and there.
        const float grass = smooth01((field(p, 17) - 0.62f) / 0.12f);
        return mix(shade(theme::terrain_color(t), 0.92f + 0.14f * tone), {120, 132, 88, 255}, 0.55f * grass);
    }
    return shade(theme::terrain_color(t), 0.94f + 0.12f * tone);
}

// Where two kinds of ground meet, the one of the higher rank reaches over the
// line in a ragged strip: grass into fields, tracks and road verges, the
// forest floor into the grass. -1: straight edges (water has shores instead;
// buildings, works and rails keep their lines).
int reach_rank(engine::Terrain t) {
    using engine::Terrain;
    switch (t) {
        case Terrain::Road: return 1;
        case Terrain::Riverbed: return 2;
        case Terrain::Swamp: return 3;
        case Terrain::Crater: return 5;
        case Terrain::Plowed:
        case Terrain::Garden: return 6;
        case Terrain::Crops:
        case Terrain::Wheat: return 7;
        case Terrain::DirtRoad: return 8;
        case Terrain::Urban:
        case Terrain::Ruins: return 9;
        case Terrain::Trail: return 10;
        case Terrain::Grass:
        case Terrain::Wire:
        case Terrain::Hedgehogs:
        case Terrain::Orchard: return 11;
        case Terrain::Forest:
        case Terrain::Chalk: return 12;
        case Terrain::Rock: return 13;
        case Terrain::Slag: return 14;
        default: return -1;
    }
}

// A triangle shaded corner by corner, batched with raylib's own shapes;
// either winding. Its grain: the one of what is being drawn, or corner by corner.
void gradient_triangle(Vector2 a, Vector2 b, Vector2 c, Color ca, Color cb, Color cc, const Vector3* grain) {
    Vector3 g[3] = {g_grain, g_grain, g_grain};
    if (grain) g[0] = grain[0], g[1] = grain[1], g[2] = grain[2];
    const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross > 0) {
        std::swap(b, c);
        std::swap(cb, cc);
        std::swap(g[1], g[2]);
    }
    const Texture2D shapes = GetShapesTexture();
    const Rectangle r = GetShapesTextureRectangle();
    const float u = (r.x + r.width * 0.5f) / static_cast<float>(shapes.width);
    const float v = (r.y + r.height * 0.5f) / static_cast<float>(shapes.height);
    rlSetTexture(shapes.id);
    rlBegin(RL_QUADS);
    const Vector2 p[4] = {a, b, c, c};
    const Color k[4] = {ca, cb, cc, cc};
    const Vector3 n[4] = {g[0], g[1], g[2], g[2]};
    for (int i = 0; i < 4; ++i) {
        rlNormal3f(n[i].x, n[i].y, n[i].z);
        rlColor4ub(k[i].r, k[i].g, k[i].b, k[i].a);
        rlTexCoord2f(u, v);
        rlVertex2f(p[i].x, p[i].y);
    }
    rlEnd();
    rlSetTexture(0);
    rlNormal3f(g_grain.x, g_grain.y, g_grain.z);
}

void gradient_quad(Vector2 a, Vector2 b, Vector2 c, Vector2 d, Color ca, Color cb, Color cc, Color cd,
                   const Vector3* grain = nullptr) {
    if (!grain) {
        gradient_triangle(a, b, c, ca, cb, cc);
        gradient_triangle(a, c, d, ca, cc, cd);
        return;
    }
    const Vector3 first[3] = {grain[0], grain[1], grain[2]};
    const Vector3 second[3] = {grain[0], grain[2], grain[3]};
    gradient_triangle(a, b, c, ca, cb, cc, first);
    gradient_triangle(a, c, d, ca, cc, cd, second);
}

// The grain of each kind of ground: black scree coarse and full of gravel,
// earth and tracks gritty, grass and crops mottled, water barely.
Grain grain_of(engine::Terrain t) {
    using engine::Terrain;
    switch (t) {
        case Terrain::Slag: return {1.3f, 0.8f, 1.4f};
        case Terrain::Chalk: return {0.6f, 0.7f, 0.3f};
        case Terrain::Rock: return {0.8f, 0.6f, 0.6f};
        case Terrain::Water: return {0.15f, 0.35f, 0.0f};
        case Terrain::Swamp: return {0.5f, 0.8f, 0.1f};
        case Terrain::Riverbed:
        case Terrain::DirtRoad:
        case Terrain::Trail:
        case Terrain::Crater: return {0.9f, 0.6f, 0.6f};
        case Terrain::Plowed:
        case Terrain::Garden: return {0.9f, 0.6f, 0.4f};
        case Terrain::Road:
        case Terrain::Airstrip: return {0.6f, 0.6f, 0.5f};
        case Terrain::Wheat: return {0.9f, 0.4f, 0.1f};
        case Terrain::Crops:
        case Terrain::Orchard: return {0.6f, 0.8f, 0.05f};
        case Terrain::Urban:
        case Terrain::Ruins: return {0.7f, 0.6f, 0.35f};
        default: return {0.6f, 0.62f, 0.05f};  // grass, the forest floor
    }
}
Vector3 grain_normal(Grain g) { return {g.fine, g.mottle, 1.0f - g.speck}; }

// How high a structure is drawn above its tiles, pixels: what a click on it may hit.
float drawn_height(const engine::Structure& s) {
    switch (s.type) {
        case engine::StructureType::House: return s.tiles.size() >= engine::kSpaciousTiles ? 28.0f : 23.0f;
        case engine::StructureType::Apartment: return 74.0f;
        case engine::StructureType::CellTower: return 92.0f;
        case engine::StructureType::GasStation: return 30.0f;
        case engine::StructureType::Elevator: return 112.0f;
        case engine::StructureType::Pillbox: return 10.0f;
        case engine::StructureType::Bridge:
        case engine::StructureType::Airfield:
        case engine::StructureType::Dugout: return 4.0f;
        default:
            if (engine::is_fieldwork(s.type) || engine::is_obstacle(s.type)) return 4.0f;
            return style_of(s.type).wall + 12.0f;  // the roof edge and the flag above it
    }
}

// A five-storey panel block: rows of windows, a flat roof.
void draw_apartment(const engine::TileMap& map, const engine::Structure& s, float damage) {
    constexpr float kWall = 62.0f;
    constexpr int kFloors = 5;
    const Rectangle r = footprint(s, 0.06f);
    const Vector2 ground[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}};
    Vector2 base[4];
    Vector2 top[4];
    for (int i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, ground[i]);
        top[i] = {base[i].x, base[i].y - kWall};
    }
    const float soot = 1.0f - 0.45f * damage;
    const Color wall = shade({186, 180, 166, 255}, soot);
    fill_quad(base[1], base[2], top[2], top[1], wall);
    fill_quad(base[2], base[3], top[3], top[2], shade(wall, 0.72f));
    fill_quad(top[0], top[1], top[2], top[3], shade({120, 116, 110, 255}, soot));
    for (int i = 0; i < 4; ++i) DrawLineV(top[i], top[(i + 1) % 4], lit(shade(wall, 0.5f)));
    for (int side = 1; side <= 2; ++side) {
        const Vector2 a = base[side];
        const Vector2 b = base[(side + 1) % 4];
        const int columns = static_cast<int>(std::hypot(b.x - a.x, b.y - a.y) / 10.0f);
        for (int floor = 0; floor < kFloors; ++floor) {
            const float lift = kWall * (0.12f + 0.17f * static_cast<float>(floor));
            for (int i = 1; i < columns; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(columns);
                const Vector2 p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                DrawRectangleRec({p.x - 2.0f, p.y - lift - 6.0f, 4.0f, 5.0f}, lit({58, 66, 74, 255}));
            }
        }
    }
}

// A lattice mast with antenna panels near the top and a red light.
void draw_cell_tower(const engine::TileMap& map, const engine::Structure& s, float damage) {
    constexpr float kHeight = 90.0f;
    const Vector2 c = to_vector2(s.center);
    const Vector2 left = on_terrain(map, {c.x - 0.3f, c.y + 0.3f});
    const Vector2 right = on_terrain(map, {c.x + 0.3f, c.y - 0.3f});
    const Vector2 foot = on_terrain(map, c);
    const Vector2 tip{foot.x, foot.y - kHeight};
    const Color steel = lit(shade({150, 152, 150, 255}, 1.0f - 0.4f * damage));
    DrawLineEx(left, tip, 1.5f, steel);
    DrawLineEx(right, tip, 1.5f, steel);
    for (int i = 0; i < 6; ++i) {  // cross bracing up the mast
        const float t0 = static_cast<float>(i) / 6.0f;
        const float t1 = static_cast<float>(i + 1) / 6.0f;
        DrawLineV(lerp(left, tip, t0), lerp(right, tip, t1), steel);
        DrawLineV(lerp(right, tip, t0), lerp(left, tip, t1), steel);
    }
    for (const float dx : {-4.0f, 0.0f, 4.0f}) {
        DrawRectangleRec({tip.x + dx - 1.5f, tip.y + 10.0f, 3.0f, 8.0f}, lit({210, 210, 204, 255}));
    }
    DrawCircleV(tip, 2.0f, {230, 60, 50, 255});
}

// A gas station: a canopy on four posts over the pumps, a small shop behind.
void draw_gas_station(const engine::TileMap& map, const engine::Structure& s, float damage) {
    const Rectangle r = footprint(s, 0.1f);
    const float soot = 1.0f - 0.45f * damage;
    // The shop along the back edge.
    const Vector2 shop_ground[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height * 0.35f},
                                    {r.x, r.y + r.height * 0.35f}};
    Vector2 base[4];
    Vector2 top[4];
    for (int i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, shop_ground[i]);
        top[i] = {base[i].x, base[i].y - 14.0f};
    }
    const Color wall = shade({200, 196, 186, 255}, soot);
    fill_quad(base[1], base[2], top[2], top[1], wall);
    fill_quad(base[2], base[3], top[3], top[2], shade(wall, 0.72f));
    fill_quad(top[0], top[1], top[2], top[3], shade({150, 60, 50, 255}, soot));
    // Pumps, and the canopy over them on four posts.
    for (const float k : {0.35f, 0.65f}) {
        const Vector2 pump = on_terrain(map, {r.x + r.width * k, r.y + r.height * 0.7f});
        DrawRectangleRec({pump.x - 2.0f, pump.y - 8.0f, 4.0f, 8.0f}, lit({200, 60, 50, 255}));
    }
    const Vector2 canopy_ground[4] = {{r.x + r.width * 0.1f, r.y + r.height * 0.45f},
                                      {r.x + r.width * 0.9f, r.y + r.height * 0.45f},
                                      {r.x + r.width * 0.9f, r.y + r.height * 0.95f},
                                      {r.x + r.width * 0.1f, r.y + r.height * 0.95f}};
    Vector2 roof[4];
    for (int i = 0; i < 4; ++i) {
        const Vector2 post = on_terrain(map, canopy_ground[i]);
        roof[i] = {post.x, post.y - 20.0f};
        DrawLineEx(post, roof[i], 1.5f, lit({210, 210, 204, 255}));
    }
    fill_quad(roof[0], roof[1], roof[2], roof[3], ColorAlpha(lit(shade({226, 222, 212, 255}, soot)), 0.92f));
    DrawLineEx(roof[2], roof[3], 2.0f, lit({200, 60, 50, 255}));
}

// A grain elevator: a row of tall concrete silos and a head house above them.
void draw_elevator(const engine::TileMap& map, const engine::Structure& s, float damage) {
    const Rectangle r = footprint(s, 0.1f);
    const float soot = 1.0f - 0.45f * damage;
    const Color concrete = shade({196, 190, 176, 255}, soot);
    constexpr float kSilo = 80.0f;
    const bool along_x = r.width >= r.height;
    const int silos = 3;
    for (int i = 0; i < silos; ++i) {
        const float k = (static_cast<float>(i) + 0.5f) / static_cast<float>(silos);
        const Vector2 g = along_x ? Vector2{r.x + r.width * k, r.y + r.height * 0.5f}
                                  : Vector2{r.x + r.width * 0.5f, r.y + r.height * k};
        const Vector2 foot = on_terrain(map, g);
        constexpr float kRadius = 11.0f;
        DrawRectangleRec({foot.x - kRadius, foot.y - kSilo, 2.0f * kRadius, kSilo}, lit(concrete));
        DrawRectangleRec({foot.x, foot.y - kSilo, kRadius, kSilo}, lit(shade(concrete, 0.8f)));
        DrawEllipse(static_cast<int>(foot.x), static_cast<int>(foot.y), kRadius, kRadius * 0.5f, lit(shade(concrete, 0.8f)));
        DrawEllipse(static_cast<int>(foot.x), static_cast<int>(foot.y - kSilo), kRadius, kRadius * 0.5f,
                    lit(shade(concrete, 1.1f)));
    }
    // The head house on top of the middle silo.
    const Vector2 mid = on_terrain(map, {r.x + r.width * 0.5f, r.y + r.height * 0.5f});
    DrawRectangleRec({mid.x - 8.0f, mid.y - kSilo - 26.0f, 16.0f, 26.0f}, lit(shade(concrete, 0.95f)));
    for (int row = 0; row < 3; ++row) {
        DrawRectangleRec({mid.x - 4.0f, mid.y - kSilo - 22.0f + 7.0f * static_cast<float>(row), 3.0f, 4.0f},
                         lit({70, 76, 82, 255}));
    }
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

    if (style.cross) {
        // A red cross on the roof.
        const Vector2 c{(top[0].x + top[2].x) * 0.5f, (top[0].y + top[2].y) * 0.5f};
        DrawRectangleRec({c.x - 7.0f, c.y - 2.0f, 14.0f, 4.0f}, lit({200, 40, 40, 255}));
        DrawRectangleRec({c.x - 2.0f, c.y - 5.0f, 4.0f, 10.0f}, lit({200, 40, 40, 255}));
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

// A boulder: an angular stone of facets, lit from the upper left, its
// shadow to the lower right, moss on some.
void draw_boulder(Vector2 base, float r, uint32_t h, float tint, bool warm) {
    DrawEllipse(static_cast<int>(base.x + r * 0.45f), static_cast<int>(base.y + r * 0.1f), r * 1.15f, r * 0.42f,
                lit({20, 20, 18, 70}));
    constexpr int kPoints = 7;
    const Vector2 c{base.x, base.y - r * 0.45f};
    Vector2 ring[kPoints];
    for (int i = 0; i < kPoints; ++i) {
        const float angle = (static_cast<float>(i) + (hash_unit(h >> i) - 0.5f) * 0.5f) * 6.2831853f / kPoints;
        const float k = 0.78f + 0.34f * hash_unit(h >> (i + 9));
        ring[i] = {c.x + std::cos(angle) * r * k, c.y + std::sin(angle) * r * k * 0.72f};
    }
    const Color stone = shade(warm ? Color{142, 128, 106, 255} : Color{128, 128, 124, 255}, tint);
    const Vector2 peak{c.x - r * 0.12f, c.y - r * 0.28f};  // the facets meet a little up and left
    for (int i = 0; i < kPoints; ++i) {
        const Vector2 a = ring[i];
        const Vector2 d = ring[(i + 1) % kPoints];
        // The facet faces the way its edge lies from the middle.
        const float mx = (a.x + d.x) * 0.5f - c.x;
        const float my = (a.y + d.y) * 0.5f - c.y;
        const float len = std::max(0.001f, std::sqrt(mx * mx + my * my));
        const float facing = (-0.6f * mx - 0.8f * my) / len;
        fill_triangle(peak, a, d, shade(stone, 1.0f + 0.32f * facing));
    }
    if ((h >> 20) % 3 == 0) disc({c.x - r * 0.25f, c.y - r * 0.35f}, r * 0.18f, shade({104, 116, 72, 255}, tint));
}

// A stone outcrop: boulders and scree, laid out differently on every tile;
// the big ones deep inside it, smaller at its edge; fewer and smaller as it
// is quarried away.
void draw_rock(const engine::TileMap& map, int tx, int ty, int32_t left) {
    const uint32_t h = tile_hash(tx, ty);
    const float share = std::clamp(static_cast<float>(left) / static_cast<float>(engine::kRockMaterials), 0.0f, 1.0f);
    const auto x = static_cast<float>(tx);
    const auto y = static_cast<float>(ty);
    int around = 0;
    for (const auto& d : {std::pair{0, -1}, std::pair{1, 0}, std::pair{0, 1}, std::pair{-1, 0}}) {
        around += map.contains_tile(tx + d.first, ty + d.second) &&
                          map.terrain(tx + d.first, ty + d.second) == engine::Terrain::Rock
                      ? 1
                      : 0;
    }
    const bool deep = around == 4;
    // Scree first, under the boulders.
    for (int i = 0; i < 7; ++i) {
        const Vector2 p = on_terrain(map, {x + 0.05f + 0.9f * hash_unit(h >> i), y + 0.05f + 0.9f * hash_unit(h >> (i + 11))});
        DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), 1.8f, 1.1f, lit({112, 108, 100, 255}));
        DrawCircleV({p.x - 0.5f, p.y - 0.6f}, 0.7f, lit({158, 154, 146, 255}));
    }
    const int count = share > 0.75f ? 3 : share > 0.4f ? 2 : 1;
    const float shrink = 0.7f + 0.3f * share;
    struct Placed {
        Vector2 at;
        float size;
        uint32_t h;
    };
    std::array<Placed, 3> stones{};
    for (int i = 0; i < count; ++i) {
        const uint32_t hi = tile_hash(tx * 31 + i * 7, ty * 17 - i * 13);
        // The first is the tile's big one; the rest smaller, anywhere on it.
        const float big = i == 0 ? (deep ? 13.0f : 9.5f) : (i == 1 ? 6.5f : 4.5f);
        const float size = big * shrink * (0.8f + 0.4f * hash_unit(hi >> 16));
        stones[static_cast<size_t>(i)] = {{x + 0.2f + 0.6f * hash_unit(hi), y + 0.2f + 0.6f * hash_unit(hi >> 8)}, size, hi};
    }
    // Back to front, so the nearer stone hides the farther.
    std::sort(stones.begin(), stones.begin() + count,
              [](const Placed& a, const Placed& b) { return a.at.x + a.at.y < b.at.x + b.at.y; });
    for (int i = 0; i < count; ++i) {
        const Placed& p = stones[static_cast<size_t>(i)];
        // Grey granite to warm sandstone.
        draw_boulder(on_terrain(map, p.at), p.size, p.h, 0.88f + 0.24f * hash_unit(p.h >> 24), hash_unit(p.h >> 4) < 0.3f);
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
        if (s.cache > 0 && (reveal_ || s.cache_owner == viewer_)) {
            // Ammunition boxes stacked at a position: one to three, as full as it is.
            const int32_t capacity = std::max(1, engine::structure_type(s.type).cache_capacity);
            const int boxes = 1 + static_cast<int>(2 * s.cache / capacity);
            const Vector2 g = on_terrain(map, {to_vector2(s.center).x + 0.25f, to_vector2(s.center).y + 0.25f});
            for (int i = 0; i < boxes; ++i) {
                const Rectangle box{g.x - 6.0f + 4.0f * static_cast<float>(i), g.y - 4.0f - 2.0f * static_cast<float>(i % 2),
                                    6.0f, 4.0f};
                DrawRectangleRec(box, {96, 104, 64, 255});
                DrawRectangleLinesEx(box, 1.0f, {52, 58, 34, 255});
            }
        }
        if (s.converted != engine::StructureType::Count) {
            // A depot in a village building: its owner's flag, crates by the door.
            const Color flag = theme::player_color(s.owner);
            DrawLineEx({c.x, c.y + 18}, {c.x, c.y - 10}, 1.5f, {40, 40, 40, 255});
            DrawRectangleRec({c.x, c.y - 10, 16, 11}, flag);
            const char* letter = s.converted == engine::StructureType::AmmoDepot   ? "A"
                                 : s.converted == engine::StructureType::FuelDepot ? "F"
                                                                                   : "W";
            DrawText(letter, static_cast<int>(c.x + 4), static_cast<int>(c.y - 9), 10, WHITE);
            const Vector2 door = on_terrain(map, {to_vector2(s.center).x + 1.4f, to_vector2(s.center).y + 1.4f});
            for (int i = 0; i < 3; ++i) {
                DrawRectangleRec({door.x - 9.0f + 6.0f * static_cast<float>(i), door.y - 5.0f, 5.0f, 5.0f},
                                 {132, 104, 62, 255});
            }
            if (!s.built) {  // still being turned
                const float done = static_cast<float>(s.build_progress) / static_cast<float>(engine::kConversionWork);
                DrawRectangleRec({c.x - 21, c.y + 6, 42, 5}, {0, 0, 0, 170});
                DrawRectangleRec({c.x - 20, c.y + 7, 40 * done, 3}, {230, 200, 60, 255});
            }
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
    if (grain_.id == 0) {
        grain_ = LoadShaderFromMemory(kGrainVertex, kGrainFragment);
        grain_zoom_loc_ = GetShaderLocation(grain_, "zoom");
    }
    const float zoom = camera.camera2d().zoom;
    g_zoom = zoom;
    SetShaderValue(grain_, grain_zoom_loc_, &zoom, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(grain_);

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
        const engine::UnitTypeDef& def = engine::unit_type(u.type);
        if (def.relay_range.raw > 0 && !u.silent) {
            draw_ground_ellipse(on_terrain(map, unit_ground_pos(u, alpha)), to_float(world.relay_reach(u)),
                                ColorAlpha(kRadioColor, 0.5f));
        }
        if (def.df_range.raw > 0 && u.deployed) {
            draw_ground_ellipse(on_terrain(map, unit_ground_pos(u, alpha)), to_float(def.df_range),
                                ColorAlpha(kBearingColor, 0.35f));
        }
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
        if (s->rally_set && s->owner == viewer_) {
            // Its rally point: a flag on a pole, a line from the building.
            const Vector2 to = on_terrain(map, to_vector2(s->rally));
            DrawLineV(on_terrain(map, to_vector2(s->center)), to, ColorAlpha(theme::kSelection, 0.45f));
            DrawLineEx(to, {to.x, to.y - 18.0f}, 1.5f, {40, 40, 40, 255});
            DrawRectangleRec({to.x, to.y - 18.0f, 11.0f, 7.0f}, theme::player_color(s->owner));
        }
        if (s->type == engine::StructureType::Headquarters && s->owner == viewer_) {
            draw_ground_ellipse(on_terrain(map, to_vector2(s->center)), to_float(world.headquarters_relay(s->owner)),
                                ColorAlpha(kRadioColor, 0.5f));
        }
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
        const engine::Structure* barn = nullptr;  // a spacious village building, drawn whole
        float damage = 0.0f;  // 0 = intact, 1 = about to collapse
        const engine::Structure* building = nullptr;
        const TrainCar* car = nullptr;
        float light = 1.0f;  // dimmer under the fog of war
    };
    std::vector<Drawable> drawables;
    drawables.reserve(world.units().size() + world.projectiles().size() + 1024);
    std::vector<const engine::Structure*> barns;
    for (const engine::Unit& u : world.units()) {
        if (u.inside || u.airborne || !shows(world, u)) continue;  // behind walls, up in the sky, or unseen
        const Vector2 g = unit_ground_pos(u, alpha);
        const Vector2 p = on_terrain(map, g);
        if (!CheckCollisionPointRec(p, {view.x - 60, view.y - 60, view.width + 120, view.height + 120})) continue;
        drawables.push_back({.depth = g.x + g.y, .unit = &u});
    }
    for (const engine::Projectile& p : world.projectiles()) {
        const Vector2 g = lerp(to_vector2(p.prev_pos), to_vector2(p.pos), alpha);
        if (in_view(world, g)) drawables.push_back({.depth = g.x + g.y, .projectile = &p});
    }
    // Scenery comes from the remembered ground: what the fog hides stays as it
    // was; what stands behind a spoil tip is out of sight.
    for_each_visible_tile(map, view, [&](int tx, int ty) {
        const int state = fog(world, tx, ty);
        if (state == kUnexplored || behind_relief(tx, ty)) return;
        const float light = state == kInView ? 1.0f : kFogLight;
        switch (seen_terrain_[static_cast<size_t>(ty * map.width() + tx)]) {
            case engine::Terrain::Orchard: {
                // Apple trees in rows a tile apart, two to a tile along the row.
                for (int k = 0; k < 2; ++k) {
                    const uint32_t hk = tile_hash(tx * 2 + k, ty);
                    Tree t{{static_cast<float>(tx) + 0.25f + 0.5f * static_cast<float>(k), static_cast<float>(ty) + 0.5f},
                           0.95f + 0.2f * hash_unit(hk), 0.9f + 0.2f * hash_unit(hk >> 16), TreeKind::Apple};
                    t.height = 0.85f + 0.3f * hash_unit(hk >> 8);
                    t.girth = 0.9f + 0.2f * hash_unit(hk >> 12);
                    t.bend = (hash_unit(hk >> 20) - 0.5f) * 3.0f;
                    t.seed = hk;
                    drawables.push_back({.depth = t.ground.x + t.ground.y, .tree = t, .light = light});
                }
                break;
            }
            case engine::Terrain::Forest: {
                // A tree line (one tile wide, running along x or y) gets poplars in a row.
                auto wood = [&](int x, int y) {
                    return map.contains_tile(x, y) && seen_terrain_[static_cast<size_t>(y * map.width() + x)] == engine::Terrain::Forest;
                };
                const bool along_x = wood(tx - 1, ty) || wood(tx + 1, ty);
                const bool along_y = wood(tx, ty - 1) || wood(tx, ty + 1);
                const int line = along_x && !along_y ? 1 : along_y && !along_x ? 2 : !along_x && !along_y ? 3 : 0;
                const float left = state == kInView ? static_cast<float>(map.resource({tx, ty})) / engine::kForestMaterials : 1.0f;
                size_t count = 0;
                const std::array<Tree, 3> trees = trees_on_tile(tx, ty, line, std::clamp(left, 0.0f, 1.0f), count);
                for (size_t i = 0; i < count; ++i) {
                    const Tree& t = trees[i];
                    drawables.push_back({.depth = t.ground.x + t.ground.y, .tree = t, .light = light});
                }
                break;
            }
            case engine::Terrain::House: {
                float damage = 0.0f;
                const engine::Structure* s = world.structure_at({tx, ty});
                if (s && state == kInView) {
                    const int32_t max_hp = engine::structure_type(s->type).max_hp;
                    damage = 1.0f - static_cast<float>(s->hp) / static_cast<float>(max_hp);
                }
                if (s && (s->tiles.size() >= engine::kSpaciousTiles || s->look != engine::HouseLook::House)) {
                    // A barn is one building, drawn once whichever of its tiles is in view.
                    if (std::find(barns.begin(), barns.end(), s) != barns.end()) break;
                    barns.push_back(s);
                    const Vector2 c = to_vector2(s->center);
                    drawables.push_back({.depth = c.x + c.y + 1.0f, .barn = s, .damage = damage, .light = light});
                    break;
                }
                drawables.push_back({.depth = static_cast<float>(tx + ty) + 1.0f, .house_x = tx, .house_y = ty,
                                     .damage = damage, .light = light});
                break;
            }
            case engine::Terrain::Apartment:
            case engine::Terrain::Tower:
            case engine::Terrain::GasStation:
            case engine::Terrain::Elevator: {
                // One building, drawn once whichever of its tiles is in view.
                const engine::Structure* s = world.structure_at({tx, ty});
                if (!s || std::find(barns.begin(), barns.end(), s) != barns.end()) break;
                barns.push_back(s);
                float damage = 0.0f;
                if (state == kInView) {
                    damage = 1.0f - static_cast<float>(s->hp) / static_cast<float>(engine::structure_type(s->type).max_hp);
                }
                const Vector2 c = to_vector2(s->center);
                drawables.push_back({.depth = c.x + c.y + 1.0f, .barn = s, .damage = damage, .light = light});
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
               !engine::is_fieldwork(s.type) && !engine::is_obstacle(s.type) && s.type != engine::StructureType::Dugout &&
               s.type != engine::StructureType::Airfield && s.type != engine::StructureType::Apartment &&
               s.type != engine::StructureType::CellTower && s.type != engine::StructureType::GasStation &&
               s.type != engine::StructureType::Elevator;  // drawn with the ground's tiles, like houses
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
        set_grain(d.unit || d.projectile ? Grain{} : kObjectGrain);
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
        } else if (d.barn) {
            if (d.barn->type == engine::StructureType::Apartment) {
                draw_apartment(map, *d.barn, d.damage);
            } else if (d.barn->type == engine::StructureType::CellTower) {
                draw_cell_tower(map, *d.barn, d.damage);
            } else if (d.barn->type == engine::StructureType::GasStation) {
                draw_gas_station(map, *d.barn, d.damage);
            } else if (d.barn->type == engine::StructureType::Elevator) {
                draw_elevator(map, *d.barn, d.damage);
            } else if (d.barn->look == engine::HouseLook::Cowshed) {
                draw_cowshed(map, *d.barn, d.damage);
            } else if (d.barn->look == engine::HouseLook::Coop) {
                draw_coop(map, *d.barn, d.damage);
            } else {
                draw_barn(map, *d.barn, d.damage);
            }
        } else if (d.house_x >= 0) {
            draw_house(map, d.house_x, d.house_y, d.damage);
        } else {
            draw_tree(map, d.tree);
        }
    }
    g_light = 1.0f;
    set_grain({});

    // Fires: flames flickering over the burning ground.
    for (const engine::Fire& f : world.fires()) {
        const Vector2 c = to_vector2(f.center);
        if (!reveal_ && fog(world, static_cast<int>(c.x), static_cast<int>(c.y)) == kUnexplored) continue;
        const float r = to_float(f.radius);
        fill_ground_ellipse(on_terrain(map, c), r, ColorAlpha({40, 26, 18, 255}, 0.55f));  // scorched
        const auto t = static_cast<float>(world.tick());
        for (int i = 0; i < 9; ++i) {
            const float a = static_cast<float>(i) * 2.4f;
            const float d = r * (0.25f + 0.6f * static_cast<float>(i % 3) / 2.0f);
            const Vector2 g{c.x + std::cos(a) * d, c.y + std::sin(a) * d};
            const Vector2 base = on_terrain(map, g);
            const float h = 7.0f + 4.0f * std::sin(t * 0.7f + static_cast<float>(i));
            DrawTriangle({base.x + 3, base.y}, {base.x, base.y - h}, {base.x - 3, base.y}, {230, 120, 30, 230});
            DrawTriangle({base.x + 1.5f, base.y}, {base.x, base.y - h * 0.6f}, {base.x - 1.5f, base.y},
                         {255, 210, 80, 240});
        }
    }

    // Smoke screens: billowing puffs over whatever is in them.
    for (const engine::Smoke& s : world.smokes()) {
        const Vector2 c = to_vector2(s.center);
        if (!reveal_ && fog(world, static_cast<int>(c.x), static_cast<int>(c.y)) == kUnexplored) continue;
        const float r = to_float(s.radius);
        const float left = std::clamp(static_cast<float>(s.clears - world.tick()) / engine::kSmokeTicks, 0.0f, 1.0f);
        const float fade = std::min(1.0f, left * 4.0f);  // thins out at the end
        for (int i = 0; i < 7; ++i) {
            const float a = static_cast<float>(i) * 0.9f;
            const Vector2 g{c.x + std::cos(a) * r * 0.5f, c.y + std::sin(a) * r * 0.5f};
            fill_ground_ellipse(on_terrain(map, g, 10.0f + static_cast<float>(i % 3) * 6.0f), r * 0.6f,
                                ColorAlpha({200, 200, 196, 255}, 0.55f * fade));
        }
    }

    // Aircraft in the air: a shadow on the ground, the aircraft high above it.
    for (const engine::Unit& u : world.units()) {
        if (u.airborne && shows(world, u)) draw_aircraft(map, u, alpha);
    }

    draw_shots(world, alpha);
    draw_blasts(map);
    for (const engine::Bearing& b : world.bearings()) {
        if (b.owner != viewer_) continue;
        const engine::Unit* station = world.find_unit(b.station);
        if (!station) continue;
        const Vector2 from = to_vector2(b.from);
        Vector2 dir = to_vector2(b.dir);
        const float len = std::hypot(dir.x, dir.y);
        if (len <= 0.0f) continue;
        dir = {dir.x / len, dir.y / len};
        const float reach = to_float(engine::unit_type(station->type).df_range);
        for (float t = 0.5f; t < reach; t += 1.0f) {  // dashes, a tile apart
            const Vector2 a{from.x + dir.x * t, from.y + dir.y * t};
            const Vector2 z{from.x + dir.x * (t + 0.5f), from.y + dir.y * (t + 0.5f)};
            DrawLineEx(on_terrain(map, a, 2.0f), on_terrain(map, z, 2.0f), 1.5f,
                       ColorAlpha(kBearingColor, 0.55f * (1.0f - t / reach) + 0.15f));
        }
    }
    draw_structure_overlays(world, view);
    for (const Drawable& d : drawables) {
        if (d.unit && !d.unit->inside &&
            (is_selected(d.unit->id) || d.unit->hp < engine::unit_type(d.unit->type).max_hp)) {
            draw_health_bar(map, *d.unit, alpha);
        }
        if (d.unit && !d.unit->inside && d.unit->owner == viewer_) {
            draw_supply_warning(map, *d.unit, alpha);
            draw_radio_marks(world, *d.unit, alpha);
            draw_load_bar(map, *d.unit, alpha);
        }
        if (d.unit && d.unit->owner != viewer_ && world.fixed_by(viewer_, *d.unit)) {
            // Located by our direction finders: a target marker around it.
            const Vector2 p = on_terrain(map, unit_ground_pos(*d.unit, alpha));
            const float r = to_float(engine::unit_type(d.unit->type).radius) + 0.35f;
            draw_ground_ellipse(p, r, kBearingColor);
            const float w = r * 32.0f;
            DrawLineV({p.x - w - 4, p.y}, {p.x - w + 4, p.y}, kBearingColor);
            DrawLineV({p.x + w - 4, p.y}, {p.x + w + 4, p.y}, kBearingColor);
        }
    }

    EndShaderMode();
    EndMode2D();
}

float WorldRenderer::corner_light(int cx, int cy) const {
    auto at = [&](int x, int y) { return corner(std::clamp(x, 0, cache_width_), std::clamp(y, 0, cache_height_)); };
    // Light comes from the upper left of the screen: slopes rising towards
    // +x face it, slopes rising towards +y turn away.
    const float slope_x = (at(cx + 1, cy) - at(cx - 1, cy)) * 0.5f;
    const float slope_y = (at(cx, cy + 1) - at(cx, cy - 1)) * 0.5f;
    return 1.0f + slope_x * 0.30f - slope_y * 0.18f + at(cx, cy) * 0.05f;
}

bool WorldRenderer::behind_relief(int tx, int ty) const {
    if (tx < 0 || ty < 0 || tx >= cache_width_ || ty >= cache_height_) return false;
    auto centre = [&](int x, int y) {
        return (corner(x, y) + corner(x + 1, y) + corner(x, y + 1) + corner(x + 1, y + 1)) * 0.25f;
    };
    // A tile nearer the bottom of the screen by k along the diagonal is drawn
    // 2k levels lower down it: ground rising faster than that hides this one.
    const float own = centre(tx, ty);
    for (int k = 1; k <= 6; ++k) {
        const int x = tx + k;
        const int y = ty + k;
        if (x >= cache_width_ || y >= cache_height_) break;
        if (corner(x, y) - own > 2.0f * (static_cast<float>(k) - 0.5f) + 0.25f) return true;
        if (centre(x, y) - own > 2.0f * static_cast<float>(k) + 0.25f) return true;
    }
    return false;
}

float WorldRenderer::light_at(Vector2 ground) const {
    const int tx = std::clamp(static_cast<int>(std::floor(ground.x)), 0, std::max(0, cache_width_ - 1));
    const int ty = std::clamp(static_cast<int>(std::floor(ground.y)), 0, std::max(0, cache_height_ - 1));
    const float lx = std::clamp(ground.x - static_cast<float>(tx), 0.0f, 1.0f);
    const float ly = std::clamp(ground.y - static_cast<float>(ty), 0.0f, 1.0f);
    const float top = corner_light(tx, ty) * (1 - lx) + corner_light(tx + 1, ty) * lx;
    const float bottom = corner_light(tx, ty + 1) * (1 - lx) + corner_light(tx + 1, ty + 1) * lx;
    return top * (1 - ly) + bottom * ly;
}

WorldRenderer::GroundAt WorldRenderer::ground_at(const engine::World& world, Vector2 p, engine::Terrain fallback) const {
    using engine::Terrain;
    const engine::TileMap& map = world.map();
    const float gx = p.x - 0.5f;
    const float gy = p.y - 0.5f;
    const int x0 = static_cast<int>(std::floor(gx));
    const int y0 = static_cast<int>(std::floor(gy));
    const float ax = gx - static_cast<float>(x0);
    const float ay = gy - static_cast<float>(y0);
    struct Weight {
        Terrain terrain;
        float weight;
    };
    std::array<Weight, 4> weights{};
    size_t count = 0;
    float total = 0;
    float water = 0;
    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            const int x = x0 + dx;
            const int y = y0 + dy;
            // Nothing is known of an unexplored tile; a bridge is drawn as it is.
            if (!map.contains_tile(x, y) || fog(world, x, y) == kUnexplored) continue;
            const Terrain t = seen_terrain_[static_cast<size_t>(y * map.width() + x)];
            if (t == Terrain::Bridge) continue;
            const float w = (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay);
            total += w;
            if (t == Terrain::Water) water += w;
            size_t k = 0;
            while (k < count && weights[k].terrain != t) ++k;
            if (k == count) weights[count++] = {t, 0.0f};
            weights[k].weight += w;
        }
    }
    if (count == 0 || total <= 0) return {fallback, fallback == Terrain::Water ? 1.0f : 0.0f};
    Terrain best = weights[0].terrain;
    float best_score = -1.0f;
    for (size_t k = 0; k < count; ++k) {
        const int seed = 50 + static_cast<int>(weights[k].terrain);
        float score = weights[k].weight / total + (noise_at(p, 0.8f, seed) - 0.5f) * 0.36f +
                      static_cast<float>(std::max(0, reach_rank(weights[k].terrain))) * 0.004f;
        // Scree strewn over the grass: the foot of a spoil tip runs out finer.
        if (weights[k].terrain == Terrain::Slag) score += (noise_at(p, 0.45f, 91) - 0.5f) * 0.3f;
        if (score > best_score) {
            best = weights[k].terrain;
            best_score = score;
        }
    }
    return {best, water / total};
}

void WorldRenderer::paint_ground(const engine::World& world, int tx, int ty, engine::Terrain terrain,
                                 bool overlay) const {
    using engine::Terrain;
    const engine::TileMap& map = world.map();
    const auto fx = static_cast<float>(tx);
    const auto fy = static_cast<float>(ty);
    const uint32_t h = tile_hash(tx, ty);
    auto screen = [&](Vector2 p) { return on_terrain(map, p); };
    constexpr Color kSand{184, 168, 122, 255};
    constexpr Color kShallows{84, 130, 160, 255};

    if (!overlay) {
        const float corner_h[4] = {corner(tx, ty), corner(tx + 1, ty), corner(tx, ty + 1), corner(tx + 1, ty + 1)};
        if (terrain == Terrain::Bridge) {
            // The deck, as it is.
            const Vector2 a = iso::project({fx, fy}, corner_h[0]);
            const Vector2 b = iso::project({fx + 1, fy}, corner_h[1]);
            const Vector2 c = iso::project({fx + 1, fy + 1}, corner_h[3]);
            const Vector2 d = iso::project({fx, fy + 1}, corner_h[2]);
            fill_quad(a, b, c, d, shade(theme::terrain_color(terrain), light_at({fx + 0.5f, fy + 0.5f})));
            return;
        }
        // A 4x4 grid over the tile, each point the colour of the ground that
        // shows there: the kinds of ground blend into each other in ragged lines.
        constexpr int kSub = 4;
        // A tile a shade lighter or darker than the next (not on a ripe field: it shows there).
        const float grain = terrain == Terrain::Wheat ? 1.0f : 1.0f + (hash01(h >> 3) - 0.5f) * 0.04f;
        Vector2 pos[kSub + 1][kSub + 1];
        Color col[kSub + 1][kSub + 1];
        Vector3 grn[kSub + 1][kSub + 1];
        for (int j = 0; j <= kSub; ++j) {
            for (int i = 0; i <= kSub; ++i) {
                const float lx = static_cast<float>(i) / kSub;
                const float ly = static_cast<float>(j) / kSub;
                const Vector2 p{fx + lx, fy + ly};
                const float height = (corner_h[0] * (1 - lx) + corner_h[1] * lx) * (1 - ly) +
                                     (corner_h[2] * (1 - lx) + corner_h[3] * lx) * ly;
                pos[j][i] = iso::project(p, height);
                const GroundAt g = ground_at(world, p, terrain);
                Color c = ground_colour(g.terrain, p);
                if (g.terrain == Terrain::Slag) {
                    // Up the top of a spoil tip, the rock that smouldered for years, burnt rusty red;
                    // round its foot the grass coming up through the scree.
                    c = mix(c, {124, 74, 58, 255}, 0.7f * smooth01((height - 5.5f) / 3.5f + (field(p, 13) - 0.5f) * 0.4f));
                    const int px = static_cast<int>(std::floor(p.x));  // the same for a corner shared by two tiles
                    const int py = static_cast<int>(std::floor(p.y));
                    int low = engine::TileMap::kMaxElevation;
                    for (int y = py - 1; y <= py + 1; ++y) {
                        for (int x = px - 1; x <= px + 1; ++x) {
                            if (map.contains_tile(x, y)) low = std::min<int>(low, map.elevation(x, y));
                        }
                    }
                    const float foot = 1.0f - smooth01((height - static_cast<float>(low)) / 0.8f);
                    c = mix(c, ground_colour(Terrain::Grass, p), foot * (0.3f + 0.5f * noise_at(p, 0.5f, 77)));
                }
                if (g.terrain == Terrain::Water) {
                    c = mix(kShallows, c, smooth01((g.water - 0.55f) / 0.3f));  // the shallows by the shore
                } else if (g.water > 0) {
                    c = mix(c, kSand, smooth01((g.water - 0.1f) / 0.22f));  // sand along the water
                }
                grn[j][i] = grain_normal(grain_of(g.water > 0.5f ? Terrain::Water : g.terrain));
                float k = light_at(p);
                if (g.terrain == Terrain::Chalk) k = 1.0f + (k - 1.0f) * 2.2f;
                k = std::clamp(k, 0.5f, 1.4f);  // the steep sides of a spoil tip
                col[j][i] = lit(shade(c, k * grain));
            }
        }
        for (int j = 0; j < kSub; ++j) {
            for (int i = 0; i < kSub; ++i) {
                const Vector3 corners[4] = {grn[j][i], grn[j][i + 1], grn[j + 1][i + 1], grn[j + 1][i]};
                gradient_quad(pos[j][i], pos[j][i + 1], pos[j + 1][i + 1], pos[j + 1][i], col[j][i], col[j][i + 1],
                              col[j + 1][i + 1], col[j + 1][i], corners);
            }
        }
        rlNormal3f(g_grain.x, g_grain.y, g_grain.z);
        return;
    }

    // What lies on the ground, where that ground shows.
    auto shows = [&](Vector2 p, Terrain t) { return ground_at(world, p, terrain).terrain == t; };
    const Vector2 mid{fx + 0.5f, fy + 0.5f};
    const float light = light_at(mid);
    if (terrain == Terrain::Water) {
        // Reeds in the shallows, glints on open water.
        const GroundAt here = ground_at(world, mid, terrain);
        if (here.water < 0.9f && (h >> 9) % 3 == 0) {
            for (int k = 0; k < 6; ++k) {
                const Vector2 at{fx + 0.15f + 0.7f * hash01(h >> k), fy + 0.15f + 0.7f * hash01(h >> (k + 7))};
                const GroundAt g = ground_at(world, at, terrain);
                if (g.terrain != Terrain::Water || g.water > 0.85f) continue;
                const Vector2 p = screen(at);
                const float lean = (hash01(h >> (k + 3)) - 0.5f) * 3.0f;
                DrawLineV(p, {p.x + lean, p.y - 6.0f - 3.0f * hash01(h >> (k + 5))}, lit(shade({96, 122, 62, 255}, light)));
            }
        } else if (here.water >= 0.99f) {
            for (int k = 0; k < 2; ++k) {
                const Vector2 p = screen({fx + 0.2f + 0.6f * hash01(h >> (k * 5)), fy + 0.2f + 0.6f * hash01(h >> (k * 5 + 9))});
                DrawLineV(p, {p.x + 5.0f, p.y}, lit({104, 142, 176, 255}));
            }
        }
        return;
    }
    // Which way is down the slope here: rills and screes run that way.
    const float h00 = corner(tx, ty);
    const float h10 = corner(tx + 1, ty);
    const float h01 = corner(tx, ty + 1);
    const float h11 = corner(tx + 1, ty + 1);
    Vector2 down{-(h10 + h11 - h00 - h01), -(h01 + h11 - h00 - h10)};
    const float steep = std::sqrt(down.x * down.x + down.y * down.y);
    if (steep > 0.01f) down = {down.x / steep, down.y / steep};
    auto rill = [&](Vector2 at, float length, Color c, float width) {
        const Vector2 end{at.x + down.x * length, at.y + down.y * length};
        DrawLineEx(screen(at), screen(end), width, lit(shade(c, light)));
    };
    if (terrain == Terrain::Chalk) {
        // Streaks washed down the slope, steppe grass in tufts.
        for (int k = 0; k < 3; ++k) {
            const Vector2 at{fx + 0.15f + 0.7f * hash01(h >> (k * 3)), fy + 0.15f + 0.7f * hash01(h >> (k * 3 + 11))};
            if (!shows(at, terrain) || steep < 0.01f) continue;
            rill(at, 0.3f, {150, 146, 136, 255}, 1.0f);
        }
        for (int k = 0; k < static_cast<int>(h & 1u) + 1; ++k) {
            const Vector2 at{fx + 0.2f + 0.6f * hash01(h >> (k * 5 + 2)), fy + 0.2f + 0.6f * hash01(h >> (k * 5 + 17))};
            if (!shows(at, terrain)) continue;
            const Vector2 base = screen(at);
            for (int side = -1; side <= 1; ++side) {
                DrawLineV(base, {base.x + static_cast<float>(side) * 1.8f, base.y - (side == 0 ? 4.5f : 3.0f)},
                          lit(shade({132, 138, 82, 255}, light)));
            }
        }
        return;
    }
    if (terrain == Terrain::Slag) {
        // Loose black rock (the gullies down its sides are drawn whole, from
        // the top: draw_spoil_gullies): lumps of burnt rock, and low on the
        // slopes the odd birch sapling seeding itself.
        const float shadow = std::clamp(light, 0.5f, 1.4f);
        // At the foot, where the scree lies flat, the grass comes through.
        bool foot = true;
        for (const auto& [dx, dy] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
            foot = foot && (!map.contains_tile(tx + dx, ty + dy) || map.elevation(tx + dx, ty + dy) >= map.elevation(tx, ty));
        }
        for (int k = 0; k < 3; ++k) {
            const Vector2 at{fx + 0.1f + 0.8f * hash01(h >> (k * 3 + 1)), fy + 0.1f + 0.8f * hash01(h >> (k * 3 + 14))};
            if (!shows(at, terrain)) continue;
            const Vector2 p = screen(at);
            const Color lump = k == 0 && (h >> 5) % 3 == 0 ? Color{120, 78, 62, 255} : Color{30, 28, 28, 255};
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), 1.6f, 1.0f, lit(shade(lump, shadow)));
        }
        for (int k = 0; foot && k < 4; ++k) {
            const Vector2 at{fx + 0.1f + 0.8f * hash01(h >> (k * 5 + 3)), fy + 0.1f + 0.8f * hash01(h >> (k * 5 + 16))};
            if (!shows(at, terrain)) continue;
            const Vector2 base = screen(at);
            for (int side = -1; side <= 1; ++side) {
                DrawLineV(base, {base.x + static_cast<float>(side) * 1.6f, base.y - (side == 0 ? 4.0f : 2.8f)},
                          lit(shade({86, 112, 56, 255}, shadow)));
            }
        }
        if (map.elevation(tx, ty) <= 4 && (h >> 13) % 6 == 0) {
            const Vector2 at{fx + 0.3f + 0.4f * hash01(h >> 3), fy + 0.3f + 0.4f * hash01(h >> 19)};
            if (shows(at, terrain)) {
                const Vector2 b = screen(at);
                const float lean = (hash01(h >> 7) - 0.5f) * 2.0f;
                DrawLineEx(b, {b.x + lean, b.y - 8.0f}, 1.0f, lit({214, 210, 198, 255}));
                const Color leaf = shade({88, 116, 56, 255}, shadow);
                DrawEllipse(static_cast<int>(b.x + lean + 0.6f), static_cast<int>(b.y - 8.5f), 2.4f, 3.4f, lit(shade(leaf, 0.8f)));
                DrawEllipse(static_cast<int>(b.x + lean - 0.4f), static_cast<int>(b.y - 9.5f), 1.4f, 2.2f, lit(leaf));
            }
        }
        return;
    }
    if (!is_grass(terrain)) return;

    // On the grass: bald spots at the edges of the trodden patches, tufts,
    // flowers, a bush now and then (more of them at the forest's edge), stones.
    auto blob = [&](Vector2 centre, float radius, Color fill, int seed) {
        constexpr int kPoints = 9;
        Vector2 ring[kPoints];
        for (int i = 0; i < kPoints; ++i) {
            const float angle = static_cast<float>(i) * 6.2831853f / static_cast<float>(kPoints);
            const float r = radius * (0.7f + 0.5f * hash01(tile_hash(tx * 7 + i + seed, ty * 11 - i)));
            ring[i] = screen({centre.x + std::cos(angle) * r, centre.y + std::sin(angle) * r});
        }
        const Vector2 c = screen(centre);
        for (int i = 0; i < kPoints; ++i) fill_triangle(c, ring[i], ring[(i + 1) % kPoints], shade(fill, light));
    };
    const float bare = field(mid, 9);
    if (bare > kBareFrom - 0.06f && bare < kBareFrom + 0.06f && (h >> 4) % 3 == 0) {
        const Vector2 c{mid.x + (hash01(h >> 4) - 0.5f) * 0.5f, mid.y + (hash01(h >> 12) - 0.5f) * 0.5f};
        if (shows(c, terrain)) blob(c, 0.22f + 0.12f * hash01(h >> 18), kBareEarth, 2);
    }
    const float tone = field(mid, 1);
    const Color blade = tone > 0.6f ? Color{138, 138, 80, 255} : Color{58, 90, 44, 255};
    const int tufts = bare > kBareFrom ? static_cast<int>(h & 1u) : static_cast<int>(h & 3u) + 1;
    for (int k = 0; k < tufts; ++k) {
        const Vector2 at{fx + 0.1f + 0.8f * hash01(h >> (k * 4 + 1)), fy + 0.1f + 0.8f * hash01(h >> (k * 4 + 13))};
        if (!shows(at, terrain)) continue;
        const Vector2 base = screen(at);
        for (int s = -1; s <= 1; ++s) {
            DrawLineV(base, {base.x + static_cast<float>(s) * 1.8f, base.y - (s == 0 ? 5.0f : 3.5f)}, lit(shade(blade, light)));
        }
    }
    if ((h >> 8) % 17 == 0 && bare < kBareFrom) {
        static constexpr Color kFlowers[] = {{236, 228, 128, 255}, {240, 240, 236, 255}, {186, 132, 206, 255}, {226, 96, 76, 255}};
        const Color petal = kFlowers[(h >> 20) % std::size(kFlowers)];
        for (int k = 0; k < 6; ++k) {
            const Vector2 at{fx + 0.25f + 0.5f * hash01(h >> (k + 2)), fy + 0.25f + 0.5f * hash01(h >> (k + 17))};
            if (shows(at, terrain)) DrawCircleV(screen(at), 1.0f, lit(petal));
        }
    }
    bool by_forest = false;
    for (const auto& d : {std::pair{0, -1}, std::pair{1, 0}, std::pair{0, 1}, std::pair{-1, 0}}) {
        const int x = tx + d.first;
        const int y = ty + d.second;
        by_forest = by_forest || (map.contains_tile(x, y) && fog(world, x, y) != kUnexplored &&
                                  seen_terrain_[static_cast<size_t>(y * map.width() + x)] == Terrain::Forest);
    }
    if ((h >> 12) % 19 == 0 || (by_forest && (h >> 12) % 4 == 0)) {
        const Vector2 at{fx + 0.3f + 0.4f * hash01(h >> 5), fy + 0.3f + 0.4f * hash01(h >> 21)};
        if (shows(at, terrain)) {
            const Vector2 b = screen(at);
            const float size = 0.8f + 0.5f * hash01(h >> 14);
            DrawEllipse(static_cast<int>(b.x), static_cast<int>(b.y), 7.0f * size, 3.0f * size, lit({24, 34, 18, 70}));
            DrawCircleV({b.x - 3.0f * size, b.y - 3.0f * size}, 3.2f * size, lit(shade({48, 80, 40, 255}, light)));
            DrawCircleV({b.x + 3.0f * size, b.y - 3.0f * size}, 3.0f * size, lit(shade({50, 84, 42, 255}, light)));
            DrawCircleV({b.x, b.y - 6.0f * size}, 3.6f * size, lit(shade({62, 98, 48, 255}, light)));
            DrawCircleV({b.x - 1.2f * size, b.y - 7.5f * size}, 1.6f * size, lit(shade({94, 130, 64, 255}, light)));
        }
    }
    if ((h >> 16) % 29 == 0) {
        for (int k = 0; k < 1 + static_cast<int>((h >> 26) % 3); ++k) {
            const Vector2 at{fx + 0.2f + 0.6f * hash01(h >> (k + 6)), fy + 0.2f + 0.6f * hash01(h >> (k + 19))};
            if (!shows(at, terrain)) continue;
            const Vector2 p = screen(at);
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), 2.4f, 1.6f, lit(shade({122, 120, 112, 255}, light)));
            DrawCircleV({p.x - 0.6f, p.y - 0.6f}, 0.9f, lit(shade({170, 168, 158, 255}, light)));
        }
    }
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

        const int state = fog(world, tx, ty);
        if (state == kUnexplored) return;  // black, like the background
        const engine::Terrain terrain = seen_terrain_[static_cast<size_t>(ty * map.width() + tx)];
        g_light = state == kInView ? 1.0f : kFogLight;
        paint_ground(world, tx, ty, terrain, false);
        if (behind_relief(tx, ty)) return;  // the ground in front covers the rest
        if (terrain == engine::Terrain::Trench || terrain == engine::Terrain::Foxhole ||
            terrain == engine::Terrain::Dugout || terrain == engine::Terrain::GunPit ||
            terrain == engine::Terrain::Wire || terrain == engine::Terrain::Hedgehogs) {
            draw_works(map, tx, ty, terrain, [&](int x, int y) {
                if (!map.contains_tile(x, y)) return false;
                const engine::Terrain t = seen_terrain_[static_cast<size_t>(y * map.width() + x)];
                return t == engine::Terrain::Trench || t == engine::Terrain::Foxhole || t == engine::Terrain::Dugout;
            });
        }
        draw_ground_detail(map, tx, ty, terrain, top, right, bottom, left);
        if (terrain == engine::Terrain::Airstrip) {
            // Concrete slabs; a dashed centre line down the middle row of the runway.
            const Color seam = shade(theme::terrain_color(terrain), 0.8f);
            DrawLineV(lerp(top, right, 0.5f), lerp(left, bottom, 0.5f), lit(seam));
            if (const engine::Structure* s = world.structure_at({tx, ty});
                s && s->type == engine::StructureType::Airfield && ty == s->tiles.front().y + 1) {
                DrawLineEx(lerp(lerp(top, left, 0.5f), lerp(right, bottom, 0.5f), 0.2f),
                           lerp(lerp(top, left, 0.5f), lerp(right, bottom, 0.5f), 0.6f), 2.0f, lit({230, 230, 220, 255}));
            }
        }
        if (terrain == engine::Terrain::Rail) {
            auto track = [&](int x, int y) {
                return map.contains_tile(x, y) &&
                       (map.terrain(x, y) == engine::Terrain::Rail || map.terrain(x, y) == engine::Terrain::Building);
            };
            draw_rail(map, tx, ty, track(tx - 1, ty) || track(tx + 1, ty));
        }
        paint_ground(world, tx, ty, terrain, true);
    });
    g_light = 1.0f;
    draw_spoil_gullies(world, view);
}

// Gullies the rain washed down the spoil tips: rays from the top to the foot,
// some from the very top, some starting lower, each wavering a little; a
// dark furrow with its lip catching the light on the left. The ones down the
// far side are out of sight.
void WorldRenderer::draw_spoil_gullies(const engine::World& world, Rectangle view) const {
    const engine::TileMap& map = world.map();
    const Rectangle near{view.x - 400.0f, view.y - 300.0f, view.width + 800.0f, view.height + 600.0f};
    auto foot = [&](int tx, int ty) {
        for (const auto& [dx, dy] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
            if (map.contains_tile(tx + dx, ty + dy) && map.elevation(tx + dx, ty + dy) < map.elevation(tx, ty)) return false;
        }
        return true;
    };
    set_grain(grain_of(engine::Terrain::Slag));
    for (const Vector2 peak : spoil_peaks_) {
        if (!CheckCollisionPointRec(on_terrain(map, peak), near)) continue;
        const uint32_t h = tile_hash(static_cast<int>(peak.x), static_cast<int>(peak.y));
        constexpr int kGullies = 46;
        for (int k = 0; k < kGullies; ++k) {
            const uint32_t hk = tile_hash(static_cast<int>(h >> 4) + k * 7, k * 31);
            const float angle = (static_cast<float>(k) + 0.5f * (hash_unit(hk) - 0.5f)) * 6.2831853f / kGullies;
            const Vector2 dir{std::cos(angle), std::sin(angle)};
            if ((dir.x + dir.y) * 0.7071f < -0.45f) continue;  // down the far side
            const Vector2 side{-dir.y, dir.x};
            const float from = k % 3 == 0 ? 0.25f : 0.7f + 1.8f * hash_unit(hk >> 8);
            const float phase = hash_unit(hk >> 16) * 6.2831853f;
            const float deep = 0.8f + 0.8f * hash_unit(hk >> 24);
            Vector2 prev{};
            bool have = false;
            for (float r = from; r < 8.0f; r += 0.2f) {
                const float wobble = 0.06f * std::sin(r * 2.4f + phase);
                const Vector2 g{peak.x + dir.x * r + side.x * wobble, peak.y + dir.y * r + side.y * wobble};
                const int tx = static_cast<int>(std::floor(g.x));
                const int ty = static_cast<int>(std::floor(g.y));
                if (!map.contains_tile(tx, ty) || seen_terrain_[static_cast<size_t>(ty * map.width() + tx)] != engine::Terrain::Slag ||
                    foot(tx, ty)) {
                    break;
                }
                const int state = fog(world, tx, ty);
                if (state == kUnexplored) break;
                const Vector2 p = on_terrain(map, g);
                if (have) {
                    g_light = state == kInView ? 1.0f : kFogLight;
                    const float k2 = std::clamp(light_at(g), 0.5f, 1.4f);
                    const float width = deep * (0.7f + 1.1f * std::min(1.0f, (r - from) / 2.5f));
                    DrawLineEx({prev.x - 1.0f, prev.y}, {p.x - 1.0f, p.y}, 0.9f, lit(shade({112, 104, 96, 255}, k2)));
                    DrawLineEx(prev, p, width, lit(shade({26, 24, 24, 255}, k2)));
                }
                prev = p;
                have = true;
            }
        }
    }
    g_light = 1.0f;
    set_grain({});
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
        case engine::Order::Collect: {
            const Vector2 to = on_terrain(map, to_vector2(u.order_point));
            DrawLineV(from, to, ColorAlpha(theme::kSelection, 0.4f));
            draw_ground_ellipse(to, 0.6f, ColorAlpha(theme::kSelection, 0.6f));
            break;
        }
        case engine::Order::Supply:
            if (const engine::Structure* post = world.find_structure(u.serves)) {
                const Vector2 to = on_terrain(map, to_vector2(post->center));
                DrawLineV(from, to, ColorAlpha(theme::kSelection, 0.45f));
                draw_ground_ellipse(to, 0.7f, ColorAlpha(theme::kSelection, 0.6f));
            } else if (const engine::Unit* v = world.find_unit(u.serves)) {
                const Vector2 to = on_terrain(map, unit_ground_pos(*v, alpha));
                DrawLineV(from, to, ColorAlpha(theme::kSelection, 0.45f));
                draw_ground_ellipse(to, engine::unit_type(v->type).radius.raw / 65536.0f + 0.3f,
                                    ColorAlpha(theme::kSelection, 0.6f));
            }
            break;
        case engine::Order::Haul: {
            // Its run: the station, and the depot it takes its freight to.
            const engine::Structure* station = world.station_of(u.owner);
            const engine::Resource cargo = u.carrying > 0 ? u.carrying_type : u.haul_cargo;
            const engine::Structure* depot =
                cargo != engine::Resource::Count && engine::depot_for(cargo) ? world.haul_destination(u, cargo) : nullptr;
            const Color color = ColorAlpha(theme::kSelection, 0.45f);
            if (station && depot) {
                const Vector2 a = to_vector2(station->center);
                const Vector2 b = to_vector2(depot->center);
                const float len = std::hypot(b.x - a.x, b.y - a.y);
                for (float t = 0.0f; t < len; t += 1.0f) {  // dashes, a tile apart
                    const float t1 = std::min(len, t + 0.5f);
                    DrawLineV(on_terrain(map, lerp(a, b, t / len)), on_terrain(map, lerp(a, b, t1 / len)), color);
                }
                draw_ground_ellipse(on_terrain(map, b), 0.6f, color);
            }
            if (station) DrawLineV(from, on_terrain(map, to_vector2(u.carrying > 0 && depot ? depot->center : station->center)),
                                   ColorAlpha(theme::kSelection, 0.25f));
            break;
        }
        case engine::Order::Idle:
            break;
    }
}

// An attack aircraft from above: swept wings, a long nose, twin engines.
// Up in the air its shadow is on the ground below.
void WorldRenderer::draw_aircraft(const engine::TileMap& map, const engine::Unit& u, float alpha) const {
    const Vector2 ground = unit_ground_pos(u, alpha);
    const Vector2 f = unit_facing(u);
    const Vector2 side{-f.y, f.x};
    auto shape = [&](float lift, Color body, Color wings) {
        auto at = [&](float along, float across) {
            return on_terrain(map, {ground.x + f.x * along + side.x * across, ground.y + f.y * along + side.y * across},
                              lift);
        };
        for (const float s : {-1.0f, 1.0f}) {
            fill_quad(at(0.18f, 0.0f), at(-0.08f, 0.62f * s), at(-0.2f, 0.62f * s), at(-0.12f, 0.0f), wings);
            fill_quad(at(-0.4f, 0.0f), at(-0.52f, 0.24f * s), at(-0.58f, 0.24f * s), at(-0.55f, 0.0f), wings);
        }
        DrawLineEx(at(0.6f, 0.0f), at(-0.6f, 0.0f), 4.0f, body);
        for (const float s : {-1.0f, 1.0f}) DrawLineEx(at(0.05f, 0.1f * s), at(-0.3f, 0.1f * s), 3.0f, body);
    };
    const Color color = shade(theme::player_color(u.owner), 0.8f);
    if (u.airborne) shape(0.0f, {0, 0, 0, 60}, {0, 0, 0, 50});
    shape(u.airborne ? kFlightLift : 4.0f, color, shade(color, 0.8f));
}

void WorldRenderer::draw_unit(const engine::TileMap& map, const engine::Unit& u, float alpha) const {
    const engine::UnitTypeDef& def = engine::unit_type(u.type);
    if (def.aircraft) return draw_aircraft(map, u, alpha);
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
    if (u.type == engine::UnitTypeId::Worker && u.order == engine::Order::Gather && u.work > 0) {
        // At work: an axe (or a pick) swinging up and down, a stroke a second.
        const float phase = static_cast<float>(u.work % engine::kChopTicks) / static_cast<float>(engine::kChopTicks);
        const float lift = 7.0f * std::cos(phase * 6.2831853f);
        const Vector2 head{hands.x + offset.x * 1.3f, hands.y + offset.y * 1.3f - lift};
        DrawLineEx(hands, head, 1.5f, {110, 84, 54, 255});
        DrawRectangleRec({head.x - 2.0f, head.y - 2.0f, 4.0f, 3.0f}, {150, 150, 150, 255});
    } else {
        DrawLineEx(hands, {hands.x + offset.x, hands.y + offset.y}, thickness, weapon);
    }
    if (u.carrying > 0) {
        // A bundle of timber or stone on the back, as big as it's got.
        const float size = 0.4f + 0.6f * std::min(1.0f, static_cast<float>(u.carrying) / engine::kCarryCapacity);
        const Vector2 back = iso_offset({-facing.x * 0.12f, -facing.y * 0.12f});
        DrawRectangleRec({hands.x + back.x - 3.5f * size, hands.y + back.y - 6.0f * size, 7.0f * size, 6.0f * size},
                         {120, 88, 52, 255});
    }
    DrawRectangleRoundedLines({feet.x - 3.0f, feet.y - 13.0f, 6.0f, 12.0f}, 0.6f, 4, dark);
    if (u.type == engine::UnitTypeId::Signaler) {
        const Vector2 back = iso_offset({-facing.x * 0.1f, -facing.y * 0.1f});
        const Vector2 set{hands.x + back.x, hands.y + back.y - 2.0f};
        DrawRectangleRec({set.x - 2.5f, set.y - 3.0f, 5.0f, 6.0f}, {70, 78, 58, 255});
        DrawLineEx({set.x + 1.5f, set.y - 3.0f}, {set.x + 4.0f, set.y - 22.0f}, 1.0f, {30, 30, 30, 255});
    }
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

    if (u.type == engine::UnitTypeId::FieldHq) {
        // A long eight-wheeled hull with a shelter on top and whip antennas.
        auto at = [&](float k) { return Vector2{ground.x + facing.x * k, ground.y + facing.y * k}; };
        draw_box(map, ground, facing, 0.9f, 0.46f, 8.0f, color);
        draw_box(map, at(-0.12f), facing, 0.4f, 0.34f, 5.0f, shade(color, 1.12f), 8.0f);
        const Vector2 side{-facing.y * 0.16f, facing.x * 0.16f};
        for (const float k : {-1.0f, 1.0f}) {
            const Vector2 base{ground.x - facing.x * 0.3f + side.x * k, ground.y - facing.y * 0.3f + side.y * k};
            const Vector2 foot = on_terrain(map, base, 12.0f);
            DrawLineEx(foot, {foot.x + 3.0f * k, foot.y - 26.0f}, 1.2f, {30, 32, 30, 255});
        }
        return;
    }

    if (u.type == engine::UnitTypeId::DfStation) {
        // A truck with a closed van body; set up, a mast with a loop antenna
        // stands up from it, packed it lies along the roof.
        auto at = [&](float k) { return Vector2{ground.x + facing.x * k, ground.y + facing.y * k}; };
        draw_box(map, at(-0.12f), facing, 0.56f, 0.44f, 3.0f, shade(color, 0.7f));
        draw_box(map, at(-0.12f), facing, 0.52f, 0.4f, 8.0f, {104, 110, 88, 255}, 3.0f);
        draw_box(map, at(0.3f), facing, 0.26f, 0.42f, 10.0f, color);
        const Vector2 foot = on_terrain(map, at(-0.2f), 11.0f);
        const Color mast = {40, 42, 38, 255};
        if (u.deployed) {
            const Vector2 top{foot.x, foot.y - 26.0f};
            DrawLineEx(foot, top, 1.5f, mast);
            DrawEllipseLines(static_cast<int>(top.x), static_cast<int>(top.y - 4.0f), 3.0f, 5.0f, mast);
        } else {
            DrawLineEx(foot, on_terrain(map, at(0.2f), 12.0f), 1.5f, mast);
        }
        return;
    }

    if (u.type == engine::UnitTypeId::Spg) {
        draw_box(map, ground, facing, 0.9f, 0.56f, 6.0f, color);
        const Vector2 turret{ground.x - facing.x * 0.1f, ground.y - facing.y * 0.1f};
        draw_box(map, turret, facing, 0.42f, 0.42f, 5.0f, shade(color, 1.1f), 6.0f);
        const Vector2 hub = on_terrain(map, turret, 9.0f);
        const float reach = u.deployed ? 0.4f : 0.75f;
        const Vector2 tip = on_terrain(map, {turret.x + facing.x * reach, turret.y + facing.y * reach},
                                       u.deployed ? 22.0f : 10.0f);
        DrawLineEx(hub, tip, 3.0f, shade(color, 0.4f));
        if (u.camouflaged) {
            fill_ground_ellipse(on_terrain(map, ground, 10.0f), 0.6f, {64, 88, 52, 170});
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
    const bool from_air = engine::unit_type(p.shooter_type).aircraft;
    if (p.lobbed && !from_air) height += 4.0f * t * (1.0f - t) * std::max(1.0f, total * 0.3f);  // an arc
    const Vector2 pos = iso::project(ground, height);

    Vector2 dir = iso_offset(to_vector2(p.target - p.origin));
    const float len = std::hypot(dir.x, dir.y);
    dir = len > 0.0f ? Vector2{dir.x / len, dir.y / len} : Vector2{1.0f, 0.0f};

    if (from_air || p.at_air) {
        // A rocket diving from an aircraft, or a missile climbing to one: a smoke trail.
        const Vector2 prev = to_vector2(p.prev_pos);
        const float tp = total > 0.0f ? std::hypot(prev.x - origin.x, prev.y - origin.y) / total : 1.0f;
        const float hp = to_float(p.origin_height) + (to_float(p.target_height) - to_float(p.origin_height)) * tp;
        DrawLineEx(iso::project(prev, hp), pos, 2.0f, {220, 220, 210, 150});
        DrawCircleV(pos, 2.5f, {255, 170, 60, 255});
        return;
    }
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
            const engine::Unit* target = world.find_unit(u.engaged);
            const float lift = target && target->airborne ? kFlightLift : 6.0f;  // up at an aircraft
            DrawLineV(muzzle, on_terrain(map, to_vector2(u.last_shot_at), lift), {255, 235, 160, 140});
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

// Our rear troops at the wood and trucks collecting there: how full they are.
void WorldRenderer::draw_load_bar(const engine::TileMap& map, const engine::Unit& u, float alpha) const {
    if (const int32_t seats = engine::unit_type(u.type).troop_capacity; seats > 0 && !u.passengers.empty()) {
        // The squad aboard: a pip a man, over the seats.
        const Vector2 feet = on_terrain(map, unit_ground_pos(u, alpha));
        const float x = feet.x - static_cast<float>(seats) * 2.0f;
        const float y = feet.y - 32.0f;
        DrawRectangleRec({x - 1, y - 1, static_cast<float>(seats) * 4.0f + 1, 5}, {0, 0, 0, 170});
        for (int32_t i = 0; i < static_cast<int32_t>(u.passengers.size()); ++i) {
            DrawRectangleRec({x + static_cast<float>(i) * 4.0f, y, 3, 3}, {225, 215, 160, 255});
        }
        return;
    }
    float full = -1.0f;
    if (u.order == engine::Order::Gather && u.carrying > 0) {
        full = static_cast<float>(u.carrying) / static_cast<float>(engine::kCarryCapacity);
    }
    if (u.order == engine::Order::Collect) full = static_cast<float>(u.carrying) / static_cast<float>(engine::kTruckCapacity);
    if (full < 0.0f) return;
    const Vector2 feet = on_terrain(map, unit_ground_pos(u, alpha));
    const float w = engine::unit_type(u.type).vehicle ? 26.0f : 14.0f;
    const float y = feet.y - (engine::unit_type(u.type).vehicle ? 32.0f : 30.0f);
    DrawRectangleRec({feet.x - w * 0.5f - 1, y - 1, w + 2, 5}, {0, 0, 0, 170});
    DrawRectangleRec({feet.x - w * 0.5f, y, w * std::min(1.0f, full), 3}, {196, 150, 90, 255});
}

// Our own radios: silent ones, and the ones an order is on its way to by courier.
void WorldRenderer::draw_radio_marks(const engine::World& world, const engine::Unit& u, float alpha) const {
    const bool courier = std::any_of(world.couriers().begin(), world.couriers().end(), [&](const engine::Courier& c) {
        return std::find(c.cmd.units.begin(), c.cmd.units.end(), u.id) != c.cmd.units.end();
    });
    if (!u.silent && !courier) return;
    const Vector2 feet = on_terrain(world.map(), unit_ground_pos(u, alpha));
    float x = feet.x - 29.0f;
    const float y = feet.y - 30.0f;
    if (u.silent) {
        DrawRectangleRec({x, y, 10, 11}, {60, 74, 110, 230});
        DrawText("R", static_cast<int>(x + 2), static_cast<int>(y + 1), 10, WHITE);
        DrawLineEx({x, y + 11}, {x + 10, y}, 1.5f, {230, 80, 60, 255});
        x -= 12.0f;
    }
    if (courier) {
        // An envelope.
        DrawRectangleRec({x, y + 2, 10, 7}, {236, 228, 204, 240});
        DrawLineV({x, y + 2}, {x + 5, y + 6}, {110, 90, 60, 255});
        DrawLineV({x + 10, y + 2}, {x + 5, y + 6}, {110, 90, 60, 255});
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
