#include "render/world_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include <rlgl.h>

#include "render/convert.h"
#include "render/iso.h"
#include "render/plants.h"
#include "theme/palette.h"

namespace render {

namespace {

constexpr float kPingLifetime = 0.5f;
// The fire out of a muzzle, its shapes (see bake_muzzle_fire).
enum MuzzleFire : int {
    kRifleFire,    // a rifle's, a machine gun's: a spurt
    kGunFire,      // a heavy machine gun's, an autocannon's, a rocket's
    kBrakeFire,    // an autocannon's out of its muzzle brake, a mortar's
    kTankFire,     // a tank's big gun: a great tongue of fire, a ball at its end
    kHowitzerFire, // a howitzer's out of its muzzle brake: ahead and to both sides
    kMuzzleShapes
};
constexpr float kBlastLifetime = 3.6f;  // (the longest blast's sheet: a crash's mushroom)
constexpr float kMushroomBlast = 4.5f;  // a Blast this big (tiles) is an aircraft's crash: the mushroom
constexpr float kWreckLifetime = 25.0f;
constexpr float kBodyLifetime = 20.0f;

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
// The glowing outline round what's under the cursor: drawn over the screen
// from a texture the thing alone was drawn into, where its silhouette isn't
// but is close by: a bright line a couple of pixels wide, a glow fading out
// beyond it. Its colour is the tint's.
constexpr const char* kOutlineFragment = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 texel;
out vec4 finalColor;
void main() {
    if (texture(texture0, fragTexCoord).a > 0.5) { finalColor = vec4(0.0); return; }
    float nearest = 99.0;
    for (int y = -5; y <= 5; ++y) {
        for (int x = -5; x <= 5; ++x) {
            float d = length(vec2(float(x), float(y)));
            if (d > 5.5 || d >= nearest) continue;
            if (texture(texture0, fragTexCoord + vec2(float(x), float(y)) * texel).a > 0.5) nearest = d;
        }
    }
    float a = nearest <= 2.1 ? 1.0 : nearest > 5.5 ? 0.0 : 0.55 * (1.0 - (nearest - 2.1) / 3.4);
    finalColor = vec4(fragColor.rgb, a * fragColor.a);
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

// Aircraft in the air are drawn as high above their shadow as they fly
// (see engine::kCruiseHeight), pixels: easing to it frame by frame (the
// engine moves it a tick at a time).
std::unordered_map<engine::EntityId, float> g_drawn_height;
float flight_lift(const engine::Unit& u) {
    if (!u.airborne) return 0.0f;
    const auto it = g_drawn_height.find(u.id);
    return (it != g_drawn_height.end() ? it->second : to_float(u.altitude)) * iso::kElevationStep;
}

// How high a unit's body is above its feet, pixels.
float body_lift(const engine::Unit& u) {
    if (u.airborne) return flight_lift(u);
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

soldiers::Kit kit_of(engine::UnitTypeId type);  // (with the soldiers' drawing, below)

namespace {
const std::unordered_map<engine::EntityId, Settled>* g_settled = nullptr;  // (the renderer's)
}

Vector2 unit_ground_pos(const engine::Unit& unit, float alpha) {
    Vector2 p = lerp(to_vector2(unit.prev_pos), to_vector2(unit.pos), alpha);
    if (g_settled) {
        if (const auto it = g_settled->find(unit.id); it != g_settled->end()) p = {p.x + it->second.offset.x, p.y + it->second.offset.y};
    }
    return p;
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

// Drawn as armor, a hull and a turret in pixel art: tanks, IFVs and APCs, SPGs, AA guns.
bool drawn_as_armor(const engine::UnitTypeDef& def) {
    return def.family == engine::Family::Tank || def.family == engine::Family::Apc || def.family == engine::Family::Spg ||
           def.family == engine::Family::AntiAir;
}
bool has_gun_look(engine::VehicleModel m);       // a towed howitzer
bool has_plane_look(engine::VehicleModel m);     // an aircraft
float gun_barrel_of(engine::VehicleModel m);     // a towed gun's barrel, tiles
bool raises_gun(engine::VehicleModel m);         // a self-propelled howitzer
bool loads_at_an_angle(engine::VehicleModel m);  // a tank with a carousel autoloader: its gun goes to the loading angle
// A gun's elevation, as its sprite's frames have it. A tank's turret: level,
// 4, 8 and 14 degrees up (the last for indirect fire), and 3 down (the dip
// after a shot); kTankLadder has them in the order of the angle. A
// howitzer's (an SPG's turret, a towed gun): travelling, then laid at 8,
// 16, 26 and 40 degrees.
constexpr float kTankPitch[] = {0.0f, 4.0f, 8.0f, 14.0f, -3.0f};
constexpr int kTankLadder[] = {4, 0, 1, 2, 3};
constexpr int kTankFrames = 5;
constexpr float kHowitzerPitch[] = {0.0f, 8.0f, 16.0f, 26.0f, 40.0f};
constexpr int kHowitzerFrames = 5;
// A howitzer's barrel after a shot: thrown back in its cradle at once (the
// long recoil, a quarter of its length), run out again over half a second.
// Its sprite apart from the turret (the carriage), a frame for each angle
// it's laid at and each state of the recoil.
constexpr int kRecoilStates = 3;
constexpr float kRecoilShare[kRecoilStates] = {0.0f, 0.12f, 0.24f};
constexpr int kBarrelFrames = (kHowitzerFrames - 1) * kRecoilStates;
int recoil_state(float since) { return since < 0.15f ? 2 : since < 0.5f ? 1 : 0; }
// Whether a gun turned `dir` is drawn in front of its turret, as the bake
// has it for the direction its sprite is drawn at (see draw_vehicle_turret).
bool gun_toward_viewer(Vector2 dir, int dirs) {
    float a = std::atan2(dir.y, dir.x);
    if (a < 0.0f) a += 6.2831853f;
    const int d = static_cast<int>(std::lround(a / 6.2831853f * static_cast<float>(dirs))) % dirs;
    const float q = static_cast<float>(d) * 6.2831853f / static_cast<float>(dirs);
    return std::cos(q) + std::sin(q) > 0.0f;
}
// A tile of height in a Frame's pixels: the view is from 30 degrees up.
constexpr float kZPerTile = 39.2f;
// A felled tree falls over in this long (seconds).
constexpr float kTreeFallSeconds = 1.1f;
// Tracks in the ground fade over this long (seconds).
constexpr float kTrackSeconds = 300.0f;
// A scout's platform up a tree: how high (pixels).
constexpr float kTreePostZ = 24.0f;
// A cell tower's mast is drawn this much taller than its tile would have it (see building_scale).
constexpr float kTowerScale = 1.8f;
// A towed gun's or an aircraft's sprite sheets by its wear.
int small_variant(engine::VehicleModel m, int wear) { return static_cast<int>(m) * 8 + wear; }  // (6, 7: an aircraft's crashed wreck, burnt, rusted)
// A towed gun, an aircraft: a sprite of its own, its wreck too.
bool gun_or_plane(const engine::UnitTypeDef& def) { return def.family == engine::Family::Gun || def.family == engine::Family::Aircraft; }

// Where on a tank (an IFV) its gun's muzzle and its engine deck are: along the hull
// from its middle (tiles, drawn size) and up (pixels). With the vehicles' pixel art.
Vector2 armor_muzzle(engine::VehicleModel m);
Vector2 armor_engine(engine::VehicleModel m);
int wear_of(int32_t hp, int32_t max_hp);
float track_half(engine::UnitTypeId type);  // how far each track runs from the middle, tiles
Vector2 armor_size(engine::VehicleModel m);           // its hull's length and half its width, tiles
int armor_variant(engine::VehicleModel model, int era, int wear, int sink);
constexpr float kTankWreckLifetime = 150.0f;  // a burnt-out tank stays a while on the field
constexpr float kVehicleScale = 1.25f;   // vehicles in pixel art: a little over life size against the tiles
// The trucks, in pixel art (below): which one a unit is, by its job and its
// side; its sprite sheets; what it carries; whether a radar turns on it and
// where; where its engine is (along, up).
std::optional<TruckModel> truck_model(engine::UnitTypeId type, engine::PlayerId owner);
int truck_variant(TruckModel model, int wear, int sink, int load);
// What a truck carries, as it shows: food in sacks; building materials
// (boards, bricks, cement); logs from the wood; stone from the quarry;
// ammunition in its boxes; shells; fuel in drums; a tanker's fuel (not seen).
// (LogsSome, LogsMost, StoneSome, StoneMost: a truck's bed a third, two thirds full as the rear troops load it.)
enum class Cargo : uint8_t { None, Sacks, Boards, Logs, Stone, Crates, Shells, Drums, Fuel, LogsSome, LogsMost, StoneSome, StoneMost, Count };
bool is_logs(Cargo c) { return c == Cargo::Logs || c == Cargo::LogsSome || c == Cargo::LogsMost; }
bool is_stone(Cargo c) { return c == Cargo::Stone || c == Cargo::StoneSome || c == Cargo::StoneMost; }
Cargo cargo_of(const engine::World& world, const engine::Unit& u);
// A spot a vehicle on fire burns at, the same all the while: along it and
// across it (tiles), up (pixels), how big a flame (0..2).
struct FlameSpot {
    float along;
    float across;
    float z;
    int size;
    uint32_t seed;
};
// Where a vehicle burns: its engine, its cab, its load (a tank's engine
// deck, its turret, its hatches). `wreck`: burnt out, `age` seconds ago
// (burning its first half minute, dying down after); else still running,
// burning low. A tanker's fuel burning in a pool round it.
std::vector<FlameSpot> vehicle_flames(engine::UnitTypeId type, engine::PlayerId owner, bool wreck, bool blown, Cargo cargo, float age, uint32_t seed);
int truck_load(Cargo c);  // the load its sprite shows (0: none)
int wreck_load(Cargo c);  // what its wreck still shows of it
bool truck_turns_top(TruckModel m);
Vector2 truck_top_ring_of(TruckModel m);
Vector2 truck_engine_of(TruckModel m);

float WorldRenderer::fx_random() {
    fx_rng_ ^= fx_rng_ << 13;
    fx_rng_ ^= fx_rng_ >> 17;
    fx_rng_ ^= fx_rng_ << 5;
    return static_cast<float>(fx_rng_ & 0xFFFFFF) / static_cast<float>(0x1000000);
}

// What went off, for how its smoke looks: a gun's shell (earth and black
// smoke up in a column), a mortar bomb's (a sharp grey puff), an automatic
// grenade launcher's (little ones), a rocket's (dirty, spread low), a bomb's
// (a huge black column over a brown dome), a cluster's bomblets (little
// white puffs), an incendiary's (white smoke, burning bits), white
// phosphorus (white and dense, its streamers arcing out), a mine's or a
// charge's (earth thrown up).
enum class Burst : uint8_t { Shell, Mortar, Grenade, Rocket, Bomb, Cluster, Incendiary, Phosphorus, Dirt };
Burst burst_of(const engine::Impact& im) {
    switch (im.shell) {
        case engine::Shell::Cluster: return Burst::Cluster;
        case engine::Shell::Incendiary: return Burst::Incendiary;
        case engine::Shell::Phosphorus: return Burst::Phosphorus;
        default: break;
    }
    const engine::UnitTypeDef& def = engine::unit_type(im.shooter_type);
    if (def.weapon.aerial_bomb) return Burst::Bomb;
    if (def.aircraft || im.shooter_type == engine::UnitTypeId::Mlrs) return Burst::Rocket;
    if (im.shooter_type == engine::UnitTypeId::Mortar) return Burst::Mortar;
    if (im.shooter_type == engine::UnitTypeId::Ags) return Burst::Grenade;
    if (im.shooter_type == engine::UnitTypeId::Sapper || def.weapon.splash_radius.raw == 0) return Burst::Dirt;  // a charge, a mine under a vehicle
    return Burst::Shell;
}

// A shell or a rocket going off: clods of earth flung up in arcs (spray off
// the water), dust and smoke rolling up, as what went off makes it (see Burst).
void WorldRenderer::spawn_burst(const engine::World& world, Vector2 at, float splash, uint8_t kind_of) {
    const auto kind = static_cast<Burst>(kind_of);
    const engine::TileMap& map = world.map();
    const engine::TilePos t = map.clamp_tile({static_cast<int32_t>(std::floor(at.x)), static_cast<int32_t>(std::floor(at.y))});
    const bool water = map.terrain(t) == engine::Terrain::Water;
    const float big = std::clamp(splash, 0.2f, 2.0f);
    const bool small = kind == Burst::Cluster || kind == Burst::Grenade || kind == Burst::Phosphorus;
    const int bits = (small ? 3 : 10) + static_cast<int>(big * (small ? 8.0f : 20.0f));
    for (int i = 0; i < bits; ++i) {
        const float a = fx_random() * 6.2831853f;
        const float speed = (0.4f + 1.2f * fx_random()) * (0.5f + big * 0.6f);
        Particle p{};
        p.kind = water ? Particle::Kind::Spray : Particle::Kind::Clod;
        p.ground = at;
        p.z = 1.0f;
        p.vel = {std::cos(a) * speed, std::sin(a) * speed};
        p.vz = (70.0f + 110.0f * fx_random()) * (0.6f + big * 0.5f);
        p.life = 1.8f + 1.0f * fx_random();
        p.size = 1.5f + 2.0f * fx_random();
        p.grow = 0.0f;
        static constexpr Color kEarth[3] = {{70, 56, 40, 255}, {98, 80, 58, 255}, {82, 94, 58, 255}};
        p.color = water ? Color{222, 232, 236, 255} : kEarth[i % 3];
        particles_.push_back(p);
    }
    // Its smoke: some of it up in a column, the rest rolling out low round it.
    struct Look {
        int puffs;
        float column;  // the share of it up in the column
        Color up;      // the column's
        Color low;     // what rolls out
        float rise;    // how fast the column goes up, pixels a second
        float spread;  // how fast it rolls out, tiles a second
        float life;
        float size;
    };
    Look look{4 + static_cast<int>(big * 6.0f), 0.34f, {54, 48, 42, 215}, {120, 106, 88, 195}, 55.0f, 0.3f, 1.8f, 1.0f};
    switch (kind) {
        case Burst::Shell: break;
        case Burst::Mortar: look = {3 + static_cast<int>(big * 3.0f), 0.4f, {142, 138, 130, 200}, {160, 150, 132, 180}, 38.0f, 0.25f, 1.4f, 0.8f}; break;
        case Burst::Grenade: look = {2, 0.5f, {130, 126, 118, 190}, {150, 142, 126, 170}, 26.0f, 0.2f, 1.0f, 0.6f}; break;
        case Burst::Rocket: look = {5 + static_cast<int>(big * 5.0f), 0.2f, {60, 56, 52, 215}, {92, 86, 78, 205}, 40.0f, 0.55f, 2.0f, 1.0f}; break;
        case Burst::Bomb: look = {18, 0.45f, {30, 28, 26, 235}, {132, 114, 88, 210}, 80.0f, 0.7f, 4.0f, 1.6f}; break;
        case Burst::Cluster: look = {2, 0.3f, {196, 194, 186, 190}, {200, 196, 186, 170}, 20.0f, 0.2f, 0.9f, 0.55f}; break;
        case Burst::Incendiary: look = {5, 0.5f, {206, 202, 192, 210}, {176, 170, 158, 190}, 34.0f, 0.3f, 2.2f, 1.0f}; break;
        case Burst::Phosphorus: look = {8, 0.35f, {242, 242, 236, 235}, {232, 232, 226, 220}, 30.0f, 0.4f, 2.6f, 1.1f}; break;
        case Burst::Dirt: look = {5, 0.5f, {104, 88, 66, 210}, {126, 108, 84, 190}, 50.0f, 0.3f, 1.6f, 1.0f}; break;
    }
    for (int i = 0; i < look.puffs; ++i) {
        const bool column = static_cast<float>(i) < static_cast<float>(look.puffs) * look.column;
        const float a = fx_random() * 6.2831853f;
        Particle p{};
        p.kind = Particle::Kind::Smoke;
        p.ground = {at.x + (fx_random() - 0.5f) * 0.3f * big, at.y + (fx_random() - 0.5f) * 0.3f * big};
        p.z = 2.0f + 6.0f * fx_random();
        const float out = column ? 0.1f : look.spread * (0.5f + fx_random());
        p.vel = {0.12f + std::cos(a) * out, -0.08f + std::sin(a) * out};
        p.vz = column ? look.rise * (0.6f + 0.5f * fx_random()) * (0.7f + 0.3f * big) : 6.0f + 10.0f * fx_random();
        p.life = look.life * (0.75f + 0.5f * fx_random()) + big * 0.6f;
        p.size = (2.5f + 3.5f * big * fx_random()) * look.size;
        p.grow = (5.0f + 4.0f * fx_random()) * look.size;
        const Color c = column ? look.up : look.low;
        p.color = water ? Color{214, 222, 226, 170} : c;
        particles_.push_back(p);
    }
    if (kind == Burst::Phosphorus) {  // its streamers: burning bits arcing out, white trails
        for (int i = 0; i < 18; ++i) {
            const float a = fx_random() * 6.2831853f;
            const float v = 1.2f + 2.0f * fx_random();
            Particle p{};
            p.kind = Particle::Kind::Spark;
            p.ground = at;
            p.z = 4.0f;
            p.vel = {std::cos(a) * v, std::sin(a) * v};
            p.vz = 70.0f + 70.0f * fx_random();
            p.life = 1.2f + 0.8f * fx_random();
            p.size = 1.5f;
            p.color = {255, 255, 240, 255};
            particles_.push_back(p);
        }
    }
    if (kind == Burst::Incendiary) {  // burning bits scattered
        spawn_sparks(at, 4.0f, 14, {255, 170, 60, 255}, 1.6f);
    }
}

// A shot: smoke out of the muzzle, blown along the barrel; a tank's or a
// gun's much more of it, and the dust its blast kicks up off the ground.
void WorldRenderer::spawn_muzzle(const engine::World& world, const engine::Unit& u) {
    const engine::UnitTypeDef& def = engine::unit_type(u.type);
    Vector2 f = to_vector2(u.facing);
    const float l = std::hypot(f.x, f.y);
    f = l > 0.0f ? Vector2{f.x / l, f.y / l} : Vector2{1.0f, 0.0f};
    const Vector2 g = to_vector2(u.pos);
    float reach = def.vehicle ? 0.5f : 0.25f;
    float height = def.vehicle ? 8.0f : 9.0f;
    if (drawn_as_armor(def) || has_gun_look(def.model)) {  // up where its gun is laid
        const Vector2 m = muzzle_of(u);
        reach = m.x;
        height = m.y;
    } else if (def.aircraft) {
        reach = 0.3f;
        height = flight_lift(u);
    }
    const bool big = def.vehicle || def.weapon.indirect;
    Vector2 muzzle{g.x + f.x * reach, g.y + f.y * reach};
    if (!def.vehicle) soldier_muzzle(u, g, f, muzzle, height);  // his weapon as he's drawn
    // What went out of it: a tank's area shot a heavier charge.
    float splash = to_float(def.weapon.splash_radius);
    for (const engine::Projectile& p : world.projectiles()) {
        if (p.shooter == u.id) splash = std::max(splash, to_float(p.weapon.splash_radius));
    }
    // The flash: a gun's big and white-hot, a rifle's a spark.
    const bool gun = drawn_as_armor(def) || def.weapon.indirect;
    spawn_flash(muzzle, height, gun ? 4.0f + 3.0f * splash : def.vehicle ? 3.0f : 1.6f, {255, 236, 170, 255});
    // The fire out of the muzzle, along the barrel as it's laid: a tank's a great tongue of it, a
    // howitzer's out of its muzzle brake to the sides too, an autocannon's, a machine gun's, a rifle's spurt;
    // a mortar's up out of its tube. An RPG's, a MANPADS's: the back-blast's fire out behind as well.
    {
        const engine::WeaponDef& weapon = engine::weapon_of(u);
        const bool tube = engine::is_tube_artillery(def) || u.type == engine::UnitTypeId::Spg;
        const bool mortar = u.type == engine::UnitTypeId::Mortar;
        float pitch = (drawn_as_armor(def) || has_gun_look(def.model)) ? gun_elevation(u) : mortar ? 62.0f : 0.0f;
        pitch *= 0.0174533f;
        const Vector2 flat = iso_offset(f);
        Vector2 sd{flat.x * std::cos(pitch), flat.y * std::cos(pitch) - std::sin(pitch) * kZPerTile};
        const float sl = std::hypot(sd.x, sd.y);
        sd = sl > 0.0f ? Vector2{sd.x / sl, sd.y / sl} : Vector2{1.0f, 0.0f};
        int shape = kRifleFire;
        float life = 0.12f;
        if (def.tank && drawn_as_armor(def)) {
            shape = kTankFire;
            life = 0.26f;
        } else if (tube) {
            shape = kHowitzerFire;
            life = 0.28f;
        } else if (mortar) {
            shape = kBrakeFire;
            life = 0.14f;
        } else if (def.vehicle) {
            shape = u.type == engine::UnitTypeId::Mlrs || def.aircraft ? kGunFire : kBrakeFire;
            life = 0.1f;
        } else if (u.type == engine::UnitTypeId::MachineGunner || u.type == engine::UnitTypeId::Ags) {
            shape = kGunFire;
            life = 0.1f;
        }
        if (u.type != engine::UnitTypeId::Mlrs) spawn_muzzle_fire(muzzle, height, sd, shape, life);
        const bool back_blast = weapon.damage_type == engine::DamageType::AntiTank && !def.vehicle;  // an RPG
        if (back_blast || u.type == engine::UnitTypeId::Manpads || u.type == engine::UnitTypeId::Mlrs) {
            const Vector2 behind{g.x - f.x * (u.type == engine::UnitTypeId::Mlrs ? 0.45f : 0.2f), g.y - f.y * (u.type == engine::UnitTypeId::Mlrs ? 0.45f : 0.2f)};
            spawn_muzzle_fire(behind, u.type == engine::UnitTypeId::Mlrs ? 10.0f : height, {-sd.x, -sd.y}, u.type == engine::UnitTypeId::Mlrs ? kTankFire : kGunFire, 0.16f);
            for (int i = 0; i < 4; ++i) {  // its dust, blown out behind
                Particle p{};
                p.kind = Particle::Kind::Blow;
                p.ground = behind;
                p.z = 3.0f;
                p.vel = {-f.x * (0.8f + fx_random()) + (fx_random() - 0.5f) * 0.5f, -f.y * (0.8f + fx_random()) + (fx_random() - 0.5f) * 0.5f};
                p.vz = 3.0f + 4.0f * fx_random();
                p.life = 1.0f + 0.6f * fx_random();
                p.size = 2.0f + 1.5f * fx_random();
                p.grow = 6.0f;
                p.color = {176, 160, 132, 170};
                particles_.push_back(p);
            }
        }
    }
    if (u.type == engine::UnitTypeId::Mlrs) {  // a rocket's back-blast rolling out behind the launcher
        for (int i = 0; i < 4; ++i) {
            Particle p{};
            p.kind = Particle::Kind::Blow;
            p.ground = {g.x - f.x * 0.4f, g.y - f.y * 0.4f};
            p.z = 10.0f;
            p.vel = {-f.x * (1.0f + fx_random()) + (fx_random() - 0.5f) * 0.6f, -f.y * (1.0f + fx_random()) + (fx_random() - 0.5f) * 0.6f};
            p.vz = 4.0f + 6.0f * fx_random();
            p.life = 1.4f + 0.8f * fx_random();
            p.size = 3.0f + 2.0f * fx_random();
            p.grow = 8.0f;
            p.color = {196, 190, 180, 170};
            particles_.push_back(p);
        }
    }
    const int puffs = big ? 5 : 1;
    for (int i = 0; i < puffs; ++i) {
        Particle p{};
        p.kind = big ? Particle::Kind::Blow : Particle::Kind::Smoke;
        p.ground = muzzle;
        p.z = height;
        const float k = big ? 0.4f + 0.9f * fx_random() : 0.3f;
        p.vel = {f.x * k + (fx_random() - 0.5f) * 0.3f, f.y * k + (fx_random() - 0.5f) * 0.3f};
        p.vz = 3.0f + 6.0f * fx_random();
        p.life = big ? 1.1f + 0.8f * fx_random() : 0.5f;
        p.size = big ? 2.5f + 2.0f * fx_random() : 1.2f;
        p.grow = big ? 7.0f : 3.0f;
        p.color = {186, 182, 172, static_cast<unsigned char>(big ? 190 : 140)};
        particles_.push_back(p);
    }
    if (!def.tank && !def.weapon.indirect) return;
    for (int i = 0, n = splash >= 1.0f ? 14 : 6; i < n; ++i) {  // the blast off the ground
        const float a = fx_random() * 6.2831853f;
        Particle p{};
        p.kind = Particle::Kind::Blow;
        p.ground = {g.x + f.x * reach * 0.7f, g.y + f.y * reach * 0.7f};
        p.z = 1.0f;
        p.vel = {std::cos(a) * 0.9f, std::sin(a) * 0.9f};
        p.vz = 2.0f;
        p.life = 0.8f + 0.5f * fx_random();
        p.size = 2.0f + 1.5f * fx_random();
        p.grow = 6.0f;
        p.color = {140, 126, 100, 160};
        particles_.push_back(p);
    }
    (void)world;
}

void WorldRenderer::spawn_muzzle_fire(Vector2 at, float z, Vector2 dir, int shape, float life) {
    Particle p{};
    p.kind = Particle::Kind::Muzzle;
    p.ground = at;
    p.z = z;
    p.life = life;
    p.size = static_cast<float>(shape);
    p.dir = dir;
    particles_.push_back(p);
}

// A flash: a muzzle's, a round punching through armor, reactive armor going off.
void WorldRenderer::spawn_flash(Vector2 at, float z, float size, Color color) {
    Particle p{};
    p.kind = Particle::Kind::Flash;
    p.ground = at;
    p.z = z;
    p.life = 0.1f + 0.02f * size;
    p.size = size;
    p.color = color;
    particles_.push_back(p);
}

// Sparks flying off steel: bullets striking armor, a round going through,
// welding. They fly out, fall and go out.
void WorldRenderer::spawn_sparks(Vector2 at, float z, int n, Color color, float speed) {
    for (int i = 0; i < n; ++i) {
        const float a = fx_random() * 6.2831853f;
        const float v = speed * (0.4f + 0.8f * fx_random());
        Particle p{};
        p.kind = Particle::Kind::Spark;
        p.ground = at;
        p.z = z;
        p.vel = {std::cos(a) * v, std::sin(a) * v};
        p.vz = 20.0f + 50.0f * fx_random();
        p.life = 0.25f + 0.35f * fx_random();
        p.size = 1.0f;
        p.color = color;
        particles_.push_back(p);
    }
}

// Whoever fired, beside the smoke: the recoil of a vehicle's gun; an
// instant hit striking sparks off the vehicle it hit; an autocannon's or a
// machine gun's spent cases flying out of it.
void WorldRenderer::fired(const engine::World& world, const engine::Unit& u) {
    const engine::UnitTypeDef& def = engine::unit_type(u.type);
    if (def.vehicle) vehicles_seen_[u.id].recoil = 0.0f;
    const engine::WeaponDef& weapon = engine::weapon_of(u);
    if (weapon.projectile_speed.raw == 0 && !weapon.indirect) {
        const engine::Unit* target = world.find_unit(u.engaged);
        if (target && engine::unit_type(target->type).vehicle && !target->airborne) {
            spawn_sparks(to_vector2(u.last_shot_at), 6.0f, 3 + static_cast<int>(fx_random() * 3.0f), {255, 214, 120, 255}, 0.7f);
        }
    }
    if (def.vehicle && !def.aircraft && weapon.damage_type == engine::DamageType::Bullet) {
        Vector2 f = to_vector2(u.facing);
        const float l = std::hypot(f.x, f.y);
        f = l > 0.0f ? Vector2{f.x / l, f.y / l} : Vector2{1.0f, 0.0f};
        Particle p{};
        p.kind = Particle::Kind::Casing;
        p.ground = to_vector2(u.pos);
        p.z = 9.0f;
        const float side = fx_random() < 0.5f ? 1.0f : -1.0f;
        p.vel = {-f.y * side * (0.5f + 0.4f * fx_random()), f.x * side * (0.5f + 0.4f * fx_random())};
        p.vz = 40.0f + 30.0f * fx_random();
        p.life = 1.4f;
        p.size = 1.3f;
        p.color = {206, 164, 72, 255};
        particles_.push_back(p);
    }
}

// Vehicles as they go on: struck (a jolt), topped up by a tanker (its
// hose) or an ammunition truck (crates handed over), mended at a workshop
// (welding); their exhaust, puffing on the idle, thicker on the move; the
// dust their tracks and wheels kick up off the dry ground, the mud in a bog.
void WorldRenderer::update_vehicles(const engine::World& world, float dt) {
    const engine::TileMap& map = world.map();
    auto near = [&](const engine::Unit& v, auto pred) -> const engine::Unit* {
        const engine::Unit* best = nullptr;
        float best_d = 2.5f;
        for (const engine::Unit& o : world.units()) {
            if (o.id == v.id || o.owner != v.owner || !pred(o)) continue;
            const float d = std::hypot(to_vector2(o.pos).x - to_vector2(v.pos).x, to_vector2(o.pos).y - to_vector2(v.pos).y);
            if (d < best_d) {
                best_d = d;
                best = &o;
            }
        }
        return best;
    };
    auto serve = [&](Service::Kind kind, engine::EntityId from, engine::EntityId to, Vector2 at = {}) {
        for (Service& s : services_) {
            if (s.kind == kind && s.to == to) {
                s.from = from;
                s.at = at;
                s.age = 0.0f;
                return;
            }
        }
        services_.push_back({kind, from, to, 0.0f, at});
    };
    // How far off what a gun aims at is, tiles (< 0: nothing).
    auto aim_range = [&](const engine::Unit& u) {
        const Vector2 at = to_vector2(u.pos);
        auto off = [&](engine::FixedVec2 p) { return std::hypot(to_float(p.x) - at.x, to_float(p.y) - at.y); };
        if (u.order == engine::Order::AttackGround ||
            (u.order == engine::Order::Ability &&
             (u.order_ability == engine::AbilityId::AreaShot || u.order_ability == engine::AbilityId::IndirectFire))) {
            return off(u.order_point);
        }
        if (const engine::Unit* t = world.find_unit(u.engaged)) return off(t->pos);
        if (u.last_shot_tick != engine::kNeverFired && world.tick() - u.last_shot_tick < 3 * engine::kTicksPerSecond) {
            return off(u.last_shot_at);
        }
        return -1.0f;
    };
    // Where a gun is laid, on its ladder of frames. A tank's by the range to
    // what it shoots at, all the way up for indirect fire, at the loading
    // angle while a carousel autoloader reloads it. A howitzer's as the
    // firing tables have it: the angle that throws a shell that far (half
    // the arcsine of the share of its reach), low while it's being set up or
    // packed up, raised ready once it's set up, and it stays laid when it's
    // done firing.
    auto laid = [&](const engine::Unit& u, const VehicleSeen& v, bool tank) {
        const engine::UnitTypeDef& def = engine::unit_type(u.type);
        const float reach = to_float(engine::weapon_of(u).range);
        if (tank) {
            float step = 4.0f;
            if (u.order != engine::Order::Ability || u.order_ability != engine::AbilityId::IndirectFire) {
                const float d = aim_range(u);
                step = d < reach * 0.3f ? 1.0f : d < reach * 0.65f ? 2.0f : 3.0f;
            }
            if (loads_at_an_angle(def.model) && u.cooldown > engine::kTicksPerSecond / 2 && v.recoil > 0.35f && u.rounds > 0) step = 2.0f;
            return step;
        }
        if (!u.deployed || u.deploy_work > 0) return 1.0f;
        return static_cast<float>(u.laying_to > 0 ? u.laying_to : engine::kReadyStep);  // as its crew lays it
    };
    for (const engine::Unit& u : world.units()) {
        const engine::UnitTypeDef& def = engine::unit_type(u.type);
        if (!def.vehicle) continue;
        auto [it, fresh] = vehicles_seen_.try_emplace(u.id);
        VehicleSeen& v = it->second;
        v.recoil += dt;
        v.jolt += dt;
        v.radio += dt;
        // Its gun laid, a step at a time.
        const bool tank_gun = def.tank && drawn_as_armor(def);
        if (tank_gun || raises_gun(def.model) || has_gun_look(def.model)) {
            const float target = laid(u, v, tank_gun);
            if (v.elev < 0.0f) v.elev = target;
            v.elev += std::clamp(target - v.elev, -3.0f * dt, 3.0f * dt);
        }
        // Its camouflage net going on (as the crew puts it up), coming off.
        {
            float net = u.camouflaged ? 1.0f : 0.0f;
            if (!u.camouflaged && u.order == engine::Order::Ability && u.order_ability == engine::AbilityId::Camouflage) {
                net = std::min(1.0f, static_cast<float>(u.work) / static_cast<float>(engine::kCamouflageWork));
            }
            v.camo += std::clamp(net - v.camo, -3.0f * dt, 1.5f * dt);
        }
        if (!fresh && !u.inside && shows(world, u)) {
            if (u.hp < v.hp && !def.aircraft) {  // hit: it jolts
                v.jolt = 0.0f;
                const float a = fx_random() * 6.2831853f;
                v.jolt_dir = {std::cos(a), std::sin(a) * 0.5f};
            }
            if (u.fuel > v.fuel) {
                if (const engine::Unit* tanker = near(u, [](const engine::Unit& o) { return engine::unit_type(o.type).supplies == engine::Resource::Fuel; })) {
                    serve(Service::Kind::Hose, tanker->id, u.id);
                }
            }
            if (u.rounds > v.rounds) {
                if (const engine::Unit* truck = near(u, [](const engine::Unit& o) { return engine::unit_type(o.type).supplies == engine::Resource::Ammo; })) {
                    serve(Service::Kind::Crates, truck->id, u.id);
                }
            }
            if (u.hp > v.hp) {
                for (const engine::Structure& s : world.structures()) {
                    if (s.type != engine::StructureType::Workshop || s.owner != u.owner) continue;
                    const Vector2 c = to_vector2(s.center);
                    if (std::hypot(c.x - to_vector2(u.pos).x, c.y - to_vector2(u.pos).y) < 4.0f) serve(Service::Kind::Weld, 0, u.id);
                }
            }
            // A tanker filling up at the fuel depot (a hose from it), an
            // ammunition truck loading at the ammunition depot (crates).
            if (u.carrying > v.carrying && def.supplies != engine::Resource::Count) {
                const bool fuel = def.supplies == engine::Resource::Fuel;
                const engine::StructureType depot = fuel ? engine::StructureType::FuelDepot : engine::StructureType::AmmoDepot;
                for (const engine::Structure& s : world.structures()) {
                    if (s.type != depot || s.owner != u.owner) continue;
                    const Vector2 c = to_vector2(s.center);
                    if (std::hypot(c.x - to_vector2(u.pos).x, c.y - to_vector2(u.pos).y) < 3.5f) {
                        serve(fuel ? Service::Kind::Hose : Service::Kind::Crates, 0, u.id, c);
                    }
                }
            }
            // A skill used: its cooldown started.
            for (size_t i = 0; i < engine::kMaxAbilities; ++i) {
                if (u.ability_ready[i] <= v.ready[i]) continue;
                if (def.abilities[i] == engine::AbilityId::Smoke) launch_smoke(u);
                if (def.abilities[i] == engine::AbilityId::CallSupply) v.radio = 0.0f;
            }
            // Set up or packed up: the dust off its spades and jacks.
            if (u.deployed != v.deployed && !def.aircraft) {
                Vector2 f = to_vector2(u.facing);
                const float fl = std::hypot(f.x, f.y);
                f = fl > 0.0f ? Vector2{f.x / fl, f.y / fl} : Vector2{1.0f, 0.0f};
                for (int i = 0; i < (u.deployed ? 12 : 6); ++i) {
                    const float a = fx_random() * 6.2831853f;
                    Particle p{};
                    p.kind = Particle::Kind::Smoke;
                    p.ground = {to_vector2(u.pos).x - f.x * 0.45f + std::cos(a) * 0.3f, to_vector2(u.pos).y - f.y * 0.45f + std::sin(a) * 0.3f};
                    p.z = 1.0f;
                    p.vel = {std::cos(a) * 0.5f, std::sin(a) * 0.5f};
                    p.vz = 3.0f + 3.0f * fx_random();
                    p.life = 1.0f + 0.6f * fx_random();
                    p.size = 2.0f + 1.5f * fx_random();
                    p.grow = 5.0f;
                    p.color = {150, 132, 100, 140};
                    particles_.push_back(p);
                }
            }
            // Digging a gun pit: the earth thrown out round it.
            if (u.order == engine::Order::Ability && u.order_ability == engine::AbilityId::DigGunPit && u.work > 0 && fx_random() < dt * 9.0f) {
                const float a = fx_random() * 6.2831853f;
                Particle p{};
                p.kind = Particle::Kind::Clod;
                p.ground = {to_vector2(u.pos).x + std::cos(a) * 0.55f, to_vector2(u.pos).y + std::sin(a) * 0.55f};
                p.z = 1.0f;
                p.vel = {std::cos(a) * 0.6f, std::sin(a) * 0.6f};
                p.vz = 45.0f + 30.0f * fx_random();
                p.life = 1.4f;
                p.size = 1.4f + fx_random();
                p.color = fx_random() < 0.5f ? Color{92, 74, 52, 255} : Color{70, 58, 42, 255};
                particles_.push_back(p);
            }
        }
        v.hp = u.hp;
        v.fuel = u.fuel;
        v.rounds = u.rounds;
        v.carrying = u.carrying;
        v.ready = u.ability_ready;
        v.deployed = u.deployed;

        // Exhaust and dust.
        if (u.inside || u.airborne || def.aircraft || def.family == engine::Family::Gun || !shows(world, u)) continue;
        Vector2 f = to_vector2(drawn_as_armor(def) ? u.hull : u.facing);
        const float l = std::hypot(f.x, f.y);
        f = l > 0.0f ? Vector2{f.x / l, f.y / l} : Vector2{1.0f, 0.0f};
        const std::optional<TruckModel> truck = truck_model(u.type, u.owner);
        const Vector2 e = truck ? truck_engine_of(*truck) : drawn_as_armor(def) ? armor_engine(def.model) : Vector2{-0.4f, 8.0f};
        const Vector2 pos = to_vector2(u.pos);
        auto count = [&](float rate) {
            const float n = rate * dt;
            int whole = static_cast<int>(n);
            if (fx_random() < n - static_cast<float>(whole)) ++whole;
            return whole;
        };
        for (int i = count(u.moving ? 3.5f : 1.2f); i > 0; --i) {  // exhaust
            Particle p{};
            p.kind = Particle::Kind::Smoke;
            p.ground = {pos.x + f.x * e.x + (fx_random() - 0.5f) * 0.1f, pos.y + f.y * e.x + (fx_random() - 0.5f) * 0.1f};
            p.z = e.y;
            p.vel = {-f.x * 0.2f + (fx_random() - 0.5f) * 0.15f, -f.y * 0.2f + (fx_random() - 0.5f) * 0.15f};
            p.vz = 8.0f + 6.0f * fx_random();
            p.life = 0.9f + 0.6f * fx_random();
            p.size = 1.2f;
            p.grow = u.moving ? 4.5f : 3.0f;
            p.color = u.moving ? Color{96, 98, 102, 110} : Color{130, 134, 140, 70};
            particles_.push_back(p);
        }
        if (!u.moving) continue;
        const engine::Terrain t = map.terrain(map.clamp_tile({static_cast<int32_t>(std::floor(pos.x)), static_cast<int32_t>(std::floor(pos.y))}));
        const bool bog = t == engine::Terrain::Swamp;
        const bool dry = t == engine::Terrain::Grass || t == engine::Terrain::Plowed || t == engine::Terrain::Crops ||
                         t == engine::Terrain::DirtRoad || t == engine::Terrain::Wheat || t == engine::Terrain::Garden ||
                         t == engine::Terrain::Crater || t == engine::Terrain::Riverbed || t == engine::Terrain::Slag ||
                         t == engine::Terrain::Trail;
        if (!dry && !bog) continue;
        const float back = drawn_as_armor(def) ? armor_size(def.model).x * 0.5f * kVehicleScale : 0.55f;
        for (int i = count(bog ? 5.0f : 7.0f); i > 0; --i) {
            const float side = (fx_random() - 0.5f) * 0.5f;
            Particle p{};
            p.ground = {pos.x - f.x * back - f.y * side, pos.y - f.y * back + f.x * side};
            if (bog) {  // mud flung up off the tracks
                p.kind = Particle::Kind::Clod;
                p.z = 2.0f;
                p.vel = {-f.x * 0.5f + (fx_random() - 0.5f) * 0.4f, -f.y * 0.5f + (fx_random() - 0.5f) * 0.4f};
                p.vz = 30.0f + 30.0f * fx_random();
                p.life = 0.9f;
                p.size = 1.2f + fx_random();
                p.color = {58, 56, 44, 255};
            } else {  // dust rolling up behind
                p.kind = Particle::Kind::Smoke;
                p.z = 1.5f;
                p.vel = {-f.x * 0.25f + (fx_random() - 0.5f) * 0.25f, -f.y * 0.25f + (fx_random() - 0.5f) * 0.25f};
                p.vz = 3.0f + 4.0f * fx_random();
                p.life = 1.2f + 0.8f * fx_random();
                p.size = 2.0f + 1.5f * fx_random();
                p.grow = 6.0f;
                p.color = t == engine::Terrain::Plowed || t == engine::Terrain::Slag ? Color{118, 100, 78, 120} : Color{160, 144, 110, 105};
            }
            particles_.push_back(p);
        }
    }
    std::erase_if(vehicles_seen_, [&](const auto& e) { return world.find_unit(e.first) == nullptr; });

    // Services going on: a tanker's hose drips, a workshop's welding throws sparks.
    for (Service& s : services_) {
        s.age += dt;
        const engine::Unit* to = world.find_unit(s.to);
        if (!to || !shows(world, *to)) continue;
        const Vector2 at = to_vector2(to->pos);
        if (s.kind == Service::Kind::Hose && fx_random() < dt * 5.0f) {  // a drop off the nozzle
            Particle p{};
            p.kind = Particle::Kind::Spray;
            p.ground = at;
            p.z = 7.0f;
            p.vel = {(fx_random() - 0.5f) * 0.1f, (fx_random() - 0.5f) * 0.1f};
            p.life = 1.0f;
            p.size = 1.0f;
            p.color = {150, 160, 150, 255};
            particles_.push_back(p);
        }
        if (s.kind == Service::Kind::Weld && fx_random() < dt * 6.0f) {
            const Vector2 spot{at.x + (fx_random() - 0.5f) * 0.4f, at.y + (fx_random() - 0.5f) * 0.4f};
            spawn_flash(spot, 5.0f, 2.5f, {200, 226, 255, 255});
            spawn_sparks(spot, 5.0f, 4, {255, 232, 160, 255}, 0.6f);
        }
    }
    std::erase_if(services_, [&](const Service& s) { return s.age > 1.2f || world.find_unit(s.to) == nullptr; });
}

// A tank on fire: smoke off its engine deck, grey while it's battered, black
// and thick with flames licking up when it's barely going or burnt out.
// Smoke off a vehicle on fire (its flames are drawn at their spots, with it).
void WorldRenderer::spawn_fire(Vector2 at, float height, int wear, float dt) {
    const float smoke_rate = wear >= 4 ? 3.0f : wear == 3 ? 7.0f : 3.0f;  // (a wreck's plume: the engine's smoke, drawn apart)
    // As many as the time since the last frame calls for.
    auto count = [&](float rate) {
        const float n = rate * dt;
        return static_cast<int>(n) + (fx_random() < n - std::floor(n) ? 1 : 0);
    };
    for (int i = count(smoke_rate); i > 0; --i) {
        Particle p{};
        p.kind = Particle::Kind::Smoke;
        p.ground = {at.x + (fx_random() - 0.5f) * 0.15f, at.y + (fx_random() - 0.5f) * 0.15f};
        p.z = height;
        p.vel = {0.18f + (fx_random() - 0.5f) * 0.1f, -0.12f + (fx_random() - 0.5f) * 0.1f};
        p.vz = 14.0f + 10.0f * fx_random();
        p.life = 2.5f + 1.5f * fx_random();
        p.size = 2.0f + 1.5f * fx_random();
        p.grow = 4.0f + 3.0f * fx_random();
        p.color = wear >= 3 ? Color{24, 22, 20, 225} : Color{112, 110, 106, 170};
        p.age = fx_random() * dt;  // spread over the frame
        particles_.push_back(p);
    }
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
        // The villages: the yards round their houses, away from the town and the works.
        village_.assign(static_cast<size_t>(cache_width_ * cache_height_), 0);
        auto near = [&](int x, int y, int r, auto pred) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (map.contains_tile(x + dx, y + dy) && pred(map.terrain(x + dx, y + dy), x + dx, y + dy)) return true;
                }
            }
            return false;
        };
        for (int y = 0; y < cache_height_; ++y) {
            for (int x = 0; x < cache_width_; ++x) {
                const engine::Terrain t = map.terrain(x, y);
                if (t != engine::Terrain::Urban && t != engine::Terrain::House) continue;
                const bool cottage = near(x, y, 2, [&](engine::Terrain n, int nx, int ny) {
                    if (n != engine::Terrain::House) return false;
                    const engine::Structure* s = world.structure_at({nx, ny});
                    return s && s->tiles.size() < engine::kSpaciousTiles;  // a cottage, a small holding's shed or coop
                });
                const bool town = near(x, y, 3, [](engine::Terrain n, int, int) {
                    return n == engine::Terrain::Apartment || n == engine::Terrain::Tower || n == engine::Terrain::GasStation ||
                           n == engine::Terrain::Elevator;
                });
                if (cottage && !town) village_[static_cast<size_t>(y * cache_width_ + x)] = 1;
            }
        }
        // Each village's bus stop: on its yard nearest where a road comes in
        // (the lowest such tile), facing the road; its shop the house nearest it.
        bus_stops_.clear();
        shops_.clear();
        {
            std::vector<uint8_t> done(village_.size(), 0);
            for (int y = 0; y < cache_height_; ++y) {
                for (int x = 0; x < cache_width_; ++x) {
                    const size_t i0 = static_cast<size_t>(y * cache_width_ + x);
                    if (!village_[i0] || done[i0] || map.terrain(x, y) != engine::Terrain::Urban) continue;
                    std::vector<engine::TilePos> cluster{{x, y}};
                    done[i0] = 1;
                    for (size_t k = 0; k < cluster.size(); ++k) {
                        for (const auto& [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                            const int nx = cluster[k].x + dx;
                            const int ny = cluster[k].y + dy;
                            if (!map.contains_tile(nx, ny)) continue;
                            const size_t i = static_cast<size_t>(ny * cache_width_ + nx);
                            if (done[i]) continue;
                            // Its yards, streets and houses: the whole village (across a road through it).
                            if (!village_[i]) {
                                const engine::Terrain r = map.terrain(nx, ny);
                                if (r != engine::Terrain::Road && r != engine::Terrain::DirtRoad) continue;
                                int sides = 0;
                                for (const auto& [ex, ey] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                                    if (map.contains_tile(nx + ex, ny + ey) && village_[static_cast<size_t>((ny + ey) * cache_width_ + nx + ex)]) ++sides;
                                }
                                if (sides < 2) continue;
                            }
                            done[i] = 1;
                            cluster.push_back({nx, ny});
                        }
                    }
                    if (cluster.size() < 30) continue;  // (a holding's yard, a farm's: no village)
                    std::optional<BusStop> stop;
                    for (const engine::TilePos& t : cluster) {
                        if (stop) break;
                        if (map.terrain(t.x, t.y) != engine::Terrain::Urban) continue;
                        for (const auto& [dx, dy] : {std::pair{0, 1}, std::pair{1, 0}, std::pair{0, -1}, std::pair{-1, 0}}) {
                            if (!map.contains_tile(t.x + dx, t.y + dy)) continue;
                            const engine::Terrain r = map.terrain(t.x + dx, t.y + dy);
                            if (r != engine::Terrain::Road && r != engine::Terrain::DirtRoad) continue;
                            const Vector2 f{static_cast<float>(dx), static_cast<float>(dy)};
                            stop = BusStop{{static_cast<float>(t.x) + 0.5f + f.x * 0.22f, static_cast<float>(t.y) + 0.5f + f.y * 0.22f}, f,
                                           tile_hash(t.x, t.y) % 2 == 0};
                            break;
                        }
                    }
                    if (!stop) continue;
                    bus_stops_.push_back(*stop);
                    const engine::Structure* best = nullptr;
                    float best_d = 1e9f;
                    for (const engine::Structure& s : world.structures()) {
                        if (s.type != engine::StructureType::House || s.look != engine::HouseLook::House || s.tiles.size() >= engine::kSpaciousTiles ||
                            s.tiles.size() < 4 || std::find(cluster.begin(), cluster.end(), s.tiles.front()) == cluster.end()) {
                            continue;  // (one of this village's houses)
                        }
                        const Vector2 c = to_vector2(s.center);
                        const float d = std::hypot(c.x - stop->ground.x, c.y - stop->ground.y);
                        if (d < best_d && d < 6.0f) {
                            best_d = d;
                            best = &s;
                        }
                    }
                    if (best) shops_.push_back(best->id);
                }
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

    // Bridge decks: along the bridge's tiles from bank to bank, its middle a
    // straight line through them, as wide as they lie either side of it,
    // level with the lowest bank beside it.
    const std::vector<std::pair<engine::EntityId, iso::Deck>> decks_before = bridge_decks_;
    bridge_decks_.clear();
    for (const engine::Structure& s : world.structures()) {
        if (s.type != engine::StructureType::Bridge || s.tiles.empty()) continue;
        int col_min = 1 << 30;
        int col_max = -(1 << 30);
        int bank = engine::TileMap::kMaxElevation;
        float sum_v = 0.0f;
        float sum_u = 0.0f;
        float sum_vv = 0.0f;
        float sum_uv = 0.0f;
        for (const engine::TilePos& t : s.tiles) {
            const auto u = static_cast<float>(t.x + t.y + 1);  // the tile's middle
            const auto v = static_cast<float>(t.y - t.x);
            sum_u += u;
            sum_v += v;
            sum_vv += v * v;
            sum_uv += u * v;
            col_min = std::min(col_min, t.y - t.x);
            col_max = std::max(col_max, t.y - t.x);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!map.contains_tile(t.x + dx, t.y + dy)) continue;
                    const engine::Terrain n = map.terrain(t.x + dx, t.y + dy);
                    if (n != engine::Terrain::Water && n != engine::Terrain::Bridge) bank = std::min<int>(bank, map.elevation(t.x + dx, t.y + dy));
                }
            }
        }
        const auto n = static_cast<float>(s.tiles.size());
        const float spread = sum_vv - sum_v * sum_v / n;
        const float slope = spread > 0.001f ? (sum_uv - sum_u * sum_v / n) / spread : 0.0f;
        const float mean_u = sum_u / n;
        const float mean_v = sum_v / n;
        float half = 0.0f;
        for (const engine::TilePos& t : s.tiles) {
            const auto u = static_cast<float>(t.x + t.y + 1);
            const auto v = static_cast<float>(t.y - t.x);
            half = std::max(half, std::fabs(u - (mean_u + slope * (v - mean_v))));
        }
        const float v0 = static_cast<float>(col_min) - 1.0f;  // on to where the banks begin
        const float v1 = static_cast<float>(col_max) + 1.0f;
        bridge_decks_.push_back({s.id, {v0, v1, mean_u + slope * (v0 - mean_v), mean_u + slope * (v1 - mean_v), half + 0.65f,
                                        static_cast<float>(bank)}});
    }
    {
        std::vector<iso::Deck> decks;
        for (const auto& entry : bridge_decks_) decks.push_back(entry.second);
        iso::set_decks(std::move(decks));
    }
    // A bridge gone since: it comes down, one way or the other (out of sight,
    // it's seen lying there after).
    for (const auto& [id, deck] : decks_before) {
        if (world.find_structure(id)) continue;
        FallenBridge f;
        f.deck = deck;
        f.seed = tile_hash(static_cast<int>(id) * 7 + 3, static_cast<int>(id) * 13 + 5);
        f.way = static_cast<int>((f.seed >> 11) & 1u);
        const float length = deck.v1 - deck.v0;
        auto unit01 = [](uint32_t h) { return static_cast<float>(h & 0xFFFFu) / 65536.0f; };
        f.cut = f.way == 0 ? deck.v0 + length * (0.4f + 0.2f * unit01(f.seed >> 3)) : deck.v0 + length * (0.22f + 0.12f * unit01(f.seed >> 5));
        f.cut2 = deck.v1 - length * (0.22f + 0.12f * unit01(f.seed >> 9));
        const float v = (deck.v0 + deck.v1) * 0.5f;
        const float u = deck.middle(v);
        const Vector2 g{(u - v) * 0.5f, (u + v) * 0.5f};
        if (!reveal_ && fog(world, static_cast<int>(g.x), static_cast<int>(g.y)) != kInView) {
            f.age = 60.0f;
            f.stage = 3;
        }
        fallen_bridges_.push_back(f);
    }
    for (FallenBridge& f : fallen_bridges_) {
        f.age += dt;
        auto ground_on = [&](float v, float across) {
            const float u = f.deck.middle(v) + across * f.deck.half;
            return Vector2{(u - v) * 0.5f, (u + v) * 0.5f};
        };
        if (f.stage == 0) {  // it breaks: dust out of the breaks, bits of concrete falling
            f.stage = 1;
            for (const float v : {f.cut, f.way == 1 ? f.cut2 : f.cut}) {
                for (int i = 0; i < 14; ++i) {
                    Particle p{};
                    p.kind = i % 3 == 0 ? Particle::Kind::Clod : Particle::Kind::Smoke;
                    p.ground = ground_on(v + (fx_random() - 0.5f) * 0.6f, fx_random() * 2.0f - 1.0f);
                    p.z = f.deck.height * iso::kElevationStep + 4.0f;
                    const float a = fx_random() * 6.2831853f;
                    p.vel = {std::cos(a) * 0.4f, std::sin(a) * 0.4f};
                    p.vz = p.kind == Particle::Kind::Clod ? 30.0f + 40.0f * fx_random() : 8.0f + 10.0f * fx_random();
                    p.life = p.kind == Particle::Kind::Clod ? 1.2f : 3.5f + 2.0f * fx_random();
                    p.size = p.kind == Particle::Kind::Clod ? 1.5f + fx_random() : 4.0f + 3.0f * fx_random();
                    p.grow = p.kind == Particle::Kind::Clod ? 0.0f : 5.0f;
                    p.color = p.kind == Particle::Kind::Clod ? Color{150, 146, 138, 255} : Color{176, 170, 158, 190};
                    particles_.push_back(p);
                }
            }
        }
        if (f.stage == 1 && f.age > (f.way == 0 ? 0.85f : 0.55f)) {  // into the river: spray
            f.stage = 2;
            const int spots = f.way == 0 ? 1 : 3;
            for (int k = 0; k < spots; ++k) {
                const float v = f.way == 0 ? f.cut : f.cut + (f.cut2 - f.cut) * (0.2f + 0.3f * static_cast<float>(k));
                for (int i = 0; i < 24; ++i) {
                    Particle p{};
                    p.kind = Particle::Kind::Spray;
                    p.ground = ground_on(v + (fx_random() - 0.5f) * 0.8f, fx_random() * 2.0f - 1.0f);
                    p.z = 1.0f;
                    const float a = fx_random() * 6.2831853f;
                    const float speed = 0.3f + 0.9f * fx_random();
                    p.vel = {std::cos(a) * speed, std::sin(a) * speed};
                    p.vz = 60.0f + 90.0f * fx_random();
                    p.life = 1.4f + fx_random();
                    p.size = 1.5f + 1.5f * fx_random();
                    p.color = {222, 232, 236, 255};
                    particles_.push_back(p);
                }
            }
        }
    }

    // Craters: when each appeared, so fresh ones smoke and look raw.
    const size_t tiles = static_cast<size_t>(map.width() * map.height());
    const bool first = crater_born_.size() != tiles;
    if (first || map.revision() != crater_revision_) {
        if (first) {
            crater_born_.assign(tiles, -1.0e6f);
            crater_seen_.assign(tiles, 0);
        }
        const auto now = static_cast<float>(GetTime());
        for (int y = 0; y < map.height(); ++y) {
            for (int x = 0; x < map.width(); ++x) {
                if (map.terrain(x, y) != engine::Terrain::Crater) continue;
                const size_t i = static_cast<size_t>(y * map.width() + x);
                const auto packed = static_cast<uint8_t>(static_cast<uint8_t>(map.crater_kind(x, y)) | map.crater_from(x, y) << 4);
                if (packed == crater_seen_[i]) continue;
                crater_seen_[i] = packed;
                if (!first) crater_born_[i] = now;
            }
        }
        crater_revision_ = map.revision();
    }

    remember(world);
    // The trees' state as seen: updated for the tiles in view.
    if (map.revision() != shred_revision_ || seen_shred_.size() != static_cast<size_t>(map.width() * map.height())) {
        if (seen_shred_.size() != static_cast<size_t>(map.width() * map.height())) seen_shred_.assign(static_cast<size_t>(map.width() * map.height()), 0);
        for (int ty = 0; ty < map.height(); ++ty) {
            for (int tx = 0; tx < map.width(); ++tx) {
                const uint8_t now = map.shred(tx, ty);
                uint8_t& seen = seen_shred_[static_cast<size_t>(ty * map.width() + tx)];
                if (now != seen && fog(world, tx, ty) == kInView) seen = now;
            }
        }
        shred_revision_ = map.revision();
    }

    // Shells and rockets that went off since the last frame: where they
    // actually burst, which may be a tree or a soldier in the way.
    for (const engine::Impact& impact : world.recent_impacts()) {
        if (impact.tick < impacts_seen_until_ || !in_view(world, to_vector2(impact.pos))) continue;
        const float splash = to_float(impact.splash);
        if (impact.air) {  // a missile bursting up by an aircraft: a flash, a little ball of fire, sparks, black smoke hanging
            const float up = to_float(impact.height) * iso::kElevationStep;
            blasts_.push_back({to_vector2(impact.pos), 0.0f, 0.4f, up});
            spawn_flash(to_vector2(impact.pos), up, 9.0f, {255, 240, 190, 255});
            spawn_sparks(to_vector2(impact.pos), up, 14, {255, 200, 100, 255}, 1.8f);
            for (int i = 0; i < 5; ++i) {
                Particle p{};
                p.kind = Particle::Kind::Smoke;
                p.ground = {to_vector2(impact.pos).x + (fx_random() - 0.5f) * 0.3f, to_vector2(impact.pos).y + (fx_random() - 0.5f) * 0.3f};
                p.z = up + (fx_random() - 0.5f) * 6.0f;
                p.vel = {0.1f + (fx_random() - 0.5f) * 0.2f, -0.06f + (fx_random() - 0.5f) * 0.2f};
                p.vz = 2.0f;
                p.life = 2.5f + fx_random();
                p.size = 2.5f + 1.5f * fx_random();
                p.grow = 4.0f;
                p.color = {30, 28, 26, 220};
                particles_.push_back(p);
            }
            continue;
        }
        blasts_.push_back({to_vector2(impact.pos), 0.0f, std::max(splash, 0.2f)});
        const bool fire = impact.shooter_type == engine::UnitTypeId::FuelTanker;  // a load going up: fire, not a shell's dust (drawn with the wreck)
        if (splash > 0.0f && !fire) spawn_burst(world, to_vector2(impact.pos), splash, static_cast<uint8_t>(burst_of(impact)));
    }
    impacts_seen_until_ = world.tick();

    // Rounds, rockets and missiles gone since the last frame: what they hit.
    // An anti-tank round or missile on a vehicle punches through, a white
    // flash and a spray of sparks, black smoke (a tank's reactive armor goes
    // off first, a bigger flash); an autocannon's shells strike sparks. (The
    // bursts of HE came with the impacts above.)
    std::unordered_map<uint32_t, ProjectileSeen> flying;
    for (const engine::Projectile& p : world.projectiles()) {
        flying[p.id] = {to_vector2(p.pos), p.weapon.damage_type, p.at_air};
        // A guided missile off its launcher: the back-blast out behind it.
        if (projectiles_seen_.contains(p.id) || !p.weapon.guided || p.at_air || !in_view(world, to_vector2(p.origin))) continue;
        Vector2 d = to_vector2(p.target - p.origin);
        const float dl = std::hypot(d.x, d.y);
        d = dl > 0.0f ? Vector2{d.x / dl, d.y / dl} : Vector2{1.0f, 0.0f};
        const Vector2 o = to_vector2(p.origin);
        spawn_flash({o.x + d.x * 0.2f, o.y + d.y * 0.2f}, to_float(p.origin_height), 4.0f, {255, 226, 160, 255});
        for (int i = 0; i < 6; ++i) {
            Particle q{};
            q.kind = Particle::Kind::Smoke;
            q.ground = {o.x - d.x * 0.15f, o.y - d.y * 0.15f};
            q.z = to_float(p.origin_height);
            q.vel = {-d.x * (0.8f + fx_random()) + (fx_random() - 0.5f) * 0.4f, -d.y * (0.8f + fx_random()) + (fx_random() - 0.5f) * 0.4f};
            q.vz = 2.0f + 4.0f * fx_random();
            q.life = 1.1f + 0.6f * fx_random();
            q.size = 2.0f + 1.5f * fx_random();
            q.grow = 6.0f;
            q.color = {200, 196, 186, 170};
            particles_.push_back(q);
        }
    }
    for (const auto& [id, p] : projectiles_seen_) {
        if (flying.contains(id) || p.at_air || !in_view(world, p.pos)) continue;
        const engine::Unit* hit = nullptr;
        float best = 0.8f;
        for (const engine::Unit& u : world.units()) {
            if (!engine::unit_type(u.type).vehicle || u.airborne || u.inside) continue;
            const float d = std::hypot(to_vector2(u.pos).x - p.pos.x, to_vector2(u.pos).y - p.pos.y);
            if (d < best) {
                best = d;
                hit = &u;
            }
        }
        if (p.type == engine::DamageType::AntiTank) {
            if (!hit) {
                spawn_burst(world, p.pos, 0.3f);
                continue;
            }
            const engine::UnitTypeDef& def = engine::unit_type(hit->type);
            if (def.tank && world.era_level(hit->owner) > 0 && def.era_max > 0) {  // the reactive armor goes off
                spawn_flash(p.pos, 8.0f, 12.0f, {255, 250, 230, 255});
                spawn_burst(world, p.pos, 0.35f);
            }
            spawn_flash(p.pos, 7.0f, 11.0f, {255, 244, 200, 255});
            spawn_sparks(p.pos, 7.0f, 22, {255, 196, 90, 255}, 1.8f);
            Particle smoke{};
            smoke.kind = Particle::Kind::Smoke;
            smoke.ground = p.pos;
            smoke.z = 8.0f;
            smoke.vz = 12.0f;
            smoke.life = 1.6f;
            smoke.size = 3.0f;
            smoke.grow = 6.0f;
            smoke.color = {40, 38, 36, 210};
            particles_.push_back(smoke);
        } else if (p.type == engine::DamageType::Bullet) {
            if (hit) {
                spawn_sparks(p.pos, 6.0f, 5, {255, 214, 120, 255}, 0.9f);
            } else {
                spawn_burst(world, p.pos, 0.2f);
            }
        }
    }
    projectiles_seen_ = std::move(flying);

    // Tanks whose rounds went up just now (their turrets are thrown off).
    std::vector<engine::EntityId> blown;
    for (const engine::Impact& impact : world.recent_impacts()) {
        if (impact.blown != 0) blown.push_back(impact.blown);
    }
    // Units that vanished died (a garrison buried in its house leaves no body
    // to see, and nobody sees who dies in the fog).
    std::unordered_map<engine::EntityId, Remains> alive;
    for (const engine::Unit& u : world.units()) {
        if (u.inside || !shows(world, u)) continue;
        auto unit = [](Vector2 v) {  // the engine keeps directions unnormalized
            const float l = std::hypot(v.x, v.y);
            return l > 0.0f ? Vector2{v.x / l, v.y / l} : Vector2{1.0f, 0.0f};
        };
        alive[u.id] = {to_vector2(u.pos), 0.0f, engine::unit_type(u.type).vehicle, u.type, u.owner, unit(to_vector2(u.hull)),
                       unit(to_vector2(u.facing)), static_cast<uint32_t>(u.id) * 2654435761u,
                       u.mired * 4 >= engine::kBogLimit * 3};
        alive[u.id].cargo = static_cast<uint8_t>(cargo_of(world, u));
        alive[u.id].height = u.airborne ? std::max(1.0f, flight_lift(u)) : 0.0f;
    }
    for (const auto& [id, last] : units_seen_) {
        if (alive.contains(id) || world.find_unit(id)) continue;
        if (last.height > 0.0f) {  // an aircraft gone from the sky: brought down, or off to the rear
            const auto& crashes = world.crashes();
            const auto crash = std::find_if(crashes.begin(), crashes.end(), [&](const engine::Crash& c) { return c.id == id; });
            if (crash == crashes.end()) continue;  // off to the rear
            Remains r = last;
            r.from = to_vector2(crash->from);
            r.ground = to_vector2(crash->to);
            r.height = to_float(crash->height) * iso::kElevationStep;
            r.fell = static_cast<float>(crash->hits - crash->fell) / engine::kTicksPerSecond;
            r.age = static_cast<float>(world.tick() - std::min(world.tick(), crash->fell)) / engine::kTicksPerSecond;
            const Vector2 run{r.ground.x - r.from.x, r.ground.y - r.from.y};
            const float rl = std::hypot(run.x, run.y);
            if (rl > 0.01f) r.facing = {run.x / rl, run.y / rl};
            r.crashed = true;
            remains_.push_back(r);
            continue;
        }
        remains_.push_back(last);
        remains_.back().blown = std::find(blown.begin(), blown.end(), id) != blown.end();
        if (remains_.back().blown && !last.sunk) {  // its rounds going up: a fireball, black smoke boiling up
            spawn_flash(last.ground, 12.0f, 18.0f, {255, 236, 170, 255});
            blasts_.push_back({last.ground, 0.0f, 1.6f, 4.0f});
            blasts_.push_back({last.ground, -0.18f, 1.0f, 22.0f});
            spawn_sparks(last.ground, 14.0f, 30, {255, 190, 90, 255}, 2.4f);
            for (int i = 0; i < 10; ++i) {  // burning bits of it flung up
                Particle p{};
                p.kind = Particle::Kind::Spark;
                p.ground = {last.ground.x + (fx_random() - 0.5f) * 0.4f, last.ground.y + (fx_random() - 0.5f) * 0.4f};
                p.z = 6.0f + 6.0f * fx_random();
                p.vz = 40.0f + 50.0f * fx_random();
                p.life = 0.5f + 0.4f * fx_random();
                p.size = 1.6f + 0.8f * fx_random();
                p.grow = 0.0f;
                p.color = {255, 200, 80, 230};
                particles_.push_back(p);
            }
            for (int i = 0; i < 10; ++i) {
                Particle p{};
                p.kind = Particle::Kind::Smoke;
                p.ground = {last.ground.x + (fx_random() - 0.5f) * 0.3f, last.ground.y + (fx_random() - 0.5f) * 0.3f};
                p.z = 14.0f + 10.0f * fx_random();
                p.vel = {(fx_random() - 0.5f) * 0.3f, (fx_random() - 0.5f) * 0.3f};
                p.vz = 22.0f + 14.0f * fx_random();
                p.life = 3.0f + 1.5f * fx_random();
                p.size = 4.0f + 2.0f * fx_random();
                p.grow = 9.0f;
                p.color = {30, 28, 26, 220};
                particles_.push_back(p);
            }
        }
        const auto cargo = static_cast<Cargo>(last.cargo);
        if (!last.sunk && (cargo == Cargo::Fuel || cargo == Cargo::Drums)) {
            // The fuel going up: a white flash, a ball of flame boiling up and
            // out, black smoke rolling up over it in a mushroom.
            const float big = cargo == Cargo::Fuel ? 1.0f : 0.65f;
            spawn_flash(last.ground, 10.0f, 22.0f * big, {255, 240, 190, 255});
            spawn_sparks(last.ground, 10.0f, static_cast<int>(24.0f * big), {255, 190, 90, 255}, 2.0f);
            blasts_.push_back({last.ground, -0.12f, 1.4f * big, 14.0f});  // the ball of it rising
            blasts_.push_back({{last.ground.x - 0.1f, last.ground.y + 0.1f}, -0.3f, 1.1f * big, 30.0f});
            for (int i = 0; i < static_cast<int>(18.0f * big); ++i) {  // burning bits flung out
                Particle p{};
                p.kind = Particle::Kind::Spark;
                const float a = fx_random() * 6.2831853f;
                const float core = fx_random();
                const float out = (0.3f + 0.9f * (1.0f - core)) * 0.9f * big;
                p.ground = {last.ground.x + std::cos(a) * 0.2f, last.ground.y + std::sin(a) * 0.2f};
                p.vel = {std::cos(a) * out, std::sin(a) * out};
                p.z = 4.0f + 10.0f * fx_random();
                p.vz = (15.0f + 70.0f * core) * big;
                p.life = 0.6f + 0.6f * fx_random();
                p.size = 1.6f + 0.8f * fx_random();
                p.grow = 0.0f;
                p.color = {255, 210, 110, 240};
                particles_.push_back(p);
            }
            for (int i = 0; i < static_cast<int>(12.0f * big); ++i) {  // (over the ball of fire as it dies)
                Particle p{};
                p.kind = Particle::Kind::Smoke;
                p.ground = {last.ground.x + (fx_random() - 0.5f) * 0.4f, last.ground.y + (fx_random() - 0.5f) * 0.4f};
                p.z = 30.0f + 20.0f * fx_random();
                p.vel = {(fx_random() - 0.5f) * 0.5f, (fx_random() - 0.5f) * 0.5f};
                p.vz = 30.0f + 30.0f * fx_random();
                p.life = 4.0f + 2.0f * fx_random();
                p.size = 5.0f + 3.0f * fx_random();
                p.grow = 11.0f;
                p.color = {24, 22, 20, 230};
                particles_.push_back(p);
            }
            blasts_.push_back({last.ground, 0.0f, 1.9f * big});
        }
        if (!last.sunk && (cargo == Cargo::Crates || cargo == Cargo::Shells)) {
            // The rounds going up: a blast that throws the bed apart, boxes
            // and bits flying, burning.
            spawn_flash(last.ground, 8.0f, 18.0f, {255, 236, 170, 255});
            spawn_sparks(last.ground, 8.0f, 40, {255, 200, 100, 255}, 3.0f);
            for (int i = 0; i < 14; ++i) {
                Particle p{};
                p.kind = Particle::Kind::Clod;  // bits of it, boxes, planks
                const float a = fx_random() * 6.2831853f;
                const float v = 1.2f + 2.2f * fx_random();
                p.ground = last.ground;
                p.vel = {std::cos(a) * v, std::sin(a) * v};
                p.z = 6.0f;
                p.vz = 50.0f + 60.0f * fx_random();
                p.life = 1.4f + 0.8f * fx_random();
                p.size = 1.5f + 1.5f * fx_random();
                p.color = i % 3 == 0 ? Color{88, 100, 64, 255} : i % 3 == 1 ? Color{150, 116, 76, 255} : Color{40, 36, 32, 255};
                particles_.push_back(p);
            }
            for (int i = 0; i < 6; ++i) {
                Particle p{};
                p.kind = Particle::Kind::Smoke;
                p.ground = {last.ground.x + (fx_random() - 0.5f) * 0.3f, last.ground.y + (fx_random() - 0.5f) * 0.3f};
                p.z = 18.0f + 8.0f * fx_random();
                p.vel = {(fx_random() - 0.5f) * 0.4f, (fx_random() - 0.5f) * 0.4f};
                p.vz = 26.0f + 16.0f * fx_random();
                p.life = 3.0f + 1.5f * fx_random();
                p.size = 4.0f + 2.0f * fx_random();
                p.grow = 9.0f;
                p.color = {46, 42, 38, 220};
                particles_.push_back(p);
            }
            spawn_burst(world, last.ground, 0.5f);
            blasts_.push_back({last.ground, 0.0f, 1.5f});
            blasts_.push_back({{last.ground.x + 0.15f, last.ground.y - 0.1f}, -0.15f, 0.8f, 12.0f});
        }
        if (last.vehicle) {
            const engine::Terrain under =
                map.terrain(map.clamp_tile({static_cast<int32_t>(std::floor(last.ground.x)), static_cast<int32_t>(std::floor(last.ground.y))}));
            if (under != engine::Terrain::Water && !last.sunk) blasts_.push_back({last.ground, 0.0f, 0.7f});
            if (!last.sunk) spawn_burst(world, last.ground, 0.8f);  // (off the water: spray)
        }
    }
    units_seen_ = std::move(alive);

    // Smoke out of the muzzles of whoever fired since the last frame; battered tanks smoking and burning.
    std::unordered_map<engine::EntityId, engine::Tick> shots;
    for (const engine::Unit& u : world.units()) {
        if (u.last_shot_tick == engine::kNeverFired) continue;
        shots[u.id] = u.last_shot_tick;
        const auto seen = shots_seen_.find(u.id);
        if (seen != shots_seen_.end() && u.last_shot_tick > seen->second && !u.inside && shows(world, u)) {
            shot_at_[u.id] = GetTime();
            if (u.order == engine::Order::Ability && u.order_ability == engine::AbilityId::ThrowGrenade) thrown_at_[u.id] = GetTime();
            spawn_muzzle(world, u);
            fired(world, u);
        }
    }
    shots_seen_ = std::move(shots);
    std::erase_if(shot_at_, [&](const auto& e) { return world.find_unit(e.first) == nullptr; });
    std::erase_if(thrown_at_, [&](const auto& e) { return world.find_unit(e.first) == nullptr; });
    update_work(world, dt);
    // Men hit since the last frame flinch.
    for (const engine::Unit& u : world.units()) {
        if (engine::unit_type(u.type).vehicle) continue;
        auto [it, fresh] = hurt_.try_emplace(u.id, u.hp, -99.0);
        if (!fresh && u.hp < it->second.first) it->second.second = GetTime();
        it->second.first = u.hp;
    }
    std::erase_if(hurt_, [&](const auto& e) { return world.find_unit(e.first) == nullptr; });
    for (const engine::Unit& u : world.units()) {
        const engine::UnitTypeDef& def = engine::unit_type(u.type);
        const std::optional<TruckModel> truck = truck_model(u.type, u.owner);
        if ((!drawn_as_armor(def) && !truck) || u.inside || !shows(world, u)) continue;
        const int wear = wear_of(u.hp, def.max_hp);
        if (wear < 2) continue;
        Vector2 h = to_vector2(truck ? u.facing : u.hull);  // a truck's body goes the way it faces
        const float l = std::hypot(h.x, h.y);
        h = l > 0.0f ? Vector2{h.x / l, h.y / l} : Vector2{1.0f, 0.0f};
        const Vector2 e = truck ? truck_engine_of(*truck) : armor_engine(def.model);
        spawn_fire({to_vector2(u.pos).x + h.x * e.x, to_vector2(u.pos).y + h.y * e.x}, e.y, wear, dt);
    }
    for (const Remains& r : remains_) {  // burnt-out tanks, IFVs, trucks smoulder, burning for the first half minute
        const std::optional<TruckModel> truck = truck_model(r.type, r.owner);
        const bool small = gun_or_plane(engine::unit_type(r.type));
        if ((!drawn_as_armor(engine::unit_type(r.type)) && !truck && !small) || r.age > 100.0f || r.fell > 0.0f) continue;
        if (map.terrain(map.clamp_tile({static_cast<int32_t>(std::floor(r.ground.x)), static_cast<int32_t>(std::floor(r.ground.y))})) ==
            engine::Terrain::Water ||
            r.sunk) {
            continue;  // drowned, or gone under in a bog
        }
        if (r.blown && r.age < 6.0f && fx_random() < dt * 2.5f) {  // rounds still going off in it, now and then
            const Vector2 spot{r.ground.x + (fx_random() - 0.5f) * 0.5f, r.ground.y + (fx_random() - 0.5f) * 0.5f};
            spawn_flash(spot, 10.0f, 5.0f, {255, 226, 150, 255});
            spawn_sparks(spot, 10.0f, 8, {255, 200, 100, 255}, 1.4f);
        }
        const auto cargo = static_cast<Cargo>(r.cargo);
        if ((cargo == Cargo::Fuel || cargo == Cargo::Drums) && r.age < 45.0f) {
            // The fuel burning (its flames drawn with it): black smoke thick over it.
            const float heat = (cargo == Cargo::Fuel ? 1.0f : 0.6f) * (r.age < 30.0f ? 1.0f : (45.0f - r.age) / 15.0f);
            for (const float t : {-0.35f, 0.0f, 0.3f}) {
                if (fx_random() > heat) continue;
                const Vector2 at{r.ground.x + r.facing.x * t, r.ground.y + r.facing.y * t};
                if (fx_random() < dt * 4.0f) {
                    Particle p{};
                    p.kind = Particle::Kind::Smoke;
                    p.ground = at;
                    p.z = 18.0f;
                    p.vel = {0.15f + (fx_random() - 0.5f) * 0.1f, -0.1f + (fx_random() - 0.5f) * 0.1f};
                    p.vz = 22.0f + 10.0f * fx_random();
                    p.life = 3.5f + 1.5f * fx_random();
                    p.size = 4.0f + 2.0f * fx_random();
                    p.grow = 7.0f;
                    p.color = {26, 24, 22, 220};
                    particles_.push_back(p);
                }
            }
        }
        if ((cargo == Cargo::Crates || cargo == Cargo::Shells) && r.age < 16.0f) {
            // The rounds cooking off in the fire: pops, sparks, a round flying
            // off burning now and then, now and then a bigger one going.
            const float rate = 3.2f * (1.0f - r.age / 16.0f) + 0.4f;
            if (fx_random() < dt * rate) {
                const Vector2 spot{r.ground.x + (fx_random() - 0.5f) * 0.5f, r.ground.y + (fx_random() - 0.5f) * 0.5f};
                const float z = 5.0f + 6.0f * fx_random();
                spawn_flash(spot, z, 4.0f + 5.0f * fx_random(), {255, 226, 150, 255});
                spawn_sparks(spot, z, 6, {255, 200, 100, 255}, 1.6f);
                blasts_.push_back({spot, 0.0f, 0.3f, z * 0.5f});
                if (fx_random() < 0.4f) {
                    Particle p{};
                    p.kind = Particle::Kind::Spark;
                    const float a = fx_random() * 6.2831853f;
                    const float v = 2.5f + 3.0f * fx_random();
                    p.ground = spot;
                    p.z = z;
                    p.vel = {std::cos(a) * v, std::sin(a) * v};
                    p.vz = 60.0f + 60.0f * fx_random();
                    p.life = 0.9f + 0.6f * fx_random();
                    p.size = 1.5f;
                    p.color = {255, 214, 130, 255};
                    particles_.push_back(p);
                }
                if (fx_random() < (cargo == Cargo::Shells ? 0.22f : 0.1f)) {
                    spawn_flash(spot, 8.0f, 14.0f, {255, 236, 170, 255});
                    spawn_sparks(spot, 8.0f, 16, {255, 190, 90, 255}, 2.4f);
                    spawn_burst(world, spot, 0.5f);
                    blasts_.push_back({spot, 0.0f, 0.5f});
                }
            }
        }
        const Vector2 e = truck ? truck_engine_of(*truck) : small ? Vector2{0.0f, 3.0f} : armor_engine(engine::unit_type(r.type).model);
        const Vector2 along = truck ? r.facing : r.hull;
        const float ease = r.age < 30.0f ? 1.0f : 0.4f;
        if (fx_random() < ease) spawn_fire({r.ground.x + along.x * e.x, r.ground.y + along.y * e.x}, e.y * 0.8f, r.age < 30.0f ? 4 : 2, dt);
    }
    update_vehicles(world, dt);
    for (const engine::Fire& f : world.fires()) {
        const Vector2 c = to_vector2(f.center);
        if (!in_view(world, c)) continue;
        const float r = to_float(f.radius);
        for (int i = 0; i < 5; ++i) {
            if (fx_random() > dt * 1.4f) continue;
            const uint32_t h = tile_hash(static_cast<int>(c.x * 13.0f) + i * 31, static_cast<int>(c.y * 17.0f) + i * 7);
            const float a = static_cast<float>(h & 0xFFFF) / 65536.0f * 6.2831853f;
            const float d = r * 0.8f * std::sqrt(static_cast<float>((h >> 16) & 0xFFFF) / 65536.0f);
            const Vector2 g{c.x + std::cos(a) * d, c.y + std::sin(a) * d};
            const engine::Terrain t = map.terrain(map.clamp_tile({static_cast<int32_t>(std::floor(g.x)), static_cast<int32_t>(std::floor(g.y))}));
            const bool wooded = t == engine::Terrain::Forest || t == engine::Terrain::Orchard;
            Particle p{};
            p.kind = Particle::Kind::Smoke;
            p.ground = g;
            p.z = wooded ? 22.0f : 2.0f;
            p.vel = {0.12f + (fx_random() - 0.5f) * 0.1f, -0.08f + (fx_random() - 0.5f) * 0.1f};
            p.vz = 14.0f + 8.0f * fx_random();
            p.life = 3.0f + fx_random();
            p.size = 2.5f + 1.5f * fx_random();
            p.grow = 5.0f;
            p.color = wooded ? Color{26, 24, 22, 215} : Color{150, 146, 140, 130};
            particles_.push_back(p);
        }
    }
    // Buildings burning (smoke black and thick out of the flames) and
    // smouldering (thinner, greyer): the ones drawn just now.
    for (const auto& [key, sprite] : building_sprites_) {
        if (sprite.burning.empty() || sprite.used + 2 < frame_) continue;
        for (const auto& f : sprite.burning) {
            if (fx_random() > dt * (f.flames ? 2.4f : 1.0f)) continue;
            const float dx = f.at.x - sprite.anchor.x;
            Particle p{};
            p.kind = Particle::Kind::Smoke;
            p.ground = {sprite.ground.x + dx / 64.0f + (fx_random() - 0.5f) * 0.05f, sprite.ground.y - dx / 64.0f};
            p.z = sprite.anchor.y - f.at.y + 2.0f;
            p.vel = {0.1f + (fx_random() - 0.5f) * 0.1f, -0.06f + (fx_random() - 0.5f) * 0.1f};
            p.vz = f.flames ? 16.0f + 8.0f * fx_random() : 9.0f + 5.0f * fx_random();
            p.life = f.flames ? 2.6f + 1.2f * fx_random() : 2.0f + fx_random();
            p.size = f.flames ? 2.5f + 1.5f * fx_random() : 1.8f + fx_random();
            p.grow = f.flames ? 5.0f : 3.5f;
            p.color = f.flames ? Color{22, 20, 18, 225} : Color{96, 94, 92, 150};
            particles_.push_back(p);
        }
    }

    // Tracked vehicles leave their tracks in the ground as they go, wheeled
    // ones their tyres' (not on concrete, a bridge or the water); they fade
    // over five minutes. Through the crops, whatever is under the hull is
    // flattened, for good.
    if (crushed_.size() != static_cast<size_t>(map.width() * map.height())) crushed_.assign(static_cast<size_t>(map.width() * map.height()), 0);
    for (const engine::Unit& u : world.units()) {
        const engine::UnitTypeDef& def = engine::unit_type(u.type);
        const bool marks = drawn_as_armor(def) || truck_model(u.type, u.owner).has_value();
        if (!marks || u.inside || !shows(world, u)) continue;
        const Vector2 p = to_vector2(u.pos);
        const auto it = track_last_.find(u.id);
        if (it == track_last_.end()) {
            track_last_[u.id] = p;
            continue;
        }
        const float len = std::hypot(p.x - it->second.x, p.y - it->second.y);
        if (len < 0.2f) continue;
        if (len > 4.0f) {  // (set down somewhere else: no track between)
            it->second = p;
            continue;
        }
        // The way it went since its last mark, in steps (however fast the frames come).
        const int steps = std::max(1, static_cast<int>(std::ceil(len / 0.6f)));
        Vector2 f{(p.x - it->second.x) / len, (p.y - it->second.y) / len};
        const float half_w = track_half(u.type) + 0.08f;
        const float half_l = to_float(def.radius) * 0.8f;
        for (int k = 1; k <= steps; ++k) {
            const Vector2 a0 = lerp(it->second, p, static_cast<float>(k - 1) / static_cast<float>(steps));
            const Vector2 a1 = lerp(it->second, p, static_cast<float>(k) / static_cast<float>(steps));
            const engine::Terrain t = map.terrain(map.clamp_tile({static_cast<int32_t>(std::floor(a1.x)), static_cast<int32_t>(std::floor(a1.y))}));
            const bool firm = t == engine::Terrain::Road || t == engine::Terrain::Bridge || t == engine::Terrain::Water ||
                              t == engine::Terrain::Airstrip || t == engine::Terrain::Rail;
            if (!firm) track_marks_.push_back({a0, a1, track_half(u.type), 0.0f, def.wheeled});
            // Whatever grew under the hull there, flattened.
            for (int ty = static_cast<int>(std::floor(a1.y - 0.8f)); ty <= static_cast<int>(std::floor(a1.y + 0.8f)); ++ty) {
                for (int tx = static_cast<int>(std::floor(a1.x - 0.8f)); tx <= static_cast<int>(std::floor(a1.x + 0.8f)); ++tx) {
                    if (!map.contains_tile(tx, ty)) continue;
                    const engine::Terrain under = map.terrain(tx, ty);
                    if (under != engine::Terrain::Crops && under != engine::Terrain::Wheat && under != engine::Terrain::Garden) continue;
                    uint64_t& bits = crushed_[static_cast<size_t>(ty * map.width() + tx)];
                    for (int sy = 0; sy < 8; ++sy) {
                        for (int sx = 0; sx < 8; ++sx) {
                            const Vector2 d{static_cast<float>(tx) + (static_cast<float>(sx) + 0.5f) / 8.0f - a1.x,
                                            static_cast<float>(ty) + (static_cast<float>(sy) + 0.5f) / 8.0f - a1.y};
                            if (std::fabs(d.x * f.x + d.y * f.y) <= half_l && std::fabs(-d.x * f.y + d.y * f.x) <= half_w) bits |= uint64_t{1} << (sy * 8 + sx);
                        }
                    }
                }
            }
        }
        it->second = p;
    }
    for (TrackMark& m : track_marks_) m.age += dt;
    std::erase_if(track_marks_, [](const TrackMark& m) { return m.age >= kTrackSeconds; });
    if (track_marks_.size() > 12000) track_marks_.erase(track_marks_.begin(), track_marks_.begin() + static_cast<long>(track_marks_.size() - 12000));
    std::erase_if(track_last_, [&](const auto& e) { return world.find_unit(e.first) == nullptr; });

    // Aircraft hit, in the air: smoke trailing off them, grey, then black with fire in it.
    for (const engine::Unit& u : world.units()) {
        if (!u.airborne || !shows(world, u)) continue;
        const int wear = wear_of(u.hp, engine::unit_type(u.type).max_hp);
        if (wear < 2 || fx_random() > dt * (wear >= 3 ? 30.0f : 14.0f)) continue;
        const Vector2 f = unit_facing(u);
        const Vector2 g = to_vector2(u.pos);
        Particle p{};
        p.kind = wear >= 3 && fx_random() < 0.4f ? Particle::Kind::Flame : Particle::Kind::Smoke;
        p.ground = {g.x - f.x * 0.25f, g.y - f.y * 0.25f};
        p.z = flight_lift(u) + 2.0f;
        p.vz = 2.0f;
        p.life = p.kind == Particle::Kind::Flame ? 0.3f : 2.2f + fx_random();
        p.size = p.kind == Particle::Kind::Flame ? 2.5f + fx_random() : 1.8f + fx_random();
        p.grow = p.kind == Particle::Kind::Flame ? -3.0f : 4.0f;
        p.color = p.kind == Particle::Kind::Flame ? Color{255, 200, 80, 230} : wear >= 3 ? Color{26, 24, 22, 220} : Color{120, 118, 114, 180};
        particles_.push_back(p);
    }
    // Aircraft: the heights they're drawn at easing to where they fly.
    for (const engine::Unit& u : world.units()) {
        if (!u.airborne) {
            g_drawn_height.erase(u.id);
            continue;
        }
        const float want = to_float(u.altitude);
        auto [it, fresh] = g_drawn_height.try_emplace(u.id, want);
        if (!fresh) it->second += (want - it->second) * std::min(1.0f, dt * 12.0f);
    }
    std::erase_if(g_drawn_height, [&](const auto& e) { return world.find_unit(e.first) == nullptr; });
    // The particles fly, fall, spread and fade.
    for (Particle& p : particles_) {
        const bool fresh = p.seed == 0;
        if (fresh) p.seed = ++particle_seed_ * 2654435761u | 1u;
        // (A flash, a muzzle's fire: seen at least a frame, however slow the frames come.)
        const bool brief = p.kind == Particle::Kind::Muzzle || p.kind == Particle::Kind::Flash;
        p.age += fresh && brief ? std::min(dt, p.life * 0.2f) : dt;
        if (p.age < 0.0f) continue;  // (not yet: waiting where it will come out)
        p.ground = {p.ground.x + p.vel.x * dt, p.ground.y + p.vel.y * dt};
        p.z += p.vz * dt;
        p.size = std::max(0.3f, p.size + p.grow * dt);
        switch (p.kind) {
            case Particle::Kind::Smoke:
            case Particle::Kind::Blow:
                p.vz *= 1.0f - 0.6f * dt;
                p.vel = {p.vel.x * (1.0f - 0.8f * dt) + 0.12f * dt, p.vel.y * (1.0f - 0.8f * dt) - 0.08f * dt};  // the wind
                break;
            case Particle::Kind::Clod:
            case Particle::Kind::Spray:
            case Particle::Kind::Spark:
                p.vz -= 170.0f * dt;
                if (p.z <= 0.0f && p.vz < 0.0f) {  // landed: lies there (a drop, a spark goes)
                    p.z = 0.0f;
                    p.vz = 0.0f;
                    p.vel = {};
                    if (p.kind != Particle::Kind::Clod) p.age = p.life;
                }
                break;
            case Particle::Kind::Casing:
                p.vz -= 200.0f * dt;
                if (p.z <= 0.0f && p.vz < 0.0f) {  // it bounces, then lies there glinting
                    p.z = 0.0f;
                    p.vz = p.vz < -25.0f ? -p.vz * 0.3f : 0.0f;
                    p.vel = {p.vel.x * 0.4f, p.vel.y * 0.4f};
                }
                break;
            case Particle::Kind::Flame:
            case Particle::Kind::Flash:
            case Particle::Kind::Muzzle:
            case Particle::Kind::Toss: break;
        }
    }
    std::erase_if(particles_, [](const Particle& p) { return p.age >= p.life; });
    if (particles_.size() > 3000) particles_.erase(particles_.begin(), particles_.begin() + static_cast<long>(particles_.size() - 3000));

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
    for (Remains& r : remains_) {
        r.age += dt;
        if (r.fell <= 0.0f) continue;
        Vector2 g{};
        float z = 0.0f;
        Vector2 d{};
        fall_of(r, g, z, d);
        if (r.age < r.fell) {
            // Coming down on fire: flames streaming off it, black smoke left hanging behind.
            auto count = [&](float rate) {
                const float n = rate * dt;
                return static_cast<int>(n) + (fx_random() < n - std::floor(n) ? 1 : 0);
            };
            for (int i = count(70.0f); i > 0; --i) {  // a streak of fire behind it
                Particle p{};
                p.kind = Particle::Kind::Flame;
                const float back = 0.1f + 0.35f * fx_random();
                p.ground = {g.x - d.x * back + (fx_random() - 0.5f) * 0.08f, g.y - d.y * back + (fx_random() - 0.5f) * 0.08f};
                p.z = z + back * 6.0f;
                p.vz = 3.0f;
                p.life = 0.3f + 0.3f * fx_random();
                p.size = 3.5f + 3.5f * fx_random();
                p.grow = -3.0f;
                p.color = {255, 200, 80, 230};
                particles_.push_back(p);
            }
            for (int i = count(55.0f); i > 0; --i) {
                Particle p{};
                p.kind = Particle::Kind::Smoke;
                const float back = 0.3f + 0.3f * fx_random();
                p.ground = {g.x - d.x * back + (fx_random() - 0.5f) * 0.1f, g.y - d.y * back + (fx_random() - 0.5f) * 0.1f};
                p.z = z + 2.0f;
                p.vel = {0.08f, -0.05f};
                p.vz = 4.0f + 4.0f * fx_random();
                p.life = 3.0f + 1.5f * fx_random();
                p.size = 2.5f + 1.5f * fx_random();
                p.grow = 6.0f;
                p.color = {20, 18, 16, 235};
                particles_.push_back(p);
            }
            continue;
        }
        // Crashed: a mushroom of fire rolling up, the blackest smoke boiling over it, bits of it flung about.
        const Vector2 at = r.ground;
        blasts_.push_back({at, 0.0f, kMushroomBlast + 1.0f, 0.0f});
        blasts_.push_back({{at.x + d.x * 0.45f, at.y + d.y * 0.45f}, -0.1f, 1.8f, 4.0f});  // the fuel going up along where it hit
        blasts_.push_back({{at.x - d.x * 0.5f, at.y - d.y * 0.5f}, -0.2f, 1.6f, 3.0f});
        spawn_flash(at, 10.0f, 30.0f, {255, 244, 200, 255});
        spawn_sparks(at, 8.0f, 50, {255, 190, 90, 255}, 3.2f);
        spawn_burst(world, at, 1.0f, 8);  // the earth thrown up (Burst::Dirt)
        for (int i = 0; i < 18; ++i) {  // bits of it flung out
            const float a = fx_random() * 6.2831853f;
            const float v = 1.0f + 2.5f * fx_random();
            Particle p{};
            p.kind = Particle::Kind::Clod;
            p.ground = at;
            p.vel = {std::cos(a) * v + d.x * 1.2f, std::sin(a) * v + d.y * 1.2f};
            p.z = 6.0f;
            p.vz = 50.0f + 70.0f * fx_random();
            p.life = 1.6f + fx_random();
            p.size = 1.5f + 1.5f * fx_random();
            p.color = i % 3 == 0 ? Color{120, 124, 116, 255} : i % 3 == 1 ? Color{40, 36, 32, 255} : Color{160, 160, 150, 255};
            particles_.push_back(p);
        }
        for (int i = 0; i < 30; ++i) {  // the black smoke going on up out of its cap as it cools, spreading; a column under it
            const bool cap = i >= 10;
            const float wait = cap ? 2.6f + 0.6f * fx_random() : 2.2f + 0.8f * fx_random();  // hidden till the mushroom's gone to smoke
            Particle p{};
            p.kind = Particle::Kind::Smoke;
            p.ground = {at.x + (fx_random() - 0.5f) * (cap ? 1.2f : 0.3f), at.y + (fx_random() - 0.5f) * (cap ? 1.2f : 0.3f)};
            p.vel = {(fx_random() - 0.5f) * (cap ? 0.5f : 0.1f), (fx_random() - 0.5f) * (cap ? 0.5f : 0.1f)};
            p.vz = cap ? 10.0f + 8.0f * fx_random() : 14.0f + 10.0f * fx_random();
            p.z = cap ? 96.0f + 30.0f * fx_random() : 20.0f + 60.0f * fx_random();
            p.age = -wait;
            p.life = 5.0f + 2.0f * fx_random();
            p.size = cap ? 8.0f + 4.0f * fx_random() : 5.0f + 2.0f * fx_random();
            p.grow = cap ? 10.0f : 6.0f;
            p.color = {14, 12, 12, 240};
            particles_.push_back(p);
        }
        r.fell = 0.0f;
        r.age = 0.0f;  // (a wreck from now on)
        r.cargo = static_cast<uint8_t>(Cargo::Fuel);  // its fuel burning a while, black smoke over it
    }
    // Men in a trench stand in its ditch, sunk in it to the waist; a gun sits
    // down in its pit, a self-propelled one in its caponier. They settle in
    // once they stop, and get up as they set off.
    g_settled = &settle_;
    if (seen_terrain_.size() == static_cast<size_t>(map.width() * map.height())) {
        for (const engine::Unit& u : world.units()) {
            if (u.airborne || u.inside || u.riding) {
                settle_.erase(u.id);
                continue;
            }
            const Vector2 p = to_vector2(u.pos);
            const int tx = static_cast<int>(std::floor(p.x));
            const int ty = static_cast<int>(std::floor(p.y));
            const std::optional<works::Spec> spec = map.contains_tile(tx, ty) ? works_at(world, tx, ty) : std::nullopt;
            if (!spec && !settle_.count(u.id)) continue;
            Vector2 offset{};
            float sink = 0.0f;
            if (spec) {
                const Vector2 local{p.x - static_cast<float>(tx), p.y - static_cast<float>(ty)};
                const works::Place place = works::place(*spec, local, engine::unit_type(u.type).vehicle);
                offset = {place.at.x - local.x, place.at.y - local.y};
                const bool in_it = std::hypot(offset.x, offset.y) < 0.06f;  // (walking along the ditch)
                sink = u.moving && !in_it ? 0.0f : place.sunk;
            }
            Settled& st = settle_[u.id];
            const float want = spec && !u.moving ? 1.0f : 0.0f;
            st.w += std::clamp(want - st.w, -dt * 4.0f, dt * 4.0f);
            st.offset = {offset.x * st.w, offset.y * st.w};
            st.sink += std::clamp(sink - st.sink, -dt * 30.0f, dt * 30.0f);
            if (!spec && st.w <= 0.0f && st.sink <= 0.0f) settle_.erase(u.id);
        }
        if (frame_ % 120 == 0) std::erase_if(settle_, [&](const auto& e) { return world.find_unit(e.first) == nullptr; });
    }
    std::erase_if(pings_, [](const Ping& p) { return p.age >= kPingLifetime; });
    std::erase_if(blasts_, [](const Blast& b) { return b.age >= kBlastLifetime; });
    std::erase_if(remains_, [](const Remains& r) {
        const bool whole = drawn_as_armor(engine::unit_type(r.type)) || truck_model(r.type, r.owner).has_value() || gun_or_plane(engine::unit_type(r.type));
        const float life = whole ? kTankWreckLifetime : r.vehicle ? kWreckLifetime : kBodyLifetime;
        return r.age >= life;
    });
}

void WorldRenderer::fall_of(const Remains& r, Vector2& ground, float& z, Vector2& dir) {
    const float k = r.fell > 0.0f ? std::clamp(r.age / r.fell, 0.0f, 1.0f) : 1.0f;
    ground = lerp(r.from, r.ground, k);
    z = r.height * (1.0f - k * k);  // falling ever faster
    // Tumbling round as it goes, the nose over.
    const float spin = ((r.seed & 1u) != 0 ? 1.0f : -1.0f) * 3.2f * k * k;
    dir = {r.facing.x * std::cos(spin) - r.facing.y * std::sin(spin), r.facing.y * std::cos(spin) + r.facing.x * std::sin(spin)};
}

// --- Drawing -----------------------------------------------------------------

const char* dump_dir();  // where the baked sheets are written to look at (below, with the sprites)

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

Color mix(Color a, Color b, float t);  // below, with the ground's colours

// A filled circle with as few sides as its size needs (a forest is many of them).
void disc(Vector2 centre, float radius, Color color) {
    const int sides = std::clamp(static_cast<int>(radius * 1.6f), 6, 18);
    DrawCircleSector(centre, radius, 0.0f, 360.0f, sides, lit(color));
}

float hash_unit(uint32_t h) { return static_cast<float>(h & 0xFFFF) / 65536.0f; }

float rand01(uint32_t seed, int i);  // below, with the buildings

// --- Fire, explosions and flashes in pixel art ---------------------------------
//
// Baked once into sheets of frames, as the vehicles are (a pixel of a sheet a
// pixel of the world): an explosion's ball of fire in lumps (white-hot at
// its heart, yellow, orange, dark red at the edge, a dark line round it)
// swelling and rising, cooling to dark lumps with embers glowing in them,
// then rings of smoke thinning out; a flame's tongues flickering; a flash's
// star of rays; embers smouldering in a burnt patch.

struct FxSheet {
    Texture2D tex{};
    int w = 0;
    int h = 0;
    int frames = 0;
    Vector2 origin{};  // its foot (a blast's, a flame's) or its middle (a flash's)
};
enum class Fx : uint8_t {
    Blast0, Blast1, Blast2, Blast3,
    Flame0, Flame1, Flame2,
    Star0, Star1, Star2,
    Embers,
    Scorch0, Scorch1, Scorch2,
    Smoke0, Smoke1, Smoke2, Smoke3, Smoke4, Smoke5,
    Roof0, Roof1,
    Mushroom,
    Count
};
constexpr int kSmokeFrames = 8;  // a puff's life: dense (0..2), breaking up (3, 4), rings thinning out (5..7)
constexpr int kMushroomFrames = 34;
constexpr float kMushroomSeconds = 3.4f;
constexpr float kSmokeTiers[6] = {4.0f, 6.0f, 9.0f, 13.0f, 18.0f, 26.0f};  // its sizes, pixels round
constexpr int kStreakTurns = 8;  // blown smoke: its sheets turned every 22.5 degrees
constexpr float kStreakSizes[3][2] = {{14.0f, 6.0f}, {22.0f, 9.0f}, {32.0f, 13.0f}};  // long, wide
constexpr int kFxVariants = 3;
constexpr int kBlastFrames = 18;
constexpr float kBlastSeconds[4] = {0.7f, 1.0f, 1.3f, 1.7f};  // how long a blast's sheet plays, by its size
std::vector<FxSheet> g_fx;
std::vector<FxSheet> g_streaks;  // blown smoke: by variant, size, turn

const FxSheet& fx_sheet(Fx kind, int variant) {
    return g_fx[static_cast<size_t>(kind) * kFxVariants + static_cast<size_t>(variant % kFxVariants)];
}

// Fire's colours, white-hot to the dark red at its edge; the dark line round it.
constexpr Color kHeat[5] = {{255, 250, 222, 255}, {255, 222, 92, 255}, {250, 156, 42, 255}, {212, 80, 30, 255}, {138, 40, 24, 255}};
constexpr Color kFireRim{50, 20, 14, 255};
Color heat_colour(float v) { return v > 0.74f ? kHeat[0] : v > 0.54f ? kHeat[1] : v > 0.36f ? kHeat[2] : v > 0.2f ? kHeat[3] : kHeat[4]; }

float fx_noise(uint32_t seed, int x, int y) {
    return hash_unit(tile_hash(x * 7 + static_cast<int>(seed % 9973u), y * 13 + static_cast<int>((seed / 9973u) % 9973u)));
}

// A frame being drawn, pixel by pixel.
struct Canvas {
    int w;
    int h;
    std::vector<Color> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_ * h_), Color{0, 0, 0, 0}) {}
    bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    Color get(int x, int y) const { return in(x, y) ? px[static_cast<size_t>(y * w + x)] : Color{0, 0, 0, 0}; }
    void set(int x, int y, Color c) {
        if (in(x, y)) px[static_cast<size_t>(y * w + x)] = c;
    }
    void set_if_empty(int x, int y, Color c) {
        if (in(x, y) && get(x, y).a == 0) set(x, y, c);
    }
    void disc(float cx, float cy, float r, Color c) {
        for (int y = static_cast<int>(cy - r) - 1; y <= static_cast<int>(cy + r) + 1; ++y) {
            for (int x = static_cast<int>(cx - r) - 1; x <= static_cast<int>(cx + r) + 1; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - cx;
                const float dy = static_cast<float>(y) + 0.5f - cy;
                if (dx * dx + dy * dy <= r * r) set(x, y, c);
            }
        }
    }
    // A dark line round what's drawn.
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
};

FxSheet bake_fx(int w, int h, int frames, Vector2 origin, const std::function<void(Canvas&, int)>& draw) {
    Image img = GenImageColor(w * frames, h, {0, 0, 0, 0});
    auto* out = static_cast<Color*>(img.data);
    for (int f = 0; f < frames; ++f) {
        Canvas c(w, h);
        draw(c, f);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) out[static_cast<size_t>(y * w * frames + f * w + x)] = c.px[static_cast<size_t>(y * w + x)];
        }
    }
    if (const char* dump = dump_dir()) {
        static int n = 0;
        ExportImage(img, TextFormat("%s/fx_%02d.png", dump, n++));
    }
    FxSheet sheet;
    sheet.tex = LoadTextureFromImage(img);
    SetTextureFilter(sheet.tex, TEXTURE_FILTER_POINT);
    sheet.w = w;
    sheet.h = h;
    sheet.frames = frames;
    sheet.origin = origin;
    UnloadImage(img);
    return sheet;
}

// A star of rays out of a white-hot heart: a flash, the start of a blast.
void star_on(Canvas& c, float cx, float cy, float r, float k, uint32_t seed) {
    c.disc(cx, cy, r * 0.45f * k, kHeat[1]);
    c.disc(cx, cy, r * 0.28f * k, kHeat[0]);
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * 0.7853982f + (rand01(seed, i) - 0.5f) * 0.3f;
        const float len = r * k * (i % 2 == 1 ? 0.6f : 1.0f) * (0.8f + 0.4f * rand01(seed, i + 9));
        for (float d = r * 0.2f * k; d <= len; d += 0.5f) {
            const float q = d / std::max(len, 0.5f);
            c.set(static_cast<int>(std::floor(cx + std::cos(a) * d)), static_cast<int>(std::floor(cy + std::sin(a) * d * 0.8f)),
                  q < 0.45f ? kHeat[0] : q < 0.8f ? kHeat[1] : kHeat[2]);
        }
    }
}

// An explosion `r` pixels round at its biggest: a flash, then a ball of fire
// in lumps swelling and rising; cooling to dark lumps with embers in them;
// rings of smoke thinning out.
FxSheet bake_blast(float r, uint32_t seed) {
    const int w = static_cast<int>(r * 3.0f) + 8;
    const int h = static_cast<int>(r * 3.6f) + 8;
    const Vector2 o{static_cast<float>(w / 2), static_cast<float>(h - 4)};
    struct Bubble {
        float x, y, r, lump;
    };
    std::vector<Bubble> bubbles{{0.0f, -r * 0.5f, r * 0.62f, rand01(seed, 0) * 6.28f}};
    const int n = 6 + static_cast<int>(r / 3.0f);
    for (int i = 1; i < n; ++i) {
        const float a = rand01(seed, i * 3) * 6.2831853f;
        const float d = r * (0.28f + 0.38f * rand01(seed, i * 3 + 1));
        bubbles.push_back({std::cos(a) * d, -r * 0.5f + std::sin(a) * d * 0.75f, r * (0.28f + 0.22f * rand01(seed, i * 3 + 2)), rand01(seed, i * 5) * 6.28f});
    }
    return bake_fx(w, h, kBlastFrames, o, [&](Canvas& c, int f) {
        const float t = static_cast<float>(f) / static_cast<float>(kBlastFrames - 1);
        const float grow = std::clamp((t - 0.04f) / 0.4f, 0.0f, 1.0f);
        const float swell = 0.3f + 0.7f * (1.0f - (1.0f - grow) * (1.0f - grow));
        const float rise = r * 0.55f * t;
        const float heat = t < 0.4f ? 1.0f : t < 0.7f ? 1.0f - (t - 0.4f) / 0.3f * 0.95f : 0.0f;
        const bool rings = t >= 0.8f;
        const float fade = rings ? (t - 0.8f) / 0.2f : 0.0f;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float px = static_cast<float>(x) - o.x + 0.5f;
                const float py = static_cast<float>(y) - o.y + 0.5f;
                float hot = 0.0f;
                float nearest = 9.0f;
                bool ring = false;
                for (const Bubble& b : bubbles) {
                    const float bx = b.x * swell * (1.0f + 0.25f * t);
                    const float by = b.y * swell - rise;
                    const float br = b.r * swell * (1.0f + 0.3f * t);
                    const float dx = px - bx;
                    const float dy = (py - by) * 1.1f;
                    const float ang = std::atan2(dy, dx);
                    const float rr = br * (1.0f + 0.12f * std::sin(3.0f * ang + b.lump) + 0.06f * std::sin(5.0f * ang + 2.0f * b.lump));
                    const float d = std::sqrt(dx * dx + dy * dy) / std::max(rr, 0.5f);
                    if (d < 1.0f) hot = std::max(hot, 1.0f - d);
                    if (d >= 0.7f && d < 1.0f) ring = true;
                    nearest = std::min(nearest, d);
                }
                if (nearest >= 1.0f) continue;
                const float grain = (fx_noise(seed + static_cast<uint32_t>(f), x, y) - 0.5f) * 0.12f;
                if (!rings) {
                    // Each lump white-hot at its heart, yellow, orange where it meets the next, red at the edge.
                    const float v = (std::pow(hot, 1.25f) * 1.25f + grain) * heat;
                    if (heat > 0.05f && v > 0.16f) {
                        c.set(x, y, v > 0.8f ? kHeat[0] : v > 0.56f ? kHeat[1] : v > 0.34f ? kHeat[2] : kHeat[3]);
                    } else {  // cooled: the dark lumps of it, embers glowing in them
                        const bool ember = fx_noise(seed * 3u + static_cast<uint32_t>(f), x, y) < 0.07f * heat + (t < 0.75f ? 0.03f : 0.0f);
                        const Color lump = hot + grain > 0.35f ? Color{70, 60, 56, 255} : Color{48, 42, 40, 255};
                        c.set(x, y, ember ? kHeat[2] : hot + grain > 0.18f && ring ? Color{36, 30, 28, 255} : lump);
                    }
                } else if (ring && nearest > 0.45f && fx_noise(seed * 7u + static_cast<uint32_t>(f), x, y) > fade * 0.9f) {  // each lump's ring of smoke, thinning out
                    const auto g = static_cast<unsigned char>(62.0f + 56.0f * fade);
                    c.set(x, y, {g, static_cast<unsigned char>(g - 2), static_cast<unsigned char>(g - 4), 255});
                }
            }
        }
        if (t < 0.45f) {  // bits of it flung out
            for (int i = 0; i < 10; ++i) {
                const float a = rand01(seed, 100 + i) * 6.2831853f;
                const float d = r * (0.7f + 1.5f * t) * (0.8f + 0.4f * rand01(seed, 120 + i));
                c.set(static_cast<int>(o.x + std::cos(a) * d), static_cast<int>(o.y - r * 0.5f - rise + std::sin(a) * d * 0.8f), i % 2 == 0 ? kHeat[1] : kHeat[3]);
            }
        }
        if (t < 0.16f) star_on(c, o.x, o.y - r * 0.45f, r * 1.1f, 1.0f - t * 3.0f, seed);  // the flash
        if (!rings) c.outline(kFireRim);
    });
}

// A flame `fw` pixels wide, `fh` high: its tongues flickering, split at the
// top; white-hot at the foot, red at the tips; a spark going up off it now and then.
FxSheet bake_flame(float fw, float fh, uint32_t seed) {
    const int w = static_cast<int>(fw) + 6;
    const int h = static_cast<int>(fh) + 5;
    const Vector2 o{static_cast<float>(w / 2), static_cast<float>(h - 2)};
    constexpr int kFrames = 8;
    struct Tongue {
        float x, base, tall, width, phase;
    };
    const Tongue tongues[3] = {{0.0f, 0.0f, 1.0f, 0.5f, 0.0f},
                               {-fw * 0.24f, 0.15f, 0.62f + 0.12f * rand01(seed, 1), 0.3f, 2.1f},
                               {fw * 0.24f, 0.1f, 0.7f + 0.12f * rand01(seed, 2), 0.3f, 4.2f}};
    return bake_fx(w, h, kFrames, o, [&](Canvas& c, int f) {
        const float ph = static_cast<float>(f) / kFrames * 6.2831853f;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float yn = (o.y - static_cast<float>(y)) / fh;  // 0 at its foot, 1 at the tip
                if (yn < 0.0f || yn > 1.1f) continue;
                const float xr = static_cast<float>(x) - o.x + 0.5f;
                float hv = -1.0f;
                for (const Tongue& t : tongues) {
                    const float tall = t.tall * (0.88f + 0.12f * std::sin(ph + t.phase));  // leaping up and down
                    const float k = (yn - t.base * 0.3f) / tall;
                    if (k < 0.0f || k > 1.0f) continue;
                    const float lean = std::sin(k * 3.2f + ph + t.phase) * fw * 0.12f * k;  // licking side to side
                    const float bulge = k < 0.25f ? 0.75f + k : 1.0f;  // round at its foot
                    const float hw = fw * t.width * std::pow(1.0f - k, 0.85f) * bulge;
                    hv = std::max(hv, (1.0f - std::fabs(xr - t.x * (1.0f - k * 0.5f) - lean) / std::max(hw, 0.35f)) * (1.0f - 0.25f * k));
                }
                hv += (fx_noise(seed + static_cast<uint32_t>(f), x, y) - 0.5f) * 0.16f;
                if (hv <= 0.0f) continue;
                const float v = hv * (1.1f - 0.6f * yn);
                c.set(x, y, v > 0.78f ? kHeat[0] : v > 0.55f ? kHeat[1] : v > 0.3f ? kHeat[2] : kHeat[3]);
            }
        }
        if (f % 3 == 0) c.set(static_cast<int>(o.x + (rand01(seed, f) - 0.5f) * fw * 0.6f), static_cast<int>(o.y - fh) - 1 - f % 2, kHeat[1]);  // a spark off it
        c.outline({110, 30, 18, 255});
    });
}

// A flash: a star of rays, shrinking and going orange over its four frames.
FxSheet bake_star(float r, uint32_t seed) {
    const int s = static_cast<int>(r * 2.0f) + 5;
    const Vector2 o{static_cast<float>(s / 2), static_cast<float>(s / 2)};
    return bake_fx(s, s, 4, o, [&](Canvas& c, int f) { star_on(c, o.x, o.y, r, 1.0f - static_cast<float>(f) * 0.22f, seed); });
}

// Embers smouldering in a burnt patch: dark lumps, specks glowing, flickering.
FxSheet bake_embers(uint32_t seed) {
    constexpr int w = 30;
    constexpr int h = 14;
    return bake_fx(w, h, 4, {static_cast<float>(w / 2), static_cast<float>(h / 2)}, [&](Canvas& c, int f) {
        for (int i = 0; i < 22; ++i) {
            const float a = rand01(seed, i) * 6.2831853f;
            const float d = std::sqrt(rand01(seed, i + 40));
            const float cx = w * 0.5f + std::cos(a) * d * w * 0.42f;
            const float cy = h * 0.5f + std::sin(a) * d * h * 0.38f;
            c.disc(cx, cy, 1.0f + 1.3f * rand01(seed, i + 80), i % 3 == 0 ? Color{66, 46, 38, 255} : Color{44, 34, 30, 255});
        }
        for (int i = 0; i < 16; ++i) {
            const int x = static_cast<int>(rand01(seed, 200 + i) * w);
            const int y = static_cast<int>(rand01(seed, 220 + i) * h);
            if (c.get(x, y).a == 0) continue;
            const float glow = rand01(seed, 240 + i * 4 + f);
            c.set(x, y, glow > 0.7f ? kHeat[1] : glow > 0.35f ? kHeat[2] : kHeat[3]);
        }
    });
}

// The ground burnt round a fire, `rx` pixels across its half: a ragged patch
// of soot, black at the heart, burnt earth and singed grass at its edges,
// ash grey in flecks, bits of it spattered out round it. Frames 0..3: embers
// glowing in it, flickering (still burning); 4: cold.
FxSheet bake_scorch(float rx, uint32_t seed) {
    const float ry = rx * 0.5f;
    const int w = static_cast<int>(rx * 2.5f) + 4;
    const int h = static_cast<int>(ry * 2.5f) + 4;
    const Vector2 o{static_cast<float>(w / 2), static_cast<float>(h / 2)};
    const float p1 = rand01(seed, 1) * 6.28f;
    const float p2 = rand01(seed, 2) * 6.28f;
    return bake_fx(w, h, 5, o, [&](Canvas& c, int f) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float dx = (static_cast<float>(x) - o.x + 0.5f) / rx;
                const float dy = (static_cast<float>(y) - o.y + 0.5f) / ry;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float a = std::atan2(dy, dx);
                const float lumpy = fx_noise(seed, x / 3, y / 2) - 0.5f;
                const float edge = 0.86f + 0.1f * std::sin(3.0f * a + p1) + 0.06f * std::sin(7.0f * a + p2) + 0.1f * lumpy;
                const float n = fx_noise(seed * 3u, x, y);
                if (d > edge) {  // spattered round it
                    if (d < edge + 0.3f && n < 0.08f * (1.0f - (d - edge) / 0.3f) * 2.0f) c.set(x, y, n < 0.05f ? Color{62, 50, 38, 255} : Color{92, 80, 52, 255});
                    continue;
                }
                const float t = d / edge;
                Color col = t < 0.5f ? (n < 0.5f ? Color{28, 24, 22, 255} : Color{36, 30, 26, 255})
                          : t < 0.8f ? (n < 0.5f ? Color{46, 38, 32, 255} : Color{56, 46, 36, 255})
                                     : (n < 0.55f ? Color{76, 60, 42, 255} : Color{98, 86, 54, 255});
                if (fx_noise(seed * 7u, x, y) < 0.05f && t < 0.85f) col = Color{110, 106, 100, 255};  // ash
                if (f < 4 && t < 0.72f) {  // embers
                    const float e = fx_noise(seed * 11u, x, y);
                    if (e < 0.045f) {
                        const float glow = fx_noise(seed * 13u + static_cast<uint32_t>(f), x, y);
                        col = glow > 0.66f ? kHeat[1] : glow > 0.3f ? kHeat[2] : kHeat[3];
                    }
                }
                c.set(x, y, col);
            }
        }
    });
}

// A cloud of smoke in lumps (`lumps`: where they are round its middle, how
// big; `r` how big it is), in grey (tinted as it's drawn: black off a
// fire, white off a screen, brown off a burst): lumps inside a dark line,
// lit on top to the left, a crescent of shade under each; then breaking up,
// the lumps shrinking apart, a few gone; then only its rim left, a thick
// ring broken into arcs, thinning out.
struct CloudLump {
    float x, y, r, wob;
};
FxSheet bake_cloud(std::vector<CloudLump> lumps, float r, uint32_t seed) {
    constexpr float kMostSpread = 1.75f;
    float ex = 0.0f;
    float ey = 0.0f;
    for (const CloudLump& l : lumps) {
        ex = std::max(ex, std::fabs(l.x) * kMostSpread + l.r * 1.15f);
        ey = std::max(ey, std::fabs(l.y) * kMostSpread + l.r * 1.15f);
    }
    const int w = static_cast<int>(ex * 2.0f) + 6;
    const int h = static_cast<int>(ey * 2.0f + r * 0.4f) + 6;
    const Vector2 o{static_cast<float>(w / 2), static_cast<float>(h / 2) + r * 0.2f};
    std::sort(lumps.begin(), lumps.end(), [](const CloudLump& a, const CloudLump& b) { return a.y < b.y; });  // the lower ones in front
    constexpr Color kLight{238, 238, 238, 255};
    constexpr Color kMid{196, 196, 196, 255};
    constexpr Color kShadow{148, 148, 148, 255};
    constexpr Color kLine{72, 72, 72, 255};
    return bake_fx(w, h, kSmokeFrames, o, [&](Canvas& c, int f) {
        const float rise = r * 0.05f * static_cast<float>(f);
        const float spread = f < 3 ? 0.86f + 0.07f * static_cast<float>(f) : 1.0f + 0.15f * static_cast<float>(f - 2);
        auto shape = [&](const CloudLump& l, float scale, auto&& pixel) {
            const float lx = o.x + l.x * spread;
            const float ly = o.y + l.y * spread - rise;
            const float lr = l.r * scale;
            for (int y = static_cast<int>(ly - lr) - 2; y <= static_cast<int>(ly + lr) + 2; ++y) {
                for (int x = static_cast<int>(lx - lr) - 2; x <= static_cast<int>(lx + lr) + 2; ++x) {
                    const float dx = static_cast<float>(x) + 0.5f - lx;
                    const float dy = static_cast<float>(y) + 0.5f - ly;
                    const float a = std::atan2(dy, dx);
                    const float rr = lr * (1.0f + 0.08f * std::sin(3.0f * a + l.wob) + 0.04f * std::sin(5.0f * a + 2.0f * l.wob));
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (d <= rr) pixel(x, y, dx / rr, dy / rr, d / rr, a);
                }
            }
        };
        if (f < 5) {
            // Dense, then breaking up: the lumps shrinking apart, a few of them gone.
            const float shrink = f < 3 ? 1.0f : f == 3 ? 0.84f : 0.66f;
            for (size_t li = 0; li < lumps.size(); ++li) {
                if (f == 4 && li > 0 && rand01(seed, 300 + static_cast<int>(li)) < 0.3f) continue;
                shape(lumps[li], shrink, [&](int x, int y, float nx, float ny, float d, float) {
                    Color tone = kMid;
                    if (ny < -0.28f && nx < 0.4f && d < 0.86f) tone = kLight;
                    if (ny > 0.2f && d > 0.6f) tone = kShadow;
                    if (f >= 3 && ny > 0.0f && d > 0.45f) tone = kShadow;  // breaking up: darker
                    c.set(x, y, tone);
                });
            }
            c.outline(kLine);
            return;
        }
        // Only its rim left: a thick ring round where it was, broken into
        // arcs, thinning out; a crescent or two inside it.
        std::vector<uint8_t> in(static_cast<size_t>(w * h), 0);
        for (const CloudLump& l : lumps) {
            shape(l, 1.0f, [&](int x, int y, float, float, float, float) {
                if (x >= 0 && y >= 0 && x < w && y < h) in[static_cast<size_t>(y * w + x)] = 1;
            });
        }
        auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && in[static_cast<size_t>(y * w + x)] != 0; };
        const float keep = 1.0f - 0.28f * static_cast<float>(f - 5);
        const int thick = r >= 9.0f ? 3 : 2;
        const auto g = static_cast<unsigned char>(112 + 24 * (f - 5));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (!inside(x, y)) continue;
                bool rim = false;
                for (int k = 1; k <= thick && !rim; ++k) rim = !inside(x - k, y) || !inside(x + k, y) || !inside(x, y - k) || !inside(x, y + k);
                if (!rim) continue;
                const float a = std::atan2(static_cast<float>(y) - o.y, static_cast<float>(x) - o.x);
                if (std::sin(5.0f * a + rand01(seed, 400) * 6.28f + 0.7f * static_cast<float>(f)) < 1.0f - 2.0f * keep) continue;  // broken into arcs
                c.set(x, y, {g, g, g, 255});
            }
        }
        if (f < 7) {  // a crescent under the biggest lump or two
            for (size_t li = lumps.size() >= 2 ? lumps.size() - 2 : 0; li < lumps.size(); ++li) {
                const CloudLump& l = lumps[li];
                shape(l, 0.8f, [&](int x, int y, float nx, float ny, float d, float) {
                    if (d > 0.72f && ny > 0.25f && std::fabs(nx) < 0.75f) c.set(x, y, {g, g, g, 255});
                });
            }
        }
    });
}

// A round puff `r` pixels round.
FxSheet bake_smoke(float r, uint32_t seed) {
    std::vector<CloudLump> lumps{{0.0f, 0.0f, r * 0.62f, rand01(seed, 0) * 6.28f}};
    const int n = 4 + static_cast<int>(r / 5.0f);
    for (int i = 1; i <= n; ++i) {
        const float a = (static_cast<float>(i) + rand01(seed, i) * 0.6f) * 6.2831853f / static_cast<float>(n);
        const float d = r * (0.38f + 0.26f * rand01(seed, i + 20));
        lumps.push_back({std::cos(a) * d, std::sin(a) * d * 0.8f, r * (0.32f + 0.18f * rand01(seed, i + 40)), rand01(seed, i + 60) * 6.28f});
    }
    return bake_cloud(lumps, r, seed);
}

// Smoke blown out along `angle` (on the screen): a drawn-out cloud `len`
// pixels long, `wide` across, lumps along it, fattest in its middle.
FxSheet bake_streak(float len, float wide, float angle, uint32_t seed) {
    std::vector<CloudLump> lumps;
    const Vector2 d{std::cos(angle), std::sin(angle)};
    const Vector2 n{-d.y, d.x};
    const int k = std::max(3, static_cast<int>(len / (wide * 0.55f)));
    for (int i = 0; i < k; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(k) * 2.0f - 1.0f;  // -1..1 along it
        const float at = t * len * 0.42f;
        const float side = (rand01(seed, i) - 0.5f) * wide * 0.3f;
        const float lr = wide * 0.5f * (1.0f - 0.45f * t * t) * (0.85f + 0.3f * rand01(seed, i + 20));
        lumps.push_back({d.x * at + n.x * side, d.y * at + n.y * side, lr, rand01(seed, i + 40) * 6.28f});
    }
    return bake_cloud(lumps, wide, seed);
}

// Fire over a roof, `fw` pixels across: a bed of embers, dark red, and
// small tongues of fire all along it flaring up and dying down by turns.
FxSheet bake_roof_fire(float fw, uint32_t seed) {
    const float fh = fw * 0.9f;
    const int w = static_cast<int>(fw) + 6;
    const int h = static_cast<int>(fh) + 6;
    const Vector2 o{static_cast<float>(w / 2), static_cast<float>(h - 3)};
    constexpr int kFrames = 8;
    const int tongues = 4 + static_cast<int>(fw / 4.0f);
    return bake_fx(w, h, kFrames, o, [&](Canvas& c, int f) {
        const float ph = static_cast<float>(f) / kFrames * 6.2831853f;
        const float bx = fw * 0.5f;
        const float by = std::max(1.5f, fw * 0.16f);
        for (int y = 0; y < h; ++y) {  // the bed of embers
            for (int x = 0; x < w; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f - o.x) / bx;
                const float dy = (static_cast<float>(y) + 0.5f - (o.y - by)) / by;
                if (dx * dx + dy * dy > 1.0f) continue;
                const float nz = fx_noise(seed + static_cast<uint32_t>(f / 2), x, y);
                c.set(x, y, nz < 0.12f ? kHeat[1] : nz < 0.4f ? kHeat[3] : nz < 0.7f ? kHeat[4] : Color{70, 26, 18, 255});
            }
        }
        for (int i = 0; i < tongues; ++i) {  // the tongues, each flaring its own way
            const float tx = o.x + (rand01(seed, i) - 0.5f) * fw * 0.85f;
            const float edge = 1.0f - std::fabs(tx - o.x) / (fw * 0.5f);
            const float tall = fh * (0.35f + 0.55f * rand01(seed, i + 20)) * (0.5f + 0.5f * edge) * (0.55f + 0.45f * std::sin(ph + rand01(seed, i + 40) * 6.28f));
            const float hw = 1.2f + 1.4f * rand01(seed, i + 60);
            const float base = o.y - by * (0.6f + 0.8f * rand01(seed, i + 80));
            for (int y = static_cast<int>(base - tall); y <= static_cast<int>(base); ++y) {
                const float k = (base - static_cast<float>(y)) / std::max(tall, 1.0f);  // 0 at its foot, 1 at its tip
                const float half = hw * (1.0f - k * k);
                const float sway = std::sin(k * 3.0f + ph + static_cast<float>(i)) * 0.8f * k;
                for (int x = static_cast<int>(tx - half + sway); x <= static_cast<int>(tx + half + sway); ++x) {
                    c.set(x, y, k < 0.3f ? kHeat[0] : k < 0.6f ? kHeat[1] : k < 0.85f ? kHeat[2] : kHeat[3]);
                }
            }
        }
        c.outline({90, 26, 16, 255});
    });
}

// An aircraft crashing: a mushroom of fire. A white flash on the ground; a
// ball of fire boiling up off it on a stem, swelling into a cap of lumps,
// pale yellow at its top, orange, deep orange under, a band of grey smoke
// rolling under its rim; a skirt of fire spreading on the ground round its
// foot. Then it cools as it goes on up: the cap to dark red and to black
// smoke, the stem first; the fire on the ground last.
FxSheet bake_mushroom(uint32_t seed) {
    constexpr int w = 150;
    constexpr int h = 170;
    const Vector2 o{static_cast<float>(w / 2), static_cast<float>(h - 12)};
    auto rnd = [&](int i) { return rand01(seed, i); };
    struct Lump {
        float x, y, r, wob, heat;  // heat: how hot it burns, where it is in the cloud (1 its top)
        int part;                  // 0 the stem, 1 the grey under the cap, 2 the cap, 3 the skirt on the ground
    };
    // The lumps, as the cloud stands grown, round its cap's middle (for the
    // cap, the band) or up from the ground (the stem, the skirt): scaled as it grows.
    std::vector<Lump> lumps;
    for (int i = 0; i < 9; ++i) {  // the stem, narrowest at its waist
        const float t = static_cast<float>(i) / 8.0f;
        const float wide = 0.34f - 0.14f * std::sin(t * 3.14159f);
        lumps.push_back({(rnd(i) - 0.5f) * 0.22f, -t, wide * (0.8f + 0.3f * rnd(i + 20)), rnd(i + 40) * 6.28f, 0.5f + 0.2f * t, 0});
    }
    for (int i = 0; i < 9; ++i) {  // the grey band under the cap's rim
        const float a = 3.14159f * (0.08f + 0.84f * static_cast<float>(i) / 8.0f);
        lumps.push_back({-std::cos(a) * 0.8f, 0.3f + 0.05f * std::sin(a), 0.26f + 0.08f * rnd(i + 60), rnd(i + 80) * 6.28f, 0.0f, 1});
    }
    for (int i = 0; i < 22; ++i) {  // the cap: a dome of lumps, the ones on top hottest
        const float a = 3.14159f * rnd(i + 100);
        const float d = 0.35f + 0.55f * std::sqrt(rnd(i + 120));
        const float x = -std::cos(a) * d * 0.9f;
        const float y = -std::sin(a) * d * 0.72f + 0.1f;
        lumps.push_back({x, y, 0.28f + 0.14f * rnd(i + 140), rnd(i + 160) * 6.28f, 0.55f + 0.45f * (-y + 0.1f) / 0.82f, 2});
    }
    lumps.push_back({0.0f, -0.05f, 0.5f, rnd(180) * 6.28f, 0.8f, 2});
    for (int i = 0; i < 10; ++i) {  // the skirt of fire on the ground
        const float x = (static_cast<float>(i) / 9.0f - 0.5f) * 2.0f;
        lumps.push_back({x, -0.05f * rnd(i + 200), 0.22f + 0.1f * rnd(i + 220) - 0.06f * std::fabs(x), rnd(i + 240) * 6.28f, 0.75f, 3});
    }
    // Drawn from the bottom up (the stem, the grey, the cap's lower lumps first, its top over them), the skirt last.
    auto order = [](const Lump& l) { return l.part == 3 ? 10.0f : l.part == 0 ? -10.0f + l.y : l.part == 1 ? -5.0f : -l.y; };
    std::sort(lumps.begin(), lumps.end(), [&](const Lump& a, const Lump& b) { return order(a) < order(b); });
    constexpr Color kPale{255, 238, 176, 255};
    constexpr Color kYellow{255, 206, 96, 255};
    constexpr Color kOrange{246, 146, 44, 255};
    constexpr Color kDeep{214, 88, 30, 255};
    constexpr Color kRed{150, 48, 26, 255};
    constexpr Color kGreyLit{186, 192, 196, 255};
    constexpr Color kGrey{140, 148, 154, 255};
    constexpr Color kSmokeLit{92, 84, 80, 255};
    constexpr Color kSmoke{56, 50, 48, 255};
    constexpr Color kBlack{34, 30, 30, 255};
    return bake_fx(w, h, kMushroomFrames, o, [&](Canvas& c, int f) {
        const float t = static_cast<float>(f) / static_cast<float>(kMushroomFrames - 1);
        if (f < 3) {  // the flash, a ball of fire off the ground
            star_on(c, o.x, o.y - 10.0f, 34.0f - 6.0f * static_cast<float>(f), 1.0f, seed);
            c.disc(o.x, o.y - 12.0f - 4.0f * static_cast<float>(f), 12.0f + 6.0f * static_cast<float>(f), kYellow);
            c.disc(o.x, o.y - 12.0f - 4.0f * static_cast<float>(f), 8.0f + 4.0f * static_cast<float>(f), kPale);
            c.outline({90, 30, 16, 255});
            return;
        }
        auto ease = [](float k) { k = std::clamp(k, 0.0f, 1.0f); return 1.0f - (1.0f - k) * (1.0f - k); };
        const float grow = ease((t - 0.06f) / 0.45f);
        const float cap_r = 16.0f + 30.0f * grow;                  // the cap's half width, pixels
        const float cap_up = 24.0f + 86.0f * ease((t - 0.06f) / 0.8f);  // its middle, up off the ground
        const float stem_up = std::max(0.0f, cap_up - cap_r * 0.35f);
        const float skirt = 14.0f + 30.0f * ease((t - 0.04f) / 0.5f);
        // Cooling: the stem first, the cap after, the fire on the ground last.
        auto heat_of = [&](const Lump& l) {
            const float start = l.part == 0 ? 0.38f : l.part == 2 ? 0.5f + 0.2f * l.heat : l.part == 3 ? 0.72f : 0.4f;
            const float k = std::clamp((t - start) / 0.3f, 0.0f, 1.0f);
            return l.heat * (1.0f - k) - k * 0.5f;  // below 0: gone to smoke, blacker as it goes
        };
        for (const Lump& l : lumps) {
            float lx = 0.0f;
            float ly = 0.0f;
            float lr = 0.0f;
            switch (l.part) {
                case 0:
                    lx = o.x + l.x * cap_r;
                    ly = o.y - 4.0f + l.y * stem_up;
                    lr = l.r * cap_r;
                    break;
                case 1:
                case 2:
                    lx = o.x + l.x * cap_r;
                    ly = o.y - cap_up + l.y * cap_r;
                    lr = l.r * cap_r;
                    break;
                default:
                    lx = o.x + l.x * skirt;
                    ly = o.y - 3.0f + l.y * 10.0f;
                    lr = l.r * skirt * (t < 0.75f ? 1.0f : 1.0f - (t - 0.75f) * 2.0f);
                    break;
            }
            if (lr < 1.0f) continue;
            const float hh = heat_of(l);
            if (l.part == 0 && t > 0.8f && rnd(300 + static_cast<int>(l.y * 50.0f)) < (t - 0.8f) * 4.0f) continue;  // the stem breaking up
            for (int y = static_cast<int>(ly - lr) - 1; y <= static_cast<int>(ly + lr) + 1; ++y) {
                for (int x = static_cast<int>(lx - lr) - 1; x <= static_cast<int>(lx + lr) + 1; ++x) {
                    const float dx = static_cast<float>(x) + 0.5f - lx;
                    const float dy = static_cast<float>(y) + 0.5f - ly;
                    const float a = std::atan2(dy, dx);
                    const float rr = lr * (1.0f + 0.12f * std::sin(3.0f * a + l.wob) + 0.06f * std::sin(5.0f * a + 2.0f * l.wob));
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (d > rr) continue;
                    const float nx = dx / rr;
                    const float ny = dy / rr;
                    const float lit = 0.5f - 0.5f * ny - 0.2f * nx + (fx_noise(seed + static_cast<uint32_t>(f), x, y) - 0.5f) * 0.14f;  // lit on top, to the left
                    Color col;
                    if (l.part == 1 && hh > -0.2f) {  // the grey band
                        col = lit > 0.55f ? kGreyLit : kGrey;
                    } else if (hh > 0.0f) {
                        const float v = hh * (0.55f + 0.6f * lit);
                        col = v > 0.72f ? kPale : v > 0.52f ? kYellow : v > 0.34f ? kOrange : v > 0.18f ? kDeep : kRed;
                        if (hh < 0.2f && lit < 0.4f) col = kSmoke;  // cooling: smoke coming through it
                    } else {
                        col = lit > 0.6f ? kSmokeLit : hh > -0.3f ? kSmoke : kBlack;
                        if (hh > -0.15f && fx_noise(seed * 3u + static_cast<uint32_t>(f), x, y) < 0.06f) col = kOrange;  // embers in it
                    }
                    c.set(x, y, col);
                }
            }
        }
        if (t < 0.7f) {  // burning bits flung out of it
            for (int i = 0; i < 14; ++i) {
                const float a = rnd(400 + i) * 3.14159f + 3.14159f;
                const float d = cap_r * (1.1f + 0.5f * rnd(420 + i)) * (0.6f + t);
                c.set(static_cast<int>(o.x + std::cos(a) * d * 1.2f), static_cast<int>(o.y - cap_up * 0.6f + std::sin(a) * d * 0.8f), i % 2 == 0 ? kYellow : kDeep);
            }
        }
        c.outline(t < 0.6f ? Color{96, 34, 18, 255} : Color{30, 26, 24, 255});
    });
}

// Fire out of a muzzle, baked into sheets turned every way on the screen:
// a white-hot core at the muzzle, a tongue of flame bulging out ahead of
// it, yellow in its heart, orange, red at its ragged tip; out of a muzzle
// brake, jets of fire to the sides too. Four frames: blazing, swelling
// orange, breaking up red, wisps.
// (Its shapes: MuzzleFire, above.)
constexpr int kMuzzleTurns = 16;
constexpr int kMuzzleFrames = 4;
std::vector<FxSheet> g_muzzles;  // by shape, then turn

FxSheet bake_muzzle_fire(float len, float wide, bool brake, float angle, uint32_t seed) {
    const int sz = static_cast<int>((len + wide) * 2.0f) + 6;
    const Vector2 o{static_cast<float>(sz / 2), static_cast<float>(sz / 2)};
    const Vector2 d{std::cos(angle), std::sin(angle)};
    const Vector2 n{-d.y, d.x};
    return bake_fx(sz, sz, kMuzzleFrames, o, [&](Canvas& c, int f) {
        const float grow = 1.0f + 0.18f * static_cast<float>(f);
        const float cool = static_cast<float>(f) / static_cast<float>(kMuzzleFrames - 1);
        // How hot a point is (0 outside the fire), by where it is along the tongue and off its middle.
        auto tongue = [&](float u, float v, float l, float w) {
            if (u < -w * 0.3f || u > l) return 0.0f;
            const float k = std::max(0.0f, u) / l;
            const float half = w * 0.5f * std::sin(3.14159f * std::pow(std::max(0.02f, k), 0.6f)) * (1.0f + 0.25f * (fx_noise(seed + static_cast<uint32_t>(f), static_cast<int>(u), 7) - 0.5f));
            const float tip = k > 0.7f ? 0.35f * std::sin(u * 1.7f + static_cast<float>(f) * 2.0f) : 0.0f;  // ragged at its tip
            const float off = std::fabs(v) / std::max(0.5f, half);
            if (off > 1.0f + tip) return 0.0f;
            return (1.0f - off * 0.7f) * (1.0f - 0.55f * k);
        };
        for (int y = 0; y < sz; ++y) {
            for (int x = 0; x < sz; ++x) {
                const float px = static_cast<float>(x) + 0.5f - o.x;
                const float py = static_cast<float>(y) + 0.5f - o.y;
                const float u = px * d.x + py * d.y;
                const float v = px * n.x + py * n.y;
                float heat = tongue(u, v, len * grow, wide * grow);
                if (brake) {  // the side jets, swept a little back
                    for (const float side : {-1.0f, 1.0f}) {
                        const float su = v * side * 0.94f - u * 0.34f;
                        const float sv = u * 0.94f + v * side * 0.34f - len * 0.08f;
                        heat = std::max(heat, 0.85f * tongue(su, sv, len * 0.42f * grow, wide * 0.55f * grow));
                    }
                }
                const float core = 1.0f - std::sqrt(px * px + py * py) / (wide * 0.45f);
                if (f == 0 && core > 0.0f) heat = std::max(heat, 0.9f + core * 0.2f);
                if (heat <= 0.0f) continue;
                heat = heat * (1.0f - 0.55f * cool) + (fx_noise(seed * 3u + static_cast<uint32_t>(f), x, y) - 0.5f) * 0.18f;
                if (f >= 2 && fx_noise(seed * 5u + static_cast<uint32_t>(f), x, y) < 0.18f * static_cast<float>(f - 1)) continue;  // breaking up
                if (heat <= 0.08f) continue;
                c.set(x, y, heat > 0.72f ? kHeat[0] : heat > 0.5f ? kHeat[1] : heat > 0.32f ? kHeat[2] : heat > 0.18f ? kHeat[3] : kHeat[4]);
            }
        }
        if (f < 3) c.outline({110, 34, 18, 255});
    });
}

// The crops in pixel art (see plants.h): a sheet each kind.
std::vector<FxSheet> g_plants;
void ensure_plants() {
    if (!g_plants.empty()) return;
    for (int p = 0; p < static_cast<int>(plants::Plant::Count); ++p) {
        const plants::Layout l = plants::layout(static_cast<plants::Plant>(p));
        Image img = plants::bake(static_cast<plants::Plant>(p));
        if (const char* dump = dump_dir()) ExportImage(img, TextFormat("%s/plants_%d.png", dump, p));
        FxSheet sheet;
        sheet.tex = LoadTextureFromImage(img);
        SetTextureFilter(sheet.tex, TEXTURE_FILTER_POINT);
        sheet.w = l.w;
        sheet.h = l.h;
        sheet.frames = l.frames;
        sheet.origin = {l.origin_x, l.origin_y};
        UnloadImage(img);
        g_plants.push_back(sheet);
    }
}
const FxSheet& plant_sheet(plants::Plant p) { return g_plants[static_cast<size_t>(p)]; }
// The crops flattened where vehicles went (see WorldRenderer::crushed_), as the ground is drawn.
const std::vector<uint64_t>* g_crushed = nullptr;
int g_crushed_width = 0;
bool crushed_at(Vector2 g) {
    if (!g_crushed || g_crushed->empty() || g.x < 0.0f || g.y < 0.0f) return false;
    const int tx = static_cast<int>(g.x);
    const int ty = static_cast<int>(g.y);
    const size_t i = static_cast<size_t>(ty * g_crushed_width + tx);
    if (tx >= g_crushed_width || i >= g_crushed->size()) return false;
    const int sx = std::clamp(static_cast<int>((g.x - static_cast<float>(tx)) * 8.0f), 0, 7);
    const int sy = std::clamp(static_cast<int>((g.y - static_cast<float>(ty)) * 8.0f), 0, 7);
    return ((*g_crushed)[i] >> (sy * 8 + sx) & 1u) != 0;
}

void ensure_muzzles() {
    if (!g_muzzles.empty()) return;
    static constexpr float kShapes[kMuzzleShapes][3] = {{10.0f, 4.0f, 0.0f}, {13.0f, 5.0f, 0.0f}, {14.0f, 7.0f, 1.0f}, {34.0f, 17.0f, 0.0f}, {28.0f, 15.0f, 1.0f}};
    for (int k = 0; k < kMuzzleShapes; ++k) {
        for (int t = 0; t < kMuzzleTurns; ++t) {
            g_muzzles.push_back(bake_muzzle_fire(kShapes[k][0], kShapes[k][1], kShapes[k][2] > 0.0f, static_cast<float>(t) * 6.2831853f / kMuzzleTurns,
                                                 0x3C1u + static_cast<uint32_t>(k * 31 + t)));
        }
    }
}

// Every sheet, once there's a window to make textures in.
void ensure_fx() {
    if (!g_fx.empty()) return;
    g_fx.resize(static_cast<size_t>(Fx::Count) * kFxVariants);
    for (int v = 0; v < kFxVariants; ++v) {
        const auto seed = static_cast<uint32_t>(v * 7919 + 17);
        auto put = [&](Fx kind, FxSheet sheet) { g_fx[static_cast<size_t>(kind) * kFxVariants + static_cast<size_t>(v)] = sheet; };
        put(Fx::Blast0, bake_blast(7.0f, seed));
        put(Fx::Blast1, bake_blast(12.0f, seed + 1u));
        put(Fx::Blast2, bake_blast(18.0f, seed + 2u));
        put(Fx::Blast3, bake_blast(26.0f, seed + 3u));
        put(Fx::Flame0, bake_flame(5.0f, 9.0f, seed + 4u));
        put(Fx::Flame1, bake_flame(8.0f, 14.0f, seed + 5u));
        put(Fx::Flame2, bake_flame(12.0f, 21.0f, seed + 6u));
        put(Fx::Star0, bake_star(4.0f, seed + 7u));
        put(Fx::Star1, bake_star(8.0f, seed + 8u));
        put(Fx::Star2, bake_star(13.0f, seed + 9u));
        put(Fx::Embers, bake_embers(seed + 10u));
        put(Fx::Scorch0, bake_scorch(34.0f, seed + 11u));
        put(Fx::Scorch1, bake_scorch(48.0f, seed + 12u));
        put(Fx::Scorch2, bake_scorch(68.0f, seed + 13u));
        for (int k = 0; k < 6; ++k) put(static_cast<Fx>(static_cast<int>(Fx::Smoke0) + k), bake_smoke(kSmokeTiers[k], seed + 20u + static_cast<uint32_t>(k)));
        put(Fx::Roof0, bake_roof_fire(12.0f, seed + 30u));
        put(Fx::Mushroom, bake_mushroom(seed + 40u));
        if (v < 2) {
            for (int k = 0; k < 3; ++k) {
                for (int t = 0; t < kStreakTurns; ++t) {
                    g_streaks.push_back(bake_streak(kStreakSizes[k][0], kStreakSizes[k][1], static_cast<float>(t) * 3.14159265f / kStreakTurns,
                                                    seed + 50u + static_cast<uint32_t>(k * 16 + t)));
                }
            }
        }
        put(Fx::Roof1, bake_roof_fire(20.0f, seed + 31u));
    }
}

// A frame of a sheet, its foot (or its middle) at `anchor`; fire lights itself
// (the ground's patches dimmed with the rest: `tint`).
void draw_fx(const FxSheet& s, int frame, Vector2 anchor, Color tint = WHITE) {
    if (s.frames == 0) return;
    frame = std::clamp(frame, 0, s.frames - 1);
    DrawTexturePro(s.tex, {static_cast<float>(frame * s.w), 0.0f, static_cast<float>(s.w), static_cast<float>(s.h)},
                   {std::round(anchor.x - s.origin.x), std::round(anchor.y - s.origin.y), static_cast<float>(s.w), static_cast<float>(s.h)},
                   {0.0f, 0.0f}, 0.0f, tint);
}

// Flames at their spots on a vehicle standing at `ground` facing `f`: the
// far ones first; each flickering its own way.
void draw_flames(const engine::TileMap& map, Vector2 ground, Vector2 f, std::vector<FlameSpot> spots) {
    const Vector2 left{-f.y, f.x};
    auto at = [&](const FlameSpot& sp) { return Vector2{ground.x + f.x * sp.along + left.x * sp.across, ground.y + f.y * sp.along + left.y * sp.across}; };
    std::sort(spots.begin(), spots.end(), [&](const FlameSpot& a, const FlameSpot& b) { return at(a).x + at(a).y < at(b).x + at(b).y; });
    const auto now = static_cast<float>(GetTime());
    for (const FlameSpot& sp : spots) {
        const Vector2 p = on_terrain(map, at(sp), sp.z);
        const FxSheet& s = fx_sheet(static_cast<Fx>(static_cast<int>(Fx::Flame0) + std::clamp(sp.size, 0, 2)), static_cast<int>(sp.seed % 3u));
        draw_fx(s, static_cast<int>(now * 11.0f + static_cast<float>(sp.seed % 97u) * 0.37f) % s.frames, {p.x, p.y + 1.0f});
    }
}

// The ground burnt round a wreck: `size` 0..2; embers glowing while it's burning.
void draw_scorch(Vector2 at, int size, bool hot, float alpha, uint32_t seed) {
    const FxSheet& s = fx_sheet(static_cast<Fx>(static_cast<int>(Fx::Scorch0) + std::clamp(size, 0, 2)), static_cast<int>(seed % 3u));
    const int frame = hot ? static_cast<int>(GetTime() * 5.0 + static_cast<double>(seed % 7u)) % 4 : 4;
    draw_fx(s, frame, at, ColorAlpha(lit(WHITE), alpha));
}

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
    uint8_t shred = 0;    // cut up by shelling, 0 whole .. TileMap::kMaxShred
};

// How much foliage a tree has left, cut up by shelling (1: all of it), and
// the bark its trunk and branches show. Set around each tree drawn.
float g_leaf = 1.0f;
float g_bare = 0.0f;
Color g_bark{72, 54, 38, 255};

// Bare branches in a crown's oval, from the trunk's top up and out: when the
// leaves are going, then gone, broken off short the worse it's been hit, a
// twig or two on each; splintered pale where they snapped.
void branch(Vector2 p, float a, float len, float width, int depth, uint32_t seed) {
    const Vector2 end{p.x + std::cos(a) * len, p.y + std::sin(a) * len * 0.9f};
    // The thin branches darker than the bark of the trunk (a birch's twigs are brown, not white).
    const Color wood = width > 1.8f ? g_bark : mix(g_bark, {64, 52, 42, 255}, 0.65f);
    DrawLineEx(p, end, width, lit(shade(wood, 0.9f)));
    if (width > 1.6f) DrawLineV({p.x - 0.5f, p.y}, {end.x - 0.5f, end.y}, lit(shade(wood, 1.3f)));  // lit on the left
    const float r = hash_unit(tile_hash(static_cast<int>(seed & 0xFFFF), depth * 31 + static_cast<int>(a * 10.0f)));
    if (depth == 0) return;
    if (depth < 2 && r < g_bare * 0.45f) {  // snapped off here: the pale wood
        disc(end, std::max(0.8f, width * 0.6f), {206, 190, 150, 255});
        return;
    }
    const float spread = 0.35f + 0.25f * r;
    branch(end, a - spread, len * 0.62f, std::max(1.0f, width * 0.66f), depth - 1, seed * 2654435761u + 1u);
    branch(end, a + spread, len * 0.58f, std::max(1.0f, width * 0.62f), depth - 1, seed * 2246822519u + 7u);
}

// A crown's bare branches, when its leaves are going or gone: three or four
// limbs up and out of the trunk's top, forking twice into twigs.
void draw_bare_crown(Vector2 c, float w, float h, uint32_t seed) {
    const Vector2 foot{c.x, c.y + h * 0.45f};
    const int limbs = 3 + static_cast<int>(seed % 2);
    for (int i = 0; i < limbs; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(limbs - 1);
        const float a = -2.45f + 1.75f * t + (hash_unit(tile_hash(static_cast<int>(seed >> 3) + i, 11)) - 0.5f) * 0.3f;  // up and out
        const float len = std::max(w, h) * (0.4f + 0.15f * hash_unit(tile_hash(static_cast<int>(seed >> 5) + i, 13)));
        branch(foot, a, len, 2.4f, std::max(w, h) > 22.0f ? 3 : 2, seed + static_cast<uint32_t>(i) * 977u);
    }
}

void draw_limbs(Vector2 c, float w, float h, uint32_t seed) {
    if (g_bare <= 0.1f) return;
    if (g_bare >= 0.6f) {
        draw_bare_crown(c, w, h, seed);
        return;
    }
    auto rnd = [seed](int i, int salt) { return hash_unit(tile_hash(static_cast<int>(seed >> 2) + i * 11 + salt * 97, i * 5 + salt)); };
    const Vector2 foot{c.x, c.y + h * 0.45f};
    const int limbs = 5;
    for (int i = 0; i < limbs; ++i) {
        const float a = -2.6f + 2.2f * static_cast<float>(i) / static_cast<float>(limbs - 1) + (rnd(i, 1) - 0.5f) * 0.4f;  // up and out
        const float full = 0.55f + 0.4f * rnd(i, 2);
        const float len = full * (1.0f - 0.55f * g_bare * rnd(i, 3));  // broken off short
        const Vector2 end{foot.x + std::cos(a) * w * 0.5f * len, foot.y + std::sin(a) * h * 0.75f * len};
        const float thick = 1.6f - 0.15f * static_cast<float>(i % 3);
        DrawLineEx(foot, end, thick, lit(shade(g_bark, 0.85f)));
        if (len < full * 0.8f) disc(end, thick * 0.6f, {206, 190, 150, 255});  // snapped: the pale wood
        const Vector2 mid{(foot.x + end.x) * 0.5f, (foot.y + end.y) * 0.5f};
        const float t = a + (rnd(i, 4) < 0.5f ? 0.7f : -0.7f);
        DrawLineV(mid, {mid.x + std::cos(t) * w * 0.14f, mid.y + std::sin(t) * h * 0.18f}, lit(shade(g_bark, 0.75f)));
    }
}

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
    // The roots spreading out at its foot, either side.
    for (const float side : {-1.0f, 1.0f}) {
        const Vector2 from{base.x + side * w[0] * 0.3f, base.y - w[0] * 0.3f};
        DrawLineEx(from, {base.x + side * std::min(w[0] * 0.8f, 5.0f + w[0] * 0.3f), base.y + 0.8f}, std::max(1.0f, std::min(w[0] * 0.24f, 2.2f)),
                   lit(side < 0 ? light : dark));
    }
    for (int i = 0; i < kSteps; ++i) {
        const Vector2 a{p[i].x - w[i] * 0.5f, p[i].y};
        const Vector2 d{p[i + 1].x - w[i + 1] * 0.5f, p[i + 1].y};
        const Vector2 b{p[i].x + w[i] * 0.5f, p[i].y};
        const Vector2 c{p[i + 1].x + w[i + 1] * 0.5f, p[i + 1].y};
        // Twisted: the light side sweeping round the trunk as it goes up.
        const float twist = 0.18f * std::sin(static_cast<float>(i) * 1.3f + bend);
        const Vector2 m0{p[i].x + w[i] * twist, p[i].y};
        const Vector2 m1{p[i + 1].x + w[i + 1] * twist, p[i + 1].y};
        fill_quad(a, m0, m1, d, light);
        fill_quad(m0, b, c, m1, dark);
        if (w[i] >= 2.5f) {  // the bark in streaks: a dark one in the shade, a pale one on the lit side
            DrawLineV({m0.x + w[i] * 0.2f, m0.y}, {m1.x + w[i + 1] * 0.2f, m1.y}, lit(shade(bark, 0.5f)));
            DrawLineV({a.x + w[i] * 0.18f, a.y}, {d.x + w[i + 1] * 0.18f, d.y}, lit(shade(bark, 1.4f)));
        }
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
    draw_limbs(c, w, h, seed);
    if (g_leaf <= 0.02f) return;
    if (g_leaf > 0.55f) {
        DrawEllipse(static_cast<int>(c.x + w * 0.05f), static_cast<int>(c.y + h * 0.1f), w * 0.4f * g_leaf, h * 0.38f * g_leaf,
                    lit(shade(leaf, 0.6f * tint)));
    }
    const int tufts = far ? 8 : 16;
    for (int i = 0; i < tufts; ++i) {
        if (rnd(i, 21) >= g_leaf) continue;  // stripped
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
    constexpr int kMaxClumps = 32;
    std::array<Clump, kMaxClumps> clumps{};
    int count = 0;
    const int tiers = std::clamp(static_cast<int>(h / (lump * 0.9f)), 2, 5);
    for (int j = 0; j < tiers && count < kMaxClumps; ++j) {
        const float f = static_cast<float>(j) / static_cast<float>(tiers - 1);  // 0 the top .. 1 the bottom
        const float y = c.y - h * 0.38f + h * 0.72f * f;
        const float across = w * (0.45f + 0.55f * std::sin(0.3f + f * 2.4f));  // widest a little below the middle
        const int n = std::max(1, static_cast<int>(across / (lump * 0.95f)));
        for (int i = 0; i < n && count < kMaxClumps; ++i) {
            const int id = j * 8 + i;
            const float u = n == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(n - 1) - 0.5f;
            const Vector2 at{c.x + u * across * 0.9f + (rnd(id, 1) - 0.5f) * lump * 0.5f, y + (rnd(id, 2) - 0.5f) * lump * 0.4f};
            const float k = 1.12f - 0.42f * f - 0.22f * u + 0.1f * (rnd(id, 3) - 0.5f);
            clumps[static_cast<size_t>(count++)] = {at, lump * (0.42f + 0.16f * rnd(id, 4)), k};
        }
    }
    if (far) count = std::max(1, count / 2);
    // Back to front: the bottom tier first, the top one over it.
    std::sort(clumps.begin(), clumps.begin() + count, [](const Clump& a, const Clump& b) { return a.at.y > b.at.y; });
    for (int i = 0; i < count; ++i) {
        const Clump& l = clumps[static_cast<size_t>(i)];
        if (hash_unit(tile_hash(static_cast<int>(seed >> 4) + i * 29, i * 3 + 7)) >= g_leaf) continue;  // stripped
        // The leaves hanging down from it in points, dark in its shade.
        if (!far) {
            for (int k = -2; k <= 2; ++k) {
                const float x = l.at.x + static_cast<float>(k) * l.r * 0.36f;
                const float y = l.at.y + l.r * (0.62f - 0.06f * std::fabs(static_cast<float>(k)));
                const float drop = l.r * (0.5f + 0.25f * static_cast<float>((k + 2) % 2));
                fill_triangle({x - l.r * 0.2f, y}, {x + l.r * 0.2f, y}, {x + l.r * 0.04f, y + drop}, shade(leaf, l.k * 0.55f * tint));
            }
        }
        disc({l.at.x + l.r * 0.2f, l.at.y + l.r * 0.25f}, l.r, shade(leaf, l.k * 0.72f * tint));
        disc({l.at.x - l.r * 0.05f, l.at.y - l.r * 0.05f}, l.r * 0.8f, shade(leaf, l.k * tint));
        if (!far) {  // its top catching the light, in short strokes of leaves
            disc({l.at.x - l.r * 0.3f, l.at.y - l.r * 0.36f}, l.r * 0.42f, shade(leaf, l.k * 1.35f * tint));
            DrawLineV({l.at.x - l.r * 0.55f, l.at.y - l.r * 0.1f}, {l.at.x - l.r * 0.15f, l.at.y - l.r * 0.5f},
                      lit(mix(shade(leaf, l.k * 1.6f * tint), {214, 214, 110, 255}, 0.35f)));
        }
    }
}

// A tree standing at `b` on screen; `shadowed`: its shadow on the ground too
// (not when baking it into a sprite).
void draw_tree_at(Vector2 b, const Tree& t, bool shadowed) {
    const float s = t.size;
    if (t.stump) {
        DrawEllipse(static_cast<int>(b.x), static_cast<int>(b.y), 2.6f * s * t.girth, 1.3f * s, lit({70, 52, 36, 255}));
        DrawEllipse(static_cast<int>(b.x), static_cast<int>(b.y - 1.5f), 2.2f * s * t.girth, 1.1f * s, lit({176, 150, 110, 255}));
        return;
    }
    const float spread = t.kind == TreeKind::Oak ? 1.5f : t.kind == TreeKind::Poplar ? 0.6f : t.kind == TreeKind::Apple ? 0.8f : 1.0f;
    // Cut up by shelling: the leaves going, the branches broken, at worst the trunk snapped.
    g_bare = std::clamp(static_cast<float>(t.shred) / 6.0f, 0.0f, 1.0f);
    g_leaf = 1.0f - g_bare;
    switch (t.kind) {
        case TreeKind::Oak: g_bark = {78, 62, 46, 255}; break;
        case TreeKind::Beech: g_bark = {146, 142, 134, 255}; break;
        case TreeKind::Birch: g_bark = {222, 220, 208, 255}; break;
        case TreeKind::Pine: g_bark = {146, 92, 62, 255}; break;
        case TreeKind::Apple: g_bark = {92, 70, 50, 255}; break;
        default: g_bark = {72, 54, 38, 255}; break;
    }
    if (t.shred >= 3) {  // branches fallen round it
        for (int i = 0; i < 1 + t.shred / 3; ++i) {
            const uint32_t hi = tile_hash(static_cast<int>(t.seed >> 3) + i * 19, i * 7);
            const float a = hash_unit(hi) * 6.2831853f;
            const Vector2 p{b.x + std::cos(a) * 7.0f * s, b.y + std::sin(a) * 3.0f * s};
            const float r = hash_unit(hi >> 8) * 3.14f;
            DrawLineEx({p.x - std::cos(r) * 4.0f * s, p.y - std::sin(r) * 1.5f * s}, {p.x + std::cos(r) * 4.0f * s, p.y + std::sin(r) * 1.5f * s},
                       1.3f, lit(shade(g_bark, 0.7f)));
        }
    }
    if (t.shred >= 7) {
        // Snapped off: a tall pole of a trunk, splintered at the break, stubs of
        // its limbs sticking out either side.
        if (shadowed) DrawEllipse(static_cast<int>(b.x + 3.0f * s), static_cast<int>(b.y + 1.0f * s), 4.0f * s, 1.8f * s, lit({16, 24, 12, 60}));
        const uint32_t hs = tile_hash(static_cast<int>(t.seed >> 6), 41);
        const float height = (18.0f + 14.0f * hash_unit(hs)) * s * t.height;
        const Vector2 top = draw_trunk(b, height, 3.6f * s * t.girth, 1.6f * s * t.girth, t.bend * s * 0.4f, g_bark);
        const float lean = (hash_unit(hs >> 8) - 0.5f) * 3.0f * s;
        fill_triangle({top.x - 1.0f * s * t.girth, top.y + 1.0f}, {top.x + 1.0f * s * t.girth, top.y + 1.0f}, {top.x + lean, top.y - 3.5f * s},
                      {214, 198, 158, 255});
        for (int k = 0; k < 4; ++k) {
            const float f = 0.35f + 0.15f * static_cast<float>(k);
            const float side = k % 2 == 0 ? -1.0f : 1.0f;
            const Vector2 at{b.x + t.bend * s * 0.4f * 4.0f * f * (1.0f - f), b.y - height * f};
            const float len = (2.5f + 2.5f * hash_unit(hs >> (k + 3))) * s;
            DrawLineEx(at, {at.x + side * len, at.y - len * 0.7f}, 1.2f, lit(shade(g_bark, 0.85f)));
        }
        g_leaf = 1.0f;
        g_bare = 0.0f;
        return;
    }
    // Its shadow on the ground, away from the light (upper left): thinner as the crown thins.
    if (shadowed) DrawEllipse(static_cast<int>(b.x + 5.0f * s), static_cast<int>(b.y + 1.5f * s), 9.0f * s * spread * (0.4f + 0.6f * g_leaf), 3.4f * s,
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
            draw_crown({top.x, top.y - 10.0f * s}, 15.0f * s, 23.0f * s, 5.5f * s, {70, 112, 50, 255}, t.tint, t.seed);
            break;
        }
        case TreeKind::Broadleaf: {
            // Linden, maple: a brown trunk, a round crown.
            const Vector2 top = draw_trunk(b, 9.5f * s * t.height, 4.0f * s * t.girth, 2.4f * s, bend, {72, 54, 38, 255});
            draw_crown({top.x, top.y - 9.0f * s}, 19.0f * s, 21.0f * s, 6.0f * s, {50, 96, 44, 255}, t.tint, t.seed);
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
            draw_crown({top.x + 0.5f * s, top.y - 7.0f * s}, 14.0f * s, 18.0f * s, 4.6f * s, {98, 138, 60, 255}, t.tint, t.seed);
            break;
        }
        case TreeKind::Pine: {
            // Old pines: a tall bare reddish trunk, the needles up top; young ones in tiers near the ground.
            const bool old = t.height > 1.0f;  // (sprites: > 0.98 old)
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
                if (g_leaf > 0.6f) fill_triangle({x - wid, y0 + 1.0f * s}, {x + wid, y0 + 1.0f * s}, {x, apex_y}, shade(needles, 0.5f));
                if (g_bare > 0.1f) {  // the boughs showing, broken
                    for (const float u : {-1.0f, 1.0f}) {
                        const float len = wid * (1.0f - 0.5f * g_bare * hash_unit(tile_hash(static_cast<int>(t.seed) + k, static_cast<int>(u * 3.0f))));
                        DrawLineEx({x, y0 - 2.0f * s}, {x + u * len, y0 + 0.5f * s}, 1.1f, lit(shade(g_bark, 0.6f)));
                    }
                    DrawLineEx({x, y0}, {x, apex_y}, 1.3f, lit(shade(g_bark, 0.7f)));
                }
                constexpr int kRows = 4;
                for (int row = 0; row < kRows; ++row) {
                    const float f = static_cast<float>(row + 1) / kRows;
                    const float y = apex_y + (y0 - apex_y) * f;
                    const int n = row + 2;
                    for (int i = 0; i < n; ++i) {
                        const float u = static_cast<float>(i) / static_cast<float>(n - 1) * 2.0f - 1.0f;  // -1 left .. 1 right
                        if (hash_unit(tile_hash(static_cast<int>(t.seed >> 7) + k * 13 + row * 3 + i, i * 5)) >= g_leaf) continue;  // stripped
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
            // An orchard's apple tree: a short trunk whitewashed against the
            // hares, parting low into three or four boughs spreading up and out;
            // a broad crown, wider than it's tall, over them, the boughs showing
            // in the gaps under it; the apples, red, dark red, some yellow,
            // hanging round its lower half, heaviest at its edges; a prop under
            // a laden bough on some; windfalls under it.
            auto rnd = [&](int i) { return hash_unit(tile_hash(static_cast<int>(t.seed >> 3) + i * 29, i * 11 + 5)); };
            const float trunk = 5.5f * s * t.height;
            const Vector2 top = draw_trunk(b, trunk, 3.2f * s * t.girth, 2.4f * s, bend * 0.25f, {96, 74, 54, 255});
            // The whitewash, lit on the left, greyer in the shade.
            const float ww = 3.2f * s * t.girth;
            const float wash = trunk * 0.6f;
            fill_quad({b.x - ww * 0.5f, b.y}, {b.x, b.y}, {b.x + bend * 0.05f, b.y - wash}, {b.x - ww * 0.42f + bend * 0.05f, b.y - wash},
                      {238, 238, 230, 255});
            fill_quad({b.x, b.y}, {b.x + ww * 0.5f, b.y}, {b.x + ww * 0.42f + bend * 0.05f, b.y - wash}, {b.x + bend * 0.05f, b.y - wash},
                      {196, 200, 200, 255});
            const float w = 22.0f * s;
            const float h = 13.0f * s * (0.9f + 0.2f * t.height);
            const Vector2 c{top.x, top.y - h * 0.5f - 1.5f * s};
            // The boughs, up and out from the fork, their twigs.
            const int boughs = 3 + static_cast<int>(rnd(1) * 1.99f);
            for (int i = 0; i < boughs; ++i) {
                const float a = -2.5f + 1.86f * static_cast<float>(i) / static_cast<float>(boughs - 1) + (rnd(10 + i) - 0.5f) * 0.3f;
                const Vector2 end{top.x + std::cos(a) * w * 0.42f, top.y + std::sin(a) * h * 0.72f};
                DrawLineEx(top, end, 1.8f * s, lit(shade(g_bark, 0.9f)));
                DrawLineV({top.x - 0.5f, top.y}, {end.x - 0.5f, end.y}, lit(shade(g_bark, 1.2f)));
            }
            if (rnd(2) < 0.3f && g_leaf > 0.5f) {  // a prop under a laden bough
                const Vector2 under{top.x + w * 0.34f, top.y - h * 0.18f};
                DrawLineEx({under.x + 3.0f * s, b.y + 1.0f}, under, 1.0f, lit({150, 124, 86, 255}));
            }
            draw_crown(c, w, h, 4.4f * s, {72, 116, 54, 255}, t.tint, t.seed);
            // The apples.
            if (g_leaf > 0.05f) {
                const int apples = static_cast<int>(16.0f * g_leaf);
                for (int k = 0; k < apples; ++k) {
                    // Anywhere over the crown, more of them low down and out at its edges.
                    const float a = rnd(20 + k) * 6.2831853f;
                    const float r = std::sqrt(0.15f + 0.85f * rnd(40 + k)) * 0.9f;
                    const float low = std::sin(a) < 0.0f && rnd(60 + k) < 0.5f ? -std::sin(a) : std::sin(a);  // (half of the high ones moved low)
                    const Vector2 ap{c.x + std::cos(a) * r * w * 0.46f, c.y + low * r * h * 0.46f};
                    const float roll = rnd(80 + k);
                    const Color apple = roll < 0.55f ? Color{202, 38, 34, 255} : roll < 0.8f ? Color{150, 28, 32, 255} : Color{222, 196, 70, 255};
                    disc(ap, 1.25f * s, shade(apple, 0.8f));
                    disc({ap.x - 0.3f, ap.y - 0.3f}, 0.85f * s, apple);
                    DrawPixelV({ap.x - 0.7f, ap.y - 0.8f}, lit(shade(apple, 1.45f)));
                }
            }
            for (int k = 0; k < 2; ++k) {  // windfalls
                if (rnd(100 + k) > 0.6f) continue;
                const Vector2 fall{b.x + (rnd(110 + k) - 0.5f) * w * 0.8f, b.y + (rnd(120 + k) - 0.3f) * 3.0f};
                disc(fall, 0.9f * s, k == 0 ? Color{176, 40, 36, 255} : Color{200, 176, 64, 255});
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
            if (g_leaf > 0.6f) DrawEllipse(static_cast<int>(top.x + 0.6f * s), static_cast<int>(mid), 5.0f * s, half, lit(shade(leaf, 0.5f)));
            constexpr int kClumps = 11;
            if (g_bare > 0.1f) DrawLineEx(top, {top.x, mid - half * (1.0f - 0.5f * g_bare)}, 1.4f, lit(shade(g_bark, 0.8f)));  // the bare stem
            for (int i = 0; i < kClumps; ++i) {
                if (hash_unit(tile_hash(static_cast<int>(t.seed >> 6) + i * 23, i)) >= g_leaf) continue;  // stripped
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

void draw_tree(const engine::TileMap& map, const Tree& t) { draw_tree_at(on_terrain(map, t.ground), t, true); }

// A tree on fire: its crown burning at a few spots (the snapped one's trunk),
// a flame at its foot; `less` a size smaller as the fire dies down.
void tree_flames(Vector2 base, const Tree& t, uint8_t shred, int less, uint32_t seed) {
    const float s = t.size;
    const float hh = t.height;
    float crown = 0.0f;  // the crown's middle, up from the foot
    float half = 8.0f * s;
    float wide = 8.0f * s;
    switch (t.kind) {
        case TreeKind::Oak: crown = 10.0f * s * hh + 11.0f * s; wide = 11.0f * s; break;
        case TreeKind::Beech: crown = 14.0f * s * hh + 10.0f * s; wide = 7.0f * s; half = 10.0f * s; break;
        case TreeKind::Birch: crown = 12.0f * s * hh + 7.0f * s; wide = 6.5f * s; break;
        case TreeKind::Pine: crown = (hh > 1.0f ? 11.0f : 4.0f) * s * hh + 9.0f * s; wide = 5.0f * s; break;
        case TreeKind::Poplar: crown = 5.0f * s + 13.0f * s * hh; wide = 4.0f * s; half = 12.0f * s * hh; break;
        case TreeKind::Apple: crown = 5.5f * s * hh + 7.5f * s; wide = 9.0f * s; half = 6.0f * s; break;
        default: crown = 9.5f * s * hh + 9.0f * s; wide = 9.0f * s; break;
    }
    struct Spot {
        float dx, up;
        int size;
    };
    std::vector<Spot> spots;
    if (t.stump) {
        spots.push_back({0.0f, 1.0f, 0});
    } else if (shred >= 7) {  // snapped: the trunk burning
        spots.push_back({0.0f, crown * 0.45f, 1});
        spots.push_back({0.5f * s, 1.0f, 0});
    } else {
        spots.push_back({-0.5f * wide, crown - 0.2f * half, 2});
        spots.push_back({0.45f * wide, crown + 0.2f * half, 1});
        spots.push_back({0.05f * wide, crown + 0.55f * half, 1});
        if (t.kind == TreeKind::Poplar) spots.push_back({0.3f * wide, crown - 0.6f * half, 1});
        spots.push_back({0.6f * s, 1.0f, 0});
    }
    const auto now = static_cast<float>(GetTime());
    for (size_t i = 0; i < spots.size(); ++i) {
        const Spot& sp = spots[i];
        const int size = sp.size - less;
        if (size < 0) continue;
        const uint32_t k = seed + static_cast<uint32_t>(i) * 7919u;
        const FxSheet& sheet = fx_sheet(static_cast<Fx>(static_cast<int>(Fx::Flame0) + size), static_cast<int>(k % 3u));
        draw_fx(sheet, static_cast<int>(now * 11.0f + static_cast<float>(k % 97u) * 0.37f) % sheet.frames, {std::round(base.x + sp.dx), std::round(base.y - sp.up)});
    }
}

// Trees in pixel art, baked like the tanks: for each kind and each stage of
// being cut up (whole, thinned, half bare, bare, snapped off), a row of
// variants of its own height, girth, bow and crown.
constexpr int kTreeKinds = 7;
constexpr int kTreeStages = 5;
constexpr int kTreeVariants = 6;
constexpr int kTreeW = 96;
constexpr int kTreeH = 128;
constexpr Vector2 kTreeOrigin{48.0f, 120.0f};

int tree_stage(uint8_t shred) { return shred == 0 ? 0 : shred <= 2 ? 1 : shred <= 4 ? 2 : shred <= 6 ? 3 : 4; }
uint8_t stage_shred(int stage) { return static_cast<uint8_t>(stage == 0 ? 0 : stage == 1 ? 2 : stage == 2 ? 4 : stage == 3 ? 6 : 8); }

// A kind's variant `v`: bigger the higher `v` (a lone old oak takes the biggest).
Tree tree_variant(TreeKind kind, int v, int stage) {
    const uint32_t h = tile_hash(static_cast<int>(kind) * 101 + v * 7, v * 13 + 5);
    Tree t{};
    t.kind = kind;
    const float grow = static_cast<float>(v) / static_cast<float>(kTreeVariants - 1);
    t.size = (kind == TreeKind::Oak ? 0.95f + 0.6f * grow : 0.8f + 0.4f * grow) * 1.3f;  // over life size, as the vehicles
    t.size += (hash_unit(h) - 0.5f) * 0.06f;
    t.tint = 0.9f + 0.2f * hash_unit(h >> 4);
    t.height = (0.8f + 0.45f * hash_unit(h >> 8)) * 1.25f;
    if (kind == TreeKind::Pine) t.height = v >= 3 ? std::max(t.height, 1.3f) : std::min(t.height, 0.98f);  // old pines and young ones
    t.girth = 0.85f + 0.35f * hash_unit(h >> 12);
    t.bend = (hash_unit(h >> 16) - 0.5f) * (kind == TreeKind::Birch ? 7.0f : kind == TreeKind::Poplar ? 1.0f : 4.0f);
    t.seed = h;
    t.shred = stage_shred(stage);
    return t;
}

// Its colours: the leaves' and the bark's ramps, and what else it shows.
std::vector<Color> tree_palette(TreeKind kind) {
    Color leaf{50, 96, 44, 255};
    Color bark{72, 54, 38, 255};
    switch (kind) {
        case TreeKind::Oak: leaf = {54, 92, 44, 255}; bark = {78, 62, 46, 255}; break;
        case TreeKind::Beech: leaf = {70, 112, 50, 255}; bark = {146, 142, 134, 255}; break;
        case TreeKind::Birch: leaf = {98, 138, 60, 255}; bark = {222, 220, 208, 255}; break;
        case TreeKind::Pine: leaf = {38, 74, 52, 255}; bark = {146, 92, 62, 255}; break;
        case TreeKind::Apple: leaf = {72, 114, 50, 255}; bark = {92, 70, 50, 255}; break;
        case TreeKind::Poplar: leaf = {58, 100, 48, 255}; bark = {72, 56, 40, 255}; break;
        default: break;
    }
    std::vector<Color> p;
    for (const float k : {0.34f, 0.48f, 0.64f, 0.82f, 1.0f, 1.2f, 1.42f, 1.7f}) p.push_back(shade(leaf, k));
    p.push_back(mix(shade(leaf, 1.6f), {214, 214, 110, 255}, 0.35f));  // the sunlit tips
    for (const float k : {0.4f, 0.55f, 0.72f, 0.9f, 1.1f, 1.35f}) p.push_back(shade(bark, k));
    if (kind != TreeKind::Birch) {
        for (const float k : {0.5f, 0.8f, 1.1f}) p.push_back(shade({72, 54, 38, 255}, k));  // the bows' own brown
    }
    for (const Color c : {Color{206, 190, 150, 255}, Color{214, 198, 158, 255}, Color{40, 40, 38, 255}}) p.push_back(c);
    if (kind == TreeKind::Apple) {
        for (const Color c : {Color{234, 232, 224, 255}, Color{196, 40, 36, 255}, Color{220, 176, 60, 255}}) p.push_back(c);
    }
    return p;
}

// --- Buildings, textured ---------------------------------------------------------
//
// Drawn by hand, as everything else, then baked into pixel art as the
// vehicles are (see WorldRenderer::bake_buildings): whitewash and plaster,
// brick courses, concrete panels, corrugated iron, planks; slate, tin,
// clay tiles, bitumen; windows in their frames, doors, pipes and aerials;
// soot, cracks, plaster fallen off and holes as it's hit.

// Random in 0..1, from a seed and a number.
float rand01(uint32_t seed, int i) {
    return hash_unit(tile_hash(static_cast<int>(seed & 0xFFFFFu) + i * 7919, i * 104729 + static_cast<int>(seed >> 20)));
}

// Thin things (aerials, masts, wires): a pixel wide, drawn over a baked
// building after it's made pixel art (an outline would make them thick);
// by hand, straight away.
struct FineLine {
    Vector2 a;
    Vector2 b;
    Color c;
};
std::vector<FineLine>* g_fine = nullptr;
// Where a building is burning (a window, a hole in the roof: flames and
// smoke) or smouldering (smoke only), as it's baked: the flames and the
// smoke are drawn over it as it stands, alive.
struct FireSpot {
    Vector2 at;
    bool flames;
    bool roof = false;  // on a roof: a patch of fire over it
};
std::vector<FireSpot>* g_fires = nullptr;
void fire_spot(Vector2 at, bool flames, bool roof = false) {
    if (g_fires) g_fires->push_back({at, flames, roof});
}

// A building's damage in four stages (0 whole, 1 hit, 2 burning, badly
// hit, 3 burnt out, its roof gone): each drawn as its middle.
int stage_of(float damage) { return damage < 0.2f ? 0 : damage < 0.45f ? 1 : damage < 0.7f ? 2 : 3; }

// A window's frame, charred when the building's burnt out.
Color charred(Color frame, float damage) { return stage_of(damage) >= 3 ? Color{58, 50, 44, 255} : frame; }

void fine_line(Vector2 a, Vector2 b, Color c) {
    if (g_fine) {
        g_fine->push_back({a, b, c});
    } else {
        DrawLineV(a, b, lit(c));
    }
}

// A wall on the screen: along its foot from `a` to `b` (on the ground), up
// `v` pixels; u runs 0..1 along it.
struct Face {
    Vector2 a;
    Vector2 b;
    Vector2 at(float u, float v) const { return {a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u - v}; }
    float px() const { return 1.0f / std::max(1.0f, std::fabs(b.x - a.x)); }  // a screen pixel along it, in u
    float pixels() const { return std::fabs(b.x - a.x); }
};
void face_fill(const Face& f, float u0, float u1, float v0, float v1, Color c) {
    fill_quad(f.at(u0, v0), f.at(u1, v0), f.at(u1, v1), f.at(u0, v1), c);
}
void face_line(const Face& f, float u0, float v0, float u1, float v1, Color c) { DrawLineV(f.at(u0, v0), f.at(u1, v1), lit(c)); }

// A roof slope on the screen: its eave from e0 to e1, its top from t0 to t1
// (a hip's: one point); s runs 0..1 along it, t 0..1 up it.
struct Slope {
    Vector2 e0;
    Vector2 e1;
    Vector2 t0;
    Vector2 t1;
    Vector2 at(float s, float t) const { return lerp(lerp(e0, e1, s), lerp(t0, t1, s), t); }
    float along() const { return std::max(1.0f, std::hypot(e1.x - e0.x, e1.y - e0.y)); }
    float up() const { return std::max(1.0f, std::hypot(t0.x - e0.x, t0.y - e0.y)); }
};

enum class Wall : uint8_t { Whitewash, Pastel, RedBrick, YellowBrick, Silicate, Panel, Concrete, Corrugated, Planks };
enum class Roof : uint8_t { Slate, Tin, Tiles, Rusty, Bitumen };

// A stretch of wall in its material. Brick in courses: the mortar between
// them, the joints staggered, a brick here and there darker or paler.
// Whitewash and plaster in patches, streaks down from the eaves, the dirt
// splashed up at its foot. Concrete in its pours. Corrugated iron in ribs,
// planks side by side, a darker one here and there.
void wall_texture(const Face& f, float u0, float u1, float v0, float v1, Wall kind, Color c, uint32_t seed) {
    face_fill(f, u0, u1, v0, v1, c);
    const float px = f.px();
    const float wide = (u1 - u0) / px;
    switch (kind) {
        case Wall::RedBrick:
        case Wall::YellowBrick:
        case Wall::Silicate: {
            const Color joint = kind == Wall::Silicate ? shade(c, 0.84f) : shade(c, 0.74f);
            int row = 0;
            for (float v = v0 + 3.0f; v < v1 - 0.5f; v += 3.0f, ++row) {
                face_line(f, u0, v, u1, v, joint);
                const float off = static_cast<float>(row % 2) * 2.5f;
                for (float s = u0 + off * px; s < u1; s += 5.0f * px) {
                    DrawPixelV(f.at(s, v - 1.5f), lit(joint));
                    const float r = rand01(seed, row * 131 + static_cast<int>((s - u0) / px));
                    if (r < 0.2f && s + 4.0f * px < u1) {
                        face_line(f, s + px, v - 1.5f, s + 4.0f * px, v - 1.5f, shade(c, r < 0.1f ? 0.84f : 1.12f));
                    }
                }
            }
            break;
        }
        case Wall::Whitewash:
        case Wall::Pastel:
        case Wall::Concrete: {
            const int n = static_cast<int>(wide * (v1 - v0) / 22.0f);
            for (int i = 0; i < n; ++i) {  // patches of it, newer and older
                const float u = u0 + (u1 - u0) * rand01(seed, i);
                const float v = v0 + (v1 - v0) * rand01(seed, i + 500);
                const float w = (2.0f + 5.0f * rand01(seed, i + 900)) * px;
                const float h = 1.0f + 2.0f * rand01(seed, i + 1300);
                face_fill(f, u, std::min(u1, u + w), v, std::min(v1, v + h), shade(c, 0.93f + 0.12f * rand01(seed, i + 1700)));
            }
            for (int i = 0; i < static_cast<int>(wide / 9.0f); ++i) {  // streaks down from the eaves
                const float u = u0 + (u1 - u0) * rand01(seed, i + 2100);
                const float len = (v1 - v0) * (0.2f + 0.35f * rand01(seed, i + 2300));
                face_line(f, u, v1 - 0.5f, u, v1 - len, shade(c, 0.9f));
            }
            if (kind == Wall::Concrete) {
                for (float v = v0 + 6.0f; v < v1; v += 6.0f) face_line(f, u0, v, u1, v, shade(c, 0.92f));  // the pours
            }
            face_fill(f, u0, u1, v0, v0 + 1.5f, shade(c, 0.82f));  // the dirt splashed up
            break;
        }
        case Wall::Panel:
            break;  // (its seams go with its floors and bays: see panels)
        case Wall::Corrugated: {
            int i = 0;
            for (float s = u0 + px; s < u1; s += 2.0f * px, ++i) face_line(f, s, v0, s, v1, shade(c, i % 2 ? 1.12f : 0.8f));
            break;
        }
        case Wall::Planks: {
            int i = 0;
            for (float s = u0 + 3.0f * px; s < u1; s += 3.0f * px, ++i) {
                face_line(f, s, v0, s, v1, shade(c, 0.7f));
                if (rand01(seed, i) < 0.3f) face_fill(f, s - 2.0f * px, s - px, v0, v1, shade(c, 0.88f));
            }
            break;
        }
    }
}

// Concrete panels over a stretch of a face (bays `b0` to `b1` of `bays`):
// a seam at each floor and each bay, each panel its own shade.
void panels_in(const Face& f, int b0, int b1, int bays, float v0, float floor_h, int floors, Color c, uint32_t seed) {
    for (int fl = 0; fl < floors; ++fl) {
        for (int b = b0; b < b1; ++b) {
            const float t = 0.95f + 0.1f * rand01(seed, fl * 97 + b);
            face_fill(f, static_cast<float>(b) / bays, static_cast<float>(b + 1) / bays, v0 + floor_h * fl, v0 + floor_h * (fl + 1), shade(c, t));
        }
        face_line(f, static_cast<float>(b0) / bays, v0 + floor_h * fl, static_cast<float>(b1) / bays, v0 + floor_h * fl, shade(c, 0.76f));
    }
    for (int b = b0 + 1; b < b1; ++b) face_line(f, static_cast<float>(b) / bays, v0, static_cast<float>(b) / bays, v0 + floor_h * floors, shade(c, 0.8f));
}

// Concrete panels: a seam at each floor and each bay, each panel its own shade.
void panels(const Face& f, float v0, float floor_h, int floors, float bay_px, Color c, uint32_t seed) {
    const float px = f.px();
    const int bays = std::max(1, static_cast<int>(f.pixels() / bay_px));
    for (int fl = 0; fl < floors; ++fl) {
        for (int b = 0; b < bays; ++b) {
            const float t = 0.95f + 0.1f * rand01(seed, fl * 97 + b);
            face_fill(f, static_cast<float>(b) / bays, static_cast<float>(b + 1) / bays, v0 + floor_h * fl, v0 + floor_h * (fl + 1), shade(c, t));
        }
        face_line(f, 0.0f, v0 + floor_h * fl, 1.0f, v0 + floor_h * fl, shade(c, 0.76f));
    }
    for (int b = 1; b < bays; ++b) face_line(f, static_cast<float>(b) / bays, v0, static_cast<float>(b) / bays, v0 + floor_h * floors, shade(c, 0.8f));
    (void)px;
}

// A roof slope in its covering: slate (corrugated asbestos-cement sheets:
// waves down the slope, the courses overlapping, a sheet replaced here and
// there, lichen), tin (standing seams), clay tiles (in rows), tin gone to
// rust, bitumen (patched).
void roof_texture(const Slope& sl, Roof kind, Color c, float k, uint32_t seed) {
    const Color base = shade(c, k);
    fill_quad(sl.e0, sl.e1, sl.t1, sl.t0, base);
    const float along = sl.along();
    const float up = sl.up();
    switch (kind) {
        case Roof::Slate: {
            const int waves = std::max(2, static_cast<int>(along / 2.5f));
            for (int i = 1; i < waves; ++i) {
                const float s = static_cast<float>(i) / static_cast<float>(waves);
                DrawLineV(sl.at(s, 0.0f), sl.at(s, 1.0f), lit(shade(base, i % 2 ? 1.12f : 0.86f)));
            }
            const int courses = std::max(1, static_cast<int>(up / 6.0f));
            for (int j = 1; j < courses; ++j) {
                const float t = static_cast<float>(j) / static_cast<float>(courses);
                DrawLineV(sl.at(0.0f, t), sl.at(1.0f, t), lit(shade(base, 0.7f)));
            }
            const int sheets = std::max(1, static_cast<int>(along / 9.0f));
            for (int i = 0; i < sheets * courses / 3 + 1; ++i) {  // sheets replaced (paler), or grown over with lichen
                const float s = std::floor(rand01(seed, i) * static_cast<float>(sheets)) / static_cast<float>(sheets);
                const float t = std::floor(rand01(seed, i + 40) * static_cast<float>(courses)) / static_cast<float>(courses);
                const float ds = 1.0f / static_cast<float>(sheets);
                const float dt = 1.0f / static_cast<float>(courses);
                const Color patch = rand01(seed, i + 80) < 0.5f ? shade(base, 1.12f) : mix(base, {150, 150, 96, 255}, 0.35f);
                fill_quad(sl.at(s + ds * 0.1f, t + dt * 0.1f), sl.at(s + ds * 0.9f, t + dt * 0.1f), sl.at(s + ds * 0.9f, t + dt * 0.9f),
                          sl.at(s + ds * 0.1f, t + dt * 0.9f), patch);
            }
            break;
        }
        case Roof::Tin:
        case Roof::Rusty: {
            const int seams = std::max(2, static_cast<int>(along / 5.0f));
            for (int i = 1; i < seams; ++i) {
                const float s = static_cast<float>(i) / static_cast<float>(seams);
                DrawLineV(sl.at(s, 0.0f), sl.at(s, 1.0f), lit(shade(base, 1.2f)));
                DrawLineV(sl.at(s + 0.5f / along, 0.0f), sl.at(s + 0.5f / along, 1.0f), lit(shade(base, 0.72f)));
            }
            const int rust = kind == Roof::Rusty ? 10 : 2;
            for (int i = 0; i < rust; ++i) {
                const float s = rand01(seed, i);
                const float t = rand01(seed, i + 50);
                const float w = (3.0f + 5.0f * rand01(seed, i + 90)) / along;
                const float h = (2.0f + 3.0f * rand01(seed, i + 130)) / up;
                fill_quad(sl.at(s, t), sl.at(std::min(1.0f, s + w), t), sl.at(std::min(1.0f, s + w), std::min(1.0f, t + h)),
                          sl.at(s, std::min(1.0f, t + h)), mix(base, {126, 70, 40, 255}, 0.55f));
            }
            break;
        }
        case Roof::Tiles: {
            const int rows = std::max(2, static_cast<int>(up / 2.5f));
            for (int j = 1; j < rows; ++j) {
                const float t = static_cast<float>(j) / static_cast<float>(rows);
                DrawLineV(sl.at(0.0f, t), sl.at(1.0f, t), lit(shade(base, 0.72f)));
                for (float s = (j % 2) * 1.5f / along; s < 1.0f; s += 3.0f / along) DrawPixelV(sl.at(s, t - 0.5f / static_cast<float>(rows)), lit(shade(base, 0.8f)));
            }
            break;
        }
        case Roof::Bitumen: {
            for (int i = 0; i < static_cast<int>(along * up / 60.0f); ++i) {
                const float s = rand01(seed, i);
                const float t = rand01(seed, i + 70);
                const float w = (3.0f + 6.0f * rand01(seed, i + 140)) / along;
                const float h = (2.0f + 4.0f * rand01(seed, i + 210)) / up;
                fill_quad(sl.at(s, t), sl.at(std::min(1.0f, s + w), t), sl.at(std::min(1.0f, s + w), std::min(1.0f, t + h)),
                          sl.at(s, std::min(1.0f, t + h)), shade(base, 0.88f + 0.2f * rand01(seed, i + 280)));
            }
            break;
        }
    }
}

// A window: `w` by `h` pixels, its sill at `v`; its frame round it, the
// glass with the sky in its upper panes, the frame's cross, the sill under
// it; shutters beside it, net curtains in it; broken, dark, a jag of glass left.
struct WindowLook {
    Color frame;
    bool cross = true;
    bool shutters = false;
    Color shutter{};
    bool curtains = false;
};
void window_on(const Face& f, float u, float v, float w, float h, const WindowLook& look, bool broken, float k) {
    const float px = f.px();
    const float du = w * 0.5f * px;
    face_fill(f, u - du - px, u + du + px, v - 1.0f, v + h + 1.0f, shade(look.frame, k));
    if (broken) {
        face_fill(f, u - du, u + du, v, v + h, {18, 16, 16, 255});
        fill_triangle(f.at(u - du, v + h), f.at(u - du + 2.0f * px, v + h), f.at(u - du, v + h - 2.0f), shade({120, 140, 156, 255}, k));
    } else {
        face_fill(f, u - du, u + du, v, v + h, shade({58, 74, 92, 255}, k));
        face_fill(f, u - du, u + du, v + h * 0.55f, v + h, shade({112, 138, 160, 255}, k));  // the sky in its upper panes
        if (look.curtains) face_fill(f, u - du, u - du + std::max(px, du * 0.6f), v, v + h, shade({224, 220, 206, 255}, k));
        if (look.cross) {
            face_line(f, u, v, u, v + h, shade(look.frame, k));
            face_line(f, u - du, v + h * 0.6f, u + du, v + h * 0.6f, shade(look.frame, k));
        }
    }
    face_fill(f, u - du - 2.0f * px, u + du + 2.0f * px, v - 1.6f, v - 0.6f, shade({206, 204, 196, 255}, k));  // the sill
    if (look.shutters) {
        const float sw = std::max(2.0f * px, du);
        face_fill(f, u - du - px - sw, u - du - px, v - 0.5f, v + h + 0.5f, shade(look.shutter, k));
        face_fill(f, u + du + px, u + du + px + sw, v - 0.5f, v + h + 0.5f, shade(look.shutter, k));
    }
}

// A door in its frame, a step before it.
void door_on(const Face& f, float u, float w, float h, Color c, float k) {
    const float px = f.px();
    const float du = w * 0.5f * px;
    face_fill(f, u - du - px, u + du + px, 0.0f, h + 1.0f, shade(c, 0.6f * k));
    face_fill(f, u - du, u + du, 0.0f, h, shade(c, k));
    face_line(f, u - du + px, h * 0.5f, u + du - px, h * 0.5f, shade(c, 0.75f * k));
    DrawPixelV(f.at(u + du - 1.5f * px, h * 0.45f), lit(shade({200, 190, 150, 255}, k)));
    face_fill(f, u - du - 2.0f * px, u + du + 2.0f * px, -1.0f, 0.5f, shade({150, 146, 138, 255}, k));  // the step
}

// Soot licked up a wall from a window or a hole.
void soot_on(const Face& f, float u, float v, float w, float h) {
    const float du = w * 0.5f * f.px();
    fill_triangle(f.at(u - du, v), f.at(u + du, v), f.at(u + du * 0.3f, v + h), ColorAlpha({20, 18, 16, 255}, 0.75f));
    fill_triangle(f.at(u - du * 0.6f, v), f.at(u + du * 0.8f, v), f.at(u - du * 0.4f, v + h * 0.8f), ColorAlpha({30, 26, 24, 255}, 0.6f));
}

// A crack down a wall: a zigzag.
void crack_on(const Face& f, float u, float v, float len, uint32_t seed) {
    Vector2 p = f.at(u, v);
    for (int i = 0; i < 4; ++i) {
        const Vector2 q{p.x + (rand01(seed, i) - 0.5f) * 3.0f, p.y + len / 4.0f};
        DrawLineV(p, q, lit({40, 34, 30, 255}));
        p = q;
    }
}

// Shrapnel's pits in a wall, `n` of them: dark pocks with a pale rim where
// the plaster chipped round them.
void pockmarks(const Face& f, float u0, float u1, float v0, float v1, int n, uint32_t seed) {
    for (int i = 0; i < n; ++i) {
        const Vector2 p = f.at(u0 + (u1 - u0) * rand01(seed, 7000 + i), v0 + (v1 - v0) * rand01(seed, 7100 + i));
        DrawPixelV({std::round(p.x) + 1.0f, std::round(p.y)}, lit({206, 200, 188, 255}));
        DrawPixelV({std::round(p.x), std::round(p.y)}, lit({36, 32, 30, 255}));
    }
}

// Plaster (a facing, a panel's skin) fallen off in a ragged patch: what's under it showing.
void peeled(const Face& f, float u, float v, float w, float h, Color under, bool brick, uint32_t seed) {
    const float px = f.px();
    for (int k = 0; k < 3; ++k) {
        const float uu = u + (rand01(seed, k) - 0.5f) * w * 0.5f * px;
        const float vv = v + (rand01(seed, k + 9) - 0.5f) * h * 0.5f;
        const float ww = w * (0.5f + 0.5f * rand01(seed, k + 18)) * px;
        const float hh = h * (0.5f + 0.5f * rand01(seed, k + 27));
        wall_texture(f, uu - ww * 0.5f, uu + ww * 0.5f, vv - hh * 0.5f, vv + hh * 0.5f, brick ? Wall::RedBrick : Wall::Concrete, under,
                     seed + static_cast<uint32_t>(k));
    }
}

// A hole blown through a wall: ragged, its broken edge pale, the rooms dark
// inside; a floor's slab across it, the rebar bent out.
void wall_hole(const Face& f, float u, float v, float w, float h, uint32_t seed) {
    const float px = f.px();
    constexpr int kN = 12;
    Vector2 edge[kN];
    Vector2 inner[kN];
    for (int i = 0; i < kN; ++i) {
        const float a = static_cast<float>(i) * 6.2831853f / kN;
        const float k = 0.7f + 0.5f * rand01(seed, i);
        edge[i] = f.at(u + std::cos(a) * w * 0.5f * k * px, v + h * 0.5f + std::sin(a) * h * 0.5f * k);
        inner[i] = f.at(u + std::cos(a) * w * 0.5f * k * 0.78f * px, v + h * 0.5f + std::sin(a) * h * 0.5f * k * 0.78f);
    }
    const Vector2 c = f.at(u, v + h * 0.5f);
    for (int i = 0; i < kN; ++i) fill_triangle(c, edge[i], edge[(i + 1) % kN], {168, 162, 150, 255});
    for (int i = 0; i < kN; ++i) fill_triangle(c, inner[i], inner[(i + 1) % kN], {24, 20, 18, 255});
    if (h > 8.0f) face_line(f, u - w * 0.35f * px, v + h * 0.45f, u + w * 0.35f * px, v + h * 0.45f, {120, 116, 108, 255});  // a slab across
    for (int i = 0; i < 3; ++i) {  // rebar bent out of it
        const Vector2 a = edge[static_cast<int>(rand01(seed, 40 + i) * kN) % kN];
        DrawLineV(a, {a.x + (rand01(seed, 50 + i) - 0.5f) * 5.0f, a.y + 2.0f + 2.0f * rand01(seed, 60 + i)}, lit({70, 52, 40, 255}));
    }
}

// A wall burnt: soot over it (`amount` 0..1), blackest in tongues up from where it burned.
void burnt(const Face& f, float u0, float u1, float v0, float v1, float amount, uint32_t seed) {
    face_fill(f, u0, u1, v0, v1, ColorAlpha({28, 24, 22, 255}, 0.55f * amount));
    const int tongues = static_cast<int>(12.0f * (u1 - u0) / f.px() / 60.0f) + 2;
    for (int i = 0; i < tongues; ++i) {
        const float u = u0 + (u1 - u0) * rand01(seed, 8000 + i);
        const float v = v0 + (v1 - v0) * 0.6f * rand01(seed, 8100 + i);
        soot_on(f, u, v, 5.0f + 5.0f * rand01(seed, 8200 + i), (v1 - v) * (0.4f + 0.6f * amount));
    }
}

// A piece of rubble: a chunk of concrete or brick on its column of the heap
// down to the ground (dark, the gaps between them), lit on top; a slab
// broken off lying tilted; a bit of rebar sticking out.
struct RubblePiece {
    enum Kind : uint8_t { Chunk, Slab, Rebar } kind;
    float depth;  // back to front
    Vector2 at;   // on the heap's top
    float foot;   // the ground under it (screen y)
    float size;
    Color c;
    float angle;
};

void draw_rubble(std::vector<RubblePiece>& pieces) {
    std::stable_sort(pieces.begin(), pieces.end(), [](const RubblePiece& a, const RubblePiece& b) { return a.depth < b.depth; });
    for (const RubblePiece& p : pieces) {
        switch (p.kind) {
            case RubblePiece::Chunk: {
                DrawRectangleRec({p.at.x - p.size, p.at.y, p.size * 2.0f, std::max(1.0f, p.foot - p.at.y + 1.0f)}, lit(shade(p.c, 0.46f)));
                const uint32_t h = tile_hash(static_cast<int>(p.at.x * 3.0f), static_cast<int>(p.foot * 5.0f));
                int k = 0;
                for (float y = p.at.y + p.size * 2.0f; y < p.foot - p.size * 0.5f; y += p.size * 1.8f, ++k) {  // down its face, in the shade
                    const float s = p.size * (0.7f + 0.4f * rand01(h, k));
                    const float dx = (rand01(h, k + 20) - 0.5f) * p.size;
                    const Color c = shade(p.c, 0.5f + 0.25f * rand01(h, k + 40));
                    DrawEllipse(static_cast<int>(p.at.x + dx), static_cast<int>(y), s, s * 0.7f, lit(c));
                }
                DrawEllipse(static_cast<int>(p.at.x), static_cast<int>(p.at.y), p.size, p.size * 0.7f, lit(shade(p.c, 0.62f)));
                DrawEllipse(static_cast<int>(p.at.x - p.size * 0.2f), static_cast<int>(p.at.y - p.size * 0.25f), p.size * 0.7f, p.size * 0.45f, lit(p.c));
                break;
            }
            case RubblePiece::Slab: {
                const float len = p.size;
                const float thick = 1.5f + len * 0.12f;
                const Vector2 d{std::cos(p.angle) * len * 0.5f, std::sin(p.angle) * len * 0.3f};
                const Vector2 n{-std::sin(p.angle) * thick * 0.5f, std::cos(p.angle) * thick * 0.5f - thick * 0.4f};
                const Vector2 c{p.at.x, p.at.y - 1.0f};
                fill_quad({c.x - d.x - n.x, c.y - d.y - n.y}, {c.x + d.x - n.x, c.y + d.y - n.y}, {c.x + d.x + n.x, c.y + d.y + n.y},
                          {c.x - d.x + n.x, c.y - d.y + n.y}, p.c);
                DrawLineV({c.x - d.x + n.x, c.y - d.y + n.y}, {c.x + d.x + n.x, c.y + d.y + n.y}, lit(shade(p.c, 0.6f)));
                break;
            }
            case RubblePiece::Rebar:
                DrawLineV(p.at, {p.at.x + std::cos(p.angle) * p.size, p.at.y - 3.0f - std::fabs(std::sin(p.angle)) * p.size}, lit({74, 56, 44, 255}));
                break;
        }
    }
}

// A heap of rubble at `foot` (screen), `w` pixels across and `h` high: chunks
// of concrete and brick piled up, higher in the middle, lit on top, dark in
// the gaps between them (a scrap of a room's wallpaper here and there);
// broken slabs tilted every way on it, bent rebar sticking out.
void rubble_heap(Vector2 foot, float w, float h, Color concrete, Color brick, uint32_t seed, int slabs) {
    static constexpr Color kPaper[4] = {{186, 204, 170, 255}, {214, 196, 150, 255}, {170, 190, 210, 255}, {214, 176, 170, 255}};
    std::vector<RubblePiece> pieces;
    const float deep = w * 0.16f;
    auto top = [&](float across, float dep) { return h * std::pow(std::max(0.0f, 1.0f - across * across - dep * dep), 0.7f); };
    const int n = static_cast<int>(w * deep * 0.5f) + 8;
    for (int i = 0; i < n; ++i) {
        const float across = (rand01(seed, i) - 0.5f) * 2.0f;  // -1..1
        const float dep = (rand01(seed, i + 3000) - 0.5f) * 2.0f;
        const float up = top(across, dep);
        if (up <= 0.0f) continue;
        const float roll = rand01(seed, i + 3200);
        const Color c = roll < 0.15f ? brick : roll < 0.19f ? kPaper[i % 4] : shade(concrete, 0.62f + 0.5f * rand01(seed, i + 3300));
        const float ground = foot.y + dep * deep;
        pieces.push_back({RubblePiece::Chunk, ground, {foot.x + across * w * 0.5f, ground - up * (0.8f + 0.2f * rand01(seed, i + 3100))}, ground,
                          1.4f + 2.2f * rand01(seed, i + 3400), c, 0.0f});
    }
    for (int i = 0; i < slabs + 2 + slabs / 4; ++i) {
        const float across = (rand01(seed, i + 100) - 0.5f) * 1.6f;
        const float dep = (rand01(seed, i + 150) - 0.5f) * 1.4f;
        const float ground = foot.y + dep * deep;
        const Vector2 at{foot.x + across * w * 0.5f, ground - top(across, dep)};
        if (i < slabs) {
            pieces.push_back({RubblePiece::Slab, ground + 0.5f, at, ground, 5.0f + 8.0f * rand01(seed, i + 300),
                              shade(concrete, 0.85f + 0.3f * rand01(seed, i + 500)), rand01(seed, i + 200) * 3.14159f});
        } else {
            pieces.push_back({RubblePiece::Rebar, ground + 0.5f, at, ground, 3.0f + 5.0f * rand01(seed, i + 1000), {}, rand01(seed, i + 990) * 3.14159f});
        }
    }
    draw_rubble(pieces);
}

// What a pile of rubble is of: its main stuff (concrete, plaster, iron,
// earth), a second (brick, timber) in a share of it, scraps of the rooms
// (wallpaper, a sofa) in it or not.
struct RubbleMix {
    Color main;
    Color second;
    float seconds;
    bool rooms;
};

// The rubble's lie over the ground, the same from tile to tile (a hummock
// here, a dip there): 0..1.
float ruin_noise(Vector2 g) {
    return 0.5f + 0.25f * std::sin(g.x * 2.3f + g.y * 1.1f) + 0.25f * std::sin(g.x * 0.9f - g.y * 2.7f + 1.3f);
}

// A pile of rubble over the ground from `g0`, `size` tiles across, as high
// (pixels) as `height` says at each point: chunks all over it, each on its
// column down to the ground; broken slabs lying on it, rebar out of it.
// Only the part `keep` says (another call with the same seed draws the rest:
// before a wall standing in it, after).
void rubble_pile(const engine::TileMap& map, Vector2 g0, Vector2 size, const std::function<float(Vector2)>& height, const RubbleMix& mix,
                 uint32_t seed, int slabs, const std::function<bool(Vector2)>& keep = {}) {
    static constexpr Color kPaper[5] = {{186, 204, 170, 255}, {214, 196, 150, 255}, {170, 190, 210, 255}, {214, 176, 170, 255}, {120, 84, 60, 255}};
    const Vector2 a = on_terrain(map, g0);
    const Vector2 b = on_terrain(map, {g0.x + size.x, g0.y});
    const Vector2 c = on_terrain(map, {g0.x, g0.y + size.y});
    const float area = std::fabs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
    const int n = static_cast<int>(area / 6.0f) + 6;
    std::vector<RubblePiece> pieces;
    pieces.reserve(static_cast<size_t>(n + slabs * 2));
    for (int i = 0; i < n + slabs + slabs / 2; ++i) {
        const Vector2 g{g0.x + size.x * rand01(seed, i), g0.y + size.y * rand01(seed, i + 7000)};
        if (keep && !keep(g)) continue;
        const float up = height(g);
        if (up < 0.5f) continue;
        const Vector2 foot = on_terrain(map, g);
        if (i >= n) {  // a slab lying on it, rebar out of it
            const bool slab = i < n + slabs;
            pieces.push_back({slab ? RubblePiece::Slab : RubblePiece::Rebar, g.x + g.y + 0.02f, {foot.x, foot.y - up}, foot.y,
                              slab ? 6.0f + 9.0f * rand01(seed, i + 7600) : 3.0f + 5.0f * rand01(seed, i + 7600),
                              shade(mix.main, 0.85f + 0.3f * rand01(seed, i + 7700)), rand01(seed, i + 7800) * 3.14159f});
            continue;
        }
        const float roll = rand01(seed, i + 7100);
        Color col = roll < mix.seconds ? shade(mix.second, 0.78f + 0.4f * rand01(seed, i + 7200))
                                       : shade(mix.main, 0.62f + 0.5f * rand01(seed, i + 7300));
        if (mix.rooms && roll > 0.97f) col = kPaper[i % 5];
        pieces.push_back({RubblePiece::Chunk, g.x + g.y, {foot.x, foot.y - up * (0.82f + 0.18f * rand01(seed, i + 7400))}, foot.y,
                          1.4f + 2.2f * rand01(seed, i + 7500), col, 0.0f});
    }
    draw_rubble(pieces);
}

// The usual wear of a building's walls by its stage: shrapnel's pits,
// plaster off; burnt at the worst; soot licked up from the windows.
void wear_walls(const Face& f, float k, float v0, float v1, int stage, Color under, bool brick, uint32_t seed) {
    if (stage <= 0) return;
    pockmarks(f, 0.0f, 1.0f, v0, v1, static_cast<int>(f.pixels() * (v1 - v0) / (stage == 1 ? 120.0f : stage == 2 ? 50.0f : 30.0f)), seed);
    const int patches = stage * 2;
    for (int i = 0; i < patches; ++i) {
        peeled(f, 0.08f + 0.84f * rand01(seed, 300 + i), v0 + (v1 - v0) * (0.15f + 0.7f * rand01(seed, 310 + i)), 6.0f, 3.0f, shade(under, k), brick,
               seed + static_cast<uint32_t>(i) * 7u);
    }
    if (stage >= 3) burnt(f, 0.0f, 1.0f, v0, v1, 0.75f, seed);
}

void fallen_roof(const engine::TileMap& map, Rectangle r, float wall, float rise, bool along_x, bool gable, Color walls, Color cover, bool steel,
                 uint32_t seed);  // below, with the roofs

// A shop's sign in tiny pixel letters: ПРОДУКТЫ, on a face `f` from u0,
// its lower edge at `v`, each letter's pixel a pixel.
void sign_text(const Face& f, float u0, float v, Color c) {
    static constexpr const char* kLetters[8][5] = {
        {"111", "101", "101", "101", "101"},            // П
        {"110", "101", "110", "100", "100"},            // Р
        {"111", "101", "101", "101", "111"},            // О
        {"0110", "0110", "0110", "1111", "1001"},       // Д
        {"101", "101", "011", "001", "110"},            // У
        {"101", "110", "100", "110", "101"},            // К
        {"111", "010", "010", "010", "010"},            // Т
        {"10001", "10001", "11101", "10101", "11101"},  // Ы
    };
    const float px = f.px();
    float u = u0;
    for (const auto& letter : kLetters) {
        int wide = 0;
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; letter[row][col] != 0; ++col) {
                wide = std::max(wide, col + 1);
                if (letter[row][col] != '1') continue;
                face_fill(f, u + static_cast<float>(col) * px, u + static_cast<float>(col + 1) * px, v + static_cast<float>(4 - row),
                          v + static_cast<float>(5 - row), c);
            }
        }
        u += static_cast<float>(wide + 1) * px;
    }
}

// A plot's fence along one of its edges, from `a` to `b` on the ground:
// pickets (painted, pointed) on their rails, or corrugated sheets on posts;
// left out between `gap0` and `gap1` (along it, 0..1) for the gate.
void plot_fence(const engine::TileMap& map, Vector2 a, Vector2 b, bool sheets, Color paint, float tall, float light, float gap0 = 2.0f,
                float gap1 = 2.0f) {
    const Face f{on_terrain(map, a), on_terrain(map, b)};
    const float px = f.px();
    const Color c = shade(paint, light);
    if (sheets) {  // corrugated sheets: the ribs, a rail on top, a post every few metres
        for (const auto& [u0, u1] : {std::pair{0.0f, std::min(1.0f, gap0)}, std::pair{std::max(0.0f, gap1), 1.0f}}) {
            if (u1 <= u0) continue;
            face_fill(f, u0, u1, 0.5f, tall, c);
            for (float u = u0; u < u1; u += 2.0f * px) face_line(f, u, 0.5f, u, tall, shade(c, 0.8f));
            face_line(f, u0, tall, u1, tall, shade(c, 1.2f));
            face_fill(f, u0, u1, 0.0f, 1.0f, shade(c, 0.6f));
        }
        return;
    }
    for (const float rail : {tall * 0.3f, tall * 0.75f}) {
        if (gap0 > 0.0f) face_line(f, 0.0f, rail, std::min(1.0f, gap0), rail, shade(c, 0.7f));
        if (gap1 < 1.0f) face_line(f, std::max(0.0f, gap1), rail, 1.0f, rail, shade(c, 0.7f));
    }
    int k = 0;
    for (float u = px; u < 1.0f; u += 2.0f * px, ++k) {
        if (u > gap0 && u < gap1) continue;
        const float top = tall + (k % 2 == 0 ? 1.0f : 0.0f);
        face_line(f, u, 0.0f, u, top, k % 6 == 0 ? shade(c, 0.8f) : c);
    }
}

// A house on its plot, as they stand in the villages of the Donbas (on a
// village's 2 x 2 plot, a good deal bigger than a tank; its walls, its roof,
// its windows, its chimney all to that size):
// whitewashed (the old clay ones), red, yellow or grey silicate brick, or
// plastered in a pale colour; windows in painted frames (white, blue,
// green or brown), blue shutters on the whitewashed ones; a dark painted
// plinth; hipped or gabled, under slate, painted tin (green, red, brown),
// clay tiles or rusty tin; the yellow gas pipe along the wall, a brick
// chimney, a TV aerial or a satellite dish; a glazed porch on some. Hit:
// windows broken, soot over them, plaster fallen off showing the brick
// under it, cracks; holes in the roof, the rafters showing; burnt black.
void draw_house(const engine::TileMap& map, Rectangle plot, uint32_t h, float damage, bool shop = false) {
    auto rnd = [h](int i) { return hash_unit(tile_hash(static_cast<int>(h >> 5) + i * 13, i * 31 + 7)); };
    // The plot's size (1 a tile); the house's size by it (1 on one tile).
    const float s = std::sqrt(plot.width * plot.height);
    const float k = 0.25f + 0.75f * s;
    // Square, or long one way or the other; a little off the middle of a big plot.
    const int shape = static_cast<int>(h % 3);
    const float ix = (shape == 2 ? 0.23f : shape == 1 ? 0.1f : 0.14f) * s;
    const float iy = (shape == 1 ? 0.23f : shape == 2 ? 0.1f : 0.14f) * s;
    const float ox = s > 1.0f ? (rnd(23) - 0.5f) * ix * 0.8f : 0.0f;
    const float oy = s > 1.0f ? (rnd(24) - 0.5f) * iy * 0.8f : 0.0f;
    const float x0 = plot.x + ix + ox;
    const float x1 = plot.x + plot.width - ix + ox;
    const float y0 = plot.y + iy + oy;
    const float y1 = plot.y + plot.height - iy + oy;
    const float wall_h = (11.0f + 3.0f * rnd(1)) * k;
    const float roof_h = (8.0f + 2.5f * rnd(2)) * k;
    const float plinth_h = 2.5f * k;
    const Vector2 ground[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    Vector2 base[4];
    Vector2 top[4];
    for (int i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, ground[i]);
        top[i] = {base[i].x, base[i].y - wall_h};
    }
    const float soot = 1.0f - 0.3f * damage;
    const int stage = stage_of(damage);

    // Its fence round the plot (a village's): pickets or corrugated sheets;
    // the far sides first, the near ones over the house after it. The shop's
    // front stands open to the street.
    const bool fenced = s > 1.2f && stage < 3;
    const bool sheets = rnd(30) < 0.3f;
    static constexpr Color kFences[4] = {{96, 136, 90, 255}, {84, 116, 170, 255}, {150, 126, 96, 255}, {150, 84, 60, 255}};
    const Color fence_paint = sheets ? (rnd(31) < 0.5f ? Color{138, 74, 52, 255} : Color{70, 108, 82, 255}) : kFences[static_cast<int>(rnd(32) * 4.0f) % 4];
    const float fence_h = (sheets ? 6.0f : 4.5f) * k;
    const float px0 = plot.x + 0.03f;
    const float px1 = plot.x + plot.width - 0.03f;
    const float py0 = plot.y + 0.03f;
    const float py1 = plot.y + plot.height - 0.03f;
    if (fenced) {
        plot_fence(map, {px0, py0}, {px1, py0}, sheets, fence_paint, fence_h, 0.74f);
        plot_fence(map, {px0, py0}, {px0, py1}, sheets, fence_paint, fence_h, 1.0f);
    }
    // The path trodden from its door out to the street, across the yard.
    if (plot.y + plot.height > y1 + 0.05f) {
        const float door_x = x1 - 0.78f * (x1 - x0);
        const float half = 0.07f * s;
        const float out = plot.y + plot.height;
        const Vector2 a0 = on_terrain(map, {door_x - half, y1});
        const Vector2 a1 = on_terrain(map, {door_x + half, y1});
        const Vector2 b1 = on_terrain(map, {door_x + half * 1.4f, out});
        const Vector2 b0 = on_terrain(map, {door_x - half * 1.4f, out});
        fill_quad(a0, a1, b1, b0, {150, 128, 94, 255});
        DrawLineV(a0, b0, lit({120, 100, 72, 255}));
        DrawLineV(a1, b1, lit({176, 156, 118, 255}));
        for (int k2 = 0; k2 < 3; ++k2) {  // a pebble, a bare patch
            const Vector2 g = on_terrain(map, {door_x + (rnd(90 + k2) - 0.5f) * half * 1.4f, y1 + (out - y1) * (0.2f + 0.6f * rnd(95 + k2))});
            DrawPixelV(g, lit({112, 94, 70, 255}));
        }
    }

    // The walls.
    static constexpr Wall kKinds[5] = {Wall::Whitewash, Wall::RedBrick, Wall::YellowBrick, Wall::Silicate, Wall::Pastel};
    static constexpr Color kWalls[5] = {{232, 232, 224, 255}, {160, 84, 60, 255}, {206, 174, 112, 255}, {190, 188, 180, 255}, {222, 206, 160, 255}};
    static constexpr Color kPastels[3] = {{222, 206, 160, 255}, {196, 214, 196, 255}, {226, 200, 190, 255}};
    const int material = std::min(4, static_cast<int>(rnd(3) * 5.0f));
    const Wall kind = kKinds[material];
    Color wall = material == 4 ? kPastels[static_cast<int>(rnd(12) * 3.0f) % 3] : kWalls[material];
    if (kind == Wall::Whitewash && rnd(13) < 0.4f) wall = {214, 226, 234, 255};  // blued whitewash
    wall = shade(wall, (0.96f + 0.08f * rnd(4)) * soot);
    const Face lit_face{base[1], base[2]};    // +x: in the light
    const Face shade_face{base[2], base[3]};  // +y: in shade
    wall_texture(lit_face, 0.0f, 1.0f, 0.0f, wall_h, kind, wall, h);
    wall_texture(shade_face, 0.0f, 1.0f, 0.0f, wall_h, kind, shade(wall, 0.72f), h ^ 0x5555u);
    // Shrapnel's pits, the plaster off where it's been hit (the brick, the clay under it), burnt black at the worst.
    const bool plastered = kind == Wall::Whitewash || kind == Wall::Pastel;
    const Color under = plastered ? Color{150, 96, 70, 255} : shade(wall, 0.8f);
    wear_walls(lit_face, 1.0f, plinth_h, wall_h, stage, under, plastered, h);
    wear_walls(shade_face, 0.72f, plinth_h, wall_h, stage, under, plastered, h ^ 0x77u);
    // A dark painted plinth.
    static constexpr Color kPlinths[3] = {{96, 86, 78, 255}, {70, 74, 80, 255}, {110, 66, 52, 255}};
    const Color plinth = shade(kPlinths[static_cast<int>(rnd(14) * 3.0f) % 3], soot);
    face_fill(lit_face, 0.0f, 1.0f, 0.0f, plinth_h, plinth);
    face_fill(shade_face, 0.0f, 1.0f, 0.0f, plinth_h, shade(plinth, 0.75f));
    face_line(lit_face, 0.0f, plinth_h, 1.0f, plinth_h, shade(plinth, 1.3f));

    // Windows in painted frames; blue shutters on the whitewashed ones; a door with its step.
    static constexpr Color kFrames[4] = {{236, 234, 226, 255}, {70, 110, 172, 255}, {74, 132, 92, 255}, {120, 84, 56, 255}};
    WindowLook look{kFrames[static_cast<int>(rnd(5) * 4.0f) % 4]};
    look.shutters = kind == Wall::Whitewash && rnd(15) < 0.7f;
    look.shutter = rnd(16) < 0.6f ? Color{70, 110, 172, 255} : Color{74, 132, 92, 255};
    look.curtains = rnd(17) < 0.6f;
    if (stage >= 3) {  // burnt out: the frames charred, the shutters gone
        look.frame = {52, 44, 38, 255};
        look.shutters = false;
    }
    const float sill = wall_h * 0.34f;
    const float wh = std::min(5.5f * k, wall_h * 0.42f);
    auto broken = [&](int i) { return stage >= 3 || (damage > 0.25f && rnd(40 + i) < damage * 1.2f); };
    // Along the lit wall a window every half a tile or so (two on a small
    // house); on the shaded one the door at its end, windows before it.
    struct Pane {
        const Face* face;
        float at;
    };
    std::vector<Pane> panes;
    const int lit_n = std::max(2, static_cast<int>(std::lround((y1 - y0) / 0.5f)));
    for (int i = 0; i < lit_n; ++i) panes.push_back({&lit_face, (static_cast<float>(i) + 0.5f) / static_cast<float>(lit_n) * 0.84f + 0.08f});
    const int shade_n = std::max(1, static_cast<int>(std::lround((x1 - x0) / 0.5f)) - 1);
    for (int i = 0; i < shade_n; ++i) panes.push_back({&shade_face, (static_cast<float>(i) + 0.5f) / static_cast<float>(shade_n) * 0.56f + 0.06f});
    for (size_t i = 0; i < panes.size(); ++i) {
        window_on(*panes[i].face, panes[i].at, sill, 3.6f * k, wh, look, broken(static_cast<int>(i)), panes[i].face == &lit_face ? 1.0f : 0.72f);
    }
    door_on(shade_face, 0.78f, 3.4f * k, wall_h * 0.68f, {112, 78, 52, 255}, 0.8f * soot);
    if (damage > 0.3f) {  // soot over the broken windows
        for (size_t i = 0; i < panes.size(); ++i) {
            if (!broken(static_cast<int>(i))) continue;
            soot_on(*panes[i].face, panes[i].at, sill + wh, 5.0f * k, wall_h - sill - wh + 2.0f);
        }
        crack_on(rnd(18) < 0.5f ? lit_face : shade_face, 0.5f + 0.3f * rnd(19), wall_h - 1.0f, wall_h * 0.6f, h);
    }
    if (stage == 2 && broken(0)) fire_spot(lit_face.at(panes.front().at, sill + wh * 0.3f), true);  // burning inside
    // The yellow gas pipe along the lit wall under the eaves, down at the corner.
    if (rnd(6) < 0.7f) {
        const Color gas = shade({214, 180, 40, 255}, stage >= 3 ? 0.4f : 1.0f);
        DrawLineEx({base[1].x, base[1].y - wall_h + 2.5f * k}, {base[2].x, base[2].y - wall_h + 2.5f * k}, 1.4f * std::sqrt(k), lit(gas));
        DrawLineEx({base[2].x - 1.0f, base[2].y - wall_h + 2.5f * k}, {base[2].x - 1.0f, base[2].y - 3.0f * k}, 1.4f * std::sqrt(k), lit(gas));
    }
    // A glazed porch against the shaded wall on some: a lean-to of small panes.
    if (rnd(20) < 0.35f && shape != 2 && stage < 3) {  // (burnt down with the house)
        const float p0 = 0.08f;
        const float p1 = 0.5f;
        const float depth = 0.16f * s;
        const Vector2 g0 = on_terrain(map, {x0 + (x1 - x0) * p0, y1 + depth});
        const Vector2 g1 = on_terrain(map, {x0 + (x1 - x0) * p1, y1 + depth});
        const Face porch{g1, g0};
        const float ph = wall_h * 0.7f;
        const Color frame = shade(look.frame, 0.85f * soot);
        face_fill(porch, 0.0f, 1.0f, 0.0f, 3.0f * k, shade(wall, 0.8f));
        face_fill(porch, 0.0f, 1.0f, 3.0f * k, ph, shade({70, 88, 104, 255}, 0.85f));
        for (float u = 0.0f; u <= 1.001f; u += 0.2f / std::max(1.0f, s)) face_line(porch, u, 3.0f * k, u, ph, frame);
        face_line(porch, 0.0f, ph * 0.65f, 1.0f, ph * 0.65f, frame);
        face_line(porch, 0.0f, ph, 1.0f, ph, frame);
        const Vector2 w0 = lerp(base[2], base[3], 1.0f - p1);
        const Vector2 w1 = lerp(base[2], base[3], 1.0f - p0);
        fill_quad({g1.x, g1.y - ph}, {g0.x, g0.y - ph}, {w1.x, w1.y - ph - 3.0f * k}, {w0.x, w0.y - ph - 3.0f * k}, shade({120, 124, 126, 255}, soot));
    }

    // The roof, over the eaves: hipped, or gabled along the long side.
    static constexpr Roof kCovers[5] = {Roof::Slate, Roof::Tin, Roof::Tin, Roof::Tiles, Roof::Rusty};
    static constexpr Color kRoofs[5] = {{128, 130, 130, 255}, {86, 120, 90, 255}, {144, 72, 56, 255}, {164, 86, 58, 255}, {126, 86, 60, 255}};
    const int cover = std::min(4, static_cast<int>(rnd(7) * 5.0f));
    const Color roof = shade(kRoofs[cover], (0.94f + 0.1f * rnd(8)) * soot);
    const float kEave = 0.05f * s;
    Vector2 eave[4];
    const Vector2 out[4] = {{x0 - kEave, y0 - kEave}, {x1 + kEave, y0 - kEave}, {x1 + kEave, y1 + kEave}, {x0 - kEave, y1 + kEave}};
    for (int i = 0; i < 4; ++i) eave[i] = on_terrain(map, out[i], wall_h - 1.0f);
    const bool hipped = shape == 0 && rnd(9) < 0.55f;
    const bool along_x = shape == 1 || (shape == 0 && rnd(10) < 0.5f);
    Vector2 ridge0{};
    Vector2 ridge1{};
    const float ym = (y0 + y1) * 0.5f;
    const float xm = (x0 + x1) * 0.5f;
    if (stage >= 3) {  // burnt out: the roof fallen in
        fallen_roof(map, {x0, y0, x1 - x0, y1 - y0}, wall_h, roof_h, along_x, !hipped, wall, roof, false, h);
    } else if (hipped) {
        ridge0 = ridge1 = on_terrain(map, {xm, ym}, wall_h + roof_h);
        roof_texture({eave[0], eave[1], ridge0, ridge0}, kCovers[cover], roof, 1.1f, h);
        roof_texture({eave[3], eave[0], ridge0, ridge0}, kCovers[cover], roof, 0.95f, h + 1);
        roof_texture({eave[1], eave[2], ridge0, ridge0}, kCovers[cover], roof, 1.2f, h + 2);
        roof_texture({eave[2], eave[3], ridge0, ridge0}, kCovers[cover], roof, 0.8f, h + 3);
    } else if (along_x) {
        ridge0 = on_terrain(map, {x0 - kEave, ym}, wall_h + roof_h);
        ridge1 = on_terrain(map, {x1 + kEave, ym}, wall_h + roof_h);
        roof_texture({eave[0], eave[1], ridge0, ridge1}, kCovers[cover], roof, 1.1f, h);
        const Vector2 g1 = on_terrain(map, {x1, ym}, wall_h + roof_h - 1.0f);
        fill_triangle(top[1], top[2], g1, shade(wall, 0.9f));  // the gable end
        fill_triangle({top[1].x, top[1].y}, {top[2].x, top[2].y}, {g1.x, g1.y + 2.0f}, shade(wall, 0.95f));
        roof_texture({eave[3], eave[2], ridge0, ridge1}, kCovers[cover], roof, 0.82f, h + 1);
    } else {
        ridge0 = on_terrain(map, {xm, y0 - kEave}, wall_h + roof_h);
        ridge1 = on_terrain(map, {xm, y1 + kEave}, wall_h + roof_h);
        roof_texture({eave[0], eave[3], ridge0, ridge1}, kCovers[cover], roof, 1.1f, h);
        roof_texture({eave[1], eave[2], ridge0, ridge1}, kCovers[cover], roof, 0.95f, h + 1);
        const Vector2 g1 = on_terrain(map, {xm, y1}, wall_h + roof_h - 1.0f);
        fill_triangle(top[2], top[3], g1, shade(wall, 0.66f));  // the gable end
    }
    if (stage < 3) {
        if (!hipped) DrawLineEx(ridge0, ridge1, 1.5f, lit(shade(roof, 0.6f)));  // the ridge capped
        DrawLineV(eave[1], eave[2], lit(shade(roof, 0.55f)));                   // the gutters along the eaves
        DrawLineV(eave[2], eave[3], lit(shade(roof, 0.5f)));
    }
    // Holes where it's been hit: the dark inside, the rafters across.
    if (damage > 0.3f && stage < 3) {
        const int holes = 1 + static_cast<int>(damage * 3.0f);
        for (int hole = 0; hole < holes; ++hole) {
            const Vector2 g{x0 + (x1 - x0) * (0.25f + 0.5f * rnd(60 + hole)), y0 + (y1 - y0) * (0.25f + 0.5f * rnd(70 + hole))};
            const Vector2 c = on_terrain(map, g, wall_h + roof_h * 0.55f);
            const float r = (2.5f + 2.5f * damage) * k;
            DrawEllipse(static_cast<int>(c.x), static_cast<int>(c.y), r + 1.0f, r * 0.6f + 0.8f, lit(shade(roof, 0.55f)));
            DrawEllipse(static_cast<int>(c.x), static_cast<int>(c.y), r, r * 0.6f, lit({22, 18, 16, 255}));
            for (const float d : {-1.5f, 1.2f}) DrawLineV({c.x - r, c.y + d * k - 0.5f}, {c.x + r, c.y + d * k + 0.8f}, lit({92, 70, 50, 255}));
            if (hole == 0 && stage == 2 && !broken(0)) fire_spot(c, true);
        }
    }
    // A brick chimney, capped (standing out of the burnt shell, all of it showing); a TV aerial or a satellite dish on some.
    {
        const float tall = stage >= 3 ? roof_h * 0.7f + 8.0f * k : 6.0f * k;
        const float cw = 1.8f * k;
        const Vector2 c = on_terrain(map, {xm + (x1 - xm) * 0.4f, ym - (ym - y0) * 0.2f}, stage >= 3 ? wall_h - 2.0f : wall_h + roof_h * 0.7f);
        const Color brick = shade({150, 78, 60, 255}, soot);
        fill_quad({c.x - cw, c.y}, {c.x + cw, c.y}, {c.x + cw, c.y - tall}, {c.x - cw, c.y - tall}, brick);
        fill_quad({c.x - cw, c.y}, {c.x, c.y}, {c.x, c.y - tall}, {c.x - cw, c.y - tall}, shade(brick, 1.2f));
        for (float row = 2.0f; row < tall; row += 2.0f) DrawLineV({c.x - cw, c.y - row}, {c.x + cw, c.y - row}, lit(shade(brick, 0.75f)));
        fill_quad({c.x - cw - 0.6f * k, c.y - tall}, {c.x + cw + 0.6f * k, c.y - tall}, {c.x + cw + 0.6f * k, c.y - tall - 1.2f * k},
                  {c.x - cw - 0.6f * k, c.y - tall - 1.2f * k}, shade(brick, 0.7f));
    }
    if (stage >= 3) rubble_heap(on_terrain(map, {x1 + 0.1f * s, y0 + (y1 - y0) * 0.6f}), 12.0f * k, 3.5f * k, {190, 182, 168, 255}, {150, 86, 62, 255}, h + 9u, 2);
    if (rnd(11) < 0.45f && damage < 0.6f) {
        const Vector2 b = on_terrain(map, {xm - (xm - x0) * 0.4f, ym}, wall_h + roof_h * 0.8f);
        fine_line(b, {b.x, b.y - 11.0f * k}, {70, 70, 70, 255});
        for (const float up : {8.0f, 10.5f}) fine_line({b.x - 3.5f * k, b.y - up * k + 1.0f}, {b.x + 3.5f * k, b.y - up * k - 1.0f}, {70, 70, 70, 255});
    } else if (rnd(21) < 0.35f && damage < 0.6f) {  // a satellite dish on the lit wall
        const Vector2 d = lit_face.at(0.97f, wall_h - 2.5f * k);
        DrawEllipse(static_cast<int>(d.x + 1.0f * k), static_cast<int>(d.y), 2.2f * k, 2.6f * k, lit({226, 226, 220, 255}));
        DrawEllipse(static_cast<int>(d.x + 1.4f * k), static_cast<int>(d.y + 0.4f * k), 1.4f * k, 1.8f * k, lit({180, 180, 176, 255}));
        DrawLineV({d.x + 1.0f * k, d.y}, {d.x + 3.5f * k, d.y + 1.0f * k}, lit({90, 90, 90, 255}));
    }
    // The shop: its sign over the door (ПРОДУКТЫ), the counter's window lit, a crate or two by the door.
    if (shop && stage < 3) {
        const Face front{base[3], base[2]};  // (left to right, to read)
        const float fpx = front.px();
        const float sign0 = 0.5f - 18.0f * fpx;
        face_fill(front, sign0 - 2.0f * fpx, sign0 + 36.0f * fpx, wall_h - 1.5f * k - 8.0f, wall_h - 1.5f * k, {40, 84, 170, 255});
        face_line(front, sign0 - 2.0f * fpx, wall_h - 1.5f * k - 8.0f, sign0 + 36.0f * fpx, wall_h - 1.5f * k - 8.0f, {220, 220, 214, 255});
        sign_text(front, sign0, wall_h - 1.5f * k - 6.5f, {246, 244, 236, 255});
        const Vector2 crate = on_terrain(map, {x1 - 0.3f * (x1 - x0), y1 + 0.12f});
        DrawRectangleRec({crate.x - 3.0f, crate.y - 4.0f, 6.0f, 4.0f}, lit({150, 116, 70, 255}));
        DrawLineV({crate.x - 3.0f, crate.y - 2.0f}, {crate.x + 3.0f, crate.y - 2.0f}, lit({110, 84, 50, 255}));
    }
    if (fenced) {
        // The near sides: the gate for a car and the wicket where the path comes out, the bench by them.
        const float door_x = x1 - 0.78f * (x1 - x0);
        const float span = px1 - px0;
        const float wicket0 = (door_x - 0.08f - px0) / span;
        const float wicket1 = (door_x + 0.08f - px0) / span;
        const bool gate_right = door_x + 0.5f < px1;
        const float gate0 = gate_right ? wicket1 + 0.02f / span : wicket0 - 0.4f / span;
        const float gate1 = gate_right ? wicket1 + 0.4f / span : wicket0 - 0.02f / span;
        plot_fence(map, {px1, py0}, {px1, py1}, sheets, fence_paint, fence_h, 1.0f);
        if (!shop) {
            plot_fence(map, {px0, py1}, {px1, py1}, sheets, fence_paint, fence_h, 0.74f, std::min(wicket0, gate0), std::max(wicket1, gate1));
            const Face front{on_terrain(map, {px0, py1}), on_terrain(map, {px1, py1})};
            const float fpx = front.px();
            const Color leaf = shade(sheets ? fence_paint : shade(fence_paint, 0.9f), 0.8f);
            // The gate's two leaves, shut; its posts.
            face_fill(front, gate0, gate1, 0.5f, fence_h + 1.0f, leaf);
            for (float u = gate0; u < gate1; u += 3.0f * fpx) face_line(front, u, 0.5f, u, fence_h + 1.0f, shade(leaf, 0.75f));
            face_line(front, (gate0 + gate1) * 0.5f, 0.5f, (gate0 + gate1) * 0.5f, fence_h + 1.0f, shade(leaf, 0.5f));
            for (const float u : {gate0, gate1, wicket0, wicket1}) face_fill(front, u - fpx, u + fpx, 0.0f, fence_h + 2.5f * k * 0.6f, {70, 64, 58, 255});
            // The wicket, ajar.
            const Vector2 hinge = front.at(wicket0, 0.5f);
            const Vector2 swing = on_terrain(map, {px0 + span * wicket0 + 0.1f, py1 - 0.09f});
            fill_quad(hinge, swing, {swing.x, swing.y - fence_h}, {hinge.x, hinge.y - fence_h}, shade(leaf, 1.1f));
            // A bench outside by the gate, for the old women of an evening.
            if (rnd(33) < 0.75f) {
                const float bu = gate_right ? wicket0 - 0.28f / span : wicket1 + 0.06f / span;
                const Vector2 b0 = on_terrain(map, {px0 + span * bu, py1 + 0.1f});
                const Vector2 b1 = on_terrain(map, {px0 + span * (bu + 0.22f / span), py1 + 0.1f});
                const float seat = 2.5f * k;
                fill_quad({b0.x, b0.y - seat}, {b1.x, b1.y - seat}, {b1.x, b1.y - seat - 1.5f}, {b0.x, b0.y - seat - 1.5f}, {140, 104, 66, 255});
                for (const Vector2& leg : {b0, b1}) DrawLineV({leg.x + (leg.x == b0.x ? 1.0f : -1.0f), leg.y}, {leg.x + (leg.x == b0.x ? 1.0f : -1.0f), leg.y - seat},
                                                           lit({96, 72, 48, 255}));
            }
        }
    }
}

// A house on one tile (where its building isn't known, as remembered).
void draw_house(const engine::TileMap& map, int tx, int ty, float damage) {
    draw_house(map, {static_cast<float>(tx), static_cast<float>(ty), 1.0f, 1.0f}, tile_hash(tx, ty), damage);
}

// A building's box on the ground: its corners at the foot and at the top
// of its walls; its two walls in view: +x in the light, +y in shade.
struct Box {
    Vector2 base[4];
    Vector2 top[4];
    Face lit_face;
    Face shade_face;
};
Box box_on(const engine::TileMap& map, Rectangle r, float wall) {
    Box b{};
    const Vector2 ground[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}};
    for (int i = 0; i < 4; ++i) {
        b.base[i] = on_terrain(map, ground[i]);
        b.top[i] = {b.base[i].x, b.base[i].y - wall};
    }
    b.lit_face = {b.base[1], b.base[2]};
    b.shade_face = {b.base[2], b.base[3]};
    return b;
}

// A gable roof over a box, its ridge along x or along y, the eaves out a
// little; the gable end in view filled in with `gable`.
void gable_roof(const engine::TileMap& map, Rectangle r, float wall, float rise, bool along_x, Roof kind, Color roof, Color gable,
                uint32_t seed, float eave = 0.06f) {
    const Box b = box_on(map, r, wall);
    Vector2 e[4];
    const Vector2 out[4] = {{r.x - eave, r.y - eave}, {r.x + r.width + eave, r.y - eave}, {r.x + r.width + eave, r.y + r.height + eave},
                            {r.x - eave, r.y + r.height + eave}};
    for (int i = 0; i < 4; ++i) e[i] = on_terrain(map, out[i], wall - 1.0f);
    const float xm = r.x + r.width * 0.5f;
    const float ym = r.y + r.height * 0.5f;
    if (along_x) {
        const Vector2 r0 = on_terrain(map, {r.x - eave, ym}, wall + rise);
        const Vector2 r1 = on_terrain(map, {r.x + r.width + eave, ym}, wall + rise);
        roof_texture({e[0], e[1], r0, r1}, kind, roof, 1.1f, seed);
        fill_triangle(b.top[1], b.top[2], on_terrain(map, {r.x + r.width, ym}, wall + rise - 1.0f), shade(gable, 0.95f));
        roof_texture({e[3], e[2], r0, r1}, kind, roof, 0.82f, seed + 1);
        DrawLineEx(r0, r1, 1.5f, lit(shade(roof, 0.6f)));
    } else {
        const Vector2 r0 = on_terrain(map, {xm, r.y - eave}, wall + rise);
        const Vector2 r1 = on_terrain(map, {xm, r.y + r.height + eave}, wall + rise);
        roof_texture({e[0], e[3], r0, r1}, kind, roof, 1.1f, seed);
        roof_texture({e[1], e[2], r0, r1}, kind, roof, 0.95f, seed + 1);
        fill_triangle(b.top[2], b.top[3], on_terrain(map, {xm, r.y + r.height}, wall + rise - 1.0f), shade(gable, 0.68f));
        DrawLineEx(r0, r1, 1.5f, lit(shade(roof, 0.6f)));
    }
    DrawLineV(e[1], e[2], lit(shade(roof, 0.55f)));
    DrawLineV(e[2], e[3], lit(shade(roof, 0.5f)));
}

// Holes in a roof where it's been hit: the dark inside, the rafters across.
void roof_holes(const engine::TileMap& map, Rectangle r, float z, float damage, Color roof, uint32_t seed) {
    if (damage <= 0.3f) return;
    const int holes = 1 + static_cast<int>(damage * 3.0f * std::sqrt(r.width * r.height));
    for (int k = 0; k < holes; ++k) {
        const Vector2 g{r.x + r.width * (0.15f + 0.7f * rand01(seed, 60 + k)), r.y + r.height * (0.15f + 0.7f * rand01(seed, 70 + k))};
        const Vector2 c = on_terrain(map, g, z);
        const float rr = 2.5f + 3.0f * damage;
        DrawEllipse(static_cast<int>(c.x), static_cast<int>(c.y), rr + 1.0f, rr * 0.6f + 0.8f, lit(shade(roof, 0.55f)));
        DrawEllipse(static_cast<int>(c.x), static_cast<int>(c.y), rr, rr * 0.6f, lit({22, 18, 16, 255}));
        for (const float d : {-1.5f, 1.2f}) DrawLineV({c.x - rr, c.y + d - 0.5f}, {c.x + rr, c.y + d + 0.8f}, lit({92, 70, 50, 255}));
        if (k < 2) fire_spot(c, stage_of(damage) == 2, true);
    }
}

// A roof burnt and fallen in (the worst of it): the dark inside of the
// walls, the soot up the far ones' insides; the charred rafters left over
// it, burnt through and short (a works' steel trusses whole but bent), a
// few bits of its cover still on them; the gable in view standing, black;
// smoke still coming up out of it.
void fallen_roof(const engine::TileMap& map, Rectangle r, float wall, float rise, bool along_x, bool gable, Color walls, Color cover, bool steel,
                 uint32_t seed) {
    const Box b = box_on(map, r, wall);
    fill_quad(b.top[0], b.top[1], b.top[2], b.top[3], {40, 36, 33, 255});
    for (int i = 0; i < 18 + static_cast<int>(r.width * r.height * 10.0f); ++i) {  // what fell in: ash, bits of the cover, beams
        const Vector2 g{r.x + r.width * (0.12f + 0.76f * rand01(seed, 500 + i)), r.y + r.height * (0.12f + 0.76f * rand01(seed, 1500 + i))};
        const Vector2 p = on_terrain(map, g, wall - 2.0f);
        const float a = rand01(seed, 2500 + i) * 3.14159f;
        switch (i % 3) {
            case 0: DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), 3.0f, 1.5f, lit({74, 68, 62, 255})); break;
            case 1:
                fill_quad({p.x - 2.5f, p.y - 1.0f}, {p.x + 2.5f, p.y - 1.5f}, {p.x + 2.5f, p.y + 0.5f}, {p.x - 2.5f, p.y + 1.0f},
                          shade(cover, 0.5f + 0.3f * rand01(seed, 3500 + i)));
                break;
            default: DrawLineV(p, {p.x + std::cos(a) * 5.0f, p.y + std::sin(a) * 2.5f}, lit({30, 24, 20, 255})); break;
        }
    }
    for (const Face& f : {Face{b.base[0], b.base[1]}, Face{b.base[0], b.base[3]}}) {  // the far walls' insides
        face_fill(f, 0.0f, 1.0f, wall - 7.0f, wall - 3.0f, {44, 38, 34, 255});
        face_fill(f, 0.0f, 1.0f, wall - 3.0f, wall, shade(walls, 0.42f));
        face_line(f, 0.0f, wall, 1.0f, wall, shade(walls, 0.7f));
    }
    const float xm = r.x + r.width * 0.5f;
    const float ym = r.y + r.height * 0.5f;
    auto eave_a = [&](float t) { return along_x ? on_terrain(map, {r.x + r.width * t, r.y}, wall) : on_terrain(map, {r.x, r.y + r.height * t}, wall); };
    auto eave_b = [&](float t) {
        return along_x ? on_terrain(map, {r.x + r.width * t, r.y + r.height}, wall) : on_terrain(map, {r.x + r.width, r.y + r.height * t}, wall);
    };
    auto ridge = [&](float t) { return along_x ? on_terrain(map, {r.x + r.width * t, ym}, wall + rise) : on_terrain(map, {xm, r.y + r.height * t}, wall + rise); };
    const float len = along_x ? r.width : r.height;
    const int n = std::max(3, static_cast<int>(len / (steel ? 0.3f : 0.2f)));
    const Color beam = steel ? Color{86, 82, 78, 255} : Color{76, 62, 52, 255};
    for (int i = 0; i < n; ++i) {  // bits of the cover left on the far slope
        if (rand01(seed, 400 + i) > (steel ? 0.35f : 0.2f)) continue;
        const float t0 = static_cast<float>(i) / static_cast<float>(n);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(n);
        const float up = 0.3f + 0.4f * rand01(seed, 420 + i);
        fill_quad(eave_a(t0), eave_a(t1), lerp(eave_a(t1), ridge(t1), up), lerp(eave_a(t0), ridge(t0), up), shade(cover, 0.62f));
    }
    for (int i = 0; i <= n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        if (rand01(seed, 300 + i) < (steel ? 0.15f : 0.4f)) continue;  // burnt through, fallen in
        const Vector2 a = eave_a(t);
        const Vector2 c = eave_b(t);
        Vector2 top = ridge(t);
        if (steel) top.y += 3.0f * rand01(seed, 340 + i);  // bent in the heat
        const float ka = steel ? 1.0f : 0.45f + 0.55f * rand01(seed, 320 + i);
        const float kc = steel ? 1.0f : 0.45f + 0.55f * rand01(seed, 360 + i);
        DrawLineEx(a, lerp(a, top, ka), 1.6f, lit(beam));
        DrawLineEx(c, lerp(c, top, kc), 1.6f, lit(shade(beam, 1.25f)));
        if (steel) {  // the truss's tie and its web
            DrawLineV(a, c, lit(beam));
            DrawLineV(lerp(a, c, 0.25f), lerp(a, top, 0.5f), lit(beam));
            DrawLineV(lerp(a, c, 0.75f), lerp(c, top, 0.5f), lit(beam));
            DrawLineV(lerp(a, c, 0.5f), top, lit(beam));
        }
    }
    const float r0 = rand01(seed, 380) * 0.3f;  // the ridge beam, what's left of it
    const float r1 = 1.0f - rand01(seed, 381) * (steel ? 0.1f : 0.5f);
    DrawLineEx(ridge(r0), ridge(r1), 1.8f, lit(beam));
    if (steel) {  // the purlins
        for (const float up : {0.33f, 0.66f}) DrawLineV(lerp(eave_a(0.0f), ridge(0.0f), up), lerp(eave_a(1.0f), ridge(1.0f), up), lit(beam));
    }
    if (gable) {  // the gable in view standing, black
        const Vector2 g = along_x ? on_terrain(map, {r.x + r.width, ym}, wall + rise - 1.0f) : on_terrain(map, {xm, r.y + r.height}, wall + rise - 1.0f);
        const Vector2 p = along_x ? b.top[1] : b.top[2];
        const Vector2 q = along_x ? b.top[2] : b.top[3];
        fill_triangle(p, q, g, shade(walls, along_x ? 0.5f : 0.36f));
        fill_triangle(lerp(p, q, 0.2f), lerp(p, q, 0.8f), lerp(lerp(p, q, 0.5f), g, 0.8f), ColorAlpha({20, 18, 16, 255}, 0.55f));
    }
    fire_spot(on_terrain(map, {xm, ym}, wall * 0.4f), false);
    if (len > 1.5f) fire_spot(on_terrain(map, along_x ? Vector2{r.x + r.width * 0.25f, ym} : Vector2{xm, r.y + r.height * 0.25f}, wall * 0.4f), false);
}

// Sheets of a wall's corrugated iron blown off, `share` of them, between
// `v0` and `v1`: the dark inside between the posts.
void sheets_off(const Face& f, float v0, float v1, float share, uint32_t seed) {
    const float px = f.px();
    int i = 0;
    for (float u = 0.0f; u < 1.0f - 4.0f * px; u += 8.0f * px, ++i) {
        if (rand01(seed, 600 + i) > share) continue;
        face_fill(f, u + 0.5f * px, u + 7.5f * px, v0 + 3.0f * rand01(seed, 800 + i), v1 - 1.0f - 6.0f * rand01(seed, 700 + i), {24, 20, 18, 255});
    }
}

// A wide door of planks, two leaves, braced in a Z; one hanging open, gone, when it's been hit.
void plank_doors(const Face& f, float u, float w, float h, Color c, float k, bool broken, uint32_t seed) {
    const float px = f.px();
    const float du = w * 0.5f * px;
    face_fill(f, u - du - px, u + du + px, 0.0f, h + 1.0f, shade(c, 0.55f * k));
    for (const float side : {-1.0f, 1.0f}) {
        const float a = side < 0.0f ? u - du : u + px * 0.5f;
        const float b = side < 0.0f ? u - px * 0.5f : u + du;
        if (broken && side > 0.0f) {
            face_fill(f, a, b, 0.0f, h, {20, 18, 16, 255});
            continue;
        }
        wall_texture(f, a, b, 0.0f, h, Wall::Planks, shade(c, k), seed + (side > 0.0f ? 7u : 0u));
        face_line(f, a, 1.0f, b, 1.0f, shade(c, 0.7f * k));
        face_line(f, a, h - 1.0f, b, h - 1.0f, shade(c, 0.7f * k));
        face_line(f, a, 1.0f, b, h - 1.0f, shade(c, 0.78f * k));
    }
}

// A band of small panes under the eaves (a works' ribbon glazing), many broken.
void ribbon_glazing(const Face& f, float u0, float u1, float v0, float v1, float k, float broken, uint32_t seed) {
    const float px = f.px();
    face_fill(f, u0, u1, v0 - 1.0f, v1 + 1.0f, shade({90, 96, 100, 255}, k));
    int i = 0;
    for (float u = u0; u < u1 - px; u += 3.0f * px, ++i) {
        for (int row = 0; row < 2; ++row) {
            const float va = v0 + (v1 - v0) * static_cast<float>(row) / 2.0f;
            const float vb = v0 + (v1 - v0) * static_cast<float>(row + 1) / 2.0f - 0.6f;
            const bool gone = rand01(seed, i * 2 + row) < broken;
            face_fill(f, u + px * 0.5f, std::min(u1, u + 2.5f * px), va, vb,
                      gone ? Color{24, 22, 20, 255} : shade(rand01(seed, 900 + i) < 0.5f ? Color{120, 144, 160, 255} : Color{92, 112, 126, 255}, k));
        }
    }
}

// A barn or a machine shed: one long building with a gable roof and big doors.
void draw_barn(const engine::TileMap& map, const engine::Structure& s, float damage);

// Ground rectangle covering a structure's tiles, shrunk by `inset` tiles.
// A building drawn bigger than life against its tiles (see
// building_scale): footprint() shrinks it about its middle by this much,
// and it's drawn scaled up about that middle as much again (scaled_about):
// its walls, floors, windows that much bigger, on the same ground.
float g_shrink = 1.0f;

Rectangle footprint(const engine::Structure& s, float inset) {
    int x0 = s.tiles.front().x, x1 = x0, y0 = s.tiles.front().y, y1 = y0;
    for (const engine::TilePos& t : s.tiles) {
        x0 = std::min(x0, t.x);
        x1 = std::max(x1, t.x);
        y0 = std::min(y0, t.y);
        y1 = std::max(y1, t.y);
    }
    const auto w = static_cast<float>(x1 - x0 + 1);
    const auto h = static_cast<float>(y1 - y0 + 1);
    const float cx = static_cast<float>(x0) + w * 0.5f;
    const float cy = static_cast<float>(y0) + h * 0.5f;
    const float k = 1.0f / g_shrink;
    return {cx + (-w * 0.5f + inset) * k, cy + (-h * 0.5f + inset) * k, (w - 2 * inset) * k, (h - 2 * inset) * k};
}

// How much bigger than its tiles would have it a building is drawn, to
// stand as it should against the men and the tanks: a block of flats' floors
// each as high as a tank, a mast, a grain elevator's silos towering, a
// works' shop floor, a cowshed, the offices, the station.
float building_scale(const engine::Structure& s) {
    switch (s.type) {
        case engine::StructureType::Apartment: return 1.8f;
        case engine::StructureType::CellTower: return 1.8f;
        case engine::StructureType::Elevator: return 1.5f;
        case engine::StructureType::GasStation: return 1.3f;
        case engine::StructureType::Headquarters: return 1.5f;
        case engine::StructureType::Station: return 1.4f;
        case engine::StructureType::House:
            if (s.look == engine::HouseLook::Factory) return 1.4f;
            if (s.look == engine::HouseLook::Cowshed) return 1.6f;
            if (s.look == engine::HouseLook::Coop) return 1.4f;
            return s.tiles.size() >= engine::kSpaciousTiles ? 1.3f : 1.0f;  // a barn; a house is drawn to its plot's size
        default: return 1.0f;
    }
}

// Draws `fn` `k` times bigger about `anchor` (on the screen), footprint()
// shrunk by as much meanwhile: the fires and the fine lines it leaves for
// the baking moved to where they end up.
void fine_line(Vector2 a, Vector2 b, Color c);
template <typename Fn>
void scaled_about(Vector2 anchor, float k, Fn&& fn) {
    const size_t fires0 = g_fires ? g_fires->size() : 0;
    const size_t fine0 = g_fine ? g_fine->size() : 0;
    const float shrink = g_shrink;
    g_shrink = k;
    rlDrawRenderBatchActive();
    rlPushMatrix();
    rlTranslatef(anchor.x, anchor.y, 0.0f);
    rlScalef(k, k, 1.0f);
    rlTranslatef(-anchor.x, -anchor.y, 0.0f);
    fn();
    rlDrawRenderBatchActive();
    rlPopMatrix();
    g_shrink = shrink;
    auto moved = [&](Vector2 p) { return Vector2{anchor.x + (p.x - anchor.x) * k, anchor.y + (p.y - anchor.y) * k}; };
    if (g_fires) {
        for (size_t i = fires0; i < g_fires->size(); ++i) (*g_fires)[i].at = moved((*g_fires)[i].at);
    }
    if (g_fine) {
        for (size_t i = fine0; i < g_fine->size(); ++i) {
            (*g_fine)[i].a = moved((*g_fine)[i].a);
            (*g_fine)[i].b = moved((*g_fine)[i].b);
        }
    }
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
void sandbags(const engine::TileMap& map, Vector2 g0, Vector2 g1, int rows, uint32_t seed);  // with the players' buildings, below

// A pillbox, a log-and-earth firing point: walls of logs in courses, their
// ends crossing at the corners; earth banked up against them; a roof of
// logs under a mound of turf. Its embrasure a dark slit under a log lintel
// on the side it faces, sandbags either side of it. Being built: its log
// walls going up course by course, no roof yet. Shelled: its roof caved in.
void draw_pillbox(const engine::TileMap& map, const engine::Structure& s) {
    const engine::StructureDef& def = engine::structure_type(s.type);
    const Vector2 c{static_cast<float>(s.tiles.front().x) + 0.5f, static_cast<float>(s.tiles.front().y) + 0.5f};
    Vector2 f = to_vector2(s.facing);
    const float fl = std::hypot(f.x, f.y);
    f = fl > 0.0f ? Vector2{f.x / fl, f.y / fl} : Vector2{0.7071f, -0.7071f};
    const Vector2 side{-f.y, f.x};  // (a shell hole's to one side)
    const uint32_t h = s.id * 2654435761u;
    const float done = s.built ? 1.0f : std::clamp(static_cast<float>(s.build_progress) / static_cast<float>(std::max<engine::Tick>(1, def.build_time)), 0.0f, 1.0f);
    const float damage = s.built ? 1.0f - static_cast<float>(s.hp) / static_cast<float>(std::max(1, def.max_hp)) : 0.0f;
    constexpr float kHalf = 0.33f;
    constexpr float kCourse = 2.6f;
    constexpr int kCourses = 3;
    const int courses = s.built ? kCourses : std::max(1, static_cast<int>(done * static_cast<float>(kCourses + 1)));
    const float wall = kCourse * static_cast<float>(std::min(courses, kCourses));
    const Color log{132, 97, 64, 255};
    const Color seam{70, 50, 34, 255};
    const Color end{202, 170, 122, 255};
    const Color earth{120, 98, 70, 255};
    const Color turf{98, 112, 62, 255};
    // Its corners round it (square to the map), and its sides: each from one
    // corner to the next, facing out. The embrasure in the side most towards
    // the front, the nearer one of two as much so.
    const Vector2 corner[4] = {{c.x - kHalf, c.y - kHalf}, {c.x + kHalf, c.y - kHalf}, {c.x + kHalf, c.y + kHalf}, {c.x - kHalf, c.y + kHalf}};
    const Vector2 out[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    int slit = 0;
    for (int k = 1; k < 4; ++k) {
        const float dk = out[k].x * f.x + out[k].y * f.y + (out[k].x + out[k].y > 0.0f ? 0.05f : 0.0f);
        const float ds = out[slit].x * f.x + out[slit].y * f.y + (out[slit].x + out[slit].y > 0.0f ? 0.05f : 0.0f);
        if (dk > ds) slit = k;
    }
    auto light_of = [](Vector2 n) { return 0.86f + 0.14f * (n.x - n.y); };  // (lit on the right, as the houses are)
    fill_ground_ellipse(on_terrain(map, c), kHalf + 0.24f, {104, 86, 62, 255});  // the trodden, dug ground round it
    for (int k = 0; k < 4; ++k) {
        const Vector2 n = out[k];
        if (n.x + n.y <= 0.0f) continue;  // (a side turned away)
        const Vector2 a = corner[k];
        const Vector2 b = corner[(k + 1) % 4];
        const float lk = light_of(n);
        const bool embrasure = k == slit && s.built;
        // The logs, course over course: each lit along its top, a dark seam under it.
        for (int course = 0; course < std::min(courses, kCourses); ++course) {
            const float z0 = kCourse * static_cast<float>(course);
            const float z1 = z0 + kCourse;
            fill_quad(on_terrain(map, a, z0), on_terrain(map, b, z0), on_terrain(map, b, z1), on_terrain(map, a, z1), shade(log, lk * (0.92f + 0.12f * rand01(h, course * 4 + k))));
            DrawLineV(on_terrain(map, a, z1 - 0.6f), on_terrain(map, b, z1 - 0.6f), lit(shade(log, lk * 1.18f)));
            DrawLineV(on_terrain(map, a, z0 + 0.3f), on_terrain(map, b, z0 + 0.3f), lit(shade(seam, lk)));
            for (const Vector2& e : {a, b}) {  // their ends crossing at the corners
                const Vector2 p = on_terrain(map, {e.x + n.x * 0.03f, e.y + n.y * 0.03f}, z0 + kCourse * 0.5f);
                disc(p, 1.3f, lit(end));
            }
        }
        if (embrasure) {  // the slit under a lintel, the dark inside
            const Vector2 a1 = lerp(a, b, 0.27f);
            const Vector2 b1 = lerp(a, b, 0.73f);
            fill_quad(on_terrain(map, a1, wall - 4.0f), on_terrain(map, b1, wall - 4.0f), on_terrain(map, b1, wall - 0.8f), on_terrain(map, a1, wall - 0.8f), {18, 15, 12, 255});
            DrawLineEx(on_terrain(map, lerp(a, b, 0.2f), wall - 0.4f), on_terrain(map, lerp(a, b, 0.8f), wall - 0.4f), 1.6f, lit(shade(log, 1.1f)));
        }
        // Earth banked up against it (lower below the slit), out to the ground.
        const float bank = embrasure ? 1.2f : kCourse;
        if (s.built) {
            const Vector2 a2{a.x + n.x * 0.17f, a.y + n.y * 0.17f};
            const Vector2 b2{b.x + n.x * 0.17f, b.y + n.y * 0.17f};
            fill_quad(on_terrain(map, a, bank), on_terrain(map, b, bank), on_terrain(map, b2, 0.0f), on_terrain(map, a2, 0.0f), shade(earth, lk * 0.95f));
            for (int i = 0; i < 10; ++i) {  // clods, tufts on it
                const float u = rand01(h, 60 + i + k * 10);
                const float v = rand01(h, 90 + i + k * 10);
                const Vector2 g = lerp(lerp(a, b, u), lerp(a2, b2, u), v);
                const Vector2 p = on_terrain(map, g, bank * (1.0f - v));
                DrawRectangleRec({std::round(p.x), std::round(p.y), 1.0f, 1.0f}, lit(i % 3 == 0 ? turf : shade(earth, 0.8f)));
            }
        }
    }
    if (!s.built) return;
    // The roof: logs under a mound of turf, a little proud of the walls; caved in when shelled hard.
    const float rise = damage > 0.7f ? 1.0f : 3.0f;
    Vector2 eave[4];
    Vector2 top[4];
    for (int k = 0; k < 4; ++k) {
        const Vector2 d{corner[k].x - c.x, corner[k].y - c.y};
        eave[k] = on_terrain(map, {c.x + d.x * 1.12f, c.y + d.y * 1.12f}, wall);
        top[k] = on_terrain(map, {c.x + d.x * 0.5f, c.y + d.y * 0.5f}, wall + rise);
    }
    for (int k = 0; k < 4; ++k) {
        const Vector2 n = out[k];
        fill_quad(eave[k], eave[(k + 1) % 4], top[(k + 1) % 4], top[k], shade(turf, light_of(n) * (n.x + n.y > 0.0f ? 1.0f : 1.12f)));
    }
    fill_quad(top[0], top[1], top[2], top[3], shade(turf, 1.08f));
    for (int k = 0; k < 4; ++k) {  // the roof's logs, their ends out under the turf at the eaves
        const Vector2 n = out[k];
        if (n.x + n.y <= 0.0f) continue;
        for (int i = 1; i < 5; ++i) disc(lerp(eave[k], eave[(k + 1) % 4], static_cast<float>(i) / 5.0f), 1.2f, lit(end));
    }
    for (int i = 0; i < 26; ++i) {  // the turf's grass and earth
        const Vector2 g{c.x + (rand01(h, 200 + i) - 0.5f) * kHalf * 1.4f, c.y + (rand01(h, 300 + i) - 0.5f) * kHalf * 1.4f};
        const Vector2 p = on_terrain(map, g, wall + rise * 0.9f);
        DrawRectangleRec({std::round(p.x), std::round(p.y), 1.0f + static_cast<float>(i % 2), 1.0f}, lit(i % 4 == 0 ? earth : shade(turf, 1.25f)));
    }
    if (damage > 0.45f) {  // a shell's hole in the roof
        const Vector2 p = on_terrain(map, {c.x + side.x * 0.08f, c.y + side.y * 0.08f}, wall + rise * 0.6f);
        DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), 4.0f, 2.0f, lit({34, 28, 22, 255}));
    }
    // Sandbags either side of the slit, out in front of it.
    const Vector2 n = out[slit];
    const Vector2 a{corner[slit].x + n.x * 0.2f, corner[slit].y + n.y * 0.2f};
    const Vector2 b{corner[(slit + 1) % 4].x + n.x * 0.2f, corner[(slit + 1) % 4].y + n.y * 0.2f};
    sandbags(map, a, lerp(a, b, 0.24f), 2, h);
    sandbags(map, lerp(a, b, 0.76f), b, 2, h + 1);
}

// A barn or a machine shed (a farm's): silicate blocks or whitewashed
// brick, a slate or rusty tin roof along it; big doors of planks in its
// long wall, a row of small high windows; the end wall's small door, the
// hay loft's opening in its gable.
void draw_barn(const engine::TileMap& map, const engine::Structure& s, float damage) {
    const uint32_t h = tile_hash(s.tiles.front().x * 7 + 3, s.tiles.front().y * 11 + 5);
    constexpr float kWall = 15.0f;
    constexpr float kRoof = 12.0f;
    const Rectangle r = footprint(s, 0.1f);
    const float soot = 1.0f - 0.3f * damage;
    const Box b = box_on(map, r, kWall);
    const bool along_x = r.width >= r.height;
    const bool blocks = (h & 1u) != 0;
    const Color wall = shade(blocks ? Color{186, 184, 174, 255} : Color{222, 220, 208, 255}, soot);
    const Wall kind = blocks ? Wall::Silicate : Wall::Whitewash;
    wall_texture(b.lit_face, 0.0f, 1.0f, 0.0f, kWall, kind, wall, h);
    wall_texture(b.shade_face, 0.0f, 1.0f, 0.0f, kWall, kind, shade(wall, 0.72f), h ^ 0x3333u);
    face_fill(b.lit_face, 0.0f, 1.0f, 0.0f, 2.0f, shade({110, 100, 90, 255}, soot));
    face_fill(b.shade_face, 0.0f, 1.0f, 0.0f, 2.0f, shade({110, 100, 90, 255}, 0.74f * soot));
    const int stage = stage_of(damage);
    const Color under = blocks ? Color{150, 146, 138, 255} : Color{150, 96, 70, 255};
    wear_walls(b.lit_face, 1.0f, 2.0f, kWall, stage, under, !blocks, h);
    wear_walls(b.shade_face, 0.72f, 2.0f, kWall, stage, under, !blocks, h + 5u);
    const Face& long_face = along_x ? b.shade_face : b.lit_face;
    const Face& end_face = along_x ? b.lit_face : b.shade_face;
    const float kl = along_x ? 0.72f : 1.0f;
    const float ke = along_x ? 1.0f : 0.72f;
    plank_doors(long_face, 0.5f, 18.0f, kWall * 0.8f, {122, 92, 62, 255}, kl * soot, damage > 0.5f, h);
    for (int i = 0; i < 6; ++i) {  // small high windows
        const float u = (static_cast<float>(i) + 0.5f) / 6.0f;
        if (std::fabs(u - 0.5f) < 0.18f) continue;
        window_on(long_face, u, kWall * 0.62f, 3.0f, 2.5f, {charred({150, 146, 138, 255}, damage), false}, damage > 0.25f && rand01(h, i) < damage, kl);
    }
    door_on(end_face, 0.3f, 3.0f, kWall * 0.6f, {110, 84, 58, 255}, ke * soot);
    const Roof cover = (h >> 3) & 1u ? Roof::Slate : Roof::Rusty;
    const Color roof = shade(cover == Roof::Slate ? Color{126, 128, 128, 255} : Color{126, 88, 64, 255}, soot);
    if (stage >= 3) return fallen_roof(map, r, kWall, kRoof, along_x, true, wall, roof, false, h);
    gable_roof(map, r, kWall, kRoof, along_x, cover, roof, wall, h);
    {  // the hay loft's opening in the gable in view
        const float xm = r.x + r.width * 0.5f;
        const float ym = r.y + r.height * 0.5f;
        const Vector2 g = along_x ? Vector2{r.x + r.width, ym} : Vector2{xm, r.y + r.height};
        const Vector2 c = on_terrain(map, g, kWall + kRoof * 0.35f);
        DrawRectangleRec({std::round(c.x - 2.0f), std::round(c.y - 3.0f), 4.0f, 4.0f}, lit({40, 32, 26, 255}));
    }
    roof_holes(map, r, kWall + kRoof * 0.5f, damage, roof, h);
}

// A works' shop floor, as the Donbas's industrial zones have them: brick
// up to the sills, piers of darker brick; corrugated iron above, faded blue
// or green, rust run down it; a band of small panes under the eaves, many
// broken; a big sliding gate in its end, braced; a low tin roof with a
// lantern of windows along its ridge; the big one with its brick chimney.
void draw_factory(const engine::TileMap& map, const engine::Structure& s, float damage) {
    const uint32_t h = tile_hash(s.tiles.front().x * 13 + 1, s.tiles.front().y * 3 + 7);
    constexpr float kWall = 22.0f;
    constexpr float kBrick = 9.0f;
    const Rectangle r = footprint(s, 0.08f);
    const float soot = 1.0f - 0.45f * damage;
    const bool along_x = r.width >= r.height;
    // The chimney, behind it.
    if (s.tiles.size() >= 10) {
        const Vector2 foot = on_terrain(map, {r.x - 0.1f, r.y + 0.35f});
        const float tall = 74.0f;
        const Color brick = shade({150, 76, 56, 255}, soot);
        fill_quad({foot.x - 5.0f, foot.y}, {foot.x + 5.0f, foot.y}, {foot.x + 3.0f, foot.y - tall}, {foot.x - 3.0f, foot.y - tall}, brick);
        fill_quad({foot.x - 5.0f, foot.y}, {foot.x - 1.0f, foot.y}, {foot.x - 0.8f, foot.y - tall}, {foot.x - 3.0f, foot.y - tall}, shade(brick, 1.18f));
        for (float v = 6.0f; v < tall; v += 9.0f) {  // its iron hoops
            const float w = 5.0f - 2.0f * v / tall;
            DrawLineV({foot.x - w, foot.y - v}, {foot.x + w, foot.y - v}, lit({60, 56, 52, 255}));
        }
        fill_quad({foot.x - 3.6f, foot.y - tall}, {foot.x + 3.6f, foot.y - tall}, {foot.x + 3.6f, foot.y - tall - 2.0f},
                  {foot.x - 3.6f, foot.y - tall - 2.0f}, {40, 34, 30, 255});
    }
    const Box b = box_on(map, r, kWall);
    static constexpr Color kIron[3] = {{112, 132, 146, 255}, {118, 138, 118, 255}, {150, 150, 146, 255}};
    const Color iron = shade(kIron[h % 3], soot);
    const Color brick = shade((h >> 2) & 1u ? Color{156, 82, 60, 255} : Color{184, 180, 170, 255}, soot);
    const int stage = stage_of(damage);
    for (const auto& [f, k] : {std::pair{&b.lit_face, 1.0f}, std::pair{&b.shade_face, 0.72f}}) {
        wall_texture(*f, 0.0f, 1.0f, 0.0f, kBrick, (h >> 2) & 1u ? Wall::RedBrick : Wall::Silicate, shade(brick, k), h);
        wall_texture(*f, 0.0f, 1.0f, kBrick, kWall, Wall::Corrugated, shade(iron, k), h + 5);
        const float px = f->px();
        for (float u = 0.0f; u <= 1.0f; u += 16.0f * px) {  // the piers, the posts over them
            face_fill(*f, u, std::min(1.0f, u + 3.0f * px), 0.0f, kBrick + 1.0f, shade(brick, 0.8f * k));
            face_line(*f, u + 1.5f * px, kBrick + 1.0f, u + 1.5f * px, kWall, shade(iron, 0.7f * k));
        }
        for (int i = 0; i < 6; ++i) {  // rust run down the iron
            const float u = rand01(h, 40 + i + static_cast<int>(k * 10.0f));
            face_line(*f, u, kWall - 5.0f, u, kWall - 9.0f - 6.0f * rand01(h, 50 + i), mix(shade(iron, k), {130, 74, 44, 255}, 0.6f));
        }
        ribbon_glazing(*f, 0.04f, 0.96f, kWall - 7.0f, kWall - 2.5f, k, 0.25f + damage, h + static_cast<uint32_t>(k * 100.0f));
        const auto side = static_cast<int>(k * 50.0f);
        if (stage >= 1) pockmarks(*f, 0.0f, 1.0f, 0.0f, kWall, stage * 14, h + static_cast<uint32_t>(side));
        if (stage >= 2) sheets_off(*f, kBrick + 1.0f, kWall, stage == 2 ? 0.2f : 0.45f, h + static_cast<uint32_t>(side));
        if (stage >= 3) burnt(*f, 0.0f, 1.0f, 0.0f, kWall, 0.45f, h + static_cast<uint32_t>(side));
    }
    // The gate in the end in view: sliding, braced, rusty; a door beside it.
    const Face& end_face = along_x ? b.lit_face : b.shade_face;
    const float ke = along_x ? 1.0f : 0.72f;
    {
        const float px = end_face.px();
        const float u0 = 0.25f;
        const float u1 = 0.7f;
        const float gh = kWall * 0.62f;
        const Color gate = shade({96, 104, 100, 255}, ke * soot);
        if (damage > 0.5f) {
            face_fill(end_face, u0, u1, 0.0f, gh, {20, 18, 16, 255});
        } else {
            wall_texture(end_face, u0, u1, 0.0f, gh, Wall::Corrugated, gate, h + 9);
            face_line(end_face, u0, 0.5f, u1, gh - 0.5f, shade(gate, 0.7f));
            face_line(end_face, u0, gh - 0.5f, u1, 0.5f, shade(gate, 0.7f));
            face_fill(end_face, u0, u1, gh * 0.4f, gh * 0.4f + 2.0f, mix(gate, {130, 74, 44, 255}, 0.4f));
        }
        face_line(end_face, u0 - 2.0f * px, gh + 1.0f, u1 + 6.0f * px, gh + 1.0f, shade({60, 60, 58, 255}, ke));  // its rail
        door_on(end_face, 0.85f, 3.2f, 7.0f, {70, 90, 96, 255}, ke * soot);
    }
    // The roof: low, tin, a lantern of windows along its ridge.
    const Roof cover = (h >> 4) & 1u ? Roof::Rusty : Roof::Tin;
    const Color roof = shade({120, 122, 122, 255}, soot);
    if (stage >= 3) return fallen_roof(map, r, kWall, 7.0f, along_x, true, iron, roof, true, h);
    gable_roof(map, r, kWall, 7.0f, along_x, cover, roof, iron, h);
    {
        const float inset = 0.18f;
        const Rectangle lantern = along_x ? Rectangle{r.x + r.width * inset, r.y + r.height * 0.5f - 0.2f, r.width * (1.0f - 2.0f * inset), 0.4f}
                                          : Rectangle{r.x + r.width * 0.5f - 0.2f, r.y + r.height * inset, 0.4f, r.height * (1.0f - 2.0f * inset)};
        const Box l = box_on(map, lantern, 0.0f);
        const float z0 = kWall + 7.0f - 1.0f;
        Vector2 lb[4];
        Vector2 lt[4];
        for (int i = 0; i < 4; ++i) {
            lb[i] = {l.base[i].x, l.base[i].y - z0};
            lt[i] = {lb[i].x, lb[i].y - 4.0f};
        }
        const Face lf{lb[1], lb[2]};
        const Face sf{lb[2], lb[3]};
        ribbon_glazing(lf, 0.0f, 1.0f, 0.5f, 3.5f, 1.0f, 0.2f + damage, h + 70);
        ribbon_glazing(sf, 0.0f, 1.0f, 0.5f, 3.5f, 0.72f, 0.2f + damage, h + 71);
        fill_quad(lt[0], lt[1], lt[2], lt[3], shade(roof, 1.05f));
    }
    roof_holes(map, r, kWall + 5.0f, damage, roof, h);
}


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
// doors in its ends; vents on the ridge of the long ones. Its yard behind
// a fence, the haystacks, the cows; the farm's water tower by the big one.
void draw_cowshed(const engine::TileMap& map, const engine::Structure& s, float damage) {
    const Rectangle r = footprint(s, 0.1f);
    const uint32_t h = tile_hash(s.tiles.front().x, s.tiles.front().y);
    const bool long_shed = s.tiles.size() >= engine::kSpaciousTiles;
    const float yard = long_shed ? 0.8f : 0.5f;
    draw_fence(map, {r.x - yard, r.y - yard, r.width + 2 * yard, r.height + 2 * yard}, {126, 100, 70, 255});
    const float kWall = long_shed ? 11.0f : 8.0f;
    const float kRoof = long_shed ? 8.0f : 6.0f;
    const float soot = 1.0f - 0.45f * damage;
    const Box b = box_on(map, r, kWall);
    const Color wall = shade({222, 218, 204, 255}, soot);
    wall_texture(b.lit_face, 0.0f, 1.0f, 0.0f, kWall, Wall::Whitewash, wall, h);
    wall_texture(b.shade_face, 0.0f, 1.0f, 0.0f, kWall, Wall::Whitewash, shade(wall, 0.74f), h + 3);
    face_fill(b.lit_face, 0.0f, 1.0f, 0.0f, 1.5f, shade({120, 110, 96, 255}, soot));
    face_fill(b.shade_face, 0.0f, 1.0f, 0.0f, 1.5f, shade({120, 110, 96, 255}, 0.74f * soot));
    const int stage = stage_of(damage);
    wear_walls(b.lit_face, 1.0f, 1.5f, kWall, stage, {150, 96, 70, 255}, true, h);
    wear_walls(b.shade_face, 0.74f, 1.5f, kWall, stage, {150, 96, 70, 255}, true, h + 7u);
    const bool along_x = r.width >= r.height;
    const Face& long_face = along_x ? b.shade_face : b.lit_face;
    const Face& end_face = along_x ? b.lit_face : b.shade_face;
    const float kl = along_x ? 0.74f : 1.0f;
    const int windows = long_shed ? 8 : 1;
    for (int k = 1; k <= windows; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(windows + 1);
        window_on(long_face, u, kWall * 0.55f, 3.0f, 2.5f, {charred({180, 176, 166, 255}, damage), false}, damage > 0.25f && rand01(h, k) < damage, kl);
    }
    plank_doors(end_face, 0.5f, long_shed ? 9.0f : 5.0f, kWall * 0.7f, {112, 86, 60, 255}, (along_x ? 1.0f : 0.74f) * soot, damage > 0.5f, h);
    const Color roof = shade({122, 126, 130, 255}, soot);
    if (stage >= 3) {
        fallen_roof(map, r, kWall, kRoof, along_x, true, wall, roof, false, h);
    } else {
        gable_roof(map, r, kWall, kRoof, along_x, Roof::Slate, roof, wall, h);
    }
    if (long_shed && stage < 3) {  // vents on the ridge
        for (const float t : {0.3f, 0.7f}) {
            const Vector2 g = along_x ? Vector2{r.x + r.width * t, r.y + r.height * 0.5f} : Vector2{r.x + r.width * 0.5f, r.y + r.height * t};
            const Vector2 c = on_terrain(map, g, kWall + kRoof);
            fill_quad({c.x - 2.0f, c.y}, {c.x + 2.0f, c.y}, {c.x + 2.0f, c.y - 4.0f}, {c.x - 2.0f, c.y - 4.0f}, shade(roof, 0.8f));
            fill_quad({c.x - 3.0f, c.y - 4.0f}, {c.x + 3.0f, c.y - 4.0f}, {c.x + 2.0f, c.y - 5.5f}, {c.x - 2.0f, c.y - 5.5f}, shade(roof, 0.62f));
        }
    }
    if (stage < 3) roof_holes(map, r, kWall + kRoof * 0.5f, damage, roof, h);
    // The yard: haystacks by the fence, the cows out in front (gone when it's been shelled).
    draw_haystack(map, {r.x + r.width + yard * 0.4f, r.y - yard * 0.4f}, (long_shed ? 1.0f : 0.8f) + 0.2f * static_cast<float>(h & 1));
    if (long_shed) draw_haystack(map, {r.x - 0.35f, r.y + r.height + 0.2f}, 0.9f);
    const int cows = stage >= 2 ? 0 : long_shed ? 4 : 1 + static_cast<int>((h >> 5) & 1u);
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

// A chicken coop: a little plank shed under a lean-to roof of slate, a run
// of wire netting beside it with the hens pecking about.
void draw_coop(const engine::TileMap& map, const engine::Structure& s, float damage) {
    const Rectangle r = footprint(s, 0.15f);
    const uint32_t h = tile_hash(s.tiles.front().x, s.tiles.front().y);
    const Rectangle run{r.x, r.y + r.height + 0.1f, r.width, 0.9f};
    for (int k = 0; k < (damage < 0.45f ? 7 : 0); ++k) {  // the hens (gone when it's been shelled)
        const Vector2 p = on_terrain(map, {run.x + 0.1f + (run.width - 0.2f) * hash_unit(h >> k), run.y + 0.1f + 0.7f * hash_unit(h >> (k + 9))});
        DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y - 1.5f), 1.8f, 1.2f, lit(k % 3 == 0 ? Color{150, 96, 50, 255} : Color{236, 232, 222, 255}));
        DrawCircleV({p.x + 1.4f, p.y - 2.4f}, 0.6f, lit({200, 40, 36, 255}));
    }
    draw_fence(map, run, {176, 176, 170, 255});
    constexpr float kWall = 7.0f;
    const float soot = 1.0f - 0.45f * damage;
    Box b = box_on(map, r, kWall);
    for (int i = 0; i < 2; ++i) b.top[i].y -= 3.0f;  // the lean-to: its back wall higher than its front
    const Color planks = shade({152, 128, 94, 255}, soot);
    wall_texture(b.lit_face, 0.0f, 1.0f, 0.0f, kWall, Wall::Planks, planks, h);
    fill_triangle(b.top[1], {b.top[1].x, b.top[1].y + 3.0f}, b.top[2], planks);
    wall_texture(b.shade_face, 0.0f, 1.0f, 0.0f, kWall, Wall::Planks, shade(planks, 0.74f), h + 1);
    door_on(b.shade_face, 0.5f, 3.0f, 5.0f, {92, 70, 50, 255}, 0.74f * soot);
    const int stage = stage_of(damage);
    if (stage >= 1) {
        for (const Face* f : {&b.lit_face, &b.shade_face}) pockmarks(*f, 0.0f, 1.0f, 0.0f, kWall, stage * 5, h + 3u);
    }
    if (stage < 3) {
        roof_texture({b.top[3], b.top[2], b.top[0], b.top[1]}, Roof::Slate, shade({110, 110, 108, 255}, soot), 1.0f, h);
        roof_holes(map, r, kWall + 2.0f, damage, {110, 110, 108, 255}, h);
        return;
    }
    // Burnt: the planks black, the roof gone but for a rafter or two.
    burnt(b.lit_face, 0.0f, 1.0f, 0.0f, kWall, 0.9f, h);
    burnt(b.shade_face, 0.0f, 1.0f, 0.0f, kWall, 0.9f, h + 1u);
    fill_quad(b.top[0], b.top[1], b.top[2], b.top[3], {24, 20, 18, 255});
    for (int i = 0; i < 4; ++i) {
        if (rand01(h, 90 + i) < 0.4f) continue;
        const float t = 0.12f + 0.25f * static_cast<float>(i);
        DrawLineEx(lerp(b.top[3], b.top[2], t), lerp(b.top[0], b.top[1], t), 1.4f, lit({42, 32, 26, 255}));
    }
    fire_spot(lerp(b.top[0], b.top[2], 0.5f), false);
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

// A village yard: a picket fence where it runs out into the fields
// (weathered, green or blue), and now a woodpile, a well under its little roof, a well's sweep
// (a zhuravel: the long pole on its post, the bucket on its rod), the
// street's cast-iron water pump, a haystack on its pole, a dog's kennel, a
// washing line, a bench, a vegetable bed.
void draw_yard(const engine::TileMap& map, int tx, int ty) {
    const auto fx = static_cast<float>(tx);
    const auto fy = static_cast<float>(ty);
    const uint32_t h = tile_hash(tx * 13 + 5, ty * 11 + 9);
    auto rnd = [h](int i) { return hash_unit(tile_hash(static_cast<int>(h >> 7) + i * 23, i * 17 + 1)); };
    auto at = [&](float u, float v, float lift = 0.0f) { return on_terrain(map, {fx + u, fy + v}, lift); };
    static constexpr Color kPaint[3] = {{142, 128, 106, 255}, {86, 122, 82, 255}, {92, 122, 162, 255}};
    const Color wood = kPaint[static_cast<int>(rnd(0) * 3.0f)];
    auto fence = [&](Vector2 a, Vector2 b) {
        for (int k = 0; k <= 8; ++k) {
            const float t = static_cast<float>(k) / 8.0f;
            const Vector2 g{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            const Vector2 foot = at(g.x, g.y);
            DrawLineEx(foot, {foot.x, foot.y - 5.5f}, 1.4f, lit(k % 4 == 0 ? shade(wood, 0.7f) : wood));
        }
        for (const float lift : {2.0f, 4.5f}) DrawLineV(at(a.x, a.y, lift), at(b.x, b.y, lift), lit(shade(wood, 0.75f)));
    };
    // (The plots have their own fences now: on the streets, one only where a yard runs out into the fields.)
    auto open_side = [&](int dx, int dy) {
        const int x = tx + dx;
        const int y = ty + dy;
        if (!map.contains_tile(x, y)) return false;
        const engine::Terrain t = map.terrain(x, y);
        return t == engine::Terrain::Grass || t == engine::Terrain::Wheat || t == engine::Terrain::Garden || t == engine::Terrain::Plowed;
    };
    if (open_side(1, 0) && rnd(1) < 0.6f) fence({1.0f, 0.02f}, {1.0f, 0.98f});
    if (open_side(0, 1) && rnd(2) < 0.6f) fence({0.02f, 1.0f}, {0.98f, 1.0f});
    const int item = static_cast<int>(rnd(3) * 12.0f);
    const Vector2 spot = at(0.3f + 0.4f * rnd(4), 0.3f + 0.4f * rnd(5));
    switch (item) {
        case 0:
        case 1: {  // a woodpile: sawn ends in rows
            DrawRectangleRec({spot.x - 7.0f, spot.y - 6.0f, 14.0f, 6.0f}, lit({104, 78, 52, 255}));
            for (int row = 0; row < 2; ++row) {
                for (int k = 0; k < 5; ++k) {
                    DrawCircleV({spot.x - 5.6f + 2.8f * static_cast<float>(k) + row * 1.4f, spot.y - 1.6f - 2.8f * static_cast<float>(row)}, 1.3f,
                                lit({188, 160, 116, 255}));
                }
            }
            break;
        }
        case 2: {  // a well: a concrete ring, a little gable roof on posts, the winch
            DrawEllipse(static_cast<int>(spot.x), static_cast<int>(spot.y - 3.0f), 4.0f, 2.0f, lit({160, 156, 146, 255}));
            DrawRectangleRec({spot.x - 4.0f, spot.y - 3.0f, 8.0f, 3.0f}, lit({150, 146, 136, 255}));
            DrawEllipse(static_cast<int>(spot.x), static_cast<int>(spot.y - 3.0f), 2.8f, 1.3f, lit({30, 34, 36, 255}));
            for (const float side : {-3.5f, 3.5f}) DrawLineEx({spot.x + side, spot.y - 2.0f}, {spot.x + side, spot.y - 10.0f}, 1.2f, lit({96, 72, 50, 255}));
            DrawTriangle({spot.x - 6.0f, spot.y - 9.5f}, {spot.x + 6.0f, spot.y - 9.5f}, {spot.x, spot.y - 14.0f}, lit({120, 64, 50, 255}));
            DrawLineEx({spot.x - 3.5f, spot.y - 7.0f}, {spot.x + 3.5f, spot.y - 7.0f}, 1.4f, lit({80, 60, 42, 255}));
            break;
        }
        case 3: {  // a dog's kennel
            DrawRectangleRec({spot.x - 3.0f, spot.y - 4.0f, 6.0f, 4.0f}, lit({126, 94, 62, 255}));
            DrawTriangle({spot.x - 3.8f, spot.y - 4.0f}, {spot.x + 3.8f, spot.y - 4.0f}, {spot.x, spot.y - 7.0f}, lit({90, 70, 52, 255}));
            DrawEllipse(static_cast<int>(spot.x), static_cast<int>(spot.y - 1.5f), 1.2f, 1.5f, lit({30, 24, 20, 255}));
            break;
        }
        case 4: {  // a washing line between two posts, washing on it
            const Vector2 a{spot.x - 8.0f, spot.y};
            const Vector2 b{spot.x + 8.0f, spot.y - 3.0f};
            DrawLineEx(a, {a.x, a.y - 9.0f}, 1.1f, lit({96, 90, 84, 255}));
            DrawLineEx(b, {b.x, b.y - 9.0f}, 1.1f, lit({96, 90, 84, 255}));
            DrawLineV({a.x, a.y - 9.0f}, {b.x, b.y - 9.0f}, lit({200, 200, 196, 255}));
            static constexpr Color kWash[4] = {{226, 224, 216, 255}, {170, 60, 56, 255}, {80, 110, 170, 255}, {214, 190, 90, 255}};
            for (int k = 0; k < 4; ++k) {
                const float t = 0.15f + 0.22f * static_cast<float>(k);
                const Vector2 c{a.x + (b.x - a.x) * t, a.y - 9.0f + (b.y - a.y) * t};
                DrawRectangleRec({c.x - 1.4f, c.y, 2.8f, 3.5f}, lit(kWash[(k + static_cast<int>(h)) % 4]));
            }
            break;
        }
        case 5: {  // a bench by the fence
            DrawLineEx({spot.x - 5.0f, spot.y - 2.5f}, {spot.x + 5.0f, spot.y - 4.0f}, 2.0f, lit({132, 100, 66, 255}));
            for (const float side : {-4.0f, 4.0f}) DrawLineV({spot.x + side, spot.y - 2.8f + side * -0.15f}, {spot.x + side, spot.y}, lit({90, 68, 46, 255}));
            break;
        }
        case 8: {  // a zhuravel: the well's log frame, the post, the long sweep pole, the bucket on its rod
            const Color log{120, 90, 60, 255};
            DrawRectangleRec({spot.x - 4.0f, spot.y - 4.0f, 8.0f, 4.0f}, lit(log));
            DrawRectangleRec({spot.x - 4.0f, spot.y - 4.0f, 8.0f, 1.0f}, lit(shade(log, 1.3f)));
            for (float y = spot.y - 3.0f; y < spot.y; y += 1.5f) DrawLineV({spot.x - 4.0f, y}, {spot.x + 4.0f, y}, lit(shade(log, 0.75f)));
            const Vector2 post{spot.x + 9.0f, spot.y + 1.0f};
            const Vector2 pivot{post.x, post.y - 17.0f};
            DrawLineEx(post, pivot, 1.8f, lit({96, 72, 50, 255}));
            DrawLineV(pivot, {pivot.x - 1.5f, pivot.y - 2.0f}, lit({96, 72, 50, 255}));  // its fork
            DrawLineV(pivot, {pivot.x + 1.5f, pivot.y - 2.0f}, lit({96, 72, 50, 255}));
            const Vector2 high{spot.x - 1.0f, pivot.y - 12.0f};
            const Vector2 low{pivot.x + (pivot.x - high.x) * 0.45f, pivot.y + (pivot.y - high.y) * 0.45f};
            DrawLineEx(high, low, 1.2f, lit({130, 100, 66, 255}));
            DrawRectangleRec({low.x - 1.5f, low.y - 1.0f, 3.0f, 3.0f}, lit({90, 86, 80, 255}));  // the weight
            DrawLineV(high, {high.x, spot.y - 7.0f}, lit({110, 84, 56, 255}));                  // the rod
            DrawRectangleRec({high.x - 1.5f, spot.y - 7.0f, 3.0f, 3.0f}, lit({150, 150, 146, 255}));  // the bucket
            break;
        }
        case 9: {  // the street's water pump: cast iron, painted, its spout, its lever; the wet ground under it
            fill_ground_ellipse({spot.x + 1.5f, spot.y + 0.5f}, 0.08f, {70, 80, 84, 140});
            const Color iron{50, 92, 120, 255};
            DrawRectangleRec({spot.x - 1.5f, spot.y - 11.0f, 3.0f, 11.0f}, lit(iron));
            DrawRectangleRec({spot.x - 1.5f, spot.y - 11.0f, 1.0f, 11.0f}, lit(shade(iron, 1.35f)));
            DrawRectangleRec({spot.x - 2.0f, spot.y - 12.5f, 4.0f, 2.0f}, lit(shade(iron, 0.8f)));
            DrawLineEx({spot.x + 1.5f, spot.y - 6.0f}, {spot.x + 4.0f, spot.y - 5.0f}, 1.4f, lit(shade(iron, 0.85f)));  // the spout
            DrawLineEx({spot.x - 1.0f, spot.y - 11.0f}, {spot.x - 5.0f, spot.y - 14.0f}, 1.0f, lit({40, 40, 42, 255}));  // the lever
            break;
        }
        case 10: {  // a haystack: raked up round its pole, darker where it's settled
            const Color hay{190, 160, 86, 255};
            DrawEllipse(static_cast<int>(spot.x + 3.0f), static_cast<int>(spot.y + 0.5f), 9.0f, 2.5f, lit({30, 34, 20, 70}));
            for (int k = 0; k < 7; ++k) {
                const float t = static_cast<float>(k) / 6.0f;
                const float r = 7.0f * std::sqrt(1.0f - t * 0.92f);
                DrawEllipse(static_cast<int>(spot.x), static_cast<int>(spot.y - 1.0f - 2.2f * static_cast<float>(k)), r, r * 0.55f,
                            lit(shade(hay, 0.78f + 0.08f * static_cast<float>(k % 3))));
            }
            DrawEllipse(static_cast<int>(spot.x - 2.0f), static_cast<int>(spot.y - 9.0f), 3.0f, 3.5f, lit(shade(hay, 1.2f)));
            for (int k = 0; k < 5; ++k) {  // wisps
                const float a = static_cast<float>(k) * 1.2f;
                const Vector2 w{spot.x + std::cos(a) * 5.0f, spot.y - 4.0f + std::sin(a) * 2.0f};
                DrawLineV(w, {w.x + 1.5f, w.y + 1.0f}, lit(shade(hay, 1.25f)));
            }
            DrawLineV({spot.x, spot.y - 14.0f}, {spot.x + 0.5f, spot.y - 19.0f}, lit({110, 84, 56, 255}));  // its pole
            break;
        }
        case 6:
        case 7: {  // a vegetable bed
            for (int row = 0; row < 3; ++row) {
                for (int k = 0; k < 4; ++k) {
                    const Vector2 g{0.25f + 0.12f * static_cast<float>(k), 0.3f + 0.14f * static_cast<float>(row)};
                    DrawCircleV(at(g.x, g.y, 1.0f), 1.4f, lit(row == 1 ? Color{150, 184, 110, 255} : Color{70, 110, 50, 255}));
                }
            }
            break;
        }
        default:
            break;
    }
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
            // Sunflowers in rows, or maize (a field here and there), each its
            // own height, swaying a little; in bloom, ripening or dried black
            // (the maize green or dry), field patch by patch; now and then one
            // missing from the row; knocked flat where vehicles went through.
            const auto t = static_cast<float>(GetTime());
            const bool maize = field({fx, fy}, 71) > 0.62f;
            const FxSheet& sheet = plant_sheet(maize ? plants::Plant::Maize : plants::Plant::Sunflower);
            const int per_row = maize ? 2 : 3;
            struct Stem {
                float depth;
                Vector2 at;
                int frame;
            };
            std::array<Stem, 9> stems{};
            int n = 0;
            for (int row = 0; row < 3; ++row) {
                for (int k = 0; k < per_row; ++k) {
                    const uint32_t hk = tile_hash(tx * 3 + k, ty * 5 + row);
                    if (hk % 23 == 0) continue;
                    const float u = maize ? 0.25f + 0.5f * static_cast<float>(k) + (hash_unit(hk) - 0.5f) * 0.1f
                                          : 0.18f + 0.32f * static_cast<float>(k) + (hash_unit(hk) - 0.5f) * 0.12f;
                    const float v = 0.18f + 0.32f * static_cast<float>(row) + (hash_unit(hk >> 4) - 0.5f) * 0.06f;
                    const Vector2 g = along_x ? Vector2{fx + u, fy + v} : Vector2{fx + v, fy + u};
                    const float wind = std::sin(t * 1.1f + (g.x + g.y) * 0.45f);
                    int frame = 0;
                    if (maize) {
                        const int stage = sunflower_bloom(g) < 0.3f ? 1 : 0;
                        frame = crushed_at(g) ? plants::maize_down(stage, static_cast<int>((hk >> 3) & 1u))
                                              : plants::maize(stage, static_cast<int>((hk >> 12) & 1u), stage == 1 ? 1 : std::clamp(1 + static_cast<int>(std::lround(wind * 0.8f)), 0, 2));
                    } else {
                        const float bloom = sunflower_bloom(g) + (hash_unit(hk >> 8) - 0.5f) * 0.25f;
                        const int stage = bloom > 0.6f ? 0 : bloom > 0.3f ? 1 : 2;
                        const int lean = stage == 2 ? static_cast<int>((hk >> 20) % 3)
                                                    : std::clamp(1 + static_cast<int>(std::lround(wind * 0.7f + (hash_unit(hk >> 20) - 0.5f))), 0, 2);
                        frame = crushed_at(g) ? plants::sunflower_down(stage, static_cast<int>((hk >> 3) & 1u))
                                              : plants::sunflower(stage, static_cast<int>((hk >> 12) % 3), lean, (hk >> 24) % 7 == 0);
                    }
                    stems[static_cast<size_t>(n++)] = {g.x + g.y, at(g.x - fx, g.y - fy), frame};
                }
            }
            std::sort(stems.begin(), stems.begin() + n, [](const Stem& p, const Stem& q) { return p.depth < q.depth; });
            for (int i = 0; i < n; ++i) draw_fx(sheet, stems[static_cast<size_t>(i)].frame, stems[static_cast<size_t>(i)].at, lit(WHITE));
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
            // Tufts of it in rows, the wind running over them in waves;
            // flattened where it's lodged and where vehicles went through.
            {
                const FxSheet& sheet = plant_sheet(plants::Plant::Wheat);
                const int stage = green > 0.5f ? 0 : 1;
                const bool lodged = wheat_lodged(mid);
                const int per_row = far ? 2 : 3;
                for (int row = 0; row < 3; ++row) {
                    for (int k = 0; k < per_row; ++k) {
                        const uint32_t hk = tile_hash(tx * 5 + k, ty * 7 + row);
                        const float u = (per_row == 3 ? 0.17f + 0.33f * static_cast<float>(k) : 0.25f + 0.5f * static_cast<float>(k)) + (hash_unit(hk) - 0.5f) * 0.1f;
                        const float v = 0.17f + 0.33f * static_cast<float>(row) + (hash_unit(hk >> 3) - 0.5f) * 0.08f;
                        const Vector2 g = along_x ? Vector2{fx + u, fy + v} : Vector2{fx + v, fy + u};
                        const float wave = 0.5f + 0.5f * std::sin(t * 1.4f - (g.x + g.y) * 0.55f);
                        const int variant = static_cast<int>((hk >> 5) & 1u);
                        const int frame = lodged || crushed_at(g) ? plants::wheat_down(stage, variant)
                                                                  : plants::wheat(stage, variant, std::clamp(static_cast<int>(wave * 3.99f), 0, 3));
                        draw_fx(sheet, frame, at(g.x - fx, g.y - fy), lit(WHITE));
                    }
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
            // Beds of potatoes in rows (in flower here and there), a row of
            // cabbages, tomatoes tied to stakes; trodden in where vehicles went.
            const FxSheet& sheet = plant_sheet(plants::Plant::Garden);
            const int cabbages = static_cast<int>(h % 4);
            const int tomatoes = (h >> 3) % 3 == 0 ? static_cast<int>((h >> 5) % 4) : -1;
            for (int row = 0; row < 4; ++row) {
                const float v = 0.14f + 0.24f * static_cast<float>(row);
                const int kind = row == cabbages ? 1 : row == tomatoes ? 2 : 0;
                for (int k = 0; k < 3; ++k) {
                    const uint32_t hk = tile_hash(tx * 3 + k, ty * 4 + row);
                    const float u = 0.18f + 0.32f * static_cast<float>(k) + (hash_unit(hk) - 0.5f) * 0.06f;
                    const Vector2 g = along_x ? Vector2{fx + u, fy + v} : Vector2{fx + v, fy + u};
                    const int variant = static_cast<int>((hk >> 7) % 3);
                    draw_fx(sheet, crushed_at(g) ? plants::garden_down(variant) : plants::garden(kind, variant), at(g.x - fx, g.y - fy), lit(WHITE));
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
float drawn_height_unscaled(const engine::Structure& s);
float drawn_height(const engine::Structure& s) { return drawn_height_unscaled(s) * building_scale(s); }
float drawn_height_unscaled(const engine::Structure& s) {
    switch (s.type) {
        case engine::StructureType::House: return s.tiles.size() >= engine::kSpaciousTiles ? 28.0f : 44.0f;
        case engine::StructureType::Apartment: return 74.0f;
        case engine::StructureType::CellTower: return 92.0f;
        case engine::StructureType::GasStation: return 30.0f;
        case engine::StructureType::Elevator: return 112.0f;
        case engine::StructureType::Pillbox: return 10.0f;
        case engine::StructureType::ObservationPost: return s.post == engine::PostKind::Tree ? 26.0f : 10.0f;
        case engine::StructureType::Bridge:
        case engine::StructureType::Airfield:
        case engine::StructureType::Dugout: return 4.0f;
        default:
            if (engine::is_fieldwork(s.type) || engine::is_obstacle(s.type)) return 4.0f;
            return style_of(s.type).wall + 12.0f;  // the roof edge and the flag above it
    }
}

// A five-storey block of flats, a "Khrushchyovka": white silicate brick or
// concrete panels; rows of windows in white (new plastic) or brown (old
// wooden) frames, some with the curtains drawn; balconies up the long
// front, some glazed in (their frames all different), washing hung out;
// the entrances with their concrete canopies and iron doors; a plinth
// with the cellar's little windows; a flat roof behind its parapet, vent
// stacks and aerials on it; rust streaks down from the balconies.
// Hit, stage by stage: shrapnel's pits, the facing off in patches,
// windows blown out; then burning in some of them, soot up the wall over
// them, holes into the flats, balconies hanging; burnt out at the last,
// black, every window dark, the roof holed. A bomb brings down a section,
// an entrance's, from the roof to the ground: the floors' ends and the
// rooms (their wallpaper) open to the street, the back wall standing,
// the heap of it before them.
Color room_dark(Color c) { return shade(c, 0.35f); }

void draw_apartment(const engine::TileMap& map, const engine::Structure& s, float damage) {
    constexpr float kPlinth = 4.0f;
    constexpr float kFloor = 11.0f;
    constexpr int kFloors = 5;
    constexpr float kWall = kPlinth + kFloor * kFloors + 1.0f;
    const uint32_t h = tile_hash(s.tiles.front().x * 3 + 1, s.tiles.front().y * 5 + 2);
    const int stage = stage_of(damage);
    const Rectangle r = footprint(s, 0.06f);
    const Vector2 ground[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}};
    Vector2 base[4];
    Vector2 top[4];
    for (int i = 0; i < 4; ++i) {
        base[i] = on_terrain(map, ground[i]);
        top[i] = {base[i].x, base[i].y - kWall};
    }
    const float soot = 1.0f - 0.25f * damage;
    const bool brick = (h & 1u) != 0;
    const Color wall = shade(brick ? Color{200, 198, 190, 255} : Color{196, 190, 176, 255}, soot);
    const Color concrete{170, 166, 156, 255};
    const Face end{base[1], base[2]};    // +x: the end wall, in the light
    const Face front{base[2], base[3]};  // +y: the long front, in shade
    const bool long_front = r.width >= r.height;
    const Face& facade = long_front ? front : end;
    const Face& side = long_front ? end : front;
    const float kf = long_front ? 0.74f : 1.0f;  // how lit the long front is
    const float ks = long_front ? 1.0f : 0.74f;
    const float px = facade.px();
    // Its bays; the entrances every few; the sections bombs brought down (each
    // an entrance's, the one where the bomb fell; one at least left standing).
    const int bays = std::max(4, static_cast<int>(facade.pixels() / 11.0f));
    const int entrances = std::max(2, bays / 3);
    auto entrance_at = [&](int b) {
        for (int e = 0; e < entrances; ++e) {
            if (b == static_cast<int>((static_cast<float>(e) + 0.5f) * static_cast<float>(bays) / static_cast<float>(entrances))) return true;
        }
        return false;
    };
    std::vector<bool> gone(static_cast<size_t>(bays), false);
    std::vector<std::pair<int, int>> gaps;
    if (long_front) {
        for (int k = 0; k < s.bombed && static_cast<int>(gaps.size()) < entrances - 1; ++k) {
            // Where it fell along the block (from its x0 end; the facade runs from the other).
            const int sec = std::min(entrances - 1, (255 - static_cast<int>(s.bomb_at[static_cast<size_t>(k)])) * entrances / 256);
            if (std::any_of(gaps.begin(), gaps.end(), [&](auto g) { return g.first == sec * bays / entrances; })) continue;  // the same one again
            const int b0 = sec * bays / entrances;
            const int b1 = (sec + 1) * bays / entrances;
            gaps.emplace_back(b0, b1);
            for (int b = b0; b < b1; ++b) gone[static_cast<size_t>(b)] = true;
        }
    }
    auto stands = [&](int b) { return b >= 0 && b < bays && !gone[static_cast<size_t>(b)]; };
    auto x_at = [&](float u) { return r.x + r.width * (1.0f - u); };  // the long front runs from +x to x0
    // The bays beside a gap are broken off ragged, floor by floor: each
    // floor's piece of the bay reaches its own way (a stump, the whole bay,
    // out over the gap). +1: the bay on the gap's x0 side (its cut faces the
    // light), -1: on its +x side; 0: not at a gap.
    auto ragged = [&](int b) {
        if (!stands(b)) return 0;
        if (b > 0 && !stands(b - 1)) return 1;
        if (b + 1 < bays && !stands(b + 1)) return -1;
        return 0;
    };
    const float bu = 1.0f / static_cast<float>(bays);
    // The piece of a ragged bay on a floor, as a range of the facade's u, at
    // `t` of its depth (0 its back, 1 its front: broken off aslant).
    auto piece = [&](int b, int fl, float t = 1.0f) {
        const float base = 0.45f + 0.5f * rand01(h, 1290 + b);  // the bay broken off about here, each floor its own way
        const float front = std::clamp(base + 0.55f * (rand01(h, 1300 + b * 7 + fl) - 0.5f) - (fl == kFloors - 1 ? 0.15f : 0.0f), 0.2f, 1.25f);
        const float back = std::clamp(front + 0.8f * (rand01(h, 1350 + b * 7 + fl) - 0.5f), 0.15f, 1.3f);
        const float w = (back + (front - back) * t) * bu;
        return ragged(b) > 0 ? std::pair{static_cast<float>(b + 1) * bu - w, static_cast<float>(b + 1) * bu}
                             : std::pair{static_cast<float>(b) * bu, static_cast<float>(b) * bu + w};
    };
    auto floor_v = [&](int fl) {  // a floor's band up the wall (the plinth with the first, the parapet with the top)
        return std::pair{fl == 0 ? 0.0f : kPlinth + kFloor * static_cast<float>(fl), fl == kFloors - 1 ? kWall : kPlinth + kFloor * static_cast<float>(fl + 1)};
    };
    // Is the facade there, on a floor, at u (a ragged bay's piece reaching over it)?
    auto covered = [&](int b, int fl, float u0, float u1) {
        if (!stands(b)) return false;
        if (ragged(b) == 0) return true;
        const auto [pa, pb] = piece(b, fl);
        return u0 >= pa && u1 <= pb;
    };
    // Through each gap: the back wall's inside, standing part way, the rooms'
    // colours floor by floor; the cut end of the section beside it.
    static constexpr Color kRooms[5] = {{186, 204, 170, 255}, {214, 196, 150, 255}, {170, 190, 210, 255}, {214, 176, 170, 255}, {200, 196, 186, 255}};
    for (const auto& [b0, b1] : gaps) {
        const float u0 = static_cast<float>(std::max(0, b0 - 1)) / bays;  // (behind the ragged bays, too)
        const float u1 = static_cast<float>(std::min(bays, b1 + 1)) / bays;
        const Face back{on_terrain(map, {x_at(u0), r.y}), on_terrain(map, {x_at(u1), r.y})};
        const int standing = 1 + static_cast<int>(rand01(h, b0) * 3.0f);
        const float bpx = back.px();
        int room = 0;
        for (float u = 0.0f; u < 1.0f; u += 10.0f * bpx, ++room) {  // the back wall's inside, room by room, its top broken off
            const float u1r = std::min(1.0f, u + 10.0f * bpx);
            const float top_v = kPlinth + kFloor * static_cast<float>(standing) - 5.0f + 9.0f * rand01(h, 40 + room);
            for (int fl = 0; fl < kFloors; ++fl) {
                const float v = kPlinth + kFloor * fl;
                if (v >= top_v) break;
                face_fill(back, u, u1r, v, std::min(top_v, v + kFloor), shade(kRooms[(room + fl * 3 + b0) % 5], 0.62f));
                face_line(back, u, v, u1r, v, shade(concrete, 0.7f));
                if (v + 8.0f < top_v && rand01(h, 60 + room * 7 + fl) < 0.5f) face_fill(back, u + 3.0f * bpx, u + 6.0f * bpx, v + 3.0f, v + 8.0f, {20, 18, 16, 255});
            }
            face_line(back, u, 0.0f, u, top_v, shade(concrete, 0.5f));  // a partition's end
            burnt(back, u, u1r, 0.0f, top_v, 0.5f, h + static_cast<uint32_t>(room));
        }
        // The floors' tops bared where one reaches out further than the one over it (dust, a rug).
        for (const int b : {b0 - 1, b1}) {
            if (!stands(b)) continue;
            for (int fl = 0; fl + 1 < kFloors; ++fl) {
                float ua[2];
                float ub[2];
                for (int t = 0; t < 2; ++t) {  // at its back, at its front
                    const auto [pa, pb] = piece(b, fl, static_cast<float>(t));
                    const auto [qa, qb] = piece(b, fl + 1, static_cast<float>(t));
                    ua[t] = ragged(b) > 0 ? pa : qb;
                    ub[t] = std::max(ua[t], ragged(b) > 0 ? qa : pb);
                }
                if (ub[0] <= ua[0] && ub[1] <= ua[1]) continue;
                const float z = kPlinth + kFloor * static_cast<float>(fl + 1);
                fill_quad(on_terrain(map, {x_at(ub[0]), r.y}, z), on_terrain(map, {x_at(ua[0]), r.y}, z), on_terrain(map, {x_at(ua[1]), r.y + r.height}, z),
                          on_terrain(map, {x_at(ub[1]), r.y + r.height}, z), shade({96, 88, 80, 255}, soot));
                for (int k = 0; k < 14; ++k) {  // what's fallen on it: bits of the walls, plaster, a rug
                    const float kt = rand01(h, 1420 + fl * 31 + k);
                    const float ku = ua[0] + (ua[1] - ua[0]) * kt + ((ub[0] - ua[0]) + ((ub[1] - ua[1]) - (ub[0] - ua[0])) * kt) * rand01(h, 1400 + fl * 31 + k);
                    const Vector2 p = on_terrain(map, {x_at(ku), r.y + r.height * kt}, z);
                    const float size = 1.0f + 1.5f * rand01(h, 1440 + fl * 31 + k);
                    const Color bit = k == 0 ? Color{150, 60, 50, 255} : k % 3 == 0 ? kRooms[k % 5] : shade(concrete, 0.7f + 0.4f * rand01(h, 1460 + k));
                    DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), size, size * 0.6f, lit(shade(bit, soot)));
                }
            }
        }
        // The cut end of the section beside it (facing the light), floor by
        // floor where each broke off: the floors' slabs, the rooms between.
        if (b1 >= bays) continue;  // (at the far end: the cut faces away)
        std::array<int, kFloors> order{};
        for (int fl = 0; fl < kFloors; ++fl) order[static_cast<size_t>(fl)] = fl;
        auto mid_u = [&](int fl) { return piece(b1, fl, 0.0f).first + piece(b1, fl, 1.0f).first; };
        std::sort(order.begin(), order.end(), [&](int a, int b) { return mid_u(a) > mid_u(b); });  // the ones further back first
        for (const int fl : order) {
            const Face cut{on_terrain(map, {x_at(piece(b1, fl, 1.0f).first), r.y + r.height}), on_terrain(map, {x_at(piece(b1, fl, 0.0f).first), r.y})};
            const float cpx = cut.px();
            const auto [va, vb] = floor_v(fl);
            const float v = kPlinth + kFloor * static_cast<float>(fl);
            // Room by room across its depth (the partitions, a door through one), in the shadow of the floor
            // over them; the floor's dark along the foot; a wardrobe, a sofa; the outer walls' ends at its edges.
            const float split = 0.3f + 0.1f * rand01(h, 1500 + fl);
            const float kParts[4] = {0.0f, split, split + 0.3f + 0.08f * rand01(h, 1510 + fl), 1.0f};
            for (int part = 0; part < 3; ++part) {
                const float a = kParts[part];
                const float b = kParts[part + 1];
                const Color paint = kRooms[(b1 + fl * 2 + part) % 5];
                face_fill(cut, a, b, v + 1.5f, v + kFloor, shade(paint, 0.72f * soot));
                face_fill(cut, a, b, v + kFloor - 2.0f, v + kFloor, shade(paint, 0.5f * soot));  // under the ceiling
                face_fill(cut, a, b, v + 1.5f, v + 3.0f, shade({96, 80, 64, 255}, soot));       // the floor
                if (rand01(h, 20 + fl * 3 + part) < 0.35f) {
                    const float m = a + (b - a) * (0.2f + 0.5f * rand01(h, 40 + fl * 3 + part));
                    face_fill(cut, m, m + 4.0f * cpx, v + 3.0f, v + 3.0f + 3.0f + 3.0f * rand01(h, 60 + fl), shade({110, 80, 60, 255}, soot));
                }
            }
            for (int k = 1; k < 3; ++k) {  // the partitions, a door in one
                face_fill(cut, kParts[k] - cpx, kParts[k] + cpx, v + 1.5f, v + kFloor, shade(concrete, 0.62f * soot));
                if (rand01(h, 80 + fl * 2 + k) < 0.5f) face_fill(cut, kParts[k] - cpx, kParts[k] + cpx, v + 3.0f, v + 9.0f, {30, 26, 24, 255});
            }
            // The slab's end, broken: bits bitten out of it.
            face_fill(cut, 0.0f, 1.0f, v, v + 1.5f, shade(concrete, 1.05f * soot));
            for (int k = 0; k < 3; ++k) {
                const float m = rand01(h, 1520 + fl * 5 + k);
                face_fill(cut, m, std::min(1.0f, m + 3.0f * cpx), v, v + 1.5f, shade(room_dark(kRooms[(fl + k) % 5]), soot));
            }
            for (const float e : {0.0f, 1.0f - 2.0f * cpx}) face_fill(cut, e, e + 2.0f * cpx, va, vb, shade(wall, 0.9f));  // the outer walls' ends
            if (fl == 0) face_fill(cut, 0.0f, 1.0f, 0.0f, kPlinth, shade(concrete, 0.8f * soot));
            if (fl == kFloors - 1) face_fill(cut, 0.0f, 1.0f, kWall - 1.0f, kWall + 1.0f, shade(concrete, 1.1f * soot));
            if (stage >= 2) burnt(cut, 0.0f, 1.0f, va, vb, stage == 2 ? 0.35f : 0.7f, h + static_cast<uint32_t>(b1 * 7 + fl));
        }
    }
    // The heap where each section came down: leaning on the back wall, down
    // to the street, spilling into it (and out of the end, at an end).
    const Color bits = brick ? Color{200, 198, 190, 255} : Color{150, 90, 64, 255};
    auto gap_pile = [&](int b0, int b1, bool spill) {
        const float xa = x_at(b1 < bays ? std::max(piece(b1, 0, 0.0f).first, piece(b1, 0, 1.0f).first) : 1.0f);
        const float xb = x_at(b0 > 0 ? std::min(piece(b0 - 1, 0, 0.0f).second, piece(b0 - 1, 0, 1.0f).second) : 0.0f);
        const bool open_lo = b1 >= bays;
        const bool open_hi = b0 == 0;
        constexpr float kSpill = 0.45f;
        constexpr float kOut = 0.35f;
        const float front = r.y + r.height;
        const float x0 = xa - (open_lo ? kOut : 0.0f);
        const float x1 = xb + (open_hi ? kOut : 0.0f);
        rubble_pile(
            map, {x0, r.y}, {x1 - x0, r.height + kSpill},
            [&](Vector2 g) {
                const float t = (g.y - r.y) / r.height;
                float f = t <= 1.0f ? 1.0f - 0.3f * t : 0.7f * (1.0f - (g.y - front) / kSpill);
                if (open_lo) f *= std::clamp((g.x - x0) / (2.0f * kOut), 0.0f, 1.0f);
                if (open_hi) f *= std::clamp((x1 - g.x) / (2.0f * kOut), 0.0f, 1.0f);
                float cap = 1000.0f;  // under a floor reaching out over it
                const float u = 1.0f - (g.x - r.x) / r.width;
                for (const int b : {b0 - 1, b1}) {
                    if (!stands(b) || g.y > front) continue;
                    for (int fl = 1; fl < kFloors; ++fl) {
                        const auto [pa, pb] = piece(b, fl, (g.y - r.y) / r.height);
                        if (u >= pa && u <= pb) cap = std::min(cap, kPlinth + kFloor * static_cast<float>(fl) - 1.0f);
                    }
                }
                return std::min(cap, 30.0f * std::sqrt(std::max(0.0f, f)) * (0.72f + 0.28f * ruin_noise(g)));
            },
            {concrete, bits, 0.18f, true}, h + static_cast<uint32_t>(b0 * 7), 12, [&](Vector2 g) { return (g.y > front) == spill; });
    };
    for (const auto& [b0, b1] : gaps) gap_pile(b0, b1, false);
    // The walls, the plinth, standing stretch by stretch (whole bays: the ragged ones piece by piece).
    auto whole_bay = [&](int b) { return stands(b) && ragged(b) == 0; };
    auto stretches = [&](auto fn) {
        int b = 0;
        while (b < bays) {
            if (!whole_bay(b)) {
                ++b;
                continue;
            }
            int e = b;
            while (e < bays && whole_bay(e)) ++e;
            fn(b, e);
            b = e;
        }
    };
    auto wall_of = [&](const Face& f, float k, int b0, int b1, int nb) {
        const float u0 = static_cast<float>(b0) / nb;
        const float u1 = static_cast<float>(b1) / nb;
        if (brick) {
            wall_texture(f, u0, u1, kPlinth, kWall, Wall::Silicate, shade(wall, k), h);
        } else {
            panels_in(f, b0, b1, nb, kPlinth, kFloor, kFloors, shade(wall, k), h);
        }
        face_fill(f, u0, u1, 0.0f, kPlinth, shade({120, 116, 108, 255}, k * soot));
        face_line(f, u0, kPlinth, u1, kPlinth, shade({150, 146, 138, 255}, k * soot));
    };
    stretches([&](int b0, int b1) { wall_of(facade, kf, b0, b1, bays); });
    for (int b = 0; b < bays; ++b) {
        const int side_of = ragged(b);
        if (side_of == 0) continue;
        for (int fl = 0; fl < kFloors; ++fl) {
            const auto [pa, pb] = piece(b, fl);
            const auto [va, vb] = floor_v(fl);
            const float v0 = fl == 0 ? kPlinth : va;
            if (brick) {
                wall_texture(facade, pa, pb, v0, vb, Wall::Silicate, shade(wall, kf), h + static_cast<uint32_t>(fl));
            } else {  // its panel (as panels_in has them)
                face_fill(facade, pa, pb, v0, vb, shade(wall, kf * (0.95f + 0.1f * rand01(h, fl * 97 + b))));
                for (const float seam : {static_cast<float>(b) * bu, static_cast<float>(b + 1) * bu}) {
                    if (seam > pa + px && seam < pb - px) face_line(facade, seam, v0, seam, vb, shade(wall, 0.7f * kf));
                }
            }
            face_line(facade, pa, va + 0.5f, pb, va + 0.5f, shade(concrete, 0.8f * kf));  // the slab's line
            if (fl == 0) {
                face_fill(facade, pa, pb, 0.0f, kPlinth, shade({120, 116, 108, 255}, kf * soot));
                face_line(facade, pa, kPlinth, pb, kPlinth, shade({150, 146, 138, 255}, kf * soot));
            }
            // Its broken end: the panel's edge, pale, bitten out here and
            // there (the room dark behind); the rebar out of it.
            const float e = side_of > 0 ? pa : pb;
            face_line(facade, e, va, e, vb, shade(concrete, 1.1f));
            for (int k = 0; k < 2; ++k) {
                if (rand01(h, 1620 + b * 5 + fl * 2 + k) > 0.6f) continue;
                const float deep = (0.08f + 0.22f * rand01(h, 1630 + fl * 2 + k)) * bu * (side_of > 0 ? 1.0f : -1.0f);
                const float n0 = va + (vb - va) * (0.15f + 0.45f * rand01(h, 1640 + fl * 2 + k));
                const float n1 = std::min(vb - 1.0f, n0 + 2.0f + 5.0f * rand01(h, 1650 + fl * 2 + k));
                const float in = std::clamp(e + deep, pa, pb);
                face_fill(facade, std::min(e, in), std::max(e, in), n0, n1, shade(kRooms[(b + fl + k) % 5], 0.3f * soot));
                face_line(facade, in, n0, in, n1, shade(concrete, 1.05f));
            }
            if (rand01(h, 1600 + b * 5 + fl) < 0.5f) {
                const Vector2 p = facade.at(e, va + 3.0f + 5.0f * rand01(h, 1610 + fl));
                DrawLineV(p, {p.x + (side_of > 0 ? 4.0f : -4.0f), p.y + 2.0f}, lit({74, 56, 44, 255}));
            }
            // Under a floor reaching out over it (on the far side of the gap): the dark in there.
            if (side_of < 0 && fl + 1 < kFloors) {
                const float qb = piece(b, fl + 1).second;
                if (qb > pb) face_fill(facade, pb, qb, va + (fl == 0 ? kPlinth : 0.0f), vb, {28, 24, 22, 255});
            }
        }
    }
    const int side_bays = std::max(2, static_cast<int>(side.pixels() / 11.0f));
    const bool end_stands = !long_front || stands(0);  // the end wall in the light came down with its section
    if (end_stands) wall_of(side, ks, 0, side_bays, side_bays);
    // Windows: whole, blown out, burning (the soot up the wall over them), burnt out.
    auto frame_of = [&](int fl, int b) {
        return rand01(h, fl * 37 + b) < 0.55f ? Color{232, 232, 228, 255} : Color{118, 84, 58, 255};  // new plastic, old wood
    };
    auto window_state = [&](int fl, int b) {  // 0 whole, 1 blown out, 2 burning, 3 burnt out
        const float roll = rand01(h, 3000 + fl * 41 + b);
        if (stage >= 3) return roll < 0.9f ? 3 : 1;
        if (stage == 2) return roll < 0.14f ? 2 : roll < 0.55f ? 1 : 0;
        if (stage == 1) return roll < 0.18f ? 1 : 0;
        return 0;
    };
    int smoking = 0;
    for (int fl = 0; fl < kFloors; ++fl) {
        const float v = kPlinth + kFloor * fl + 3.0f;
        for (int b = 0; b < bays; ++b) {
            const float u = (static_cast<float>(b) + 0.5f) / static_cast<float>(bays);
            if (!covered(b, fl, u - 3.0f * px, u + 3.0f * px)) continue;
            if (entrance_at(b)) {  // the stairwell's window, between the floors
                if (fl > 0) window_on(facade, u, v - 3.0f, 3.0f, 4.0f, {shade({150, 150, 146, 255}, 1.0f), false}, window_state(fl, b) > 0, kf);
                continue;
            }
            const int state = window_state(fl, b);
            WindowLook look{state == 3 ? Color{58, 50, 44, 255} : frame_of(fl, b)};
            look.curtains = rand01(h, 5000 + fl * 17 + b) < 0.5f;
            window_on(facade, u, v, 4.0f, 5.0f, look, state > 0, kf);
            if (state >= 2) {
                soot_on(facade, u, v + 5.0f, 6.0f, 9.0f + 5.0f * damage);
                const Vector2 c = facade.at(u, v + 2.0f);
                if (state == 2) fire_spot(c, true);
                if (state == 3 && smoking < 3 && rand01(h, 6000 + fl * 7 + b) < 0.08f) {
                    fire_spot(c, false);
                    ++smoking;
                }
            } else if (state == 1 && stage >= 1 && rand01(h, 6100 + fl * 7 + b) < 0.3f) {
                soot_on(facade, u, v + 5.0f, 4.0f, 4.0f);
            }
        }
    }
    const int end_rows = std::max(1, static_cast<int>(side.pixels() / 26.0f));
    for (int fl = 0; fl < kFloors && end_stands; ++fl) {
        for (int c = 0; c < end_rows; ++c) {
            const float u = (static_cast<float>(c) + 0.5f) / static_cast<float>(end_rows);
            window_on(side, u, kPlinth + kFloor * fl + 3.5f, 3.0f, 4.0f, {frame_of(fl + 9, c)}, window_state(fl + 7, c) > 0, ks);
        }
    }
    // The wear: pits, the facing off, burnt at the worst.
    stretches([&](int b0, int b1) {
        if (stage <= 0) return;
        const Face part{facade.at(static_cast<float>(b0) / bays, 0.0f), facade.at(static_cast<float>(b1) / bays, 0.0f)};
        wear_walls(part, kf, kPlinth, kWall, stage, brick ? Color{170, 96, 70, 255} : concrete, brick, h + static_cast<uint32_t>(b0));
    });
    if (end_stands) wear_walls(side, ks, kPlinth, kWall, stage, brick ? Color{170, 96, 70, 255} : concrete, brick, h + 99u);
    // The balconies: a slab, the railing's panel before it; glazed in on
    // some, washing on others; rust down from them. Hanging off, gone, as it's hit.
    for (int b = 1; b < bays; b += 2) {
        if (entrance_at(b) || entrance_at(b - 1) || !whole_bay(b) || !whole_bay(b - 1)) continue;
        const float u = static_cast<float>(b) / static_cast<float>(bays);
        for (int fl = 1; fl < kFloors; ++fl) {
            const float v = kPlinth + kFloor * fl;
            const float du = 4.5f * px;
            const float roll = rand01(h, 7300 + fl * 13 + b);
            if (stage >= 2 && roll < 0.2f * static_cast<float>(stage)) {  // gone: the balcony door dark, a stump of the slab
                face_fill(facade, u - du * 0.4f, u + du * 0.4f, v, v + 7.0f, {20, 18, 16, 255});
                continue;
            }
            auto out = [&](float uu, float vv) {
                const Vector2 p = facade.at(uu, vv);
                return Vector2{p.x, p.y + 2.0f};
            };
            const bool hanging = stage >= 2 && roll < 0.35f;
            const float sag = hanging ? 4.0f : 0.0f;
            const bool glazed = rand01(h, 7000 + fl * 13 + b) < 0.5f && stage < 3;
            const Color slab = shade({176, 172, 162, 255}, kf * soot * (stage >= 3 ? 0.55f : 1.0f));
            fill_quad(facade.at(u - du, v), facade.at(u + du, v), {out(u + du, v).x, out(u + du, v).y + sag}, out(u - du, v), shade(slab, 1.1f));
            if (glazed && !hanging) {
                const Color fr = rand01(h, 7100 + fl * 13 + b) < 0.5f ? Color{226, 226, 222, 255} : Color{140, 104, 70, 255};
                const bool shattered = stage >= 1 && rand01(h, 7400 + fl * 13 + b) < 0.3f * static_cast<float>(stage);
                fill_quad(out(u - du, v), out(u + du, v), out(u + du, v + 7.0f), out(u - du, v + 7.0f), shattered ? Color{24, 22, 20, 255} : shade({86, 104, 118, 255}, kf));
                for (float uu = u - du; uu <= u + du + 0.001f; uu += du * 0.5f) DrawLineV(out(uu, v), out(uu, v + 7.0f), lit(shade(fr, kf)));
                DrawLineV(out(u - du, v + 7.0f), out(u + du, v + 7.0f), lit(shade(fr, kf)));
                fill_quad(out(u - du, v), out(u + du, v), out(u + du, v + 2.5f), out(u - du, v + 2.5f), shade(slab, 0.95f));
            } else {
                fill_quad({out(u - du, v).x, out(u - du, v).y}, {out(u + du, v).x, out(u + du, v).y + sag},
                          {out(u + du, v + 3.0f).x, out(u + du, v + 3.0f).y + sag}, out(u - du, v + 3.0f), shade(slab, 0.92f));
                if (!hanging && stage < 2 && rand01(h, 7200 + fl * 13 + b) < 0.4f) {  // washing on the line
                    static constexpr Color kWash[4] = {{220, 60, 50, 255}, {70, 110, 190, 255}, {236, 236, 230, 255}, {226, 190, 60, 255}};
                    for (int k = 0; k < 3; ++k) {
                        const Vector2 p = out(u - du * 0.7f + du * 0.6f * static_cast<float>(k), v + 5.5f);
                        DrawRectangleRec({std::round(p.x), std::round(p.y), 1.0f, 2.0f}, lit(kWash[(h + fl + k + b) % 4]));
                    }
                }
            }
            face_line(facade, u - du * 0.3f, v - 0.5f, u - du * 0.3f, v - 3.0f - 3.0f * rand01(h, fl + b), shade({150, 104, 70, 255}, kf));  // rust
        }
    }
    // The entrances: iron doors under their concrete canopies, a step.
    for (int b = 0; b < bays; ++b) {
        if (!entrance_at(b) || !whole_bay(b)) continue;
        const float u = (static_cast<float>(b) + 0.5f) / static_cast<float>(bays);
        door_on(facade, u, 4.0f, 7.5f, rand01(h, 9000 + b) < 0.5f ? Color{96, 70, 50, 255} : Color{70, 96, 80, 255}, kf * soot);
        const float du = 4.0f * px;
        const Vector2 c0 = facade.at(u - du, 9.5f);
        const Vector2 c1 = facade.at(u + du, 9.5f);
        fill_quad(c0, c1, {c1.x, c1.y + 3.0f}, {c0.x, c0.y + 3.0f}, shade({172, 168, 160, 255}, 1.05f * soot));
        DrawLineV({c0.x, c0.y + 4.0f}, {c1.x, c1.y + 4.0f}, lit(shade({120, 116, 108, 255}, soot)));
    }
    for (int b = 0; b < bays; b += 2) {  // the cellar's little windows
        if (!whole_bay(b)) continue;
        const float u = (static_cast<float>(b) + 0.5f) / static_cast<float>(bays);
        face_fill(facade, u - 1.5f * px, u + 1.5f * px, 1.0f, 2.5f, {30, 28, 26, 255});
    }
    // Holes into the flats.
    if (stage >= 2) {
        const int holes = stage == 2 ? 2 : 4;
        for (int k = 0; k < holes; ++k) {
            const int b = static_cast<int>(rand01(h, 11000 + k) * static_cast<float>(bays));
            if (!whole_bay(b)) continue;
            const float u = (static_cast<float>(b) + 0.5f) / static_cast<float>(bays);
            const float v = kPlinth + kFloor * (0.5f + 3.8f * rand01(h, 11100 + k));
            wall_hole(facade, u, v, 9.0f + 5.0f * damage, 7.0f + 5.0f * damage, h + static_cast<uint32_t>(k) * 31u);
            soot_on(facade, u, v + 9.0f, 9.0f, 10.0f);
            if (stage == 2 && k == 0) fire_spot(facade.at(u, v + 4.0f), true);
        }
    }
    // The roof: bitumen behind its parapet, standing stretch by stretch; vent stacks, aerials.
    stretches([&](int b0, int b1) {
        const float s0 = 1.0f - static_cast<float>(b1) / bays;
        const float s1 = 1.0f - static_cast<float>(b0) / bays;
        const Vector2 a0 = lerp(top[3], top[2], s0);
        const Vector2 a1 = lerp(top[3], top[2], s1);
        const Vector2 c0 = lerp(top[0], top[1], s0);
        const Vector2 c1 = lerp(top[0], top[1], s1);
        const Slope roof{a0, a1, c0, c1};
        roof_texture(roof, Roof::Bitumen, shade({98, 96, 92, 255}, stage >= 3 ? 0.6f : 1.0f), soot, h + static_cast<uint32_t>(b0));
        const bool whole = b0 == 0 && b1 == bays;
        for (const auto& [p, q] : {std::pair{a0, a1}, std::pair{c0, c1}}) DrawLineEx({p.x, p.y - 1.0f}, {q.x, q.y - 1.0f}, 2.0f, lit(shade(wall, 0.95f)));
        if (b0 == 0 || whole) DrawLineEx({a1.x, a1.y - 1.0f}, {c1.x, c1.y - 1.0f}, 2.0f, lit(shade(wall, 1.1f)));
        if (b1 == bays || whole) DrawLineEx({a0.x, a0.y - 1.0f}, {c0.x, c0.y - 1.0f}, 2.0f, lit(shade(wall, 0.95f)));
    });
    for (int b = 0; b < bays; ++b) {
        const int side_of = ragged(b);
        if (side_of == 0) continue;
        const auto [pa, pb] = piece(b, kFloors - 1, 1.0f);
        const auto [ra, rb] = piece(b, kFloors - 1, 0.0f);
        const Vector2 a0 = lerp(top[3], top[2], 1.0f - pb);
        const Vector2 a1 = lerp(top[3], top[2], 1.0f - pa);
        const Vector2 c0 = lerp(top[0], top[1], 1.0f - rb);
        const Vector2 c1 = lerp(top[0], top[1], 1.0f - ra);
        roof_texture(Slope{a0, a1, c0, c1}, Roof::Bitumen, shade({98, 96, 92, 255}, stage >= 3 ? 0.6f : 1.0f), soot, h + static_cast<uint32_t>(b));
        for (const auto& [p, q] : {std::pair{a0, a1}, std::pair{c0, c1}}) DrawLineEx({p.x, p.y - 1.0f}, {q.x, q.y - 1.0f}, 2.0f, lit(shade(wall, 0.95f)));
        // Hanging into the gap: slabs off the broken floors, the roof's edge (its tarred felt, its slab).
        const float out = side_of > 0 ? 1.0f : -1.0f;
        for (int fl = 1; fl <= kFloors; ++fl) {
            if (rand01(h, 1700 + b * 11 + fl) > (fl == kFloors ? 0.45f : 0.25f)) continue;
            const int pf = std::min(fl, kFloors - 1);
            auto edge_at = [&](float y) {
                const auto [ea, eb] = piece(b, pf, (y - r.y) / r.height);
                return x_at(side_of > 0 ? ea : eb);
            };
            const float z = fl == kFloors ? kWall : kPlinth + kFloor * static_cast<float>(fl);
            const float y0 = r.y + r.height * (0.1f + 0.5f * rand01(h, 1710 + fl));
            const float y1 = std::min(r.y + r.height * 0.95f, y0 + r.height * (0.2f + 0.25f * rand01(h, 1720 + fl)));
            const Vector2 hinge0 = on_terrain(map, {edge_at(y0), y0}, z);
            const Vector2 hinge1 = on_terrain(map, {edge_at(y1), y1}, z);
            const float len = 0.2f + 0.2f * rand01(h, 1730 + fl);
            const float drop = (fl == kFloors ? 16.0f : 10.0f) + 8.0f * rand01(h, 1740 + fl);
            const Vector2 d0{out * 32.0f * len, out * 16.0f * len + drop};
            const Vector2 d1{d0.x * (0.7f + 0.5f * rand01(h, 1750 + fl)), d0.y * (0.8f + 0.4f * rand01(h, 1760 + fl))};
            const Color slab = fl == kFloors ? shade({84, 82, 78, 255}, soot) : shade(concrete, 0.85f * soot);
            fill_quad(hinge0, hinge1, {hinge1.x + d1.x, hinge1.y + d1.y}, {hinge0.x + d0.x, hinge0.y + d0.y}, slab);
            DrawLineV({hinge0.x + d0.x, hinge0.y + d0.y + 1.0f}, {hinge1.x + d1.x, hinge1.y + d1.y + 1.0f}, lit(shade(slab, 0.55f)));  // its edge
            for (int k = 0; k < 3; ++k) {  // rebar out of it
                const Vector2 p = lerp({hinge0.x + d0.x, hinge0.y + d0.y}, {hinge1.x + d1.x, hinge1.y + d1.y}, 0.2f + 0.3f * static_cast<float>(k));
                DrawLineV(p, {p.x + out * 2.0f, p.y + 3.0f + 2.0f * rand01(h, 1770 + k)}, lit({74, 56, 44, 255}));
            }
        }
    }
    for (int k = 0; k < entrances; ++k) {  // a vent stack over each stairwell
        const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(entrances);
        const int b = static_cast<int>(t * static_cast<float>(bays));
        if (long_front && !whole_bay(b)) continue;
        const Vector2 g = long_front ? Vector2{r.x + r.width * (1.0f - t), r.y + r.height * 0.55f} : Vector2{r.x + r.width * 0.55f, r.y + r.height * (1.0f - t)};
        const Vector2 c = on_terrain(map, g, kWall);
        fill_quad({c.x - 4.0f, c.y}, {c.x + 4.0f, c.y}, {c.x + 4.0f, c.y - 5.0f}, {c.x - 4.0f, c.y - 5.0f}, shade(wall, 0.8f * soot));
        fill_quad({c.x - 4.0f, c.y - 5.0f}, {c.x + 4.0f, c.y - 5.0f}, {c.x + 3.0f, c.y - 7.0f}, {c.x - 3.0f, c.y - 7.0f}, shade({110, 106, 100, 255}, soot));
        if (stage < 2) {
            fine_line({c.x + 6.0f, c.y}, {c.x + 6.0f, c.y - 13.0f}, {80, 80, 80, 255});
            for (const float k2 : {9.0f, 12.0f}) fine_line({c.x + 2.5f, c.y - k2 + 1.0f}, {c.x + 9.5f, c.y - k2 - 1.0f}, {80, 80, 80, 255});
        }
    }
    if (stage >= 2) {
        stretches([&](int b0, int b1) {
            const bool whole = b0 == 0 && b1 == bays;
            const float xa = x_at(static_cast<float>(b1) / static_cast<float>(bays));
            const float xb = x_at(static_cast<float>(b0) / static_cast<float>(bays));
            roof_holes(map, whole ? r : Rectangle{xa, r.y, xb - xa, r.height}, kWall, damage, {98, 96, 92, 255}, h + static_cast<uint32_t>(b0));
        });
    }
    // The heaps spilt into the street; glass and brick at the foot of the rest.
    for (const auto& [b0, b1] : gaps) gap_pile(b0, b1, true);
    if (stage >= 2) {
        for (int k = 0; k < stage; ++k) {
            const float u = 0.1f + 0.8f * rand01(h, 12000 + k);
            if (!stands(static_cast<int>(u * static_cast<float>(bays)))) continue;
            const Vector2 foot = on_terrain(map, {x_at(u), r.y + r.height + 0.12f});
            rubble_heap(foot, 12.0f, 5.0f, concrete, {150, 90, 64, 255}, h + 500u + static_cast<uint32_t>(k), 4);
        }
    }
}

// A cell tower: a steel lattice mast narrowing up, red and white at the
// top, its antenna panels round the top and a dish or two below; a cabinet
// at its foot inside a fence.
void draw_cell_tower(const engine::TileMap& map, const engine::Structure& s, float damage) {
    constexpr float kHeight = 92.0f;
    const Vector2 c = to_vector2(s.center);
    const Vector2 foot = on_terrain(map, c);
    const float soot = 1.0f - 0.4f * damage;
    const int stage = stage_of(damage);
    // The fence round its foot, the cabinet.
    draw_fence(map, {c.x - 0.42f, c.y - 0.42f, 0.84f, 0.84f}, {150, 152, 148, 255});
    {
        const Box cab = box_on(map, {c.x + 0.1f, c.y - 0.35f, 0.24f, 0.18f}, 6.0f);
        face_fill(cab.lit_face, 0.0f, 1.0f, 0.0f, 6.0f, shade({206, 206, 198, 255}, soot));
        face_fill(cab.shade_face, 0.0f, 1.0f, 0.0f, 6.0f, shade({206, 206, 198, 255}, 0.74f * soot));
        fill_quad(cab.top[0], cab.top[1], cab.top[2], cab.top[3], shade({226, 226, 220, 255}, soot));
        if (stage >= 1) pockmarks(cab.lit_face, 0.0f, 1.0f, 0.0f, 6.0f, stage * 2, s.id);
        if (stage >= 3) {  // burnt out, its door hanging
            burnt(cab.lit_face, 0.0f, 1.0f, 0.0f, 6.0f, 0.9f, s.id);
            burnt(cab.shade_face, 0.0f, 1.0f, 0.0f, 6.0f, 0.9f, s.id + 1u);
            face_fill(cab.shade_face, 0.3f, 0.7f, 0.0f, 5.0f, {20, 18, 16, 255});
            fire_spot(cab.top[2], false);
        }
    }
    // The legs, the bracing between them: steel, the top section red and white.
    const Vector2 legs[3] = {on_terrain(map, {c.x - 0.28f, c.y + 0.16f}), on_terrain(map, {c.x + 0.28f, c.y + 0.16f}), on_terrain(map, {c.x, c.y - 0.32f})};
    const float lean = (s.id & 1u) != 0 ? 1.0f : -1.0f;
    constexpr float kBend = 0.55f;  // where it buckled, at the worst
    auto bend = [&](Vector2 p, float t) {
        if (stage < 3 || t <= kBend) return p;
        const float k = (t - kBend) / (1.0f - kBend);
        return Vector2{p.x + lean * k * k * 24.0f, p.y + k * k * 34.0f};  // folded over, hanging
    };
    const Vector2 tip = bend({foot.x, foot.y - kHeight}, 1.0f);
    auto up = [&](int leg, float t) {
        const Vector2 top{foot.x + (legs[leg].x - foot.x) * 0.18f, foot.y - kHeight + (legs[leg].y - foot.y) * 0.18f};
        return bend(lerp(legs[leg], top, t), t);
    };
    const Color steel = shade({168, 170, 168, 255}, soot);
    constexpr int kSections = 8;
    for (int k = 0; k < kSections; ++k) {
        const float t0 = static_cast<float>(k) / kSections;
        const float t1 = static_cast<float>(k + 1) / kSections;
        const Color col = k >= kSections - 2 ? (k % 2 ? Color{210, 60, 50, 255} : Color{236, 236, 230, 255}) : steel;
        for (int leg = 0; leg < 3; ++leg) {
            fine_line(up(leg, t0), up(leg, t1), col);
            if (leg < 2 && !(stage >= 2 && rand01(s.id, k * 3 + leg) < 0.15f * static_cast<float>(stage))) {  // the faces towards us: crossed (braces shot away)
                fine_line(up(leg, t0), up(leg + 1, t1), shade(col, 0.85f));
                fine_line(up(leg + 1, t0), up(leg, t1), shade(col, 0.85f));
            }
        }
    }
    // A platform, the antenna panels round the top, dishes below.
    for (int leg = 0; leg < 3; ++leg) fine_line(up(leg, 0.9f), up((leg + 1) % 3, 0.9f), {90, 92, 90, 255});
    int panel = 0;
    for (const float dx : {-4.0f, 0.0f, 4.0f}) {
        if (stage >= 2 && panel++ < (stage == 2 ? 1 : 3)) continue;  // shot off
        const Vector2 p{tip.x + dx, tip.y + 8.0f};
        DrawRectangleRec({std::round(p.x - 1.0f), std::round(p.y), 2.0f, 8.0f}, lit({228, 228, 222, 255}));
    }
    for (const auto& [t, dx] : {std::pair{0.7f, -4.0f}, std::pair{0.6f, 4.0f}}) {
        if (stage >= 2 && dx < 0.0f) continue;
        const Vector2 p = bend(lerp(foot, {foot.x, foot.y - kHeight}, t), t);
        DrawEllipse(static_cast<int>(p.x + dx), static_cast<int>(p.y), 2.5f, 3.0f, lit({220, 220, 214, 255}));
        DrawEllipse(static_cast<int>(p.x + dx + 0.6f), static_cast<int>(p.y + 0.4f), 1.6f, 2.0f, lit({178, 178, 172, 255}));
    }
    if (stage < 3) {  // the whip on top, its red light
        fine_line(tip, {tip.x, tip.y - 6.0f}, {90, 92, 90, 255});
        DrawCircleV({tip.x, tip.y - 6.5f}, 1.4f, {230, 60, 50, 255});
    }
}

// A gas station: a canopy on slim posts over two islands of pumps, its
// fascia in the brand's colours; the shop behind with its glass front and
// its sign band; the price board on its pole by the road.
void draw_gas_station(const engine::TileMap& map, const engine::Structure& s, float damage) {
    const Rectangle r = footprint(s, 0.1f);
    const uint32_t h = tile_hash(s.tiles.front().x * 5 + 2, s.tiles.front().y * 9 + 4);
    const float soot = 1.0f - 0.35f * damage;
    const int stage = stage_of(damage);
    static constexpr Color kBrands[4][2] = {{{206, 44, 40, 255}, {236, 236, 230, 255}},
                                             {{246, 196, 36, 255}, {40, 80, 160, 255}},
                                             {{40, 140, 70, 255}, {236, 236, 230, 255}},
                                             {{36, 90, 170, 255}, {236, 236, 230, 255}}};
    const Color brand = kBrands[h % 4][0];
    const Color brand2 = kBrands[h % 4][1];
    // The shop along the back: its glass front, its sign band, a flat roof.
    const Rectangle shop{r.x, r.y, r.width, r.height * 0.36f};
    const Box b = box_on(map, shop, 14.0f);
    const Color wall = shade({220, 218, 210, 255}, soot);
    wall_texture(b.lit_face, 0.0f, 1.0f, 0.0f, 14.0f, Wall::Pastel, wall, h);
    wall_texture(b.shade_face, 0.0f, 1.0f, 0.0f, 14.0f, Wall::Pastel, shade(wall, 0.74f), h + 1);
    {
        const Face& f = b.shade_face;
        const bool out = damage > 0.4f;
        face_fill(f, 0.12f, 0.88f, 1.5f, 9.0f, out ? Color{20, 18, 16, 255} : shade({70, 96, 118, 255}, 0.8f));
        if (!out) {
            face_fill(f, 0.12f, 0.88f, 6.0f, 9.0f, shade({130, 158, 180, 255}, 0.8f));
            for (float u = 0.12f; u <= 0.881f; u += 0.19f) face_line(f, u, 1.5f, u, 9.0f, shade({200, 200, 196, 255}, 0.8f));
        }
        face_fill(f, 0.0f, 1.0f, 10.5f, 14.0f, shade(brand, 0.85f * soot));
        face_fill(b.lit_face, 0.0f, 1.0f, 10.5f, 14.0f, shade(brand, soot));
    }
    wear_walls(b.lit_face, 1.0f, 0.0f, 14.0f, stage, {150, 146, 138, 255}, false, h);
    wear_walls(b.shade_face, 0.74f, 0.0f, 14.0f, stage, {150, 146, 138, 255}, false, h + 3u);
    if (stage == 2) fire_spot(b.shade_face.at(0.4f, 3.0f), true);  // burning in the shop
    fill_quad(b.top[0], b.top[1], b.top[2], b.top[3], stage >= 3 ? Color{40, 36, 33, 255} : shade({150, 148, 144, 255}, soot));
    if (stage >= 3) fire_spot(lerp(b.top[0], b.top[2], 0.5f), false);
    // The islands, the pumps on them.
    for (const float k : {0.35f, 0.65f}) {
        const Vector2 g{r.x + r.width * k, r.y + r.height * 0.7f};
        const Box isl = box_on(map, {g.x - 0.1f, g.y - 0.28f, 0.2f, 0.56f}, 1.5f);
        face_fill(isl.lit_face, 0.0f, 1.0f, 0.0f, 1.5f, shade({196, 194, 186, 255}, soot));
        face_fill(isl.shade_face, 0.0f, 1.0f, 0.0f, 1.5f, shade({196, 194, 186, 255}, 0.74f * soot));
        if (stage >= 2 && k > 0.5f) {  // a pump knocked down, lying by its island; burning
            const Box down = box_on(map, {g.x + 0.12f, g.y - 0.08f, 0.2f, 0.12f}, 4.0f);
            face_fill(down.lit_face, 0.0f, 1.0f, 0.0f, 4.0f, stage >= 3 ? Color{44, 40, 36, 255} : shade(brand2, soot));
            face_fill(down.shade_face, 0.0f, 1.0f, 0.0f, 4.0f, stage >= 3 ? Color{34, 30, 28, 255} : shade(brand, 0.8f * soot));
            fill_quad(down.top[0], down.top[1], down.top[2], down.top[3], stage >= 3 ? Color{50, 46, 42, 255} : shade(brand, soot));
            fire_spot(down.top[2], stage == 2);
            continue;
        }
        const Box pump = box_on(map, {g.x - 0.06f, g.y - 0.1f, 0.12f, 0.2f}, 11.0f);
        face_fill(pump.lit_face, 0.0f, 1.0f, 1.5f, 11.0f, shade(brand2, soot));
        face_fill(pump.shade_face, 0.0f, 1.0f, 1.5f, 11.0f, shade(brand, 0.8f * soot));
        face_fill(pump.shade_face, 0.2f, 0.8f, 6.0f, 9.0f, {40, 44, 48, 255});  // its display
        fill_quad(pump.top[0], pump.top[1], pump.top[2], pump.top[3], shade(brand, soot));
        DrawLineV(pump.shade_face.at(0.1f, 6.0f), {pump.shade_face.at(0.1f, 2.0f).x - 1.5f, pump.shade_face.at(0.1f, 2.0f).y}, lit({30, 30, 30, 255}));
    }
    // The canopy on its posts: white, its fascia in the brand's colours.
    const Rectangle roof{r.x + r.width * 0.08f, r.y + r.height * 0.44f, r.width * 0.84f, r.height * 0.52f};
    constexpr float kUp = 22.0f;
    // Its corners' heights: at the worst the front posts buckled, the front come down near the ground.
    const float up[4] = {kUp, kUp, stage >= 3 ? 5.0f : kUp, stage >= 3 ? 9.0f : kUp};
    const Vector2 posts[4] = {{roof.x + 0.1f, roof.y + 0.1f}, {roof.x + roof.width - 0.1f, roof.y + 0.1f},
                              {roof.x + roof.width - 0.1f, roof.y + roof.height - 0.1f}, {roof.x + 0.1f, roof.y + roof.height - 0.1f}};
    for (int i = 0; i < 4; ++i) {
        const Vector2 f = on_terrain(map, posts[i]);
        const float z = up[i] - (up[i] < kUp ? 1.0f : 0.0f);
        DrawLineEx(f, {f.x + (up[i] < kUp ? 5.0f : 0.0f), f.y - z}, 2.0f, lit(shade({226, 226, 222, 255}, soot)));
    }
    const Vector2 corner[4] = {{roof.x, roof.y}, {roof.x + roof.width, roof.y}, {roof.x + roof.width, roof.y + roof.height}, {roof.x, roof.y + roof.height}};
    Vector2 lo[4];
    for (int i = 0; i < 4; ++i) lo[i] = on_terrain(map, corner[i], up[i]);
    Vector2 hi[4];
    for (int i = 0; i < 4; ++i) hi[i] = {lo[i].x, lo[i].y - 4.0f};
    fill_quad(hi[0], hi[1], hi[2], hi[3], shade({232, 232, 228, 255}, soot * (stage >= 3 ? 0.6f : 1.0f)));
    fill_quad(lo[1], lo[2], hi[2], hi[1], shade(brand, soot));
    fill_quad(lo[2], lo[3], hi[3], hi[2], shade(brand, 0.78f * soot));
    DrawLineV({lo[1].x, lo[1].y - 2.0f}, {lo[2].x, lo[2].y - 2.0f}, lit(shade(brand2, soot)));
    DrawLineV({lo[2].x, lo[2].y - 2.0f}, {lo[3].x, lo[3].y - 2.0f}, lit(shade(brand2, 0.8f * soot)));
    if (stage >= 2) {  // holes through it, the soot of the fire under it
        for (int k = 0; k < stage * 2; ++k) {
            const float a = 0.15f + 0.7f * rand01(h, 40 + k);
            const float c = 0.15f + 0.7f * rand01(h, 50 + k);
            const Vector2 p = lerp(lerp(hi[0], hi[1], a), lerp(hi[3], hi[2], a), c);
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), 3.5f, 2.0f, lit({24, 20, 18, 255}));
            DrawEllipse(static_cast<int>(p.x + 4.0f), static_cast<int>(p.y + 1.0f), 5.0f, 2.5f, lit(ColorAlpha({40, 36, 32, 255}, 0.6f)));
        }
    }
    // The price board on its pole by the road.
    {
        const Vector2 f = on_terrain(map, {r.x + r.width - 0.05f, r.y + r.height - 0.05f});
        DrawLineEx(f, {f.x, f.y - 16.0f}, 1.6f, lit({120, 120, 118, 255}));
        DrawRectangleRec({std::round(f.x - 4.0f), std::round(f.y - 30.0f), 8.0f, 15.0f}, lit(shade(brand, soot)));
        for (int k = 0; k < 3; ++k) DrawRectangleRec({std::round(f.x - 3.0f), std::round(f.y - 27.0f + 4.0f * k), 6.0f, 2.0f}, lit(brand2));
    }
}

// A grain elevator: a row of tall concrete silos (the lines of their
// pours, streaks down them), a gallery along their tops; the work tower at
// one end, taller, its windows in bands, a tin penthouse on top; a loading
// shed at its foot.
void draw_elevator(const engine::TileMap& map, const engine::Structure& s, float damage) {
    const Rectangle r = footprint(s, 0.1f);
    const uint32_t h = tile_hash(s.tiles.front().x * 3 + 7, s.tiles.front().y * 7 + 3);
    const float soot = 1.0f - 0.35f * damage;
    const int stage = stage_of(damage);
    const Color concrete = shade({200, 194, 180, 255}, soot);
    constexpr float kSilo = 80.0f;
    constexpr float kRadius = 10.0f;
    const bool along_x = r.width >= r.height;
    const float length = along_x ? r.width : r.height;
    // The work tower at the far end, first (it's behind).
    const Rectangle tower = along_x ? Rectangle{r.x, r.y + r.height * 0.2f, 0.8f, r.height * 0.6f} : Rectangle{r.x + r.width * 0.2f, r.y, r.width * 0.6f, 0.8f};
    {
        constexpr float kTower = 100.0f;
        const Box t = box_on(map, tower, kTower);
        wall_texture(t.lit_face, 0.0f, 1.0f, 0.0f, kTower, Wall::Concrete, concrete, h);
        wall_texture(t.shade_face, 0.0f, 1.0f, 0.0f, kTower, Wall::Concrete, shade(concrete, 0.74f), h + 1);
        for (float v = 20.0f; v < kTower - 8.0f; v += 12.0f) {
            ribbon_glazing(t.lit_face, 0.15f, 0.85f, v, v + 3.0f, 1.0f, 0.15f + damage, h + static_cast<uint32_t>(v));
            ribbon_glazing(t.shade_face, 0.15f, 0.85f, v, v + 3.0f, 0.74f, 0.15f + damage, h + static_cast<uint32_t>(v) + 1);
        }
        if (stage >= 1) {
            pockmarks(t.lit_face, 0.0f, 1.0f, 0.0f, kTower, stage * 30, h + 5u);
            pockmarks(t.shade_face, 0.0f, 1.0f, 0.0f, kTower, stage * 24, h + 6u);
        }
        if (stage == 2) fire_spot(t.lit_face.at(0.5f, 45.0f), true);
        if (stage >= 3) {  // burnt out, the penthouse gone
            burnt(t.lit_face, 0.0f, 1.0f, 20.0f, kTower, 0.6f, h);
            burnt(t.shade_face, 0.0f, 1.0f, 20.0f, kTower, 0.6f, h + 1u);
            wall_hole(t.lit_face, 0.4f, 60.0f, 12.0f, 14.0f, h + 7u);
            fill_quad(t.top[0], t.top[1], t.top[2], t.top[3], {40, 36, 33, 255});
            fire_spot(lerp(t.top[0], t.top[2], 0.5f), false);
        } else {
            Vector2 pb[4];
            for (int i = 0; i < 4; ++i) pb[i] = t.top[i];
            const Face pl{pb[1], pb[2]};
            const Face ps{pb[2], pb[3]};
            wall_texture(pl, 0.1f, 0.9f, 0.0f, 9.0f, Wall::Corrugated, shade({150, 156, 160, 255}, soot), h + 2);
            wall_texture(ps, 0.1f, 0.9f, 0.0f, 9.0f, Wall::Corrugated, shade({150, 156, 160, 255}, 0.74f * soot), h + 3);
            fill_quad(t.top[0], t.top[1], t.top[2], t.top[3], shade({120, 116, 108, 255}, soot));
        }
    }
    // The silos in two rows, side by side, far to near.
    std::vector<Vector2> spots;
    const float cross = along_x ? r.height : r.width;
    for (const float c : {0.3f, 0.72f}) {
        if (cross < 1.2f && c > 0.5f) continue;
        for (float a = 1.05f; a < length - 0.2f; a += 0.62f) {
            spots.push_back(along_x ? Vector2{r.x + a, r.y + cross * c} : Vector2{r.x + cross * c, r.y + a});
        }
    }
    std::sort(spots.begin(), spots.end(), [](Vector2 p, Vector2 q) { return p.x + p.y < q.x + q.y; });
    for (size_t i = 0; i < spots.size(); ++i) {
        const Vector2 foot = on_terrain(map, spots[i]);
        const int n = static_cast<int>(i);
        // At the worst some broken off part way up, ragged, hollow.
        const bool broken_off = stage >= 3 && rand01(h, 500 + n) < 0.35f;
        const float silo = broken_off ? kSilo * (0.5f + 0.25f * rand01(h, 510 + n)) : kSilo;
        // Round: shaded across in strips, lit on the left.
        constexpr int kStrips = 8;
        for (int st = 0; st < kStrips; ++st) {
            const float x0 = foot.x - kRadius + 2.0f * kRadius * static_cast<float>(st) / kStrips;
            const float x1 = foot.x - kRadius + 2.0f * kRadius * static_cast<float>(st + 1) / kStrips;
            const float t = (static_cast<float>(st) + 0.5f) / kStrips;
            const float light = 1.18f - 0.5f * t;
            const float top = broken_off ? silo - 10.0f * rand01(h, 520 + n * 9 + st) : silo;
            DrawRectangleRec({std::round(x0), foot.y - top, std::round(x1) - std::round(x0), top}, lit(shade(concrete, light)));
        }
        for (float v = 6.0f; v < silo - 8.0f; v += 6.0f) DrawLineV({foot.x - kRadius, foot.y - v}, {foot.x + kRadius, foot.y - v}, lit(shade(concrete, 0.88f)));
        for (int k2 = 0; k2 < 4; ++k2) {  // streaks down it
            const float x = foot.x - kRadius + 2.0f * kRadius * rand01(h, n * 11 + k2);
            DrawLineV({x, foot.y - silo + 10.0f}, {x, foot.y - silo + 10.0f + 20.0f * rand01(h, n * 13 + k2)}, lit(shade(concrete, 0.8f)));
        }
        if (broken_off) {  // its inside dark, the rebar out of the break
            DrawEllipse(static_cast<int>(foot.x), static_cast<int>(foot.y - silo + 4.0f), kRadius - 2.0f, kRadius * 0.4f, lit({30, 26, 24, 255}));
            for (int k2 = 0; k2 < 4; ++k2) {
                const float x = foot.x - kRadius + 2.0f * kRadius * rand01(h, 540 + n * 5 + k2);
                DrawLineV({x, foot.y - silo + 2.0f}, {x + 2.0f * (rand01(h, 560 + k2) - 0.5f), foot.y - silo - 4.0f}, lit({74, 56, 44, 255}));
            }
            burnt(Face{{foot.x - kRadius, foot.y}, {foot.x + kRadius, foot.y}}, 0.0f, 1.0f, silo * 0.4f, silo, 0.4f, h + static_cast<uint32_t>(n));
        } else {
            DrawEllipse(static_cast<int>(foot.x), static_cast<int>(foot.y - kSilo), kRadius, kRadius * 0.5f, lit(shade(concrete, 1.1f)));
        }
        if (stage >= 1) {  // shrapnel's pits
            for (int k2 = 0; k2 < stage * 4; ++k2) {
                const Vector2 p{foot.x - kRadius + 2.0f * kRadius * rand01(h, 600 + n * 17 + k2), foot.y - silo * rand01(h, 700 + n * 17 + k2)};
                DrawPixelV({std::round(p.x), std::round(p.y)}, lit({50, 46, 42, 255}));
            }
        }
        if (damage > 0.35f && rand01(h, 300 + n) < damage) {  // a hole in it, the grain spilling
            const Vector2 p{foot.x - 3.0f + 6.0f * rand01(h, 310 + n), foot.y - 20.0f - 40.0f * rand01(h, 320 + n)};
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), 3.0f, 3.5f, lit({24, 20, 18, 255}));
            DrawTriangle({p.x - 1.0f, p.y + 2.0f}, {p.x + 3.0f, foot.y}, {p.x - 5.0f, foot.y}, lit({196, 170, 96, 255}));
        }
    }
    // The gallery along their tops, from the tower (down in the middle at the worst).
    for (const auto& [a0, a1] : stage >= 3 ? std::vector<std::pair<float, float>>{{0.0f, 0.4f}, {0.62f, 1.0f}} : std::vector<std::pair<float, float>>{{0.0f, 1.0f}}) {
        const float run = length - 0.9f;
        const Rectangle gal = along_x ? Rectangle{r.x + 0.7f + run * a0, r.y + r.height * 0.5f - 0.18f, run * (a1 - a0), 0.36f}
                                      : Rectangle{r.x + r.width * 0.5f - 0.18f, r.y + 0.7f + run * a0, 0.36f, run * (a1 - a0)};
        const Box g = box_on(map, gal, 0.0f);
        Vector2 gb[4];
        for (int i = 0; i < 4; ++i) gb[i] = {g.base[i].x, g.base[i].y - kSilo};
        const Face gl{gb[1], gb[2]};
        const Face gs{gb[2], gb[3]};
        wall_texture(gl, 0.0f, 1.0f, 0.0f, 6.0f, Wall::Corrugated, shade({160, 162, 160, 255}, soot), h + 20);
        wall_texture(gs, 0.0f, 1.0f, 0.0f, 6.0f, Wall::Corrugated, shade({160, 162, 160, 255}, 0.74f * soot), h + 21);
        Vector2 gt[4];
        for (int i = 0; i < 4; ++i) gt[i] = {gb[i].x, gb[i].y - 6.0f};
        fill_quad(gt[0], gt[1], gt[2], gt[3], shade({130, 128, 124, 255}, soot));
    }
    // The loading shed at the tower's foot.
    {
        const Rectangle shed = along_x ? Rectangle{r.x + 0.1f, r.y + r.height * 0.75f, 0.9f, r.height * 0.25f}
                                       : Rectangle{r.x + r.width * 0.75f, r.y + 0.1f, r.width * 0.25f, 0.9f};
        const Box sb = box_on(map, shed, 12.0f);
        wall_texture(sb.lit_face, 0.0f, 1.0f, 0.0f, 12.0f, Wall::Corrugated, shade({150, 156, 160, 255}, soot), h + 30);
        wall_texture(sb.shade_face, 0.0f, 1.0f, 0.0f, 12.0f, Wall::Corrugated, shade({150, 156, 160, 255}, 0.74f * soot), h + 31);
        face_fill(sb.shade_face, 0.25f, 0.75f, 0.0f, 9.0f, {40, 36, 32, 255});
        fill_quad(sb.top[0], sb.top[1], sb.top[2], sb.top[3], shade({120, 122, 122, 255}, soot));
    }
}





// --- A player's buildings ------------------------------------------------------------
//
// A front-line base as it's set up: in the buildings there are (the old
// district office for the headquarters, a school's block for the
// barracks), in steel hangars and sheds put up, in army tents; sandbags
// before the doors and the low windows, the windows taped across, nets
// over the roofs, aerials and masts, the side's flag; crates, drums and
// tyres about.

// Sandbags along the ground from g0 to g1, `rows` high: each bag a
// fat pillow, lit on top.
void sandbags(const engine::TileMap& map, Vector2 g0, Vector2 g1, int rows, uint32_t seed) {
    const float len = std::hypot(g1.x - g0.x, g1.y - g0.y);
    const int bags = std::max(1, static_cast<int>(len / 0.14f));
    for (int row = 0; row < rows; ++row) {
        for (int i = 0; i < bags; ++i) {
            const float t = (static_cast<float>(i) + (row % 2 ? 0.5f : 0.0f) + 0.5f) / static_cast<float>(bags + (row % 2));
            if (t > 1.0f) continue;
            const Vector2 p = on_terrain(map, lerp(g0, g1, t), 1.0f + 1.8f * static_cast<float>(row));
            const Color bag = shade({170, 150, 108, 255}, 0.9f + 0.18f * rand01(seed, row * 37 + i));
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), 2.6f, 1.4f, lit(shade(bag, 0.7f)));
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y - 0.5f), 2.3f, 1.1f, lit(bag));
        }
    }
}

// Crates stacked up (ammunition's green, stores' wood), a tarpaulin over some.
void crates(const engine::TileMap& map, Vector2 g, int n, Color c, uint32_t seed) {
    for (int i = 0; i < n; ++i) {
        const float dx = static_cast<float>(i % 3) * 0.2f;
        const float dy = static_cast<float>((i / 3) % 2) * 0.16f;
        const float z = static_cast<float>(i / 6) * 4.0f;
        const Box b = box_on(map, {g.x + dx, g.y + dy, 0.18f, 0.14f}, 4.0f);
        const Color k = shade(c, 0.9f + 0.2f * rand01(seed, i));
        Vector2 lb[4];
        for (int j = 0; j < 4; ++j) lb[j] = {b.base[j].x, b.base[j].y - z};
        const Face lf{lb[1], lb[2]};
        const Face sf{lb[2], lb[3]};
        face_fill(lf, 0.0f, 1.0f, 0.0f, 4.0f, k);
        face_fill(sf, 0.0f, 1.0f, 0.0f, 4.0f, shade(k, 0.74f));
        face_line(sf, 0.0f, 2.0f, 1.0f, 2.0f, shade(k, 0.55f));
        fill_quad({lb[0].x, lb[0].y - 4.0f}, {lb[1].x, lb[1].y - 4.0f}, {lb[2].x, lb[2].y - 4.0f}, {lb[3].x, lb[3].y - 4.0f}, shade(k, 1.15f));
    }
}

// Steel drums standing about: fuel's, oil's.
void drums(const engine::TileMap& map, Vector2 g, int n, Color c) {
    for (int i = 0; i < n; ++i) {
        const Vector2 p = on_terrain(map, {g.x + static_cast<float>(i % 3) * 0.13f, g.y + static_cast<float>(i / 3) * 0.13f});
        DrawRectangleRec({std::round(p.x - 2.0f), std::round(p.y - 5.0f), 4.0f, 5.0f}, lit(shade(c, 0.8f)));
        DrawRectangleRec({std::round(p.x - 2.0f), std::round(p.y - 5.0f), 1.5f, 5.0f}, lit(shade(c, 1.1f)));
        DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y - 5.0f), 2.0f, 1.0f, lit(shade(c, 1.2f)));
        DrawLineV({p.x - 2.0f, p.y - 2.5f}, {p.x + 2.0f, p.y - 2.5f}, lit(shade(c, 0.6f)));
    }
}

// Tyres in a stack.
void tyres(const engine::TileMap& map, Vector2 g, int n) {
    const Vector2 p = on_terrain(map, g);
    for (int i = 0; i < n; ++i) {
        const float y = p.y - 1.5f * static_cast<float>(i);
        DrawEllipse(static_cast<int>(p.x), static_cast<int>(y), 4.0f, 2.0f, lit({34, 34, 32, 255}));
        DrawEllipse(static_cast<int>(p.x), static_cast<int>(y - 0.5f), 2.0f, 1.0f, lit({60, 58, 54, 255}));
    }
}

// The side's flag on its pole.
void flag_on(Vector2 foot, float tall, Color team) {
    fine_line(foot, {foot.x, foot.y - tall}, {70, 70, 68, 255});
    const Vector2 t{foot.x + 1.0f, foot.y - tall + 1.0f};
    fill_quad(t, {t.x + 6.0f, t.y + 1.0f}, {t.x + 6.0f, t.y + 5.0f}, {t.x, t.y + 4.0f}, team);
    fill_quad({t.x + 6.0f, t.y + 1.0f}, {t.x + 11.0f, t.y}, {t.x + 11.0f, t.y + 4.0f}, {t.x + 6.0f, t.y + 5.0f}, shade(team, 0.82f));
}

// A lattice mast: legs narrowing up, braced; a whip or two on top.
void lattice_mast(Vector2 foot, float tall, float wide) {
    const Color steel{150, 152, 150, 255};
    const Vector2 l0{foot.x - wide, foot.y};
    const Vector2 r0{foot.x + wide, foot.y};
    const Vector2 l1{foot.x - 1.0f, foot.y - tall};
    const Vector2 r1{foot.x + 1.0f, foot.y - tall};
    fine_line(l0, l1, steel);
    fine_line(r0, r1, steel);
    for (int k = 0; k < 7; ++k) {
        const float t0 = static_cast<float>(k) / 7.0f;
        const float t1 = static_cast<float>(k + 1) / 7.0f;
        fine_line(lerp(l0, l1, t0), lerp(r0, r1, t1), shade(steel, 0.8f));
        fine_line(lerp(r0, r1, t0), lerp(l0, l1, t1), shade(steel, 0.8f));
    }
    fine_line({foot.x, foot.y - tall}, {foot.x, foot.y - tall - 9.0f}, {60, 60, 58, 255});
}

// A camouflage net thrown over a roof: its mesh with strips of cloth in greens and browns.
void net_over(const engine::TileMap& map, Rectangle g, float z, uint32_t seed) {
    const Vector2 c[4] = {on_terrain(map, {g.x, g.y}, z), on_terrain(map, {g.x + g.width, g.y}, z), on_terrain(map, {g.x + g.width, g.y + g.height}, z),
                          on_terrain(map, {g.x, g.y + g.height}, z)};
    fill_quad(c[0], c[1], c[2], c[3], ColorAlpha({72, 86, 54, 255}, 0.85f));
    constexpr Color kCloth[4] = {{50, 68, 36, 255}, {84, 98, 56, 255}, {104, 90, 60, 255}, {130, 122, 86, 255}};
    const int n = static_cast<int>(40.0f * g.width * g.height) + 10;
    for (int i = 0; i < n; ++i) {
        const Vector2 p = on_terrain(map, {g.x + g.width * rand01(seed, i), g.y + g.height * rand01(seed, i + 500)}, z + 1.0f);
        DrawRectangleRec({std::round(p.x - 1.0f), std::round(p.y), 2.0f + static_cast<float>(i % 2), 1.0f}, lit(kCloth[i % 4]));
    }
    // Its edges hanging over, pegged down.
    for (int i = 0; i < 4; ++i) DrawLineV(c[i], {c[i].x, c[i].y + z * 0.5f}, lit({60, 70, 46, 255}));
}

// A hipped roof over a box: the ridge along its long side, the ends sloping.
void hip_roof(const engine::TileMap& map, Rectangle r, float wall, float rise, Roof kind, Color roof, uint32_t seed, float eave = 0.06f) {
    Vector2 e[4];
    const Vector2 out[4] = {{r.x - eave, r.y - eave}, {r.x + r.width + eave, r.y - eave}, {r.x + r.width + eave, r.y + r.height + eave},
                            {r.x - eave, r.y + r.height + eave}};
    for (int i = 0; i < 4; ++i) e[i] = on_terrain(map, out[i], wall - 1.0f);
    const bool along_x = r.width >= r.height;
    const float half = std::min(r.width, r.height) * 0.5f;
    const Vector2 a = along_x ? Vector2{r.x + half, r.y + r.height * 0.5f} : Vector2{r.x + r.width * 0.5f, r.y + half};
    const Vector2 b = along_x ? Vector2{r.x + r.width - half, r.y + r.height * 0.5f} : Vector2{r.x + r.width * 0.5f, r.y + r.height - half};
    const Vector2 ra = on_terrain(map, a, wall + rise);
    const Vector2 rb = on_terrain(map, b, wall + rise);
    if (along_x) {
        roof_texture({e[0], e[1], ra, rb}, kind, roof, 1.08f, seed);
        roof_texture({e[3], e[0], ra, ra}, kind, roof, 0.95f, seed + 1);
        roof_texture({e[1], e[2], rb, rb}, kind, roof, 1.2f, seed + 2);
        roof_texture({e[3], e[2], ra, rb}, kind, roof, 0.8f, seed + 3);
    } else {
        roof_texture({e[0], e[3], ra, rb}, kind, roof, 1.08f, seed);
        roof_texture({e[0], e[1], ra, ra}, kind, roof, 1.1f, seed + 1);
        roof_texture({e[1], e[2], ra, rb}, kind, roof, 1.2f, seed + 2);
        roof_texture({e[3], e[2], rb, rb}, kind, roof, 0.8f, seed + 3);
    }
    DrawLineEx(ra, rb, 1.5f, lit(shade(roof, 0.62f)));
}

// A rounded steel hangar's roof (a Quonset's) over a box, its ribs down it.
void arch_roof(const engine::TileMap& map, Rectangle r, float wall, float rise, Color c, uint32_t seed) {
    const bool along_x = r.width >= r.height;
    constexpr int kSteps = 8;
    for (int k = 0; k < kSteps; ++k) {  // strips of it, from its far eave over to its near one
        const float t0 = static_cast<float>(k) / kSteps;
        const float t1 = static_cast<float>(k + 1) / kSteps;
        auto point = [&](float t, float along) {
            const float across = along_x ? r.y + r.height * t : r.x + r.width * t;
            const float z = wall + rise * std::sin(t * 3.14159f);
            return along_x ? on_terrain(map, {r.x + r.width * along, across}, z) : on_terrain(map, {across, r.y + r.height * along}, z);
        };
        const float slope = std::cos((t0 + t1) * 0.5f * 3.14159f);  // facing up-and-away, or down-and-near
        const float k_light = 0.8f + 0.35f * slope;
        roof_texture({point(t0, 0.0f), point(t0, 1.0f), point(t1, 0.0f), point(t1, 1.0f)}, Roof::Tin, c, k_light, seed + static_cast<uint32_t>(k));
    }
}

// A building put up: its foundation, its walls rising on it as the work
// goes on (the floor inside, the far walls' inner sides over the near
// ones), the scaffolding round it, the materials piled by it.
void draw_construction(const engine::TileMap& map, const engine::Structure& s, float done) {
    const Rectangle r = footprint(s, 0.1f);
    const Box f = box_on(map, r, 2.0f);
    face_fill(f.lit_face, 0.0f, 1.0f, 0.0f, 2.0f, {170, 166, 156, 255});
    face_fill(f.shade_face, 0.0f, 1.0f, 0.0f, 2.0f, {130, 126, 118, 255});
    fill_quad(f.top[0], f.top[1], f.top[2], f.top[3], {184, 180, 170, 255});
    const float wall = 3.0f + 17.0f * done;
    const Rectangle in{r.x + 0.1f, r.y + 0.1f, r.width - 0.2f, r.height - 0.2f};
    const uint32_t h = s.id * 2654435761u;
    Box b = box_on(map, in, wall);
    for (int i = 0; i < 4; ++i) {
        b.base[i].y -= 2.0f;
        b.top[i].y -= 2.0f;
    }
    b.lit_face = {b.base[1], b.base[2]};
    b.shade_face = {b.base[2], b.base[3]};
    wall_texture({b.base[0], b.base[1]}, 0.0f, 1.0f, 0.0f, wall, Wall::Silicate, {150, 148, 140, 255}, h + 2);  // the far walls, inside
    wall_texture({b.base[3], b.base[0]}, 0.0f, 1.0f, 0.0f, wall, Wall::Silicate, {176, 174, 166, 255}, h + 3);
    wall_texture(b.lit_face, 0.0f, 1.0f, 0.0f, wall, Wall::Silicate, {196, 194, 184, 255}, h);
    wall_texture(b.shade_face, 0.0f, 1.0f, 0.0f, wall, Wall::Silicate, {146, 144, 136, 255}, h + 1);
    for (int i = 0; i < 4; ++i) DrawLineEx(b.top[i], b.top[(i + 1) % 4], 2.0f, lit({214, 212, 204, 255}));  // the walls' tops
    // The scaffolding round it to the full height.
    const Color pole{120, 96, 64, 255};
    const float full = 24.0f;
    const Vector2 corners[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}};
    for (int i = 1; i < 4; ++i) {  // the near sides
        const Vector2 p = on_terrain(map, corners[i]);
        fine_line(p, {p.x, p.y - full}, pole);
        if (i < 3) {
            const Vector2 q = on_terrain(map, corners[i + 1]);
            for (const float z : {8.0f, 16.0f, full}) fine_line({p.x, p.y - z}, {q.x, q.y - z}, shade(pole, 0.85f));
        }
    }
    // Materials piled by it: planks, bricks.
    crates(map, {r.x + r.width + 0.05f, r.y + r.height * 0.2f}, 3, {150, 116, 74, 255}, h);
    crates(map, {r.x + r.width * 0.2f, r.y + r.height + 0.05f}, 4, {160, 84, 60, 255}, h + 3);
}

// The headquarters: the old district office, two storeys plastered yellow
// with white piers, a porch on columns; the windows taped across, the low
// ones sandbagged; a green tin roof half under a net, whip aerials and a
// mast beside it; the flag.
void draw_headquarters(const engine::TileMap& map, const engine::Structure& s, float damage, Color team) {
    const Rectangle r = footprint(s, 0.18f);
    const uint32_t h = s.id * 2654435761u;
    constexpr float kWall = 26.0f;
    const float soot = 1.0f - 0.4f * damage;
    const Box b = box_on(map, r, kWall);
    const Color wall = shade({216, 196, 136, 255}, soot);
    for (const auto& [f, k] : {std::pair{&b.lit_face, 1.0f}, std::pair{&b.shade_face, 0.74f}}) {
        wall_texture(*f, 0.0f, 1.0f, 3.0f, kWall, Wall::Pastel, shade(wall, k), h);
        face_fill(*f, 0.0f, 1.0f, 0.0f, 3.0f, shade({110, 100, 90, 255}, k * soot));
        face_fill(*f, 0.0f, 1.0f, 13.5f, 14.5f, shade({236, 232, 220, 255}, k * soot));  // the cornice between the floors
        wear_walls(*f, k, 3.0f, kWall, stage_of(damage), {156, 90, 64, 255}, true, h + static_cast<uint32_t>(k * 10.0f));
        const int bays = std::max(3, static_cast<int>(f->pixels() / 12.0f));
        const float px = f->px();
        for (int bay = 0; bay < bays; ++bay) {
            const float u = (static_cast<float>(bay) + 0.5f) / static_cast<float>(bays);
            face_fill(*f, static_cast<float>(bay) / bays, static_cast<float>(bay) / bays + 2.0f * px, 3.0f, kWall, shade({236, 232, 220, 255}, k * soot));  // a pier
            for (const float v : {5.0f, 17.0f}) {
                const bool broken = damage > 0.25f && rand01(h, bay * 7 + static_cast<int>(v)) < damage;
                window_on(*f, u, v, 4.0f, 6.0f, {charred({236, 234, 226, 255}, damage)}, broken, k);
                if (!broken) {  // taped across
                    face_line(*f, u - 2.0f * px, v, u + 2.0f * px, v + 6.0f, shade({230, 226, 210, 255}, k));
                    face_line(*f, u - 2.0f * px, v + 6.0f, u + 2.0f * px, v, shade({230, 226, 210, 255}, k));
                }
            }
        }
    }
    // The porch in the front: two columns, a pediment; the door; sandbags before it.
    {
        const Face& f = b.shade_face;
        const float px = f.px();
        door_on(f, 0.5f, 5.0f, 9.0f, {92, 66, 48, 255}, 0.74f * soot);
        for (const float u : {0.5f - 6.0f * px, 0.5f + 6.0f * px}) {
            const Vector2 base = f.at(u, 0.0f);
            DrawRectangleRec({std::round(base.x - 1.0f), std::round(base.y - 12.0f + 3.0f), 2.5f, 12.0f}, lit(shade({236, 232, 220, 255}, 0.9f * soot)));
        }
        const Vector2 p0 = f.at(0.5f - 8.0f * px, 12.0f);
        const Vector2 p1 = f.at(0.5f + 8.0f * px, 12.0f);
        fill_triangle({p0.x, p0.y + 3.0f}, {p1.x, p1.y + 3.0f}, {(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f - 2.0f}, shade({236, 232, 220, 255}, 0.95f * soot));
        const float yb = r.y + r.height + 0.35f;
        sandbags(map, {r.x + r.width * 0.3f, yb}, {r.x + r.width * 0.42f, yb}, 3, h);
        sandbags(map, {r.x + r.width * 0.58f, yb}, {r.x + r.width * 0.7f, yb}, 3, h + 1);
        sandbags(map, {r.x + r.width + 0.3f, r.y + r.height * 0.2f}, {r.x + r.width + 0.3f, r.y + r.height * 0.6f}, 2, h + 2);
    }
    if (stage_of(damage) >= 3) {
        fallen_roof(map, r, kWall, 11.0f, r.width >= r.height, false, wall, {86, 118, 88, 255}, false, h);
        flag_on(on_terrain(map, {r.x + r.width * 0.5f, r.y + r.height * 0.5f}, kWall - 4.0f), 30.0f, team);  // still flying
    } else {
        hip_roof(map, r, kWall, 11.0f, Roof::Tin, shade({86, 118, 88, 255}, soot), h);
        net_over(map, {r.x - 0.05f, r.y - 0.05f, r.width * 0.55f, r.height + 0.1f}, kWall + 6.0f, h);
        roof_holes(map, r, kWall + 5.0f, damage, {86, 118, 88, 255}, h);
        // Whip aerials on the roof; the flag.
        const Vector2 top = on_terrain(map, {r.x + r.width * 0.75f, r.y + r.height * 0.3f}, kWall + 8.0f);
        fine_line(top, {top.x, top.y - 22.0f}, {60, 60, 58, 255});
        fine_line({top.x + 4.0f, top.y + 2.0f}, {top.x + 4.0f, top.y - 16.0f}, {60, 60, 58, 255});
        flag_on(on_terrain(map, {r.x + r.width * 0.5f, r.y + r.height * 0.5f}, kWall + 11.0f), 20.0f, team);
    }
    lattice_mast(on_terrain(map, {r.x + r.width + 0.3f, r.y + r.height - 0.1f}), 70.0f, 4.0f);  // beside it, in front
}

// A barracks: a school's long block, grey silicate brick, a slate roof;
// its windows in a row, two doors; sandbags at its corners; the flag before it.
void draw_barracks_block(const engine::TileMap& map, const engine::Structure& s, float damage, Color team) {
    const Rectangle r = footprint(s, 0.2f);
    const uint32_t h = s.id * 2654435761u;
    constexpr float kWall = 30.0f;  // two storeys, like a village school
    const float soot = 1.0f - 0.4f * damage;
    const Rectangle block{r.x, r.y + r.height * 0.15f, r.width, r.height * 0.6f};
    const Box b = box_on(map, block, kWall);
    const Color wall = shade({196, 194, 184, 255}, soot);
    for (const auto& [f, k] : {std::pair{&b.lit_face, 1.0f}, std::pair{&b.shade_face, 0.74f}}) {
        wall_texture(*f, 0.0f, 1.0f, 2.5f, kWall, Wall::Silicate, shade(wall, k), h);
        face_fill(*f, 0.0f, 1.0f, 0.0f, 2.5f, shade({100, 94, 86, 255}, k * soot));
        wear_walls(*f, k, 2.5f, kWall, stage_of(damage), {150, 146, 138, 255}, false, h + static_cast<uint32_t>(k * 10.0f));
        const int n = std::max(2, static_cast<int>(f->pixels() / 10.0f));
        for (int floor = 0; floor < 2; ++floor) {
            for (int i = 0; i < n; ++i) {
                const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
                const int pane = i + floor * 20;
                window_on(*f, u, 5.0f + 13.0f * static_cast<float>(floor), 4.0f, 7.0f, {charred({232, 232, 228, 255}, damage)},
                          damage > 0.25f && rand01(h, pane + static_cast<int>(k * 50)) < damage, k);
            }
        }
    }
    door_on(b.shade_face, 0.28f, 5.0f, 10.0f, {70, 90, 70, 255}, 0.74f * soot);
    door_on(b.shade_face, 0.72f, 5.0f, 10.0f, {70, 90, 70, 255}, 0.74f * soot);
    face_fill(b.shade_face, 0.0f, 1.0f, kWall - 2.0f, kWall - 1.0f, shade(team, 0.8f));  // the side's stripe under the eaves
    if (stage_of(damage) >= 3) {
        fallen_roof(map, block, kWall, 12.0f, block.width >= block.height, true, wall, {118, 120, 122, 255}, false, h);
    } else {
        gable_roof(map, block, kWall, 12.0f, block.width >= block.height, Roof::Slate, shade({118, 120, 122, 255}, soot), wall, h);
        roof_holes(map, block, kWall + 4.0f, damage, {118, 120, 122, 255}, h);
    }
    sandbags(map, {r.x + r.width + 0.1f, r.y + r.height * 0.9f}, {r.x + r.width + 0.1f, r.y + r.height * 0.6f}, 2, h);
    sandbags(map, {r.x - 0.05f, r.y + r.height + 0.1f}, {r.x + r.width * 0.25f, r.y + r.height + 0.1f}, 2, h + 1);
    flag_on(on_terrain(map, {r.x + r.width * 0.5f, r.y + r.height + 0.05f}), 34.0f, team);
}

// A steel hangar: corrugated walls, a rounded roof, big gates in its end
// (the tanks' one: one of them open, dark inside), the side's stripe across them.
void draw_hangar(const engine::TileMap& map, const engine::Structure& s, float damage, Color team, float wall, bool open) {
    const Rectangle r = footprint(s, 0.15f);
    const uint32_t h = s.id * 2654435761u;
    const float soot = 1.0f - 0.4f * damage;
    const bool along_x = r.width < r.height;  // its gates face us, down the +y face: the ridge runs along y
    const Box b = box_on(map, r, wall);
    const Color iron = shade({120, 132, 118, 255}, soot);
    wall_texture(b.lit_face, 0.0f, 1.0f, 0.0f, wall, Wall::Corrugated, iron, h);
    wall_texture(b.shade_face, 0.0f, 1.0f, 0.0f, wall, Wall::Corrugated, shade(iron, 0.74f), h + 1);
    face_fill(b.lit_face, 0.0f, 1.0f, 0.0f, 2.5f, shade({130, 126, 118, 255}, soot));
    face_fill(b.shade_face, 0.0f, 1.0f, 0.0f, 2.5f, shade({130, 126, 118, 255}, 0.74f * soot));
    const int stage = stage_of(damage);
    if (stage >= 1) {
        pockmarks(b.lit_face, 0.0f, 1.0f, 0.0f, wall, stage * 16, h);
        pockmarks(b.shade_face, 0.0f, 1.0f, 0.0f, wall, stage * 12, h + 1u);
    }
    if (stage >= 2) sheets_off(b.lit_face, 3.0f, wall, stage == 2 ? 0.2f : 0.45f, h + 2u);
    // The gates in the front end.
    const Face& f = b.shade_face;
    const float gh = wall * 0.78f;
    for (const auto& [u0, u1, is_open] : {std::tuple{0.1f, 0.48f, open}, std::tuple{0.52f, 0.9f, false}}) {
        if (is_open || (damage > 0.5f && !open)) {
            face_fill(f, u0, u1, 0.0f, gh, {26, 24, 22, 255});
            face_fill(f, u0, u1, gh - 3.0f, gh, {40, 38, 34, 255});
        } else {
            wall_texture(f, u0, u1, 0.0f, gh, Wall::Corrugated, shade({110, 120, 106, 255}, 0.74f * soot), h + 5);
            face_line(f, u0, 0.5f, u1, gh - 0.5f, shade({80, 88, 76, 255}, 0.74f));
        }
        face_fill(f, u0, u1, gh * 0.55f, gh * 0.55f + 2.0f, shade(team, 0.8f));
    }
    const float rise = std::min(r.width, r.height) * 9.0f;
    arch_roof(map, r, wall, rise, shade({132, 140, 128, 255}, soot), h);
    if (stage >= 2) {  // its panels blown off, the ribs under them
        const bool ribs_x = r.width >= r.height;
        auto point = [&](float t, float along) {
            const float across = ribs_x ? r.y + r.height * t : r.x + r.width * t;
            const float z = wall + rise * std::sin(t * 3.14159f);
            return ribs_x ? on_terrain(map, {r.x + r.width * along, across}, z) : on_terrain(map, {across, r.y + r.height * along}, z);
        };
        constexpr int kCells = 8;
        for (int a = 0; a < kCells; ++a) {
            for (int k = 0; k < kCells; ++k) {
                if (rand01(h, 900 + a * kCells + k) > (stage == 2 ? 0.12f : 0.45f)) continue;
                const float t0 = static_cast<float>(k) / kCells;
                const float t1 = static_cast<float>(k + 1) / kCells;
                const float l0 = static_cast<float>(a) / kCells;
                const float l1 = static_cast<float>(a + 1) / kCells;
                fill_quad(point(t0, l0), point(t0, l1), point(t1, l1), point(t1, l0), {26, 22, 20, 255});
                for (const float l : {l0, l1}) DrawLineV(point(t0, l), point(t1, l), lit({92, 96, 90, 255}));  // the ribs
            }
        }
        if (stage >= 3) fire_spot(on_terrain(map, {r.x + r.width * 0.5f, r.y + r.height * 0.5f}, wall), false);
    }
    roof_holes(map, r, wall + 8.0f, damage, {132, 140, 128, 255}, h);
    (void)along_x;
}

// A railway station: a provincial one, plastered cream with white trims,
// tall arched windows, a hipped tin roof; its platform along the tracks
// under a canopy on posts.
void draw_station(const engine::TileMap& map, const engine::Structure& s, float damage, Color team) {
    const Rectangle r = footprint(s, 0.08f);
    const uint32_t h = s.id * 2654435761u;
    const float soot = 1.0f - 0.4f * damage;
    // The platform on the tracks' side, its canopy.
    const Rectangle platform{r.x, r.y, r.width, r.height * 0.4f};
    const Box pl = box_on(map, platform, 2.0f);
    face_fill(pl.lit_face, 0.0f, 1.0f, 0.0f, 2.0f, {150, 146, 138, 255});
    fill_quad(pl.top[0], pl.top[1], pl.top[2], pl.top[3], {176, 172, 164, 255});
    const Rectangle building{r.x + 0.3f, r.y + r.height * 0.42f, r.width - 0.6f, r.height * 0.56f};
    constexpr float kWall = 20.0f;
    const Box b = box_on(map, building, kWall);
    const Color wall = shade({226, 212, 170, 255}, soot);
    for (const auto& [f, k] : {std::pair{&b.lit_face, 1.0f}, std::pair{&b.shade_face, 0.74f}}) {
        wall_texture(*f, 0.0f, 1.0f, 2.5f, kWall, Wall::Pastel, shade(wall, k), h);
        face_fill(*f, 0.0f, 1.0f, 0.0f, 2.5f, shade({120, 104, 90, 255}, k * soot));
        face_fill(*f, 0.0f, 1.0f, kWall - 2.5f, kWall, shade({240, 236, 226, 255}, k * soot));
        wear_walls(*f, k, 2.5f, kWall, stage_of(damage), {156, 90, 64, 255}, true, h + static_cast<uint32_t>(k * 10.0f));
        const int n = std::max(2, static_cast<int>(f->pixels() / 11.0f));
        for (int i = 0; i < n; ++i) {
            const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
            const Vector2 top = f->at(u, 14.5f);
            window_on(*f, u, 4.5f, 4.0f, 9.0f, {charred({240, 236, 226, 255}, damage)}, damage > 0.25f && rand01(h, i) < damage, k);
            DrawEllipse(static_cast<int>(top.x), static_cast<int>(top.y + 1.0f), 2.8f, 2.2f, lit(shade({240, 236, 226, 255}, k)));  // its arch
        }
    }
    door_on(b.shade_face, 0.5f, 5.0f, 11.0f, {104, 76, 52, 255}, 0.74f * soot);
    if (stage_of(damage) >= 3) {
        fallen_roof(map, building, kWall, 10.0f, building.width >= building.height, false, wall, {150, 70, 54, 255}, false, h);
    } else {
        hip_roof(map, building, kWall, 10.0f, Roof::Tin, shade({150, 70, 54, 255}, soot), h);
        roof_holes(map, building, kWall + 5.0f, damage, {150, 70, 54, 255}, h);
    }
    // The canopy over the platform on its posts.
    constexpr float kUp = 15.0f;
    for (float t = 0.1f; t < 1.0f; t += 0.2f) {
        const Vector2 p = on_terrain(map, {platform.x + platform.width * t, platform.y + platform.height * 0.5f}, 2.0f);
        fine_line(p, {p.x, p.y - kUp}, {90, 90, 88, 255});
    }
    const Box c = box_on(map, {platform.x + 0.1f, platform.y + 0.05f, platform.width - 0.2f, platform.height - 0.1f}, kUp + 2.0f);
    fill_quad(c.top[0], c.top[1], c.top[2], c.top[3], shade({120, 124, 126, 255}, soot));
    DrawLineV(c.top[2], c.top[3], lit(shade({90, 94, 96, 255}, soot)));
    flag_on(b.top[2], 16.0f, team);
}

// An ammunition depot: bunkers under earth, their concrete fronts and
// steel doors; crates stacked under a net; sandbags.
void draw_ammo_depot(const engine::TileMap& map, const engine::Structure& s, float damage, Color team) {
    const Rectangle r = footprint(s, 0.1f);
    const uint32_t h = s.id * 2654435761u;
    const float soot = 1.0f - 0.4f * damage;
    // The mound: earth grown over, the bunker's front in it.
    const Vector2 c = on_terrain(map, {r.x + r.width * 0.45f, r.y + r.height * 0.4f});
    const Color earth = shade({108, 110, 70, 255}, soot);
    DrawEllipse(static_cast<int>(c.x), static_cast<int>(c.y - 3.0f), 28.0f, 13.0f, lit(shade(earth, 0.8f)));
    DrawEllipse(static_cast<int>(c.x - 3.0f), static_cast<int>(c.y - 7.0f), 24.0f, 10.0f, lit(earth));
    DrawEllipse(static_cast<int>(c.x - 6.0f), static_cast<int>(c.y - 10.0f), 14.0f, 6.0f, lit(shade(earth, 1.15f)));
    const Rectangle front{r.x + r.width * 0.3f, r.y + r.height * 0.62f, r.width * 0.3f, 0.12f};
    const Box f = box_on(map, front, 9.0f);
    wall_texture(f.shade_face, 0.0f, 1.0f, 0.0f, 9.0f, Wall::Concrete, shade({170, 166, 156, 255}, 0.8f * soot), h);
    face_fill(f.shade_face, 0.25f, 0.75f, 0.0f, 7.0f, damage > 0.5f ? Color{20, 18, 16, 255} : shade({76, 92, 70, 255}, soot));
    face_line(f.shade_face, 0.5f, 0.0f, 0.5f, 7.0f, {40, 44, 36, 255});
    fill_quad(f.top[0], f.top[1], f.top[2], f.top[3], shade({150, 146, 138, 255}, soot));
    // Crates under a net, sandbags round the front.
    const int stage = stage_of(damage);
    crates(map, {r.x + r.width * 0.65f, r.y + r.height * 0.55f}, stage >= 3 ? 5 : 9, stage >= 2 ? Color{56, 52, 44, 255} : Color{84, 96, 62, 255}, h);
    if (stage < 2) net_over(map, {r.x + r.width * 0.6f, r.y + r.height * 0.5f, 0.7f, 0.42f}, 10.0f, h);
    if (stage >= 2) fire_spot(on_terrain(map, {r.x + r.width * 0.75f, r.y + r.height * 0.6f}, 5.0f), stage == 2);
    sandbags(map, {r.x + 0.05f, r.y + r.height}, {r.x + r.width * 0.28f, r.y + r.height}, 2, h);
    flag_on(on_terrain(map, {r.x + 0.1f, r.y + r.height * 0.8f}), 18.0f, team);
}

// An upright steel tank: round (shaded across in strips, lit on the
// left), its roof a shallow cone, a ladder up its side, a band round it.
void upright_tank(Vector2 foot, float radius, float tall, Color c, Color band) {
    constexpr int kStrips = 8;
    for (int st = 0; st < kStrips; ++st) {
        const float x0 = foot.x - radius + 2.0f * radius * static_cast<float>(st) / kStrips;
        const float x1 = foot.x - radius + 2.0f * radius * static_cast<float>(st + 1) / kStrips;
        const float t = (static_cast<float>(st) + 0.5f) / kStrips;
        DrawRectangleRec({std::round(x0), foot.y - tall, std::round(x1) - std::round(x0), tall}, lit(shade(c, 1.2f - 0.5f * t)));
    }
    DrawEllipse(static_cast<int>(foot.x), static_cast<int>(foot.y), radius, radius * 0.5f, lit(shade(c, 0.7f)));
    DrawRectangleRec({std::round(foot.x - radius), foot.y - tall * 0.55f, 2.0f * radius, 2.0f}, lit(band));
    DrawEllipse(static_cast<int>(foot.x), static_cast<int>(foot.y - tall), radius, radius * 0.5f, lit(shade(c, 1.15f)));
    DrawTriangle({foot.x - radius, foot.y - tall}, {foot.x + radius, foot.y - tall}, {foot.x, foot.y - tall - radius * 0.35f}, lit(shade(c, 0.95f)));
    for (float v = 2.0f; v < tall; v += 2.0f) DrawPixelV({foot.x + radius * 0.5f, foot.y - v}, lit(shade(c, 0.55f)));  // its ladder
}

// A fuel depot: upright tanks inside an earth bank, a pipe between them,
// drums; red where it says it burns.
void draw_fuel_depot(const engine::TileMap& map, const engine::Structure& s, float damage, Color team) {
    const Rectangle r = footprint(s, 0.1f);
    const float soot = 1.0f - 0.4f * damage;
    const Box bank = box_on(map, r, 3.0f);
    face_fill(bank.lit_face, 0.0f, 1.0f, 0.0f, 3.0f, shade({112, 104, 76, 255}, soot));
    face_fill(bank.shade_face, 0.0f, 1.0f, 0.0f, 3.0f, shade({112, 104, 76, 255}, 0.74f * soot));
    const Color steel = shade(damage > 0.5f ? Color{70, 60, 54, 255} : Color{176, 184, 172, 255}, soot);
    const Vector2 a = on_terrain(map, {r.x + r.width * 0.3f, r.y + r.height * 0.35f});
    const Vector2 b = on_terrain(map, {r.x + r.width * 0.68f, r.y + r.height * 0.62f});
    const int stage = stage_of(damage);
    DrawLineEx({a.x, a.y - 3.0f}, {b.x, b.y - 3.0f}, 2.0f, lit(shade(steel, 0.7f)));  // the pipe
    upright_tank(a, 11.0f, 20.0f, steel, {200, 60, 50, 255});
    if (stage >= 3) {  // crumpled in the fire, its roof fallen in
        upright_tank(b, 11.0f, 9.0f, steel, {90, 40, 34, 255});
        for (int k = 0; k < 5; ++k) DrawLineV({b.x - 11.0f + 5.0f * k, b.y - 9.0f}, {b.x - 9.0f + 5.0f * k, b.y - 13.0f - 3.0f * (k % 2)}, lit(shade(steel, 0.8f)));
        fire_spot({b.x, b.y - 10.0f}, false);
        fire_spot({a.x, a.y - 21.0f}, false);
    } else {
        upright_tank(b, 11.0f, 20.0f, steel, {200, 60, 50, 255});
        if (stage == 2) fire_spot({b.x, b.y - 21.0f}, true);
    }
    drums(map, {r.x + r.width - 0.4f, r.y + 0.15f}, 4, {60, 90, 70, 255});
    flag_on(on_terrain(map, {r.x + 0.1f, r.y + r.height - 0.1f}), 16.0f, team);
}

// An army tent: canvas over its frame in panels, its walls low, its roof
// steep; guy ropes out to their pegs, a stovepipe; a red cross on the
// field hospital's.
void tent(const engine::TileMap& map, Rectangle r, Color canvas, uint32_t seed, bool cross, float damage) {
    const float soot = 1.0f - 0.4f * damage;
    constexpr float kWall = 8.0f;
    constexpr float kRise = 14.0f;
    const Box b = box_on(map, r, kWall);
    if (stage_of(damage) >= 3) {  // burnt away: ash, the frame standing black, scraps of the canvas
        const Box ground = box_on(map, r, 0.0f);
        fill_quad(ground.base[0], ground.base[1], ground.base[2], ground.base[3], {44, 40, 36, 255});
        face_fill(b.lit_face, 0.0f, 1.0f, 0.0f, 1.5f, {60, 56, 50, 255});
        face_fill(b.shade_face, 0.0f, 1.0f, 0.0f, 1.5f, {50, 46, 42, 255});
        const bool along = r.width >= r.height;
        const Vector2 r0 = along ? on_terrain(map, {r.x, r.y + r.height * 0.5f}, kWall + kRise) : on_terrain(map, {r.x + r.width * 0.5f, r.y}, kWall + kRise);
        const Vector2 r1 = along ? on_terrain(map, {r.x + r.width, r.y + r.height * 0.5f}, kWall + kRise) : on_terrain(map, {r.x + r.width * 0.5f, r.y + r.height}, kWall + kRise);
        for (int k = 0; k <= 3; ++k) {  // its poles, the ridge between them
            const float t = static_cast<float>(k) / 3.0f;
            const Vector2 top = lerp(r0, r1, t);
            const Vector2 foot = along ? on_terrain(map, {r.x + r.width * t, r.y + r.height * 0.5f}) : on_terrain(map, {r.x + r.width * 0.5f, r.y + r.height * t});
            fine_line(foot, top, {40, 34, 30, 255});
        }
        fine_line(r0, lerp(r0, r1, 0.7f), {40, 34, 30, 255});
        fire_spot(on_terrain(map, {r.x + r.width * 0.5f, r.y + r.height * 0.5f}, 2.0f), false);
        return;
    }
    face_fill(b.lit_face, 0.0f, 1.0f, 0.0f, kWall, shade(canvas, soot));
    face_fill(b.shade_face, 0.0f, 1.0f, 0.0f, kWall, shade(canvas, 0.74f * soot));
    for (float u = 0.2f; u < 1.0f; u += 0.2f) {  // its panels
        face_line(b.lit_face, u, 0.0f, u, kWall, shade(canvas, 0.82f * soot));
        face_line(b.shade_face, u, 0.0f, u, kWall, shade(canvas, 0.6f * soot));
    }
    const bool along_x = r.width >= r.height;
    face_fill(along_x ? b.lit_face : b.shade_face, 0.4f, 0.6f, 0.0f, kWall - 0.5f, shade(canvas, 0.4f));  // the flap open
    gable_roof(map, r, kWall, kRise, along_x, Roof::Tin, shade(canvas, 0.98f * soot), shade(canvas, soot), seed, 0.08f);
    roof_holes(map, r, kWall + kRise * 0.5f, damage, canvas, seed);  // torn, burning
    for (const Vector2& g : {Vector2{r.x - 0.2f, r.y + r.height * 0.5f}, Vector2{r.x + r.width * 0.5f, r.y + r.height + 0.2f},
                             Vector2{r.x + r.width + 0.2f, r.y + r.height * 0.5f}}) {  // guy ropes
        const Vector2 peg = on_terrain(map, g);
        const Vector2 top = on_terrain(map, {std::clamp(g.x, r.x, r.x + r.width), std::clamp(g.y, r.y, r.y + r.height)}, kWall);
        fine_line(top, peg, {150, 140, 110, 255});
    }
    if (cross) {
        const Vector2 c = on_terrain(map, {r.x + r.width * 0.5f, r.y + r.height * 0.5f}, kWall + kRise * 0.6f);
        DrawRectangleRec({std::round(c.x - 6.0f), std::round(c.y - 2.0f), 12.0f, 4.0f}, lit({220, 40, 40, 255}));
        DrawRectangleRec({std::round(c.x - 2.0f), std::round(c.y - 5.0f), 4.0f, 10.0f}, lit({220, 40, 40, 255}));
    }
    const Vector2 pipe = on_terrain(map, {r.x + r.width * 0.75f, r.y + r.height * 0.45f}, kWall + kRise * 0.7f);
    fine_line(pipe, {pipe.x, pipe.y - 9.0f}, {60, 60, 58, 255});
}

// A shed of corrugated iron under a low gable, its door; for the
// artillery's, the engineers', the air defence's, the recon's stores.
Box iron_shed(const engine::TileMap& map, Rectangle r, float wall, Color iron, float damage, uint32_t seed) {
    const float soot = 1.0f - 0.4f * damage;
    const Box b = box_on(map, r, wall);
    wall_texture(b.lit_face, 0.0f, 1.0f, 0.0f, wall, Wall::Corrugated, shade(iron, soot), seed);
    wall_texture(b.shade_face, 0.0f, 1.0f, 0.0f, wall, Wall::Corrugated, shade(iron, 0.74f * soot), seed + 1);
    face_fill(b.shade_face, 0.3f, 0.7f, 0.0f, wall * 0.75f, damage > 0.5f ? Color{20, 18, 16, 255} : shade(iron, 0.55f * soot));
    const int stage = stage_of(damage);
    if (stage >= 1) pockmarks(b.lit_face, 0.0f, 1.0f, 0.0f, wall, stage * 10, seed);
    if (stage >= 2) sheets_off(b.lit_face, 2.0f, wall, stage == 2 ? 0.2f : 0.45f, seed + 1u);
    if (stage >= 3) {
        fallen_roof(map, r, wall, 9.0f, r.width >= r.height, true, iron, {120, 118, 112, 255}, true, seed);
        return b;
    }
    gable_roof(map, r, wall, 9.0f, r.width >= r.height, Roof::Rusty, shade({120, 118, 112, 255}, soot), shade(iron, soot), seed);
    roof_holes(map, r, wall + 4.0f, damage, {120, 118, 112, 255}, seed);
    return b;
}

// A player's building, by what it is; while it's being put up, its site.
void draw_building(const engine::TileMap& map, const engine::Structure& s) {
    if (s.type == engine::StructureType::Pillbox) return draw_pillbox(map, s);
    const engine::StructureDef& def = engine::structure_type(s.type);
    if (!s.built) {
        return draw_construction(map, s, std::clamp(static_cast<float>(s.build_progress) / static_cast<float>(std::max<engine::Tick>(1, def.build_time)), 0.0f, 1.0f));
    }
    const float damage = 1.0f - static_cast<float>(s.hp) / static_cast<float>(def.max_hp);
    const Color team = theme::player_color(s.owner);
    const uint32_t h = s.id * 2654435761u;
    const Rectangle r = footprint(s, 0.12f);
    switch (s.type) {
        case engine::StructureType::Headquarters: return draw_headquarters(map, s, damage, team);
        case engine::StructureType::InfantryBarracks: return draw_barracks_block(map, s, damage, team);
        case engine::StructureType::ArmorBarracks: return draw_hangar(map, s, damage, team, 24.0f, true);
        case engine::StructureType::Warehouse: {
            draw_hangar(map, s, damage, team, 20.0f, false);
            crates(map, {r.x + r.width + 0.02f, r.y + r.height * 0.1f}, 6, {150, 116, 74, 255}, h);
            return;
        }
        case engine::StructureType::Station: return draw_station(map, s, damage, team);
        case engine::StructureType::AmmoDepot: return draw_ammo_depot(map, s, damage, team);
        case engine::StructureType::FuelDepot: return draw_fuel_depot(map, s, damage, team);
        case engine::StructureType::Quarters:
            tent(map, {r.x, r.y, r.width, r.height * 0.45f}, {104, 110, 76, 255}, h, false, damage);
            tent(map, {r.x, r.y + r.height * 0.55f, r.width, r.height * 0.45f}, {98, 104, 72, 255}, h + 1, false, damage);
            return flag_on(on_terrain(map, {r.x + r.width + 0.1f, r.y + r.height * 0.5f}), 18.0f, team);
        case engine::StructureType::Hospital:
            tent(map, {r.x, r.y + r.height * 0.2f, r.width, r.height * 0.65f}, {196, 200, 186, 255}, h, true, damage);
            return flag_on(on_terrain(map, {r.x + r.width + 0.1f, r.y + r.height * 0.9f}), 18.0f, team);
        case engine::StructureType::Workshop: {
            draw_hangar(map, s, damage, team, 22.0f, true);
            tyres(map, {r.x + r.width + 0.15f, r.y + r.height * 0.3f}, 4);
            drums(map, {r.x + r.width + 0.05f, r.y + r.height * 0.6f}, 4, {50, 60, 70, 255});
            // A gantry for lifting out engines, beside it.
            const Vector2 a = on_terrain(map, {r.x + r.width * 0.2f, r.y + r.height + 0.25f});
            const Vector2 b = on_terrain(map, {r.x + r.width * 0.6f, r.y + r.height + 0.25f});
            for (const Vector2& p : {a, b}) DrawRectangleRec({std::round(p.x - 1.0f), std::round(p.y - 28.0f), 2.0f, 28.0f}, lit({196, 160, 40, 255}));
            DrawLineEx({a.x, a.y - 28.0f}, {b.x, b.y - 28.0f}, 2.0f, lit({196, 160, 40, 255}));
            fine_line(lerp({a.x, a.y - 28.0f}, {b.x, b.y - 28.0f}, 0.5f), lerp(a, b, 0.5f), {40, 40, 38, 255});
            return;
        }
        case engine::StructureType::ReconBarracks: {
            iron_shed(map, {r.x + 0.1f, r.y + 0.2f, r.width * 0.8f, r.height * 0.6f}, 18.0f, {110, 118, 92, 255}, damage, h);
            net_over(map, {r.x, r.y + 0.1f, r.width, r.height * 0.8f}, 28.0f, h);
            return flag_on(on_terrain(map, {r.x + r.width, r.y + r.height * 0.9f}), 18.0f, team);
        }
        case engine::StructureType::ArtilleryBarracks: {
            iron_shed(map, {r.x, r.y + 0.1f, r.width, r.height * 0.55f}, 20.0f, {118, 126, 104, 255}, damage, h);
            crates(map, {r.x + r.width * 0.1f, r.y + r.height * 0.72f}, 12, {84, 96, 62, 255}, h);  // the shells'
            net_over(map, {r.x + r.width * 0.05f, r.y + r.height * 0.68f, r.width * 0.55f, r.height * 0.3f}, 9.0f, h + 1);
            return flag_on(on_terrain(map, {r.x + r.width, r.y + r.height}), 20.0f, team);
        }
        case engine::StructureType::EngineerBarracks: {
            iron_shed(map, {r.x, r.y + 0.1f, r.width * 0.7f, r.height * 0.6f}, 18.0f, {130, 124, 100, 255}, damage, h);
            for (int k = 0; k < 3; ++k) {  // a pile of logs
                const Vector2 p = on_terrain(map, {r.x + r.width * 0.15f + 0.1f * k, r.y + r.height * 0.85f}, 1.5f + 2.5f * static_cast<float>(k % 2));
                DrawLineEx({p.x - 8.0f, p.y}, {p.x + 8.0f, p.y - 4.0f}, 3.0f, lit({130, 96, 62, 255}));
            }
            for (int k = 0; k < 3; ++k) {  // coils of wire
                const Vector2 p = on_terrain(map, {r.x + r.width * 0.8f, r.y + r.height * (0.3f + 0.2f * k)});
                DrawEllipseLines(static_cast<int>(p.x), static_cast<int>(p.y - 3.0f), 3.5f, 3.0f, lit({110, 110, 106, 255}));
            }
            return flag_on(on_terrain(map, {r.x + r.width, r.y + r.height}), 18.0f, team);
        }
        case engine::StructureType::SignalsBarracks: {
            const Box b = iron_shed(map, {r.x, r.y + 0.1f, r.width * 0.7f, r.height * 0.55f}, 18.0f, {120, 128, 110, 255}, damage, h);
            lattice_mast(on_terrain(map, {r.x + r.width * 0.85f, r.y + r.height * 0.85f}), 60.0f, 3.0f);  // in front of it
            DrawEllipse(static_cast<int>(b.top[1].x - 6.0f), static_cast<int>(b.top[1].y - 8.0f), 3.5f, 4.0f, lit({220, 220, 214, 255}));  // a dish
            fine_line({b.top[1].x - 6.0f, b.top[1].y - 8.0f}, {b.top[1].x - 6.0f, b.top[1].y - 2.0f}, {80, 80, 78, 255});
            return flag_on(on_terrain(map, {r.x + r.width, r.y + r.height}), 18.0f, team);
        }
        case engine::StructureType::AirDefenseBarracks: {
            const Box b = iron_shed(map, {r.x, r.y + 0.2f, r.width * 0.75f, r.height * 0.6f}, 18.0f, {116, 124, 108, 255}, damage, h);
            const Vector2 p = on_terrain(map, {r.x + r.width * 0.85f, r.y + r.height * 0.3f});
            fine_line(p, {p.x, p.y - 30.0f}, {80, 80, 78, 255});
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y - 34.0f), 7.0f, 4.5f, lit({200, 204, 196, 255}));  // its radar
            DrawEllipse(static_cast<int>(p.x + 1.0f), static_cast<int>(p.y - 33.0f), 5.0f, 3.0f, lit({150, 154, 146, 255}));
            crates(map, {r.x + r.width * 0.1f, r.y + r.height * 0.82f}, 4, {84, 96, 62, 255}, h);
            (void)b;
            return flag_on(on_terrain(map, {r.x + r.width, r.y + r.height}), 18.0f, team);
        }
        default: {  // anything else: a brick block with a flat roof
            const Box b = box_on(map, r, 20.0f);
            wall_texture(b.lit_face, 0.0f, 1.0f, 0.0f, 20.0f, Wall::Silicate, {196, 194, 184, 255}, h);
            wall_texture(b.shade_face, 0.0f, 1.0f, 0.0f, 20.0f, Wall::Silicate, {146, 144, 136, 255}, h + 1);
            fill_quad(b.top[0], b.top[1], b.top[2], b.top[3], {110, 108, 104, 255});
            return flag_on(b.top[0], 18.0f, team);
        }
    }
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

// A piece of wall left standing: `floors` high, its top broken off ragged,
// its windows empty, burnt black round them.
void wall_fragment(const Face& f, float u0, float u1, int floors, float floor_h, Color c, bool brick, uint32_t seed) {
    const float px = f.px();
    const float top = floor_h * static_cast<float>(floors);
    if (u1 <= u0) return;
    std::vector<float> tops;
    float v = top;
    int i = 0;
    for (float u = u0; u < u1; u += 3.0f * px, ++i) {  // its ragged top, a few pixels at a time
        v = std::clamp(v + (rand01(seed, i) - 0.5f) * 10.0f, std::max(4.0f, top * 0.45f), top + 3.0f);
        tops.push_back(v);
        const float ue = std::min(u1, u + 3.0f * px);
        wall_texture(f, u, ue, 0.0f, v, brick ? Wall::Silicate : Wall::Concrete, c, seed + static_cast<uint32_t>(i));
        face_fill(f, u, ue, 0.0f, v, ColorAlpha({28, 24, 22, 255}, 0.3f));  // burnt
    }
    auto top_at = [&](float u) {
        const auto k = static_cast<size_t>(std::clamp((u - u0) / (3.0f * px), 0.0f, static_cast<float>(tops.size() - 1)));
        return tops[k];
    };
    for (int fl = 0; fl < floors; ++fl) {
        const float v0 = floor_h * static_cast<float>(fl);
        face_line(f, u0, v0 + 1.0f, u1, v0 + 1.0f, shade(c, 0.8f));  // the slab's line
        for (float u = u0 + 6.0f * px; u < u1 - 4.0f * px; u += 11.0f * px) {
            if (std::min(top_at(u - 2.0f * px), top_at(u + 2.0f * px)) < v0 + 9.0f) continue;
            face_fill(f, u - 2.0f * px, u + 2.0f * px, v0 + 3.0f, v0 + 8.0f, {18, 16, 14, 255});
            soot_on(f, u, v0 + 8.0f, 5.0f, std::min(6.0f, top_at(u) - v0 - 8.0f));
        }
    }
}

// A wall left standing seen from inside: the rooms' wallpaper and paint
// floor by floor, the floors' slabs broken off along it, the partitions'
// ends, the windows' openings with the sky through them; its top broken
// off ragged; burnt.
void inner_wall(const Face& f, float u0, float u1, int floors, float floor_h, float k, uint32_t seed) {
    if (u1 <= u0) return;
    static constexpr Color kRooms[5] = {{186, 204, 170, 255}, {214, 196, 150, 255}, {170, 190, 210, 255}, {214, 176, 170, 255}, {200, 196, 186, 255}};
    const float px = f.px();
    const float top = floor_h * static_cast<float>(floors);
    std::vector<float> tops;
    float v = top;
    int i = 0;
    for (float u = u0; u < u1; u += 3.0f * px, ++i) {  // its ragged top, a few pixels at a time
        v = std::clamp(v + (rand01(seed, i) - 0.5f) * 10.0f, std::max(4.0f, top * 0.45f), top + 3.0f);
        tops.push_back(v);
        const float ue = std::min(u1, u + 3.0f * px);
        const auto room = static_cast<uint32_t>((u - u0) / (12.0f * px));
        for (int fl = 0; fl < floors; ++fl) {
            const float a = floor_h * static_cast<float>(fl);
            if (a >= v) break;
            face_fill(f, u, ue, a, std::min(v, a + floor_h), shade(kRooms[(room + static_cast<uint32_t>(fl) * 3u + seed) % 5u], 0.7f * k));
            if (fl > 0 && a + 1.5f <= v) face_fill(f, u, ue, a - 0.5f, a + 1.5f, shade({150, 146, 138, 255}, k));  // the slab's broken end
        }
        face_fill(f, u, ue, 0.0f, v, ColorAlpha({28, 24, 22, 255}, 0.35f));  // burnt
    }
    auto top_at = [&](float u) {
        const auto n = static_cast<size_t>(std::clamp((u - u0) / (3.0f * px), 0.0f, static_cast<float>(tops.size() - 1)));
        return tops[n];
    };
    for (float u = u0 + 12.0f * px; u < u1 - px; u += 12.0f * px) face_line(f, u, 0.0f, u, top_at(u) - 1.0f, shade({110, 106, 100, 255}, k));  // partitions
    for (int fl = 0; fl < floors; ++fl) {  // the windows: the sky through them
        const float a = floor_h * static_cast<float>(fl);
        for (float u = u0 + 6.0f * px; u < u1 - 4.0f * px; u += 12.0f * px) {
            if (std::min(top_at(u - 2.0f * px), top_at(u + 2.0f * px)) < a + 9.0f) continue;
            face_fill(f, u - 2.0f * px, u + 2.0f * px, a + 3.0f, a + 8.0f, {150, 172, 190, 255});
        }
    }
}

// Charred beams, lying every which way.
void beams(Vector2 at, int n, uint32_t seed) {
    for (int i = 0; i < n; ++i) {
        const Vector2 p{at.x + (rand01(seed, i) - 0.5f) * 20.0f, at.y - 3.0f * rand01(seed, i + 10)};
        const float a = rand01(seed, i + 20) * 3.14159f;
        const float len = 8.0f + 8.0f * rand01(seed, i + 30);
        DrawLineEx({p.x - std::cos(a) * len * 0.5f, p.y - std::sin(a) * len * 0.25f}, {p.x + std::cos(a) * len * 0.5f, p.y + std::sin(a) * len * 0.25f},
                   2.0f, lit({40, 30, 24, 255}));
    }
}

// Sheets (slate, corrugated iron) fallen, tilted, lying on the rubble.
void sheets(Vector2 at, int n, Color c, uint32_t seed) {
    for (int i = 0; i < n; ++i) {
        const Vector2 p{at.x + (rand01(seed, i) - 0.5f) * 24.0f, at.y - 2.0f - 5.0f * rand01(seed, i + 10)};
        const float a = rand01(seed, i + 20) * 3.14159f;
        const Vector2 d{std::cos(a) * 5.0f, std::sin(a) * 2.5f};
        const Vector2 n2{-std::sin(a) * 3.0f, std::cos(a) * 1.5f - 1.5f};
        const Color k = shade(c, 0.8f + 0.35f * rand01(seed, i + 30));
        fill_quad({p.x - d.x - n2.x, p.y - d.y - n2.y}, {p.x + d.x - n2.x, p.y + d.y - n2.y}, {p.x + d.x + n2.x, p.y + d.y + n2.y},
                  {p.x - d.x + n2.x, p.y - d.y + n2.y}, k);
        for (float t = -0.6f; t <= 0.61f; t += 0.3f) {  // its ribs
            DrawLineV({p.x + d.x * t - n2.x, p.y + d.y * t - n2.y}, {p.x + d.x * t + n2.x, p.y + d.y * t + n2.y}, lit(shade(k, 0.75f)));
        }
    }
}

// Rubble on a tile, as what stood there came down: a block of flats a heap
// of slabs and brick with walls left standing, gutted, burnt; a house a heap
// of brick and plaster, a chimney or a corner left, the beams charred, the
// slate on top; a works' iron (its posts standing, a truss bent, sheets
// fallen); a barn's low broken walls; a silo's stumps and the grain spilt;
// a gas station's canopy down on its bent posts; a mast lying; a base's
// burnt timber, sandbags, crates. The ground dusty, bits all over it.
void draw_ruin_tile(const engine::TileMap& map, int tx, int ty) {
    const engine::RuinKind kind = map.ruin(tx, ty);
    const uint32_t h = tile_hash(tx * 13 + 5, ty * 7 + 11);
    const auto x = static_cast<float>(tx);
    const auto y = static_cast<float>(ty);
    const Vector2 mid = on_terrain(map, {x + 0.5f, y + 0.5f});
    const Color concrete{168, 164, 154, 255};
    const Color red_brick{150, 86, 62, 255};
    for (int i = 0; i < 26; ++i) {  // bits all over the ground
        const Vector2 p = on_terrain(map, {x + rand01(h, 1000 + i), y + rand01(h, 1100 + i)});
        DrawRectangleRec({std::round(p.x), std::round(p.y), 1.0f + static_cast<float>(i % 2), 1.0f},
                         lit(i % 3 == 0 ? Color{70, 62, 54, 255} : i % 3 == 1 ? concrete : Color{120, 104, 86, 255}));
    }
    // What's next to it (rubble too): the pile runs on into it; an edge with none is the building's outer wall.
    auto same = [&](int dx, int dy) { return map.contains_tile(tx + dx, ty + dy) && map.terrain(tx + dx, ty + dy) == engine::Terrain::Ruins; };
    const std::array<bool, 4> open{!same(-1, 0), !same(1, 0), !same(0, -1), !same(0, 1)};
    // The pile over the tile, `top` pixels at its highest: down to nothing at the edges `falls` says.
    auto height_of = [x, y](std::array<bool, 4> falls, float top) {
        return [falls, x, y, top](Vector2 g) {
            const float s = g.x - x;
            const float t = g.y - y;
            auto ramp = [](float d) {
                const float k = std::clamp(d / 0.5f, 0.0f, 1.0f);
                return k * k * (3.0f - 2.0f * k);
            };
            float f = 1.0f;
            if (falls[0]) f *= ramp(s);
            if (falls[1]) f *= ramp(1.0f - s);
            if (falls[2]) f *= ramp(t);
            if (falls[3]) f *= ramp(1.0f - t);
            return top * f * (0.45f + 0.55f * ruin_noise(g));
        };
    };
    // The pile, with something standing in it: the rubble behind, it, the rubble before it.
    auto pile_around = [&](const std::function<float(Vector2)>& height, const RubbleMix& mix, int slabs, const std::function<bool(Vector2)>& behind,
                           const std::function<void()>& standing) {
        rubble_pile(map, {x, y}, {1.0f, 1.0f}, height, mix, h, slabs, behind);
        standing();
        rubble_pile(map, {x, y}, {1.0f, 1.0f}, height, mix, h, slabs, [&](Vector2 g) { return !behind(g); });
    };
    switch (kind) {
        case engine::RuinKind::Apartment: {
            const bool brick = (tile_hash(tx / 4, ty / 2) & 1u) != 0;
            const Color wall = brick ? Color{176, 172, 164, 255} : Color{172, 166, 152, 255};
            const RubbleMix mix{concrete, brick ? Color{196, 194, 186, 255} : red_brick, 0.18f, true};
            // A wall left standing where the block's own walls were, on an
            // outer edge of the rubble: on the near ones (the front, the lit
            // end) its outside, its windows empty, the heap low behind it; on
            // the far ones its inside, the rooms' colours floor by floor, the
            // heap against it.
            int edge = -1;  // -x, +x, -y, +y
            if (rand01(h, 1) < 0.6f) {
                const int first = static_cast<int>(rand01(h, 8) * 4.0f);
                for (int k = 0; k < 4 && edge < 0; ++k) {
                    if (open[static_cast<size_t>((first + k) % 4)]) edge = (first + k) % 4;
                }
            }
            std::array<bool, 4> falls = open;
            if (edge == 0 || edge == 2) falls[static_cast<size_t>(edge)] = false;  // heaped against a far one; low behind a near one
            const auto height = height_of(falls, 28.0f + 16.0f * rand01(h, 6));
            if (edge >= 0) {
                const bool far = edge == 0 || edge == 2;
                const bool along_x = edge >= 2;  // on a -y or +y edge: running along x
                const float at = far ? 0.06f : 0.94f;
                const Face f = along_x ? Face{on_terrain(map, {x + 1.0f, y + at}), on_terrain(map, {x, y + at})}
                                       : Face{on_terrain(map, {x + at, y}), on_terrain(map, {x + at, y + 1.0f})};
                const float u0 = rand01(h, 4) < 0.5f ? 0.0f : 0.3f * rand01(h, 9);  // on to the next tile's, or broken off
                const float u1 = rand01(h, 5) < 0.5f ? 1.0f : 1.0f - 0.3f * rand01(h, 10);
                const int floors = far ? 2 + static_cast<int>(rand01(h, 2) * 3.5f) : 2 + static_cast<int>(rand01(h, 2) * 2.5f);
                const float k = along_x ? 0.74f : 1.0f;
                pile_around(height, mix, 10, [=](Vector2 g) { return along_x ? g.y < y + at : g.x < x + at; }, [&] {
                    if (far) {  // (its floors as high as the block's: see building_scale)
                        inner_wall(f, u0, u1, floors, 20.0f, k, h);
                    } else {
                        wall_fragment(f, u0, u1, floors, 20.0f, shade(wall, k), brick, h);
                    }
                });
            } else {
                rubble_pile(map, {x, y}, {1.0f, 1.0f}, height, mix, h, 10);
                if (rand01(h, 3) < 0.45f) {  // a floor slab leaning on the heap
                    const Vector2 a{mid.x - 13.0f, mid.y - 10.0f};
                    fill_quad(a, {a.x + 24.0f, a.y - 12.0f}, {a.x + 26.0f, a.y - 8.0f}, {a.x + 2.0f, a.y + 4.0f}, shade(concrete, 1.05f));
                    DrawLineV({a.x + 2.0f, a.y + 4.0f}, {a.x + 26.0f, a.y - 8.0f}, lit(shade(concrete, 0.6f)));
                    for (int k = 0; k < 3; ++k) DrawLineV({a.x + 4.0f + 7.0f * k, a.y + 2.0f - 3.5f * k}, {a.x + 3.0f + 7.0f * k, a.y + 7.0f - 3.5f * k}, lit({74, 56, 44, 255}));
                }
            }
            break;
        }
        case engine::RuinKind::House: {
            const Color white{214, 212, 202, 255};
            const RubbleMix mix{{190, 182, 168, 255}, red_brick, 0.45f, false};
            const auto height = height_of(open, 8.0f + 5.0f * rand01(h, 2));
            const float top = 8.0f;
            auto corner = [&] {
                if (rand01(h, 1) >= 0.4f) return;  // a corner of its walls left
                const Face a{on_terrain(map, {x + 0.8f, y + 0.25f}), on_terrain(map, {x + 0.8f, y + 0.8f})};
                const Face b{on_terrain(map, {x + 0.8f, y + 0.8f}), on_terrain(map, {x + 0.3f, y + 0.8f})};
                const bool white_walls = rand01(h, 7) < 0.5f;
                for (const auto& [f, k] : {std::pair{&a, 1.0f}, std::pair{&b, 0.74f}}) {
                    int i = 0;
                    for (float u = 0.0f; u < 1.0f; u += 0.2f, ++i) {
                        const float v = 5.0f + 9.0f * rand01(h, 20 + i + static_cast<int>(k * 10.0f)) * (1.0f - u * 0.6f);
                        wall_texture(*f, u, u + 0.2f, 0.0f, v, white_walls ? Wall::Whitewash : Wall::RedBrick, shade(white_walls ? white : red_brick, k * 0.8f),
                                     h + static_cast<uint32_t>(i));
                    }
                    face_fill(*f, 0.3f, 0.45f, 4.0f, 8.0f, {18, 16, 14, 255});  // a window
                    burnt(*f, 0.0f, 1.0f, 0.0f, 12.0f, 0.6f, h);
                }
            };
            pile_around(height, mix, 4, [=](Vector2 g) { return g.x < x + 0.8f && g.y < y + 0.8f; }, corner);
            sheets({mid.x, mid.y - top}, 3, {120, 122, 122, 255}, h);
            beams({mid.x, mid.y - top * 0.7f}, 3, h);
            if (rand01(h, 3) < 0.45f) {  // the chimney left standing
                const Vector2 c{mid.x + 6.0f, mid.y - 2.0f};
                const float tall = 14.0f + 8.0f * rand01(h, 4);
                fill_quad({c.x - 2.0f, c.y}, {c.x + 2.0f, c.y}, {c.x + 2.0f, c.y - tall}, {c.x - 2.0f, c.y - tall}, shade(red_brick, 0.8f));
                fill_quad({c.x - 2.0f, c.y}, {c.x, c.y}, {c.x, c.y - tall}, {c.x - 2.0f, c.y - tall}, red_brick);
                for (float v = 3.0f; v < tall; v += 3.0f) DrawLineV({c.x - 2.0f, c.y - v}, {c.x + 2.0f, c.y - v}, lit(shade(red_brick, 0.6f)));
                rubble_heap({c.x, c.y + 2.0f}, 12.0f, 4.0f, {190, 182, 168, 255}, red_brick, h + 5u, 1);  // its foot in the rubble
            }
            break;
        }
        case engine::RuinKind::Barn:
        case engine::RuinKind::Farm: {
            const Color wall{206, 204, 192, 255};
            const RubbleMix mix{{204, 200, 188, 255}, concrete, 0.25f, false};
            auto walls = [&] {
                for (const bool front_edge : {false, true}) {  // low broken walls along its edges
                    if (front_edge ? !open[3] : !open[1]) continue;
                    const Face f = front_edge ? Face{on_terrain(map, {x + 1.0f, y + 0.9f}), on_terrain(map, {x, y + 0.9f})}
                                              : Face{on_terrain(map, {x + 0.9f, y}), on_terrain(map, {x + 0.9f, y + 1.0f})};
                    int i = 0;
                    for (float u = 0.0f; u < 1.0f; u += 0.12f, ++i) {
                        const float v = 3.0f + 7.0f * rand01(h, 30 + i + (front_edge ? 50 : 0));
                        wall_texture(f, u, u + 0.12f, 0.0f, v, Wall::Whitewash, shade(wall, front_edge ? 0.7f : 0.95f), h + static_cast<uint32_t>(i));
                    }
                    burnt(f, 0.0f, 1.0f, 0.0f, 9.0f, 0.4f, h);
                }
            };
            pile_around(height_of(open, 6.0f), mix, 2, [=](Vector2 g) { return g.x < x + 0.9f && g.y < y + 0.9f; }, walls);
            sheets({mid.x, mid.y - 5.0f}, 4, {122, 126, 130, 255}, h);
            beams({mid.x, mid.y - 4.0f}, 2, h + 1);
            break;
        }
        case engine::RuinKind::Factory:
        case engine::RuinKind::Hangar: {
            const bool factory = kind == engine::RuinKind::Factory;
            const Color iron = factory ? Color{112, 132, 146, 255} : Color{120, 132, 118, 255};
            const RubbleMix mix{{150, 146, 138, 255}, factory ? red_brick : iron, factory ? 0.35f : 0.2f, false};
            auto standing = [&] {
                if (factory && open[3]) {  // its brick base, broken
                    const Face f{on_terrain(map, {x + 1.0f, y + 0.9f}), on_terrain(map, {x, y + 0.9f})};
                    int i = 0;
                    for (float u = 0.0f; u < 1.0f; u += 0.15f, ++i) {
                        wall_texture(f, u, u + 0.15f, 0.0f, 4.0f + 7.0f * rand01(h, 40 + i), Wall::RedBrick, shade(red_brick, 0.72f), h + static_cast<uint32_t>(i));
                    }
                }
            };
            pile_around(height_of(open, 5.0f), mix, 2, [=](Vector2 g) { return g.y < y + 0.9f; }, standing);
            sheets({mid.x, mid.y - 3.0f}, 5, iron, h);
            sheets({mid.x + 4.0f, mid.y - 1.0f}, 2, {120, 122, 122, 255}, h + 3);
            if (rand01(h, 1) < 0.6f) {  // a post standing, a bit of the truss bent off it
                const Vector2 p = on_terrain(map, {x + 0.3f + 0.4f * rand01(h, 2), y + 0.3f + 0.4f * rand01(h, 3)});
                const float tall = 18.0f + 12.0f * rand01(h, 4);
                DrawLineEx(p, {p.x, p.y - tall}, 2.0f, lit({70, 72, 70, 255}));
                const Vector2 t{p.x, p.y - tall};
                const Vector2 e{t.x + 10.0f * (rand01(h, 5) < 0.5f ? -1.0f : 1.0f), t.y + 6.0f + 6.0f * rand01(h, 6)};
                fine_line(t, e, {80, 82, 80, 255});
                fine_line({t.x, t.y + 3.0f}, {e.x, e.y + 2.0f}, {80, 82, 80, 255});
                for (float k = 0.25f; k < 1.0f; k += 0.25f) fine_line(lerp(t, e, k), {lerp(t, e, k).x, lerp(t, e, k).y + 3.0f}, {80, 82, 80, 255});
            }
            break;
        }
        case engine::RuinKind::Elevator: {
            const RubbleMix mix{{196, 190, 176, 255}, concrete, 0.3f, false};
            const bool stump = rand01(h, 1) < 0.7f;
            auto silo = [&] {
                if (!stump) return;  // a silo's stump, its top broken off ragged
                const Vector2 foot = on_terrain(map, {x + 0.5f, y + 0.5f});
                constexpr float kRadius = 10.0f;
                const float tall = 14.0f + 38.0f * rand01(h, 2);
                const Color c{196, 190, 176, 255};
                for (int st = 0; st < 8; ++st) {
                    const float x0 = foot.x - kRadius + 2.0f * kRadius * static_cast<float>(st) / 8.0f;
                    const float t = (static_cast<float>(st) + 0.5f) / 8.0f;
                    const float top = tall - 8.0f * rand01(h, 10 + st);
                    DrawRectangleRec({std::round(x0), foot.y - top, std::round(2.0f * kRadius / 8.0f), top}, lit(shade(c, 1.18f - 0.5f * t)));
                }
                burnt(Face{{foot.x - kRadius, foot.y}, {foot.x + kRadius, foot.y}}, 0.0f, 1.0f, 0.0f, tall, 0.5f, h);
            };
            pile_around(height_of(open, 9.0f), mix, 4, [=](Vector2 g) { return g.x + g.y < x + y + 1.0f; }, silo);
            DrawEllipse(static_cast<int>(mid.x - 8.0f), static_cast<int>(mid.y + 6.0f), 12.0f, 4.0f, lit({196, 170, 96, 255}));  // the grain spilt
            DrawEllipse(static_cast<int>(mid.x - 9.0f), static_cast<int>(mid.y + 5.0f), 8.0f, 2.5f, lit({214, 190, 112, 255}));
            break;
        }
        case engine::RuinKind::GasStation: {
            rubble_pile(map, {x, y}, {1.0f, 1.0f}, height_of(open, 3.0f), {{160, 156, 148, 255}, {200, 60, 50, 255}, 0.08f, false}, h, 2);
            const Vector2 a = on_terrain(map, {x + 0.1f, y + 0.3f}, 12.0f);
            const Vector2 b = on_terrain(map, {x + 0.9f, y + 0.3f}, 2.0f);
            const Vector2 c = on_terrain(map, {x + 0.9f, y + 0.8f}, 1.0f);
            const Vector2 d = on_terrain(map, {x + 0.1f, y + 0.8f}, 10.0f);
            fill_quad(a, b, c, d, {170, 164, 152, 255});  // the canopy down, on its bent posts
            DrawLineEx(d, {d.x, d.y + 10.0f}, 2.0f, lit({150, 150, 146, 255}));
            DrawLineEx({b.x, b.y}, {b.x, b.y + 2.0f}, 2.0f, lit({150, 150, 146, 255}));
            DrawLineV(a, d, lit({180, 60, 50, 255}));
            beams({mid.x, mid.y + 4.0f}, 2, h);
            break;
        }
        case engine::RuinKind::Tower: {
            const Vector2 a = on_terrain(map, {x + 0.1f, y + 0.4f});
            const Vector2 b = on_terrain(map, {x + 0.95f, y + 0.95f});
            for (const float off : {-2.0f, 2.0f}) fine_line({a.x, a.y + off}, {b.x, b.y + off}, {150, 150, 146, 255});
            for (float t = 0.0f; t < 1.0f; t += 0.12f) fine_line({lerp(a, b, t).x, lerp(a, b, t).y - 2.0f}, {lerp(a, b, t + 0.12f).x, lerp(a, b, t + 0.12f).y + 2.0f}, {130, 130, 126, 255});
            DrawRectangleRec({std::round(mid.x - 3.0f), std::round(mid.y - 5.0f), 6.0f, 5.0f}, lit({60, 56, 52, 255}));
            break;
        }
        case engine::RuinKind::Base: {
            rubble_pile(map, {x, y}, {1.0f, 1.0f}, height_of(open, 5.0f), {{150, 132, 96, 255}, {110, 90, 70, 255}, 0.3f, false}, h, 2);
            beams({mid.x, mid.y - 4.0f}, 4, h);
            sheets({mid.x, mid.y - 4.0f}, 2, {120, 128, 110, 255}, h);
            for (int i = 0; i < 6; ++i) {  // sandbags scattered
                const Vector2 p{mid.x + (rand01(h, 60 + i) - 0.5f) * 30.0f, mid.y + (rand01(h, 70 + i) - 0.5f) * 12.0f};
                DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), 2.6f, 1.4f, lit({150, 132, 96, 255}));
            }
            break;
        }
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
            const Vector2 ground{to_vector2(s.center).x + 0.25f, to_vector2(s.center).y + 0.25f};
            bool afire = false;
            for (const engine::Fire& f : world.fires()) {
                const Vector2 fc = to_vector2(f.center);
                const float r = to_float(f.radius);
                afire = afire || (ground.x - fc.x) * (ground.x - fc.x) + (ground.y - fc.y) * (ground.y - fc.y) <= r * r;
            }
            for (int i = 0; i < boxes; ++i) {
                const Rectangle box{g.x - 6.0f + 4.0f * static_cast<float>(i), g.y - 4.0f - 2.0f * static_cast<float>(i % 2),
                                    6.0f, 4.0f};
                DrawRectangleRec(box, afire ? Color{58, 54, 40, 255} : Color{96, 104, 64, 255});
                DrawRectangleLinesEx(box, 1.0f, {52, 58, 34, 255});
            }
            for (int i = 0; i < boxes && afire; ++i) {  // the boxes burning
                const FxSheet& fl = fx_sheet(i == 0 ? Fx::Flame1 : Fx::Flame0, static_cast<int>((s.id + static_cast<uint32_t>(i)) % 3u));
                const float x = g.x - 3.0f + 4.0f * static_cast<float>(i);
                const float y = g.y - 3.0f - 2.0f * static_cast<float>(i % 2);
                draw_fx(fl, static_cast<int>(GetTime() * 11.0 + static_cast<double>(i * 3 + static_cast<int>(s.id % 5u))) % fl.frames, {x, y});
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

// --- Buildings baked into pixel art ------------------------------------------------

void pixelate(Image& img, const std::vector<Color>& palette);  // with the vehicles' sprites, below
const char* dump_dir();

// A building's damage in four stages, each drawn (and baked) as its middle.
int damage_stage(float damage) { return damage < 0.2f ? 0 : damage < 0.45f ? 1 : damage < 0.7f ? 2 : 3; }
float stage_damage(int stage) {
    static constexpr float kMiddle[4] = {0.0f, 0.33f, 0.58f, 0.85f};
    return kMiddle[std::clamp(stage, 0, 3)];
}

// The colours a baked building is made of: the ones most of it is,
// spread apart enough (the rest go to the nearest of them).
std::vector<Color> palette_of_image(const Image& img, size_t most) {
    std::vector<std::pair<int, uint32_t>> bins;
    {
        std::vector<int> count(32768, 0);
        const auto* px = static_cast<const Color*>(img.data);
        for (int i = 0; i < img.width * img.height; ++i) {
            if (px[i].a < 100) continue;
            ++count[static_cast<size_t>((px[i].r >> 3) << 10 | (px[i].g >> 3) << 5 | (px[i].b >> 3))];
        }
        for (uint32_t k = 0; k < count.size(); ++k) {
            if (count[k] > 0) bins.emplace_back(count[k], k);
        }
    }
    std::sort(bins.begin(), bins.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Color> palette;
    for (const float apart : {14.0f, 7.0f}) {
        for (const auto& [n, k] : bins) {
            if (palette.size() >= most) break;
            const Color c{static_cast<unsigned char>(((k >> 10) & 31u) * 8u + 4u), static_cast<unsigned char>(((k >> 5) & 31u) * 8u + 4u),
                          static_cast<unsigned char>((k & 31u) * 8u + 4u), 255};
            bool near = false;
            for (const Color& p : palette) {
                const float dr = static_cast<float>(c.r - p.r);
                const float dg = static_cast<float>(c.g - p.g);
                const float db = static_cast<float>(c.b - p.b);
                if (dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f < apart * apart) {
                    near = true;
                    break;
                }
            }
            if (!near) palette.push_back(c);
        }
    }
    if (palette.empty()) palette.push_back({0, 0, 0, 255});
    return palette;
}

// Where a building stands on the screen, with its roof, its chimney, its
// flag, its yard: the rectangle it's baked in.
Rectangle building_bounds(const engine::TileMap& map, int kind, const engine::Structure& s, int tx, int ty) {
    Rectangle g{static_cast<float>(tx), static_cast<float>(ty), 1.0f, 1.0f};
    float up = 44.0f;
    float pad = 0.15f;
    if (kind == 4) {  // a yard's things: a well sweep's pole, a haystack, a fence
        up = 40.0f;
        pad = 0.4f;
    } else if (kind == 5) {  // a bus stop, its kiosk beside it
        up = 34.0f;
        pad = 0.8f;
    } else if (kind == 3) {
        const engine::RuinKind ruin = map.ruin(tx, ty);
        up = ruin == engine::RuinKind::Apartment ? 110.0f : ruin == engine::RuinKind::Elevator ? 64.0f : 40.0f;
        pad = 0.45f;
    } else if (kind != 0) {
        g = footprint(s, 0.0f);
        switch (s.type) {
            case engine::StructureType::Apartment: up = 104.0f; pad = 0.6f; break;
            case engine::StructureType::CellTower: up = 118.0f; pad = 0.7f; break;
            case engine::StructureType::GasStation: up = 50.0f; pad = 0.4f; break;
            case engine::StructureType::Elevator: up = 140.0f; pad = 0.4f; break;
            case engine::StructureType::House:
                up = s.look == engine::HouseLook::Cowshed ? 70.0f : s.look == engine::HouseLook::Coop ? 26.0f
                   : s.look == engine::HouseLook::Factory ? 92.0f : s.tiles.size() < engine::kSpaciousTiles ? 84.0f : 50.0f;
                pad = s.look == engine::HouseLook::Cowshed ? 2.0f : s.look == engine::HouseLook::Coop ? 1.4f : 0.3f;
                break;
            case engine::StructureType::Headquarters: up = 96.0f; pad = 0.6f; break;  // its mast
            default: up = 74.0f; pad = 0.5f; break;  // a player's: the flag above it
        }
        const float k = building_scale(s);  // (drawn that much bigger)
        up *= k;
        pad = pad * k + (k - 1.0f) * 0.5f;
    }
    const Vector2 corners[4] = {{g.x - pad, g.y - pad}, {g.x + g.width + pad, g.y - pad}, {g.x + g.width + pad, g.y + g.height + pad},
                                {g.x - pad, g.y + g.height + pad}};
    float x0 = 1e9f;
    float y0 = 1e9f;
    float x1 = -1e9f;
    float y1 = -1e9f;
    for (const Vector2& c : corners) {
        const Vector2 p = on_terrain(map, c);
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    x0 = std::floor(x0 - 4.0f);
    y0 = std::floor(y0 - up);
    return {x0, y0, std::ceil(x1 + 4.0f) - x0, std::ceil(y1 + 4.0f) - y0};
}

void WorldRenderer::draw_by_hand(const engine::TileMap& map, const BuildingBake& b) const {
    if (b.kind == 3) return draw_ruin_tile(map, b.tx, b.ty);
    if (b.kind == 4) return draw_yard(map, b.tx, b.ty);
    if (b.kind == 5) {
        for (const BusStop& stop : bus_stops_) {
            if (static_cast<int>(stop.ground.x) == b.tx && static_cast<int>(stop.ground.y) == b.ty) return draw_bus_stop(map, stop);
        }
        return;
    }
    if (b.kind == 0) return draw_house(map, b.tx, b.ty, b.damage);
    if (const float k = building_scale(b.s); k != 1.0f && g_shrink == 1.0f) {  // bigger than its tiles (see building_scale)
        const Rectangle f = footprint(b.s, 0.0f);
        return scaled_about(on_terrain(map, {f.x + f.width * 0.5f, f.y + f.height * 0.5f}), k, [&] { draw_by_hand(map, b); });
    }
    if (b.kind == 2) return draw_building(map, b.s);
    switch (b.s.type) {
        case engine::StructureType::Apartment: return draw_apartment(map, b.s, b.damage);
        case engine::StructureType::CellTower: return draw_cell_tower(map, b.s, b.damage);
        case engine::StructureType::GasStation: return draw_gas_station(map, b.s, b.damage);
        case engine::StructureType::Elevator: return draw_elevator(map, b.s, b.damage);
        default: break;
    }
    if (b.s.look == engine::HouseLook::Cowshed) return draw_cowshed(map, b.s, b.damage);
    if (b.s.look == engine::HouseLook::Coop) return draw_coop(map, b.s, b.damage);
    if (b.s.look == engine::HouseLook::Factory) return draw_factory(map, b.s, b.damage);
    if (b.s.type == engine::StructureType::House && b.s.tiles.size() < engine::kSpaciousTiles) {  // a house on its plot
        const engine::TilePos t = b.s.tiles.front();
        return draw_house(map, footprint(b.s, 0.0f), tile_hash(t.x, t.y), b.damage,
                          std::find(shops_.begin(), shops_.end(), b.s.id) != shops_.end());
    }
    draw_barn(map, b.s, b.damage);
}

bool WorldRenderer::draw_baked(const BuildingBake& want) const {
    const auto it = building_sprites_.find(want.key);
    const bool current = it != building_sprites_.end() && it->second.look == want.look;
    if (!current && std::find(unbakeable_.begin(), unbakeable_.end(), want.key) == unbakeable_.end() &&
        std::none_of(building_bakes_.begin(), building_bakes_.end(), [&](const BuildingBake& b) { return b.key == want.key; })) {
        building_bakes_.push_back(want);
    }
    if (it == building_sprites_.end()) return false;
    // (Until it's baked again as it looks now: as it was.)
    it->second.used = frame_;
    set_grain({});
    DrawTextureV(it->second.tex, it->second.at, lit(WHITE));
    if (g_light >= 1.0f) {  // (as it's seen now, not as it was)
        const float now = static_cast<float>(GetTime());
        for (size_t i = 0; i < it->second.burning.size(); ++i) {
            const auto& f = it->second.burning[i];
            if (!f.flames) continue;
            const FxSheet& s = f.roof ? fx_sheet(i % 2 == 0 ? Fx::Roof1 : Fx::Roof0, static_cast<int>(i % 3))
                                      : fx_sheet(i % 2 == 0 ? Fx::Flame1 : Fx::Flame0, static_cast<int>(i % 3));
            draw_fx(s, static_cast<int>(now * (f.roof ? 9.0f : 12.0f) + static_cast<float>(i) * 3.0f) % s.frames, {f.at.x, f.at.y + 2.0f});
        }
    }
    return true;
}

// Each building asked for, drawn by hand into a texture as it stands on the
// ground (the ground's grain on it), then made pixel art as the vehicles
// are: its colours, a dark outline, the light on its top edges. A few a
// frame; the ones no longer drawn let go.
void WorldRenderer::bake_buildings(const engine::World& world) const {
    ++frame_;
    constexpr int kW = 768;
    constexpr int kH = 512;
    if (building_target_.id == 0) {
        building_target_ = LoadRenderTexture(kW, kH);
        SetTextureFilter(building_target_.texture, TEXTURE_FILTER_POINT);
    }
    const double start = GetTime();
    const engine::TileMap& map = world.map();
    size_t done = 0;
    for (; done < building_bakes_.size() && GetTime() - start < 0.006; ++done) {
        const BuildingBake& b = building_bakes_[done];
        const Rectangle bounds = building_bounds(map, b.kind, b.s, b.tx, b.ty);
        if (bounds.width > kW || bounds.height > kH) {
            unbakeable_.push_back(b.key);
            continue;
        }
        const float light = g_light;
        g_light = 1.0f;
        BeginTextureMode(building_target_);
        ClearBackground({0, 0, 0, 0});
        Camera2D cam{};
        cam.target = {bounds.x, bounds.y};
        cam.zoom = 1.0f;
        BeginMode2D(cam);
        const float one = 1.0f;
        SetShaderValue(grain_, grain_zoom_loc_, &one, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(grain_);
        set_grain(kObjectGrain);
        std::vector<FineLine> fine;
        std::vector<FireSpot> fires;
        g_fine = &fine;
        g_fires = &fires;
        draw_by_hand(map, b);
        g_fine = nullptr;
        g_fires = nullptr;
        EndShaderMode();
        EndMode2D();
        EndTextureMode();
        g_light = light;
        Image img = LoadImageFromTexture(building_target_.texture);
        ImageFlipVertical(&img);
        ImageCrop(&img, {0.0f, 0.0f, bounds.width, bounds.height});
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        if (const char* dump = dump_dir()) ExportImage(img, TextFormat("%s/building_%llu_raw.png", dump, static_cast<unsigned long long>(b.key)));
        pixelate(img, palette_of_image(img, 64));
        for (const FineLine& l : fine) {  // the thin things, a pixel wide, over it
            ImageDrawLine(&img, static_cast<int>(std::lround(l.a.x - bounds.x)), static_cast<int>(std::lround(l.a.y - bounds.y)),
                          static_cast<int>(std::lround(l.b.x - bounds.x)), static_cast<int>(std::lround(l.b.y - bounds.y)), l.c);
        }
        if (const char* dump = dump_dir()) ExportImage(img, TextFormat("%s/building_%llu.png", dump, static_cast<unsigned long long>(b.key)));
        BuildingSprite& sprite = building_sprites_[b.key];
        if (sprite.tex.id != 0) UnloadTexture(sprite.tex);
        sprite.tex = LoadTextureFromImage(img);
        SetTextureFilter(sprite.tex, TEXTURE_FILTER_POINT);
        sprite.at = {bounds.x, bounds.y};
        sprite.look = b.look;
        sprite.burning.clear();
        for (const FireSpot& f : fires) sprite.burning.push_back({f.at, f.flames, f.roof});
        sprite.ground = b.kind == 0 || b.kind >= 3 ? Vector2{static_cast<float>(b.tx) + 0.5f, static_cast<float>(b.ty) + 0.5f} : to_vector2(b.s.center);
        sprite.anchor = on_terrain(map, sprite.ground);
        sprite.used = frame_;
        UnloadImage(img);
    }
    building_bakes_.erase(building_bakes_.begin(), building_bakes_.begin() + static_cast<long>(done));
    if (frame_ % 600 == 0) {  // let go of the ones not drawn for a while (gone, or far off)
        for (auto it = building_sprites_.begin(); it != building_sprites_.end();) {
            if (frame_ - it->second.used > 1200) {
                UnloadTexture(it->second.tex);
                it = building_sprites_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// The field works on a tile as they are, if there are any (by the ground
// as it was last seen, the structure dug there).
std::optional<works::Spec> works_now(const engine::World& world, int tx, int ty, engine::Terrain t) {
    works::Spec spec;
    const engine::Structure* s = world.structure_at({tx, ty});
    switch (t) {
        case engine::Terrain::Trench: spec.kind = works::Kind::Trench; break;
        case engine::Terrain::Foxhole: spec.kind = works::Kind::Foxhole; break;
        case engine::Terrain::Dugout: spec.kind = works::Kind::Dugout; break;
        case engine::Terrain::GunPit:
            spec.kind = !s || s->pit == engine::PitKind::Gun ? works::Kind::GunPit
                        : s->pit == engine::PitKind::Mortar ? works::Kind::MortarPit
                                                             : works::Kind::Caponier;
            break;
        case engine::Terrain::Wire: spec.kind = works::Kind::Wire; break;
        case engine::Terrain::Hedgehogs: spec.kind = works::Kind::Hedgehogs; break;
        default:
            if (s && s->type == engine::StructureType::Parapet) {
                spec.kind = works::Kind::Parapet;
                break;
            }
            return std::nullopt;
    }
    spec.facing = {0.7071f, -0.7071f};
    if (s) {
        const Vector2 f = to_vector2(s->facing);
        if (f.x != 0.0f || f.y != 0.0f) spec.facing = f;
        spec.parapet = s->parapet;
        spec.upgrading = s->upgrading && s->fitting == engine::TrenchFit::Dugout;
        spec.fit = static_cast<uint8_t>(s->fit);
        spec.fitting = s->upgrading ? static_cast<uint8_t>(s->fitting) : 0;
        if (s->upgrading && (s->fit_facing.x.raw != 0 || s->fit_facing.y.raw != 0) && s->fit == engine::TrenchFit::None) {
            spec.facing = to_vector2(s->fit_facing);  // (what's being made faces its own way)
        }
        const int32_t max_hp = engine::structure_type(s->type).max_hp;
        spec.damage = max_hp > 0 ? damage_stage(1.0f - static_cast<float>(s->hp) / static_cast<float>(max_hp)) : 0;
    }
    spec.seed = tile_hash(tx * 3 + 7, ty * 5 + 11);
    return spec;
}

std::optional<works::Spec> WorldRenderer::works_at(const engine::World& world, int tx, int ty) const {
    const engine::TileMap& map = world.map();
    if (!map.contains_tile(tx, ty)) return std::nullopt;
    const int idx = ty * map.width() + tx;
    const engine::Terrain t = seen_terrain_[static_cast<size_t>(idx)];
    const engine::Structure* s = world.structure_at({tx, ty});
    std::optional<works::Spec> spec;
    if (reveal_ || (s && s->owner == viewer_) || fog(world, tx, ty) == kInView) {  // as it is
        spec = works_now(world, tx, ty, t);
        if (spec) {
            works_known_[idx] = *spec;
        } else if (!works_known_.empty()) {
            works_known_.erase(idx);
        }
    } else if (const auto it = works_known_.find(idx); it != works_known_.end()) {  // as last seen
        spec = it->second;
    } else {
        spec = works_now(world, tx, ty, t);  // (never seen close: the ground shows it's dug)
        if (spec && spec->kind == works::Kind::Parapet) spec.reset();
    }
    if (!spec) return spec;
    // Where its ditch goes on to: the trenches, foxholes and dugouts next to it; wire to the wire.
    const bool wire = spec->kind == works::Kind::Wire;
    const bool joins = spec->kind == works::Kind::Trench || spec->kind == works::Kind::Foxhole ||
                       spec->kind == works::Kind::Dugout || spec->kind == works::Kind::MortarPit;
    static constexpr int kSteps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int bit = 0; bit < 4; ++bit) {
        const int x = tx + kSteps[bit][0];
        const int y = ty + kSteps[bit][1];
        if (!map.contains_tile(x, y)) continue;
        const engine::Terrain n = seen_terrain_[static_cast<size_t>(y * map.width() + x)];
        const bool linked = wire ? n == engine::Terrain::Wire
                                 : joins && (n == engine::Terrain::Trench || n == engine::Terrain::Foxhole || n == engine::Terrain::Dugout);
        if (linked) spec->links |= static_cast<uint8_t>(1u << bit);
    }
    if (wire && spec->links == 0) {  // a line of wire corner to corner
        static constexpr int kDiagonals[4][2] = {{1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
        for (int k = 0; k < 4; ++k) {
            const int x = tx + kDiagonals[k][0];
            const int y = ty + kDiagonals[k][1];
            if (map.contains_tile(x, y) && seen_terrain_[static_cast<size_t>(y * map.width() + x)] == engine::Terrain::Wire) {
                spec->links |= static_cast<uint8_t>(16u << k);
            }
        }
    }
    return spec;
}

works::Area WorldRenderer::works_area(const engine::World& world, int tx, int ty) const {
    works::Area area;
    for (int k = 0; k < 9; ++k) {
        const std::optional<works::Spec> spec = works_at(world, tx + k % 3 - 1, ty + k / 3 - 1);
        if (!spec) continue;
        area.spec[static_cast<size_t>(k)] = *spec;
        area.has[static_cast<size_t>(k)] = true;
    }
    return area;
}

namespace {
// What a tile's works look like (and its neighbours'): baked again when it changes.
uint32_t look_of(const works::Area& area) {
    uint32_t h = 2166136261u;
    auto in = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
    for (size_t k = 0; k < 9; ++k) {
        in(area.has[k] ? 1u : 0u);
        if (!area.has[k]) continue;
        const works::Spec& s = area.spec[k];
        in(static_cast<uint32_t>(s.kind));
        in(s.links);
        in((s.parapet ? 1u : 0u) | (s.upgrading ? 2u : 0u) | static_cast<uint32_t>(s.damage) << 2 | static_cast<uint32_t>(s.fit) << 4 |
           static_cast<uint32_t>(s.fitting) << 8);
        in(static_cast<uint32_t>(std::lround(std::atan2(s.facing.y, s.facing.x) * 8.0f / 3.14159265f)) & 15u);
    }
    return h;
}
}  // namespace

bool WorldRenderer::draw_works_baked(const engine::World& world, int tx, int ty, bool front) const {
    const int idx = ty * world.map().width() + tx;
    const auto it = works_sprites_.find(idx);
    if (!front) {  // (the back's drawn first each frame: it asks for a bake if it's changed)
        const works::Area area = works_area(world, tx, ty);
        const uint32_t look = look_of(area);
        if ((it == works_sprites_.end() || it->second.look != look) &&
            std::none_of(works_bakes_.begin(), works_bakes_.end(), [&](const WorksBake& b) { return b.tx == tx && b.ty == ty; })) {
            works_bakes_.push_back({tx, ty, area, look});
        }
    }
    if (it == works_sprites_.end()) return false;
    it->second.used = frame_;
    set_grain({});
    DrawTextureV(front ? it->second.front : it->second.back, it->second.at, lit(WHITE));
    return true;
}

// The works asked for, a few a frame; the ones no longer drawn let go.
void WorldRenderer::bake_works(const engine::World& world) const {
    const engine::TileMap& map = world.map();
    const double start = GetTime();
    size_t done = 0;
    for (; done < works_bakes_.size() && (done == 0 || GetTime() - start < 0.004); ++done) {
        const WorksBake& b = works_bakes_[done];
        std::array<float, 4> corner{};
        static constexpr int kCorner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int k = 0; k < 4; ++k) corner[static_cast<size_t>(k)] = iso::corner_height(map, b.tx + kCorner[k][0], b.ty + kCorner[k][1]) * iso::kElevationStep;
        works::Sprite sprite = works::bake(b.area, b.tx, b.ty, corner);
        if (const char* dump = dump_dir()) {
            ExportImage(sprite.back, TextFormat("%s/works_%d_%d.png", dump, b.tx, b.ty));
            ExportImage(sprite.front, TextFormat("%s/works_%d_%d_front.png", dump, b.tx, b.ty));
        }
        WorksSprite& ws = works_sprites_[b.ty * map.width() + b.tx];
        if (ws.back.id != 0) UnloadTexture(ws.back);
        if (ws.front.id != 0) UnloadTexture(ws.front);
        ws.back = LoadTextureFromImage(sprite.back);
        ws.front = LoadTextureFromImage(sprite.front);
        SetTextureFilter(ws.back, TEXTURE_FILTER_POINT);
        SetTextureFilter(ws.front, TEXTURE_FILTER_POINT);
        ws.at = sprite.at;
        ws.look = b.look;
        ws.used = frame_;
        UnloadImage(sprite.back);
        UnloadImage(sprite.front);
    }
    works_bakes_.erase(works_bakes_.begin(), works_bakes_.begin() + static_cast<long>(done));
    if (frame_ % 600 == 300) {
        for (auto it = works_sprites_.begin(); it != works_sprites_.end();) {
            if (frame_ - it->second.used > 1200) {
                UnloadTexture(it->second.back);
                UnloadTexture(it->second.front);
                it = works_sprites_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void WorldRenderer::draw(const engine::World& world, const RtsCamera& camera, float alpha,
                         std::span<const engine::EntityId> selection, engine::EntityId selected_structure,
                         const BuildGhost* ghost, std::span<const engine::TilePos> trench) const {
    const engine::TileMap& map = world.map();
    drawn_world_ = &world;
    const Rectangle view = camera.visible_world_rect();
    auto is_selected = [&](engine::EntityId id) {
        return std::binary_search(selection.begin(), selection.end(), id);
    };

    ensure_fx();
    ensure_plants();
    g_crushed = &crushed_;
    g_crushed_width = world.map().width();
    ensure_muzzles();
    if (grain_.id == 0) {
        grain_ = LoadShaderFromMemory(kGrainVertex, kGrainFragment);
        grain_zoom_loc_ = GetShaderLocation(grain_, "zoom");
    }
    if (hover_.any()) {  // (outside the 2D mode) the texture the hovered thing is drawn into, cleared
        if (hover_target_.id == 0 || hover_target_.texture.width != GetScreenWidth() || hover_target_.texture.height != GetScreenHeight()) {
            if (hover_target_.id != 0) UnloadRenderTexture(hover_target_);
            hover_target_ = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        }
        BeginTextureMode(hover_target_);
        ClearBackground({0, 0, 0, 0});
        EndTextureMode();
    }
    if (outline_.id == 0) {
        outline_ = LoadShaderFromMemory(nullptr, kOutlineFragment);
        outline_texel_loc_ = GetShaderLocation(outline_, "texel");
    }
    bake_sprites(world);  // outside the 2D mode: it draws into a texture
    bake_trees();
    bake_buildings(world);
    bake_works(world);
    BeginMode2D(camera.camera2d());
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
    // What's being fitted into the trench, where, facing which way.
    for (size_t i = 0; i < fortify_ghost_.tiles.size(); ++i) {
        const engine::TilePos t = fortify_ghost_.tiles[i];
        const auto x = static_cast<float>(t.x);
        const auto y = static_cast<float>(t.y);
        const Color color = i < fortify_ghost_.valid.size() && fortify_ghost_.valid[i] ? theme::kSelection : theme::kDanger;
        const Vector2 c[4] = {on_terrain(map, {x, y}), on_terrain(map, {x + 1, y}), on_terrain(map, {x + 1, y + 1}), on_terrain(map, {x, y + 1})};
        fill_quad(c[0], c[1], c[2], c[3], ColorAlpha(color, 0.25f));
        for (int k = 0; k < 4; ++k) DrawLineEx(c[k], c[(k + 1) % 4], 1.5f, ColorAlpha(color, 0.9f));
    }
    if (fortify_ghost_.arrow) {
        const Vector2 a = on_terrain(map, fortify_ghost_.from, 2.0f);
        const Vector2 b = on_terrain(map, fortify_ghost_.to, 2.0f);
        const Vector2 d{b.x - a.x, b.y - a.y};
        const float l = std::max(0.001f, std::hypot(d.x, d.y));
        const Vector2 u{d.x / l, d.y / l};
        DrawLineEx(a, b, 2.0f, {245, 240, 220, 230});
        DrawTriangle(b, {b.x - u.x * 7.0f - u.y * 4.0f, b.y - u.y * 7.0f + u.x * 4.0f}, {b.x - u.x * 7.0f + u.y * 4.0f, b.y - u.y * 7.0f - u.x * 4.0f},
                     {245, 240, 220, 230});
        DrawTriangle(b, {b.x - u.x * 7.0f + u.y * 4.0f, b.y - u.y * 7.0f - u.x * 4.0f}, {b.x - u.x * 7.0f - u.y * 4.0f, b.y - u.y * 7.0f + u.x * 4.0f},
                     {245, 240, 220, 230});
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
        const engine::Structure* post = nullptr;  // a scout's observation post
        const BusStop* stop = nullptr;
        bool yard = false;  // a village yard's things on house_x, house_y
        bool works = false;  // the front of the field works on house_x, house_y (their near bank, sandbags, a mound)
        const FallingTree* falling = nullptr;
        float damage = 0.0f;  // 0 = intact, 1 = about to collapse
        const engine::Structure* building = nullptr;
        const TrainCar* car = nullptr;
        const Remains* wreck = nullptr;  // a burnt-out tank
        float light = 1.0f;  // dimmer under the fog of war
    };
    std::vector<Drawable> drawables;
    drawables.reserve(world.units().size() + world.projectiles().size() + 1024);
    std::vector<const engine::Structure*> barns;
    for (const engine::Unit& u : world.units()) {
        if (u.inside || u.riding || u.airborne || !shows(world, u)) continue;  // behind walls, on the armor (with it), up in the sky, or unseen
        const Vector2 g = unit_ground_pos(u, alpha);
        const Vector2 p = on_terrain(map, g);
        if (!CheckCollisionPointRec(p, {view.x - 60, view.y - 60, view.width + 120, view.height + 120})) continue;
        drawables.push_back({.depth = g.x + g.y, .unit = &u});
    }
    for (const FallingTree& f : falling_trees_) {
        if (in_view(world, f.ground)) drawables.push_back({.depth = f.ground.x + f.ground.y, .falling = &f});
    }
    for (const BusStop& b : bus_stops_) {
        if (!reveal_ && fog(world, static_cast<int>(b.ground.x), static_cast<int>(b.ground.y)) == kUnexplored) continue;
        if (in_view(world, b.ground)) drawables.push_back({.depth = b.ground.x + b.ground.y, .stop = &b});
    }
    // Observation posts: ours as they are, others' as last seen (a hair nearer than the trees on their tile).
    for (const engine::Structure& s : world.structures()) {
        if (s.type == engine::StructureType::ObservationPost && s.owner == viewer_ && !s.tiles.empty()) {
            drawables.push_back({.depth = static_cast<float>(s.tiles.front().x + s.tiles.front().y) + 1.02f, .post = &s});
        }
    }
    for (const auto& [id, s] : remembered_) {
        if (s.type == engine::StructureType::ObservationPost && !s.tiles.empty()) {
            drawables.push_back({.depth = static_cast<float>(s.tiles.front().x + s.tiles.front().y) + 1.02f, .post = &s,
                                 .light = reveal_ || world.sees(viewer_, s) ? 1.0f : kFogLight});
        }
    }
    for (const engine::Projectile& p : world.projectiles()) {
        const Vector2 g = lerp(to_vector2(p.prev_pos), to_vector2(p.pos), alpha);
        if (in_view(world, g)) drawables.push_back({.depth = g.x + g.y, .projectile = &p});
    }
    auto shred_at = [&](int tx, int ty) -> uint8_t {
        const size_t i = static_cast<size_t>(ty * map.width() + tx);
        return i < seen_shred_.size() ? seen_shred_[i] : 0;
    };
    // Scenery comes from the remembered ground: what the fog hides stays as it
    // was; what stands behind a spoil tip is out of sight.
    for_each_visible_tile(map, view, [&](int tx, int ty) {
        const int state = fog(world, tx, ty);
        if (state == kUnexplored || behind_relief(tx, ty)) return;
        const float light = state == kInView ? 1.0f : kFogLight;
        if (works_at(world, tx, ty)) {
            drawables.push_back({.depth = static_cast<float>(tx + ty) + 1.5f, .house_x = tx, .house_y = ty, .works = true, .light = light});
        }
        switch (seen_terrain_[static_cast<size_t>(ty * map.width() + tx)]) {
            case engine::Terrain::Urban: {
                if (!village(tx, ty)) break;
                // The yard's things (a well, a haystack, a fence...), baked as the buildings are.
                drawables.push_back({.depth = static_cast<float>(tx + ty) + 0.95f, .house_x = tx, .house_y = ty, .yard = true, .light = light});
                // An apple or a cherry tree in some of the village yards.
                const uint32_t hk = tile_hash(tx * 7 + 3, ty * 5 + 1);
                if (hk % 4 != 0) break;
                Tree t{{static_cast<float>(tx) + 0.25f + 0.5f * hash_unit(hk >> 4), static_cast<float>(ty) + 0.25f + 0.5f * hash_unit(hk >> 12)},
                       0.8f + 0.25f * hash_unit(hk >> 20), 0.9f + 0.2f * hash_unit(hk >> 8), TreeKind::Apple};
                t.height = 0.9f + 0.3f * hash_unit(hk >> 16);
                t.girth = 0.9f + 0.2f * hash_unit(hk >> 24);
                t.bend = (hash_unit(hk >> 6) - 0.5f) * 3.0f;
                t.seed = hk;
                t.shred = shred_at(tx, ty);
                drawables.push_back({.depth = t.ground.x + t.ground.y, .tree = t, .light = light});
                break;
            }
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
                    t.shred = shred_at(tx, ty);
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
                    Tree t = trees[i];
                    t.shred = shred_at(tx, ty);
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
                if (s) {
                    // A house, a barn is one building, drawn once whichever of its tiles is in view.
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
               s.type != engine::StructureType::Elevator &&  // drawn with the ground's tiles, like houses
               s.type != engine::StructureType::ObservationPost;  // (drawn apart)
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
    for (const Remains& r : remains_) {
        const bool whole = drawn_as_armor(engine::unit_type(r.type)) || truck_model(r.type, r.owner).has_value() || gun_or_plane(engine::unit_type(r.type));
        if (whole && r.fell <= 0.0f && in_view(world, r.ground)) drawables.push_back({.depth = r.ground.x + r.ground.y, .wreck = &r});
    }
    std::stable_sort(drawables.begin(), drawables.end(),
                     [](const Drawable& a, const Drawable& b) { return a.depth < b.depth; });
    // The ground burning (an incendiary shell's): what stands in it burns, the trees; low as it dies down.
    struct Burning {
        Vector2 center;
        float radius;
        int less;  // flames a size smaller as it dies down
    };
    std::vector<Burning> burning;
    for (const engine::Fire& f : world.fires()) {
        const Vector2 c = to_vector2(f.center);
        if (!reveal_ && fog(world, static_cast<int>(c.x), static_cast<int>(c.y)) == kUnexplored) continue;
        const engine::Tick left = f.until > world.tick() ? f.until - world.tick() : 0;
        burning.push_back({c, to_float(f.radius), left < 3 * engine::kTicksPerSecond ? 1 : 0});
    }
    auto burning_at = [&](Vector2 g) {
        for (const Burning& b : burning) {
            if ((g.x - b.center.x) * (g.x - b.center.x) + (g.y - b.center.y) * (g.y - b.center.y) <= b.radius * b.radius) return b.less;
        }
        return -1;
    };
    auto draw_drawable = [&](const Drawable& d) {
        g_light = d.light;
        set_grain(d.unit || d.projectile ? Grain{} : kObjectGrain);
        if (d.unit) {
            draw_unit(map, *d.unit, alpha);
            if (!d.unit->riders.empty()) draw_riders(world, *d.unit, alpha);
            const engine::Unit& u = *d.unit;
            const engine::UnitTypeDef& def = engine::unit_type(u.type);
            const std::optional<TruckModel> truck = truck_model(u.type, u.owner);
            if ((truck || drawn_as_armor(def)) && wear_of(u.hp, def.max_hp) >= 3 && u.mired == 0) {  // burning
                Vector2 f = to_vector2(truck ? u.facing : u.hull);
                const float l = std::hypot(f.x, f.y);
                f = l > 0.0f ? Vector2{f.x / l, f.y / l} : Vector2{1.0f, 0.0f};
                draw_flames(map, unit_ground_pos(u, alpha), f, vehicle_flames(u.type, u.owner, false, false, Cargo::None, 0.0f, u.id * 2654435761u));
            }
        } else if (d.projectile) {
            draw_projectile(*d.projectile, alpha);
        } else if (d.works) {
            draw_works_baked(world, d.house_x, d.house_y, true);
        } else if (d.building) {
            const engine::Structure& s = *d.building;
            const engine::StructureDef& def = engine::structure_type(s.type);
            const int stage = s.built ? damage_stage(1.0f - static_cast<float>(s.hp) / static_cast<float>(def.max_hp)) : 0;
            const int progress = s.built ? 4 : std::clamp(static_cast<int>(4.0f * static_cast<float>(s.build_progress) / static_cast<float>(std::max<engine::Tick>(1, def.build_time))), 0, 3);
            BuildingBake want{s.id, static_cast<uint32_t>(stage | progress << 2 | (s.owner & 15u) << 5 | static_cast<uint32_t>(s.type) << 9), 2, s};
            want.s.hp = def.max_hp - static_cast<int32_t>(stage_damage(stage) * static_cast<float>(def.max_hp));
            if (!s.built) want.s.build_progress = static_cast<engine::Tick>(static_cast<float>(def.build_time) * (static_cast<float>(progress) + 0.5f) / 4.0f);
            if (!draw_baked(want)) {
                set_grain(kObjectGrain);
                draw_by_hand(map, want);
            }
        } else if (d.car) {
            draw_train_car(map, *d.car);
        } else if (d.wreck) {
            draw_wreck(map, *d.wreck);
            const Remains& r = *d.wreck;
            const engine::Terrain under = map.terrain(map.clamp_tile({static_cast<int32_t>(std::floor(r.ground.x)), static_cast<int32_t>(std::floor(r.ground.y))}));
            if (!r.sunk && under != engine::Terrain::Water && r.age < 45.0f) {
                const bool truck = truck_model(r.type, r.owner).has_value();
                draw_flames(map, r.ground, truck || r.crashed ? r.facing : r.hull,
                            vehicle_flames(r.type, r.owner, true, r.blown, static_cast<Cargo>(r.cargo), r.age, r.seed));
            }
        } else if (d.ruins) {
            auto rubble = [&](int dx, int dy) {
                return map.contains_tile(d.house_x + dx, d.house_y + dy) && map.terrain(d.house_x + dx, d.house_y + dy) == engine::Terrain::Ruins ? 1u : 0u;
            };
            const uint32_t around = rubble(-1, 0) | rubble(1, 0) << 1 | rubble(0, -1) << 2 | rubble(0, 1) << 3;
            BuildingBake want{uint64_t{2} << 40 | static_cast<uint64_t>(d.house_y * map.width() + d.house_x),
                              static_cast<uint32_t>(map.ruin(d.house_x, d.house_y)) | around << 4, 3};
            want.tx = d.house_x;
            want.ty = d.house_y;
            if (!draw_baked(want)) draw_by_hand(map, want);
        } else if (d.rock) {
            draw_rock(map, d.house_x, d.house_y, map.resource({d.house_x, d.house_y}));
        } else if (d.falling) {
            // Felled: over it goes, faster and faster, about its foot; then lying there a moment, fading.
            const FallingTree& f = *d.falling;
            const auto it = tree_sheets_.find(f.sheet);
            if (it != tree_sheets_.end()) {
                const SpriteSheet& sh = it->second;
                const float t = std::min(1.0f, f.age / kTreeFallSeconds);
                const float angle = f.side * 86.0f * t * t;
                const float fade = f.age < kTreeFallSeconds ? 1.0f : std::max(0.0f, 1.0f - (f.age - kTreeFallSeconds) / 1.2f);
                const Vector2 b = on_terrain(map, f.ground);
                DrawTexturePro(sh.atlas, {static_cast<float>(f.variant * sh.w), 0.0f, static_cast<float>(sh.w), static_cast<float>(sh.h)},
                               {std::round(b.x), std::round(b.y), static_cast<float>(sh.w), static_cast<float>(sh.h)}, sh.origin, angle,
                               ColorAlpha(lit(WHITE), fade));
            }
        } else if (d.stop) {
            // Baked into pixel art as the buildings are.
            const int tx = static_cast<int>(d.stop->ground.x);
            const int ty = static_cast<int>(d.stop->ground.y);
            BuildingBake want{uint64_t{4} << 40 | static_cast<uint64_t>(ty * map.width() + tx), 0, 5};
            want.tx = tx;
            want.ty = ty;
            if (!draw_baked(want)) draw_bus_stop(map, *d.stop);
        } else if (d.yard) {
            BuildingBake want{uint64_t{3} << 40 | static_cast<uint64_t>(d.house_y * map.width() + d.house_x), 0, 4};
            want.tx = d.house_x;
            want.ty = d.house_y;
            if (!draw_baked(want)) draw_yard(map, d.house_x, d.house_y);
        } else if (d.post) {
            // The post, and the scout up his tree on it (in a stump, a hide, he's not to be seen).
            const engine::Unit* man = nullptr;
            for (const engine::Unit& u : world.units()) {
                if (u.owner == d.post->owner && world.post_of(u) && world.post_of(u)->id == d.post->id && shows(world, u)) man = &u;
            }
            draw_post(map, *d.post, man != nullptr);
            if (man && d.post->post == engine::PostKind::Tree) {
                const engine::TilePos t = d.post->tiles.front();
                draw_soldier(*man, on_terrain(map, {static_cast<float>(t.x) + 0.5f, static_cast<float>(t.y) + 0.5f}, kTreePostZ + 1.0f),
                             unit_facing(*man));
            }
        } else if (d.barn) {
            const int stage = damage_stage(d.damage);
            BuildingBake want{d.barn->id, static_cast<uint32_t>(stage | d.barn->bombed << 2), 1, *d.barn};
            want.damage = stage_damage(stage);
            if (!draw_baked(want)) draw_by_hand(map, want);
            if (d.barn->type == engine::StructureType::CellTower && d.barn->antenna != engine::kNoOwner &&
                (d.barn->antenna == viewer_ || reveal_ || world.sees(viewer_, *d.barn))) {
                draw_tower_aerial(map, *d.barn, d.damage);
            }
        } else if (d.house_x >= 0) {
            const int stage = damage_stage(d.damage);
            BuildingBake want{uint64_t{1} << 40 | static_cast<uint64_t>(d.house_y * map.width() + d.house_x), static_cast<uint32_t>(stage), 0};
            want.tx = d.house_x;
            want.ty = d.house_y;
            want.damage = stage_damage(stage);
            if (!draw_baked(want)) draw_by_hand(map, want);
        } else {
            const Tree& t = d.tree;
            const auto it = tree_sheets_.find(static_cast<int>(t.kind) * kTreeStages + tree_stage(t.shred));
            if (t.stump || it == tree_sheets_.end()) {
                draw_tree(map, t);
                if (const int less = burning.empty() ? -1 : burning_at(t.ground); less >= 0) tree_flames(on_terrain(map, t.ground), t, t.shred, less, t.seed);
            } else {
                // Its shadow, then the sprite: the variant by its seed, a lone old oak the biggest.
                const Vector2 b = on_terrain(map, t.ground);
                const int v = t.kind == TreeKind::Oak && t.size >= 1.3f ? kTreeVariants - 1 - static_cast<int>(t.seed % 2)
                                                                        : static_cast<int>((t.seed >> 3) % kTreeVariants);
                const float sz = tree_variant(t.kind, v, 0).size;  // (1.3 over the procedural trees' own)
                const float leaf = t.shred >= 7 ? 0.3f : 1.0f - std::clamp(static_cast<float>(t.shred) / 6.0f, 0.0f, 1.0f);
                const float spread = t.kind == TreeKind::Oak ? 1.5f : t.kind == TreeKind::Poplar ? 0.6f : t.kind == TreeKind::Apple ? 0.8f : 1.0f;
                DrawEllipse(static_cast<int>(b.x + 5.0f * sz), static_cast<int>(b.y + 1.5f * sz), 9.0f * sz * spread * (0.4f + 0.6f * leaf),
                            3.4f * sz, lit({16, 24, 12, 70}));
                const SpriteSheet& sh = it->second;
                const int less = burning.empty() ? -1 : burning_at(t.ground);
                // An axe biting into it: the whole tree shivers, its crown the most.
                float shiver = 0.0f;
                if (const auto hit = chopped_at_.find(static_cast<int>(t.ground.y) * map.width() + static_cast<int>(t.ground.x)); hit != chopped_at_.end()) {
                    const float since = static_cast<float>(GetTime() - hit->second);
                    if (since < 0.35f) shiver = std::sin(since * 70.0f) * 1.6f * (1.0f - since / 0.35f);
                }
                DrawTexturePro(sh.atlas, {static_cast<float>(v * sh.w), 0.0f, static_cast<float>(sh.w), static_cast<float>(sh.h)},
                               {std::round(b.x), std::round(b.y), static_cast<float>(sh.w), static_cast<float>(sh.h)}, sh.origin, shiver,
                               lit(less >= 0 ? Color{128, 104, 88, 255} : WHITE));  // (burning: charred)
                if (less >= 0) tree_flames(b, tree_variant(t.kind, v, 0), t.shred, less, t.seed);
            }
            g_leaf = 1.0f;
            g_bare = 0.0f;
        }
    };
    // The one under the cursor: drawn again into a texture of its own, for its outline (after the rest).
    auto hovered = [&](const Drawable& d) {
        if (!hover_.any()) return false;
        if (d.unit) return d.unit->id == hover_.unit;
        if (d.works) {
            const engine::Structure* w = world.structure_at({d.house_x, d.house_y});
            return w && w->id == hover_.structure;
        }
        const engine::Structure* s = d.barn ? d.barn : d.building ? d.building : d.post;
        if (s) return s->id == hover_.structure;
        if (!hover_.tile) return false;
        if (d.rock) return d.house_x == hover_.tile->x && d.house_y == hover_.tile->y;
        if (d.house_x < 0 && !d.projectile && !d.car && !d.wreck && !d.stop && !d.falling && !d.yard) {  // a tree
            return static_cast<int>(std::floor(d.tree.ground.x)) == hover_.tile->x && static_cast<int>(std::floor(d.tree.ground.y)) == hover_.tile->y;
        }
        return false;
    };
    bool outlined = false;
    auto into_hover = [&](const auto& draw_it) {
        EndShaderMode();
        EndMode2D();
        BeginTextureMode(hover_target_);
        BeginMode2D(camera.camera2d());
        const float light = g_light;
        draw_it();
        g_light = light;
        EndMode2D();
        EndTextureMode();
        BeginMode2D(camera.camera2d());
        BeginShaderMode(grain_);
        outlined = true;
    };
    for (const Drawable& d : drawables) {
        draw_drawable(d);
        if (hovered(d)) {
            into_hover([&] {
                if (d.works) draw_works_baked(world, d.house_x, d.house_y, false);  // (its back is drawn with the ground)
                draw_drawable(d);
            });
        }
    }
    g_light = 1.0f;
    set_grain({});
    draw_services(world, alpha);
    draw_radio_calls(world, alpha);
    draw_particles(map);

    // Smoke screens, burning wrecks' plumes, bursts' dust: over whatever is in them.
    draw_smoke(world);

    // Aircraft in the air: a shadow on the ground, the aircraft high above it.
    for (const engine::Unit& u : world.units()) {
        if (!u.airborne || !shows(world, u)) continue;
        draw_aircraft(map, u, alpha);
        if (u.id == hover_.unit) into_hover([&] { draw_aircraft(map, u, alpha); });
    }
    for (const Remains& r : remains_) {  // aircraft brought down, falling on fire, tumbling
        if (r.fell <= 0.0f) continue;
        Vector2 g{};
        float z = 0.0f;
        Vector2 d{};
        fall_of(r, g, z, d);
        if (!in_view(world, g)) continue;
        if (const SpriteSheet* plane = sheet(SpritePart::Plane, small_variant(engine::unit_type(r.type).model, 3), r.owner)) {
            draw_sprite(*plane, on_terrain(map, g), d, 0, {0, 0, 0, 70});  // its shadow
            draw_sprite(*plane, on_terrain(map, g, z), d, 0);
        }
    }

    draw_shots(world, alpha);
    draw_blasts(map);
    for (const engine::Bearing& b : world.bearings()) {
        if (b.owner != viewer_) continue;
        const engine::Unit* station = world.find_unit(b.station);
        if (!station && !world.find_structure(b.station)) continue;
        const Vector2 from = to_vector2(b.from);
        Vector2 dir = to_vector2(b.dir);
        const float len = std::hypot(dir.x, dir.y);
        if (len <= 0.0f) continue;
        dir = {dir.x / len, dir.y / len};
        const float reach = to_float(station ? engine::unit_type(station->type).df_range : engine::kTowerDfRange);  // (an aerial up a tower)
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
    if (outlined) {  // the glow round what's under the cursor, pulsing a little
        const auto sw = static_cast<float>(hover_target_.texture.width);
        const auto sh = static_cast<float>(hover_target_.texture.height);
        const float texel[2] = {1.0f / sw, 1.0f / sh};
        SetShaderValue(outline_, outline_texel_loc_, texel, SHADER_UNIFORM_VEC2);
        BeginShaderMode(outline_);
        const float pulse = 0.78f + 0.22f * std::sin(static_cast<float>(GetTime()) * 5.0f);
        // Worked out only round the cursor (the thing is under it): as far as the biggest
        // building reaches at this zoom. (The texture's rows run bottom up: flipped.)
        const float reach = 300.0f * camera.camera2d().zoom + 12.0f;
        const float x0 = std::clamp(hover_.at.x - reach, 0.0f, sw);
        const float x1 = std::clamp(hover_.at.x + reach, 0.0f, sw);
        const float y0 = std::clamp(hover_.at.y - reach, 0.0f, sh);
        const float y1 = std::clamp(hover_.at.y + reach, 0.0f, sh);
        DrawTextureRec(hover_target_.texture, {x0, sh - y1, x1 - x0, -(y1 - y0)}, {x0, y0}, ColorAlpha(hover_.color, pulse));
        EndShaderMode();
    }
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
                const GroundAt g = ground_at(world, p, terrain == Terrain::Bridge ? Terrain::Water : terrain);  // the river under a bridge
                Color c = ground_colour(g.terrain, p);
                if (g.terrain == Terrain::Urban && village(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)))) {
                    // A village yard: grass, trodden bare along the paths and by the doors.
                    c = mix(ground_colour(Terrain::Grass, p), {124, 106, 80, 255}, 0.75f * smooth01((field(p, 61) - 0.38f) / 0.25f));
                }
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

    std::vector<engine::TilePos> craters;
    struct WorksTile {
        int tx;
        int ty;
        engine::Terrain terrain;
        float light;
    };
    std::vector<WorksTile> works_here;  // drawn over the ground, all of it (their banks spill over)
    draw_track_marks(world, view);  // (bucketed by tile: drawn with each tile's ground)
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
        draw_marks_on_tile(map, tx, ty);
        if (terrain == engine::Terrain::Crater) craters.push_back({tx, ty});
        if (works_at(world, tx, ty)) works_here.push_back({tx, ty, terrain, g_light});
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
    std::sort(works_here.begin(), works_here.end(), [](const WorksTile& a, const WorksTile& b) { return a.tx + a.ty < b.tx + b.ty; });
    for (const WorksTile& w : works_here) {
        g_light = w.light;
        if (draw_works_baked(world, w.tx, w.ty, false)) continue;
        set_grain(grain_of(engine::Terrain::Crater));
        draw_works(map, w.tx, w.ty, w.terrain, [&](int x, int y) {  // (until it's baked)
            if (!map.contains_tile(x, y)) return false;
            const engine::Terrain t = seen_terrain_[static_cast<size_t>(y * map.width() + x)];
            return t == engine::Terrain::Trench || t == engine::Terrain::Foxhole || t == engine::Terrain::Dugout;
        });
    }
    for (const engine::TilePos& t : craters) {
        g_light = fog(world, t.x, t.y) == kInView ? 1.0f : kFogLight;
        draw_crater(world, t.x, t.y);
    }
    g_light = 1.0f;
    draw_bridges(world);
    draw_fallen_bridges(world);
    draw_spoil_gullies(world, view);
}

void ground_blob(const engine::TileMap& map, Vector2 c, float rx, float ry, Vector2 dir, Color color, uint32_t seed,
                 float rag);  // just below

// A concrete road bridge from bank to bank, level with them and as wide as
// the road, like Red Alert 2's: its deck of concrete slabs like the highway's
// running on from the road, cracked and holed the more it's been hit; solid
// concrete parapets along both sides, broken in a stretch once it's been
// hit; along its near side a thick beam whose foot follows the ground, so at
// both ends it runs into the bank; the piers standing in the river, foam
// round their feet; its shadow on the water.
// A bridge brought down. Folded in two: both halves still hanging off the
// banks, sloping down into the river where they broke, the water round the
// break foaming. Or its middle gone: two stumps standing out from the banks,
// their ends broken off ragged, the rods sticking out; what fell lies in the
// river in slabs, one end up, the piers under it broken off at the water.
void WorldRenderer::draw_fallen_bridges(const engine::World& world) const {
    const engine::TileMap& map = world.map();
    for (const FallenBridge& f : fallen_bridges_) {
        const iso::Deck& d = f.deck;
        const float vm = (d.v0 + d.v1) * 0.5f;
        const Vector2 gm{(d.middle(vm) - vm) * 0.5f, (d.middle(vm) + vm) * 0.5f};
        const int state = fog(world, static_cast<int>(gm.x), static_cast<int>(gm.y));
        if (state == kUnexplored) continue;
        g_light = state == kInView ? 1.0f : kFogLight;
        auto rnd = [&](int i) { return hash_unit(tile_hash(static_cast<int>(f.seed >> 6) + i * 19, i * 7 + 11)); };
        auto ground_of = [](float u, float v) { return Vector2{(u - v) * 0.5f, (u + v) * 0.5f}; };
        auto water_y = [&](float across, float v) {  // the river's (or the bank's) surface there, on the screen
            const Vector2 g = ground_of(d.middle(v) + across * d.half, v);
            const int tx = static_cast<int>(std::floor(g.x));
            const int ty = static_cast<int>(std::floor(g.y));
            float hgt = 0.0f;
            if (tx >= 0 && ty >= 0 && tx < cache_width_ && ty < cache_height_) {
                const float fx = g.x - static_cast<float>(tx);
                const float fy = g.y - static_cast<float>(ty);
                const float top = corner(tx, ty) * (1 - fx) + corner(tx + 1, ty) * fx;
                const float bottom = corner(tx, ty + 1) * (1 - fx) + corner(tx + 1, ty + 1) * fx;
                hgt = top * (1 - fy) + bottom * fy;
            }
            // (A bridge over a river level with its banks is drawn level with the
            // water: what sinks goes under a little way below its deck.)
            return std::max(iso::project(g, hgt).y, iso::project(g, d.height).y + 12.0f);
        };
        // A point of the deck lifted (or dropped) `lift` pixels; under the water, not seen: at its surface.
        auto at = [&](float across, float v, float lift) {
            Vector2 p = iso::project(ground_of(d.middle(v) + across * d.half, v), d.height);
            p.y = std::min(p.y - lift, water_y(across, v) + 1.0f);
            return p;
        };
        auto wet = [&](float across, float v, float lift) {  // at the water's edge (or under)
            return iso::project(ground_of(d.middle(v) + across * d.half, v), d.height).y - lift >= water_y(across, v) - 0.5f;
        };
        constexpr float kBeam = 11.0f;
        constexpr float kParapet = 5.0f;
        const Color concrete{168, 164, 154, 255};
        const Color road = theme::terrain_color(engine::Terrain::Road);
        // A piece of the deck from va to vb, dropped at v by lift(v): its beam, its top, its parapets.
        auto piece = [&](float va, float vb, auto lift) {
            constexpr int kSteps = 16;
            auto v_at = [&](int k) { return va + (vb - va) * static_cast<float>(k) / kSteps; };
            for (int k = 0; k < kSteps; ++k) {  // the beam along its near side
                const float a = v_at(k);
                const float b = v_at(k + 1);
                if (wet(1.0f, a, lift(a)) && wet(1.0f, b, lift(b))) continue;  // (under the water)
                const Vector2 ta = at(1.0f, a, lift(a));
                const Vector2 tb = at(1.0f, b, lift(b));
                const Vector2 ba = at(1.0f, a, lift(a) - kBeam);
                const Vector2 bb = at(1.0f, b, lift(b) - kBeam);
                if (ba.y > ta.y + 0.5f || bb.y > tb.y + 0.5f) fill_quad(ta, tb, bb, ba, shade(concrete, 0.66f));
            }
            set_grain(grain_of(engine::Terrain::Road));
            for (int k = 0; k < kSteps; ++k) {  // its top, as far as it's out of the water
                const float a = v_at(k);
                const float b = v_at(k + 1);
                if (wet(-1.0f, a, lift(a)) && wet(-1.0f, b, lift(b)) && wet(1.0f, a, lift(a)) && wet(1.0f, b, lift(b))) continue;
                fill_quad(at(-1.0f, a, lift(a)), at(-1.0f, b, lift(b)), at(1.0f, b, lift(b)), at(1.0f, a, lift(a)), shade(road, 0.86f));
            }
            for (float v = va + 0.5f; v < vb - 0.2f; v += 0.5f) {
                if (!wet(0.0f, v, lift(v))) DrawLineV(at(-0.85f, v, lift(v)), at(0.85f, v, lift(v)), lit({104, 104, 102, 255}));
            }
            set_grain(kObjectGrain);
            for (const float across : {-1.0f, 1.0f}) {  // the parapets
                for (int k = 0; k < kSteps; ++k) {
                    const float a = v_at(k);
                    const float b = v_at(k + 1);
                    if (wet(across, a, lift(a) + kParapet) && wet(across, b, lift(b) + kParapet)) continue;
                    const float kk = across < 0.0f ? 0.9f : 0.76f;
                    fill_quad(at(across, a, lift(a)), at(across, b, lift(b)), at(across, b, lift(b) + kParapet), at(across, a, lift(a) + kParapet),
                              shade(concrete, kk));
                    DrawLineV(at(across, a, lift(a) + kParapet), at(across, b, lift(b) + kParapet), lit(shade(concrete, 1.1f)));
                }
            }
            // Foam where it goes into the water.
            for (int k = 0; k < kSteps; ++k) {
                const float a = v_at(k);
                const float b = v_at(k + 1);
                if (wet(0.0f, a, lift(a)) != wet(0.0f, b, lift(b))) {
                    const Vector2 p = at(0.0f, (a + b) * 0.5f, -100.0f);
                    DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), 8.0f, 2.5f, lit({214, 226, 228, 120}));
                    DrawEllipseLines(static_cast<int>(p.x), static_cast<int>(p.y), 10.0f, 3.0f, lit({230, 238, 240, 150}));
                }
            }
        };
        // A broken end: ragged, the steel rods sticking out of it.
        auto broken_end = [&](float v, float lift, float dir, int salt) {
            for (int i = 0; i < 6; ++i) {
                const float across = -0.8f + 0.32f * static_cast<float>(i);
                const Vector2 p = at(across, v, lift - 1.0f - 3.0f * rnd(salt + i));
                const Vector2 q = at(across + (rnd(salt + 10 + i) - 0.5f) * 0.2f, v + dir * (0.12f + 0.15f * rnd(salt + 20 + i)),
                                     lift - 2.0f + 3.0f * rnd(salt + 30 + i));
                DrawLineV(p, q, lit({70, 58, 50, 255}));
            }
            for (int i = 0; i < 4; ++i) disc(at(-0.7f + 0.45f * static_cast<float>(i), v, lift - kBeam * rnd(salt + 40 + i)), 1.4f, lit(shade(concrete, 0.7f)));
        };
        // How high the deck stands over the water at v (pixels).
        auto freeboard = [&](float v) {
            return water_y(0.0f, v) - iso::project(ground_of(d.middle(v), v), d.height).y;
        };
        if (f.way == 0) {
            // Folded in two: each half hinged at its bank, its broken end down in the river.
            const float t = std::min(1.0f, f.age / 0.95f);
            const float down = (freeboard(f.cut) + 6.0f) * t * t;  // (its broken ends just under)
            const float gap = 0.06f;
            piece(d.v0, f.cut - gap, [&](float v) { return -down * (v - d.v0) / std::max(0.01f, f.cut - d.v0); });
            piece(f.cut + gap, d.v1, [&](float v) { return -down * (d.v1 - v) / std::max(0.01f, d.v1 - f.cut); });
            if (t >= 1.0f) {
                for (int i = 0; i < 7; ++i) {  // bits of it in the water by the break
                    const Vector2 p = at(-0.9f + 1.8f * rnd(50 + i), f.cut + (rnd(60 + i) - 0.5f) * 0.9f, -100.0f);
                    DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), 2.5f + 2.0f * rnd(70 + i), 1.2f + rnd(80 + i), lit(shade(concrete, 0.6f + 0.3f * rnd(90 + i))));
                }
            }
        } else {
            // Its middle down, two stumps left.
            piece(d.v0, f.cut, [](float) { return 0.0f; });
            piece(f.cut2, d.v1, [](float) { return 0.0f; });
            broken_end(f.cut, 0.0f, 1.0f, 100);
            broken_end(f.cut2, 0.0f, -1.0f, 200);
            const float t = std::min(1.0f, f.age / 0.6f);
            const float span = f.cut2 - f.cut;
            if (t < 1.0f) {  // falling, whole, a little askew
                piece(f.cut + 0.08f, f.cut2 - 0.08f, [&](float v) { return -(freeboard(v) + kBeam + 12.0f) * t * t - 6.0f * t * (v - f.cut) / span; });
            } else {  // in the river: three slabs, each with one end up out of the water, the other under it
                for (int k = 0; k < 3; ++k) {
                    const float a = f.cut + span * (static_cast<float>(k) + 0.08f) / 3.0f;
                    const float b = f.cut + span * (static_cast<float>(k) + 0.92f) / 3.0f;
                    const bool up_front = rnd(300 + k) < 0.5f;
                    const float stick = 5.0f + 6.0f * rnd(310 + k);
                    piece(a, b, [&](float v) {
                        const float w = (v - a) / std::max(0.01f, b - a);
                        return -freeboard(v) - 8.0f + (stick + 8.0f) * (up_front ? 1.0f - w : w);
                    });
                }
            }
            // The piers under the middle, broken off above the water.
            const int spans = std::max(2, static_cast<int>((d.v1 - d.v0) / 1.1f));
            for (int k = 1; k < spans; ++k) {
                const float v = d.v0 + (d.v1 - d.v0) * static_cast<float>(k) / static_cast<float>(spans);
                if (v <= f.cut + 0.1f || v >= f.cut2 - 0.1f) continue;
                const Vector2 foot = at(1.0f, v, -100.0f);
                const float top = foot.y - 5.0f - 5.0f * rnd(400 + k);
                fill_quad({foot.x - 5.0f, top + 1.5f}, {foot.x + 5.0f, top}, {foot.x + 5.0f, foot.y}, {foot.x - 5.0f, foot.y}, shade(concrete, 0.55f));
                fill_quad({foot.x - 5.0f, top + 1.5f}, {foot.x - 1.5f, top + 0.8f}, {foot.x - 1.5f, foot.y}, {foot.x - 5.0f, foot.y}, shade(concrete, 0.8f));
                DrawEllipse(static_cast<int>(foot.x + 1.5f), static_cast<int>(foot.y), 9.0f, 2.6f, lit({214, 224, 226, 150}));
            }
        }
        set_grain({});
    }
    (void)map;
    g_light = 1.0f;
}

void WorldRenderer::draw_bridges(const engine::World& world) const {
    const engine::TileMap& map = world.map();
    for (const auto& [id, d] : bridge_decks_) {
        const engine::Structure* s = world.find_structure(id);
        if (!s) continue;
        const engine::TilePos mid = s->tiles[s->tiles.size() / 2];
        const int state = fog(world, mid.x, mid.y);
        if (state == kUnexplored) continue;
        g_light = state == kInView ? 1.0f : kFogLight;
        const float damage =
            std::clamp(1.0f - static_cast<float>(s->hp) / static_cast<float>(engine::structure_type(s->type).max_hp), 0.0f, 1.0f);
        const uint32_t h = tile_hash(mid.x, mid.y);
        auto rnd = [h](int i) { return hash_unit(tile_hash(static_cast<int>(h >> 6) + i * 19, i * 7 + 11)); };
        auto ground_of = [](float u, float v) { return Vector2{(u - v) * 0.5f, (u + v) * 0.5f}; };
        // A point on the deck: `across` -1 at its far edge, 1 at its near one; `out` beyond that, in u.
        auto at = [&](float across, float v, float lift = 0.0f, float out = 0.0f) {
            Vector2 p = iso::project(ground_of(d.middle(v) + across * d.half + out, v), d.height);
            p.y -= lift;
            return p;
        };
        auto ground_px = [&](float across, float v, float out) {  // the ground itself there, not the deck
            const Vector2 g = ground_of(d.middle(v) + across * d.half + out, v);
            const int tx = static_cast<int>(std::floor(g.x));
            const int ty = static_cast<int>(std::floor(g.y));
            float hgt = 0.0f;
            if (tx >= 0 && ty >= 0 && tx < cache_width_ && ty < cache_height_) {
                const float fx = g.x - static_cast<float>(tx);
                const float fy = g.y - static_cast<float>(ty);
                const float top = corner(tx, ty) * (1 - fx) + corner(tx + 1, ty) * fx;
                const float bottom = corner(tx, ty + 1) * (1 - fx) + corner(tx + 1, ty + 1) * fx;
                hgt = top * (1 - fy) + bottom * fy;
            }
            return iso::project(g, hgt);
        };
        auto over_water = [&](float v) {
            const Vector2 g = ground_of(d.middle(v) + d.half + 0.3f, v);
            const int tx = static_cast<int>(std::floor(g.x));
            const int ty = static_cast<int>(std::floor(g.y));
            if (!map.contains_tile(tx, ty)) return false;
            const engine::Terrain t = map.terrain(tx, ty);
            return t == engine::Terrain::Water || t == engine::Terrain::Bridge;
        };
        constexpr float kBeam = 11.0f;    // the beam under the deck's edge, pixels
        constexpr float kParapet = 5.0f;  // the parapets' height
        const Color concrete{168, 164, 154, 255};
        const float length = d.v1 - d.v0;
        const int spans = std::max(2, static_cast<int>(length / 1.1f));
        auto span_at = [&](int k) { return d.v0 + length * static_cast<float>(k) / static_cast<float>(spans); };
        constexpr int kSteps = 40;
        auto step_v = [&](int k) { return d.v0 + length * static_cast<float>(k) / kSteps; };

        // Its shadow on the water right under it, a little to the lower right.
        for (int k = 0; k < kSteps; ++k) {
            const float va = step_v(k);
            const float vb = step_v(k + 1);
            if (!over_water(va) || !over_water(vb)) continue;
            auto water = [&](float v, float across) {
                Vector2 p = iso::project(ground_of(d.middle(v) + across * d.half, v), 0.0f);
                p.x += 7.0f;
                return p;
            };
            fill_quad(water(va, -0.2f), water(vb, -0.2f), water(vb, 1.15f), water(va, 1.15f), {6, 14, 24, 90});
        }
        // The piers in the river under its near side: a cap, a column, the waterline, foam.
        for (int k = 1; k < spans; ++k) {
            const float v = span_at(k);
            if (!over_water(v - 0.3f) || !over_water(v + 0.3f)) continue;
            const Vector2 cap = at(1.0f, v, -kBeam, -0.15f);
            const Vector2 foot = ground_px(1.0f, v, -0.15f);
            if (foot.y <= cap.y + 2.0f) continue;
            fill_quad({cap.x - 5.0f, cap.y}, {cap.x + 5.0f, cap.y}, {foot.x + 5.0f, foot.y}, {foot.x - 5.0f, foot.y}, shade(concrete, 0.55f));
            fill_quad({cap.x - 5.0f, cap.y}, {cap.x - 1.5f, cap.y}, {foot.x - 1.5f, foot.y}, {foot.x - 5.0f, foot.y}, shade(concrete, 0.8f));
            DrawLineEx({foot.x - 5.0f, foot.y - 3.0f}, {foot.x + 5.0f, foot.y - 3.0f}, 1.5f, lit({66, 80, 66, 255}));
            DrawEllipse(static_cast<int>(foot.x + 1.5f), static_cast<int>(foot.y), 9.0f, 2.6f, lit({214, 224, 226, 150}));
        }
        // The beam along its near side, down to the ground where there's ground under it.
        for (int k = 0; k < kSteps; ++k) {
            const float va = step_v(k);
            const float vb = step_v(k + 1);
            const Vector2 ta = at(1.0f, va);
            const Vector2 tb = at(1.0f, vb);
            const float ba = std::min(ground_px(1.0f, va, 0.05f).y, ta.y + kBeam);
            const float bb = std::min(ground_px(1.0f, vb, 0.05f).y, tb.y + kBeam);
            if (ba <= ta.y + 0.5f && bb <= tb.y + 0.5f) continue;  // buried in the bank
            fill_quad(ta, tb, {tb.x, std::max(bb, tb.y)}, {ta.x, std::max(ba, ta.y)}, shade(concrete, 0.68f));
        }
        for (int k = 1; k < spans; ++k) {
            const float v = span_at(k);
            if (!over_water(v)) continue;
            DrawLineEx(at(1.0f, v, -2.5f), at(1.0f, v, -kBeam), 1.5f, lit(shade(concrete, 0.55f)));  // the joints
        }

        set_grain(grain_of(engine::Terrain::Road));
        // The deck: slabs like the highway's, seams across, the joints over the piers.
        fill_quad(at(-1.0f, d.v0), at(-1.0f, d.v1), at(1.0f, d.v1), at(1.0f, d.v0),
                  shade(theme::terrain_color(engine::Terrain::Road), 1.0f - 0.2f * damage));
        DrawLineEx(at(1.0f, d.v0, -1.2f), at(1.0f, d.v1, -1.2f), 2.4f, lit(shade(concrete, 0.95f)));  // the cornice
        for (float v = d.v0 + 0.5f; v < d.v1 - 0.2f; v += 0.5f) DrawLineV(at(-0.85f, v), at(0.85f, v), lit({112, 112, 110, 255}));
        for (int k = 1; k < spans; ++k) DrawLineEx(at(-0.85f, span_at(k)), at(0.85f, span_at(k)), 1.5f, lit({60, 60, 60, 255}));
        // Cracks, and holes once it's been hit.
        const int marks = 2 + static_cast<int>(damage * 10.0f);
        for (int k = 0; k < marks; ++k) {
            const Vector2 p = at(-0.7f + 1.4f * rnd(k), d.v0 + 0.4f + (length - 0.8f) * rnd(k + 40));
            if (k < 2) {
                Vector2 q = p;
                for (int j = 0; j < 4; ++j) {
                    const Vector2 r{q.x + (rnd(k * 5 + j) - 0.5f) * 10.0f, q.y + (rnd(k * 5 + j + 20) - 0.5f) * 4.0f};
                    DrawLineV(q, r, lit({70, 70, 70, 255}));
                    q = r;
                }
            } else {
                const float r = 2.0f + 2.5f * rnd(k + 60);
                DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), r * 1.7f, r * 0.8f, lit({120, 116, 108, 255}));
                DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y + 0.4f), r * 1.3f, r * 0.55f, lit({30, 28, 26, 255}));
            }
        }
        set_grain(kObjectGrain);
        // The parapets: solid concrete, a lit top; the near one broken in a stretch once it's been hit.
        const float gap = damage > 0.25f ? d.v0 + length * (0.25f + 0.5f * rnd(200)) : -1.0e6f;
        const float gap_len = 0.4f + damage * 1.0f;
        auto parapet = [&](float across, float va, float vb, float k) {
            fill_quad(at(across, va), at(across, vb), at(across, vb, kParapet), at(across, va, kParapet), shade(concrete, k));
            fill_quad(at(across, va, kParapet), at(across, vb, kParapet), at(across, vb, kParapet, -0.14f), at(across, va, kParapet, -0.14f),
                      shade(concrete, 1.08f));
            for (float v = va + 0.4f; v < vb - 0.1f; v += 0.8f) DrawLineV(at(across, v), at(across, v, kParapet), lit(shade(concrete, k * 0.8f)));
        };
        // Posts at the ends of the parapets, a little taller, where the bridge meets the road.
        auto post = [&](float across, float v) {
            const Vector2 b = at(across, v);
            fill_quad({b.x - 3.5f, b.y}, {b.x + 3.5f, b.y}, {b.x + 3.5f, b.y - kParapet - 3.0f}, {b.x - 3.5f, b.y - kParapet - 3.0f},
                      shade(concrete, 0.8f));
            fill_quad({b.x - 3.5f, b.y}, {b.x, b.y}, {b.x, b.y - kParapet - 3.0f}, {b.x - 3.5f, b.y - kParapet - 3.0f}, shade(concrete, 1.0f));
            fill_quad({b.x - 3.5f, b.y - kParapet - 3.0f}, {b.x + 3.5f, b.y - kParapet - 3.0f}, {b.x + 2.5f, b.y - kParapet - 4.5f},
                      {b.x - 4.5f, b.y - kParapet - 4.5f}, shade(concrete, 1.12f));
        };
        post(-1.0f, d.v0 + 0.1f);
        post(-1.0f, d.v1 - 0.1f);
        parapet(-1.0f, d.v0, d.v1, 0.9f);  // the far one: its inner face towards us
        if (gap > d.v0) {
            parapet(1.0f, d.v0, gap, 0.76f);
            parapet(1.0f, std::min(d.v1, gap + gap_len), d.v1, 0.76f);
            for (int k = 0; k < 4; ++k) disc(at(0.7f - 0.3f * rnd(300 + k), gap + gap_len * rnd(310 + k)), 1.5f + rnd(320 + k), shade(concrete, 0.8f));
        } else {
            parapet(1.0f, d.v0, d.v1, 0.76f);
        }
        post(1.0f, d.v0 + 0.1f);
        post(1.0f, d.v1 - 0.1f);
        set_grain({});
    }
    g_light = 1.0f;
}

// A ragged patch of ground: an ellipse `rx` along `dir` by `ry` across it,
// its edge wobbling by `rag`, laid on the terrain.
void ground_blob(const engine::TileMap& map, Vector2 c, float rx, float ry, Vector2 dir, Color color, uint32_t seed,
                 float rag) {
    constexpr int kPoints = 14;
    Vector2 ring[kPoints];
    for (int i = 0; i < kPoints; ++i) {
        const float a = static_cast<float>(i) * 6.2831853f / kPoints;
        const float k = 1.0f + rag * (hash_unit(tile_hash(static_cast<int>(seed & 0xFFFF) + i * 7, i * 13 + 5)) - 0.5f) * 2.0f;
        const float lx = std::cos(a) * rx * k;
        const float ly = std::sin(a) * ry * k;
        ring[i] = on_terrain(map, {c.x + dir.x * lx - dir.y * ly, c.y + dir.y * lx + dir.x * ly});
    }
    const Vector2 mid = on_terrain(map, c);
    for (int i = 0; i < kPoints; ++i) fill_triangle(mid, ring[i], ring[(i + 1) % kPoints], color);
}

// A shell crater, by what made it. A mortar bomb's: small and shallow in a
// star of streaks. A 122 mm shell's: a deep round bowl in a lip of thrown-up
// earth, clods all round. A rocket's: long along its flight, the furrow it
// ploughed coming in pointing back at the launcher, the earth thrown out to
// the sides; now and then the rocket's tail sticking out of it, pointing the
// same way. A heavy one's: wide, a terrace in its sides. The bowl is in
// shadow on its upper left and lit on its lower right. Fresh: raw dark
// earth, the bottom scorched, smoke curling up for a while; over the minutes
// the earth dries paler and the grass creeps back over the lip; an old deep
// one holds rainwater now and then. On a road, broken concrete instead of clods.
void WorldRenderer::draw_crater(const engine::World& world, int tx, int ty) const {
    using engine::CraterKind;
    const engine::TileMap& map = world.map();
    const size_t index = static_cast<size_t>(ty * map.width() + tx);
    const uint32_t h = tile_hash(tx * 3 + 1, ty * 5 + 2);
    auto rnd = [h](int i) { return hash_unit(tile_hash(static_cast<int>(h >> 8) + i * 17, i * 29 + 3)); };
    CraterKind kind = map.crater_kind(tx, ty);
    if (kind == CraterKind::None) kind = CraterKind::Shell;
    const float angle = static_cast<float>(map.crater_from(tx, ty)) * 0.7853982f + (rnd(0) - 0.5f) * 0.3f;
    const Vector2 from{std::cos(angle), std::sin(angle)};  // towards the gun
    const Vector2 c{static_cast<float>(tx) + 0.5f + (rnd(1) - 0.5f) * 0.14f, static_cast<float>(ty) + 0.5f + (rnd(2) - 0.5f) * 0.14f};
    const float age = index < crater_born_.size() ? static_cast<float>(GetTime()) - crater_born_[index] : 1.0e6f;
    const float old = std::clamp((age - 60.0f) / 400.0f, 0.0f, 1.0f);
    auto road_at = [&](int x, int y) {
        if (!map.contains_tile(x, y)) return false;
        const engine::Terrain t = seen_terrain_[static_cast<size_t>(y * map.width() + x)];
        return t == engine::Terrain::Road || t == engine::Terrain::Bridge;
    };
    const bool road = road_at(tx - 1, ty) || road_at(tx + 1, ty) || road_at(tx, ty - 1) || road_at(tx, ty + 1);

    float r = 0.34f;
    float stretch = 1.0f;
    int clods = 12;
    switch (kind) {
        case CraterKind::Small: r = 0.2f; clods = 6; break;
        case CraterKind::Rocket: r = 0.27f; stretch = 1.5f; clods = 12; break;
        case CraterKind::Heavy: r = 0.5f; clods = 20; break;
        default: break;
    }
    const Vector2 dir = kind == CraterKind::Rocket ? from : Vector2{1.0f, 0.0f};
    const float rx = r * stretch;
    const float ry = kind == CraterKind::Rocket ? r * 0.85f : r;
    const Color soil = mix({86, 68, 50, 255}, {120, 106, 84, 255}, old);
    const Color clod = mix({62, 48, 36, 255}, {104, 92, 72, 255}, old);
    const Color bowl = mix({52, 42, 33, 255}, {84, 72, 57, 255}, old);
    const Color wall = mix({102, 84, 62, 255}, {128, 114, 90, 255}, old);
    const Color deep = mix({30, 25, 20, 255}, {64, 54, 44, 255}, old);
    set_grain(grain_of(engine::Terrain::Crater));

    // What it threw out.
    if (kind == CraterKind::Small) {
        for (int k = 0; k < 9; ++k) {
            const float a = static_cast<float>(k) * 0.698f + (rnd(10 + k) - 0.5f) * 0.4f;
            const Vector2 u{std::cos(a), std::sin(a)};
            const float reach = r * (1.7f + 0.8f * rnd(20 + k));
            DrawLineEx(on_terrain(map, {c.x + u.x * r * 0.7f, c.y + u.y * r * 0.7f}),
                       on_terrain(map, {c.x + u.x * reach, c.y + u.y * reach}), 1.3f, lit(clod));
        }
    }
    for (int k = 0; k < clods; ++k) {
        float a = rnd(30 + k) * 6.2831853f;
        if (kind == CraterKind::Rocket) {
            // Out to the sides, like wings.
            const float side = k % 2 == 0 ? 1.5708f : -1.5708f;
            a = angle + side + (rnd(30 + k) - 0.5f) * 1.2f;
        }
        const float reach = r * (1.2f + 1.1f * rnd(50 + k));
        const Vector2 p = on_terrain(map, {c.x + std::cos(a) * reach, c.y + std::sin(a) * reach});
        const float size = (kind == CraterKind::Heavy ? 1.6f : 1.1f) + 1.2f * rnd(70 + k);
        if (road) {
            const Color slab = shade({132, 130, 124, 255}, 0.85f + 0.3f * rnd(90 + k));
            fill_quad({p.x - size, p.y - size * 0.3f}, {p.x + size * 0.4f, p.y - size * 0.8f}, {p.x + size, p.y + size * 0.2f},
                      {p.x - size * 0.3f, p.y + size * 0.6f}, slab);
        } else {
            disc(p, size, clod);
            disc({p.x - size * 0.3f, p.y - size * 0.35f}, size * 0.45f, shade(clod, 1.3f));
        }
    }
    // The lip, a heavy one's terrace, a rocket's furrow, the bowl lit on the far side, its bottom.
    ground_blob(map, c, rx * 1.4f, ry * 1.4f, dir, soil, h, 0.25f);
    if (kind == CraterKind::Rocket) {
        ground_blob(map, {c.x + from.x * rx * 0.95f, c.y + from.y * rx * 0.95f}, rx * 0.6f, ry * 0.32f, from, bowl, h + 5, 0.15f);
    }
    if (kind == CraterKind::Heavy) ground_blob(map, c, rx * 1.15f, ry * 1.15f, dir, mix(soil, bowl, 0.5f), h + 7, 0.18f);
    ground_blob(map, c, rx, ry, dir, bowl, h + 1, 0.15f);
    ground_blob(map, {c.x + r * 0.12f, c.y + r * 0.12f}, rx * 0.7f, ry * 0.7f, dir, wall, h + 2, 0.14f);
    ground_blob(map, {c.x - r * 0.06f, c.y - r * 0.06f}, rx * 0.52f, ry * 0.52f, dir, deep, h + 3, 0.14f);
    if (old < 1.0f) {
        // Scorched black at the bottom while it's fresh.
        ground_blob(map, {c.x - r * 0.05f, c.y - r * 0.05f}, rx * 0.4f, ry * 0.4f, dir,
                    ColorAlpha({20, 17, 15, 255}, 0.85f * (1.0f - old)), h + 4, 0.2f);
    } else if (kind != CraterKind::Small && kind != CraterKind::Rocket && h % 3 == 0) {
        // Rainwater in an old deep one.
        ground_blob(map, {c.x - r * 0.05f, c.y - r * 0.05f}, rx * 0.46f, ry * 0.42f, dir, {70, 90, 98, 255}, h + 6, 0.1f);
        const Vector2 glint = on_terrain(map, {c.x - r * 0.18f, c.y - r * 0.1f});
        DrawLineEx(glint, {glint.x + 4.0f, glint.y}, 1.0f, lit({150, 170, 176, 255}));
    }
    // The grass creeping back over the lip.
    const int tufts = static_cast<int>(old * 7.0f);
    for (int k = 0; k < tufts; ++k) {
        const float a = rnd(110 + k) * 6.2831853f;
        const Vector2 p = on_terrain(map, {c.x + std::cos(a) * rx * 1.25f, c.y + std::sin(a) * ry * 1.25f});
        for (int side = -1; side <= 1; ++side) {
            DrawLineV(p, {p.x + static_cast<float>(side) * 1.6f, p.y - (side == 0 ? 4.0f : 2.8f)}, lit({88, 116, 56, 255}));
        }
    }
    // A rocket's tail sticking out, pointing back along its flight.
    if (kind == CraterKind::Rocket && h % 3 == 0) {
        const Vector2 base = on_terrain(map, {c.x + from.x * 0.03f, c.y + from.y * 0.03f}, 1.0f);
        const Vector2 end = on_terrain(map, {c.x + from.x * 0.16f, c.y + from.y * 0.16f}, 10.0f);
        DrawLineEx(base, end, 3.2f, lit({64, 66, 60, 255}));
        DrawLineEx({base.x - 1.0f, base.y}, {end.x - 1.0f, end.y}, 1.0f, lit({110, 112, 104, 255}));
        const Vector2 side{-(end.y - base.y), end.x - base.x};
        const float len = std::max(1.0f, std::sqrt(side.x * side.x + side.y * side.y));
        for (const float s : {-1.0f, 1.0f}) {
            DrawLineEx(end, {end.x + side.x / len * 3.0f * s, end.y + side.y / len * 3.0f * s + 1.5f}, 1.2f, lit({54, 56, 52, 255}));
        }
        disc(end, 1.6f, {36, 36, 34, 255});
    }
    set_grain({});
    // Fresh: smoke curling up for half a minute.
    if (age < 30.0f) {
        const float fade = 1.0f - age / 30.0f;
        for (int k = 0; k < 3; ++k) {
            const float t = std::fmod(age * 0.5f + static_cast<float>(k) * 0.33f, 1.0f);
            const Vector2 p = on_terrain(map, c, 3.0f + t * 30.0f);
            DrawCircleV({p.x + t * 8.0f, p.y}, (2.5f + t * 7.0f) * (0.6f + r), ColorAlpha({70, 66, 62, 255}, (1.0f - t) * 0.45f * fade));
        }
    }
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
        if (drawn_as_armor(engine::unit_type(r.type)) || truck_model(r.type, r.owner) || gun_or_plane(engine::unit_type(r.type))) {
            continue;  // drawn whole, with the others (draw_wreck)
        }
        const float lifetime = r.vehicle ? kWreckLifetime : kBodyLifetime;
        const float fade = std::clamp((lifetime - r.age) / 3.0f, 0.0f, 1.0f);  // fade out over the last 3 s
        const Vector2 p = on_terrain(map, r.ground);
        if (r.vehicle) {
            draw_scorch(p, 0, false, fade, r.seed);
        } else {
            // He falls: reeling, on his knees, down on his back; blood under him spreading.
            const float spread = std::min(1.0f, r.age / 4.0f);
            const Vector2 under{r.ground.x - r.facing.x * 0.12f, r.ground.y - r.facing.y * 0.12f};
            if (r.age > 0.4f) fill_ground_ellipse(on_terrain(map, under), 0.05f + 0.1f * spread, ColorAlpha({74, 26, 22, 255}, 0.75f * fade));
            const int f = r.age < 0.12f ? 0 : r.age < 0.3f ? 1 : r.age < 0.42f ? 2 : 3;
            draw_sprite(soldier_sheet(kit_of(r.type), r.owner), p, r.facing, soldiers::first_frame(soldiers::Pose::Die) + f,
                        ColorAlpha(WHITE, fade));
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
    // Its queued orders (Shift, as in AoE II): a dotted way from where this
    // one ends to each in turn, a little flag at each: green to go, red to
    // fight, gold to work.
    if (u.queued.empty()) return;
    auto where = [&](const engine::Command& c) -> std::optional<Vector2> {
        if (c.target_unit != 0) {
            if (const engine::Structure* s = world.find_structure(c.target_unit)) return to_vector2(s->center);
            if (const engine::Unit* t = world.find_unit(c.target_unit); t && shows(world, *t)) return unit_ground_pos(*t, alpha);
        }
        if (c.type == engine::CommandType::Unload || c.type == engine::CommandType::Retrain) return std::nullopt;
        return to_vector2(c.target);
    };
    Vector2 last = unit_ground_pos(u, alpha);
    if (u.order == engine::Order::Move || u.order == engine::Order::AttackMove || u.order == engine::Order::AttackGround ||
        u.order == engine::Order::Observe || u.order == engine::Order::Collect || u.order == engine::Order::Ability) {
        last = to_vector2(u.order_point);
    } else if (const engine::Structure* s = world.find_structure(u.order_target);
               s && (u.order == engine::Order::Build || u.order == engine::Order::Garrison)) {
        last = to_vector2(s->center);
    }
    int n = 0;
    for (const engine::Unit::Queued& q : u.queued) {
        const std::optional<Vector2> at = where(q.cmd);
        if (!at) continue;
        const bool fight = q.cmd.type == engine::CommandType::Attack || q.cmd.type == engine::CommandType::AttackMove ||
                           q.cmd.type == engine::CommandType::AttackGround;
        const bool work = q.cmd.type == engine::CommandType::Build || q.cmd.type == engine::CommandType::Gather ||
                          q.cmd.type == engine::CommandType::Collect || q.cmd.type == engine::CommandType::Haul;
        const Color color = fight ? theme::kDanger : work ? Color{232, 190, 70, 255} : theme::kSelection;
        const float len = std::hypot(at->x - last.x, at->y - last.y);
        for (float t = 0.0f; t < len; t += 0.5f) {
            const Vector2 a = lerp(last, *at, t / std::max(0.01f, len));
            const Vector2 b = lerp(last, *at, std::min(len, t + 0.25f) / std::max(0.01f, len));
            DrawLineV(on_terrain(map, a), on_terrain(map, b), ColorAlpha(color, 0.55f));
        }
        const Vector2 foot = on_terrain(map, *at);
        DrawLineEx(foot, {foot.x, foot.y - 12.0f}, 1.4f, {60, 50, 40, 255});  // the flag
        DrawTriangle({foot.x + 0.5f, foot.y - 12.0f}, {foot.x + 0.5f, foot.y - 7.0f}, {foot.x + 7.0f, foot.y - 9.5f}, color);
        DrawText(TextFormat("%d", ++n), static_cast<int>(foot.x - 7.0f), static_cast<int>(foot.y - 13.0f), 8, {240, 240, 234, 255});
        last = *at;
    }
}

// An attack aircraft from above: swept wings, a long nose, twin engines.
// Up in the air its shadow is on the ground below.
void WorldRenderer::draw_aircraft(const engine::TileMap& map, const engine::Unit& u, float alpha) const {
    const Vector2 ground = unit_ground_pos(u, alpha);
    const Vector2 f = unit_facing(u);
    const engine::UnitTypeDef& def = engine::unit_type(u.type);
    if (has_plane_look(def.model)) {
        if (const SpriteSheet* plane = sheet(SpritePart::Plane, small_variant(def.model, wear_of(u.hp, def.max_hp)), u.owner)) {
            if (u.airborne) draw_sprite(*plane, on_terrain(map, ground), f, 0, {0, 0, 0, 70});  // its shadow
            draw_sprite(*plane, on_terrain(map, ground, flight_lift(u)), f, 0);
            return;
        }
    }
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
    shape(u.airborne ? flight_lift(u) : 4.0f, color, shade(color, 0.8f));
}

void WorldRenderer::draw_unit(const engine::TileMap& map, const engine::Unit& u, float alpha) const {
    const engine::UnitTypeDef& def = engine::unit_type(u.type);
    if (def.aircraft) return draw_aircraft(map, u, alpha);
    const Vector2 ground = unit_ground_pos(u, alpha);
    const Vector2 feet = on_terrain(map, ground);
    const Vector2 facing = unit_facing(u);
    if (!def.vehicle && drawn_world_) {
        // In his post: inside the stump or the hide (only it shows); up a tree, on its platform (drawn with it).
        if (drawn_world_->post_of(u)) return;  // (drawn with his post)
        // Up a cell tower's mast with an aerial: climbing, then at work up top.
        if (u.order == engine::Order::Ability && u.order_ability == engine::AbilityId::MountAntenna && u.work > 0) {
            const engine::TilePos t = map.clamp_tile({static_cast<int32_t>(std::floor(to_float(u.order_point.x))),
                                                      static_cast<int32_t>(std::floor(to_float(u.order_point.y)))});
            const Vector2 mast{static_cast<float>(t.x) + 0.5f, static_cast<float>(t.y) + 0.5f};
            const Vector2 off{ground.x - mast.x, ground.y - mast.y};
            const float ol = std::max(0.01f, std::hypot(off.x, off.y));
            const float up = std::min(1.0f, static_cast<float>(u.work) / static_cast<float>(engine::kAntennaWork) * 1.4f);
            const Vector2 at{mast.x + off.x / ol * 0.18f, mast.y + off.y / ol * 0.18f};
            return draw_soldier(u, on_terrain(map, at, up * 74.0f * kTowerScale), {-off.x / ol, -off.y / ol});
        }
    }

    fill_ground_ellipse(feet, to_float(def.radius) * 0.9f, {0, 0, 0, 70});  // shadow
    if (def.vehicle) {
        draw_vehicle(map, u, ground, facing);
    } else {
        draw_soldier(u, feet, facing);
    }
}

// A foot soldier: which of our pixel-art kits he wears (see soldiers.h).
soldiers::Kit kit_of(engine::UnitTypeId type) {
    using engine::UnitTypeId;
    using soldiers::Kit;
    switch (type) {
        case UnitTypeId::MachineGunner: return Kit::MachineGun;
        case UnitTypeId::Grenadier: return Kit::Rpg;
        case UnitTypeId::Scout: return Kit::Sniper;
        case UnitTypeId::Assault: return Kit::Assault;
        case UnitTypeId::Sapper: return Kit::Sapper;
        case UnitTypeId::Signaler: return Kit::Radio;
        case UnitTypeId::Manpads: return Kit::Igla;
        case UnitTypeId::Worker: return Kit::Rear;
        case UnitTypeId::Mortar: return Kit::Mortar;
        case UnitTypeId::Ags: return Kit::Ags;
        default: return Kit::Rifle;
    }
}

const WorldRenderer::SpriteSheet& WorldRenderer::soldier_sheet(soldiers::Kit kit, engine::PlayerId owner) const {
    const std::pair<int, int> key{static_cast<int>(kit), static_cast<int>(owner)};
    if (const auto it = soldier_sheets_.find(key); it != soldier_sheets_.end()) return it->second;
    Image img = soldiers::bake(kit, theme::player_color(owner));
    if (const char* dump = dump_dir()) ExportImage(img, TextFormat("%s/soldier_%d_%d.png", dump, key.first, key.second));
    SpriteSheet s;
    s.atlas = LoadTextureFromImage(img);
    SetTextureFilter(s.atlas, TEXTURE_FILTER_POINT);
    s.w = soldiers::kCellW;
    s.h = soldiers::kCellH;
    s.dirs = soldiers::kDirs;
    s.frames = soldiers::total_frames();
    s.origin = {soldiers::kOriginX, soldiers::kOriginY};
    UnloadImage(img);
    return soldier_sheets_.emplace(key, s).first->second;
}

// What he's doing, as drawn: walking; at work (an axe, a spade); at the
// weapon set up in front of him; throwing; flinching from a hit; in a fight
// (a machine gunner, a scout lying down, an assault trooper standing, the
// rest on a knee; the kick of each shot; an RPG, an Igla reloaded); sitting
// down after a long while standing about; at ease.
// As he's drawn: down in a trench he fires standing at its breastwork (not
// lying or kneeling), and doesn't sit down in it.
std::pair<soldiers::Pose, int> WorldRenderer::soldier_pose(const engine::Unit& u) const {
    auto [pose, frame] = soldier_pose_raw(u);
    if (sink_of(u.id) > 3.0f) {
        if (pose == soldiers::Pose::Prone || pose == soldiers::Pose::Kneel) pose = soldiers::Pose::Aim;
        if (pose == soldiers::Pose::Sit) return {soldiers::Pose::Stand, 0};
    }
    return {pose, frame};
}

float WorldRenderer::sink_of(engine::EntityId id) const {
    const auto it = settle_.find(id);
    return it != settle_.end() ? it->second.sink : 0.0f;
}

std::pair<soldiers::Pose, int> WorldRenderer::soldier_pose_raw(const engine::Unit& u) const {
    using engine::AbilityId;
    using engine::Order;
    using engine::UnitTypeId;
    using soldiers::Kit;
    using soldiers::Pose;
    const double now = GetTime();
    auto since = [&](const std::unordered_map<engine::EntityId, double>& m) {
        const auto it = m.find(u.id);
        return it != m.end() ? static_cast<float>(now - it->second) : 99.0f;
    };
    const float shot = since(shot_at_);
    const float thrown = since(thrown_at_);
    const auto hit = hurt_.find(u.id);
    const float hurt = hit != hurt_.end() ? static_cast<float>(now - hit->second.second) : 99.0f;
    const float t = static_cast<float>(now) + static_cast<float>(u.id % 17) * 0.37f;
    const Kit kit = kit_of(u.type);
    if (u.riding) return {Pose::Sit, 0};  // on the armor
    if (u.order == Order::Ability && u.order_ability == AbilityId::MountAntenna && u.work > 0) {
        return {Pose::Climb, u.work < engine::kAntennaWork * 5 / 7 ? static_cast<int>(t * 3.0f) % 2 : 0};  // up the mast, then at work
    }
    if (u.type == UnitTypeId::Worker) {
        // A rear trooper: throwing his load down; at the wood with his axe, at
        // the rock with his pick; carrying a log on his shoulder, stone in a sack.
        const float heave = since(heave_at_);
        if (heave < 0.5f) return {Pose::Heave, heave < 0.16f ? 0 : 1};
        const bool rock = drawn_world_ && drawn_world_->map().contains(u.gather_tile) &&
                          drawn_world_->map().terrain(u.gather_tile) == engine::Terrain::Rock;
        if (u.order == Order::Gather && u.work > 0 && !u.moving) {
            const engine::Tick needed = rock ? engine::kQuarryTicks : engine::kChopTicks;
            const float stroke = static_cast<float>(u.work % needed) / static_cast<float>(needed);
            return {rock ? Pose::Quarry : Pose::Work, stroke < 0.5f ? 0 : 1};
        }
        if (u.carrying > 0) {
            // To a truck by the wood: the whole log, the sack; all the way to the base: an armful of short bars, of blocks.
            const bool truck = drawn_world_ && truck_takes_it(*drawn_world_, u);
            const Pose carry = truck ? (rock ? Pose::CarrySack : Pose::CarryLog) : (rock ? Pose::CarryBlocks : Pose::CarryBars);
            return {carry, u.moving ? static_cast<int>(t * 8.0f) % 8 : 0};
        }
    }
    if (u.moving) return {Pose::Walk, static_cast<int>(t * 9.0f) % soldiers::frame_count(Pose::Walk)};
    const bool digging = u.order == Order::Ability &&
                         (u.order_ability == AbilityId::DigTrench || u.order_ability == AbilityId::DigFoxhole ||
                          u.order_ability == AbilityId::BuildParapet || u.order_ability == AbilityId::DigGunPit ||
                          u.order_ability == AbilityId::LayWire || u.order_ability == AbilityId::PlaceHedgehogs ||
                          u.order_ability == AbilityId::BuildPillbox || u.order_ability == AbilityId::LayApMine ||
                          u.order_ability == AbilityId::LayAtMine || u.order_ability == AbilityId::ClearMines);
    if ((u.type == UnitTypeId::Worker && u.order == Order::Build) || digging || u.order == Order::Fortify) {
        return {Pose::Work, static_cast<int>(t * 2.2f) % 2};
    }
    if ((u.type == UnitTypeId::Mortar || u.type == UnitTypeId::Ags) &&
        (u.deployed || u.engaged != 0 || u.order == Order::AttackGround || shot < 3.0f)) {
        return {Pose::Crew, 0};
    }
    if (thrown < 0.12f) return {Pose::Throw, 1};
    if (thrown < 0.45f) return {Pose::Throw, 2};
    if (u.order == Order::Ability && u.order_ability == AbilityId::ThrowGrenade) return {Pose::Throw, 0};
    if (hurt < 0.3f) return {Pose::Hurt, 0};
    if (u.engaged != 0 || shot < 2.0f || u.order == Order::Observe || u.order == Order::AttackGround) {
        if ((kit == Kit::Rpg || kit == Kit::Igla) && shot > 0.3f && u.cooldown > 0) return {Pose::Reload, static_cast<int>(t * 2.5f) % 2};
        const Pose stance = kit == Kit::MachineGun || kit == Kit::Sniper ? Pose::Prone : kit == Kit::Assault ? Pose::Aim : Pose::Kneel;
        return {stance, shot < 0.08f ? 1 : 0};
    }
    if (u.order == Order::Idle && u.still > 15 * engine::kTicksPerSecond && shot > 20.0f && u.carrying == 0) return {Pose::Sit, 0};
    return {Pose::Stand, static_cast<int>(t * 0.8f) % 2};
}

// Where his weapon's muzzle is as he's drawn now: over the ground (tiles) and up (pixels).
void WorldRenderer::soldier_muzzle(const engine::Unit& u, Vector2 ground, Vector2 facing, Vector2& at, float& z) const {
    const auto [pose, frame] = soldier_pose(u);
    const soldiers::Offset m = soldiers::muzzle(kit_of(u.type), pose, frame);
    const Vector2 left{facing.y, -facing.x};  // (his own left, as he's drawn)
    at = {ground.x + facing.x * m.ahead + left.x * m.left, ground.y + facing.y * m.ahead + left.y * m.left};
    z = m.up - sink_of(u.id);  // (down in a trench)
}

// The men on top of an IFV, an APC: sitting along its deck on both sides,
// legs over the edge, facing out; the nearer ones drawn last.
void WorldRenderer::draw_riders(const engine::World& world, const engine::Unit& carrier, float alpha) const {
    const engine::TileMap& map = world.map();
    const engine::UnitTypeDef& def = engine::unit_type(carrier.type);
    Vector2 f = to_vector2(carrier.hull);
    const float fl = std::hypot(f.x, f.y);
    f = fl > 0.0f ? Vector2{f.x / fl, f.y / fl} : Vector2{1.0f, 0.0f};
    const Vector2 side{-f.y, f.x};
    const Vector2 size = drawn_as_armor(def) ? armor_size(def.model) : Vector2{0.9f, 0.3f};
    const float deck = drawn_as_armor(def) ? armor_engine(def.model).y - kVehicleScale : 12.0f;
    const Vector2 g = unit_ground_pos(carrier, alpha);
    struct Seat {
        const engine::Unit* man;
        Vector2 at;
        Vector2 facing;
    };
    std::vector<Seat> seats;
    for (size_t i = 0; i < carrier.riders.size(); ++i) {
        const engine::Unit* man = world.find_unit(carrier.riders[i]);
        if (!man) continue;
        const float along = (0.14f - 0.27f * static_cast<float>(i / 2)) * size.x * kVehicleScale;
        const float out = (i % 2 == 0 ? 1.0f : -1.0f) * size.y * 0.72f * kVehicleScale;
        seats.push_back({man, {g.x + f.x * along + side.x * out, g.y + f.y * along + side.y * out},
                         {side.x * (out > 0.0f ? 1.0f : -1.0f), side.y * (out > 0.0f ? 1.0f : -1.0f)}});
    }
    std::sort(seats.begin(), seats.end(), [](const Seat& a, const Seat& b) { return a.at.x + a.at.y < b.at.x + b.at.y; });
    for (const Seat& s : seats) draw_soldier(*s.man, on_terrain(map, s.at, deck), s.facing);
}

// A scout's observation post, as he made it: an artificial stump out in the
// open (bark, the cut's rings on top), a hide of branches heaped in the
// crops, the reeds, the rubble; a platform of poles up a tree in a wood,
// its ladder up the trunk, branches tied round. A dark slit towards the
// sector; manned, the glint of his binoculars in it now and then.
void WorldRenderer::draw_post(const engine::TileMap& map, const engine::Structure& s, bool manned) const {
    if (s.tiles.empty()) return;
    const Vector2 c{static_cast<float>(s.tiles.front().x) + 0.5f, static_cast<float>(s.tiles.front().y) + 0.5f};
    Vector2 f = to_vector2(s.facing);
    const float fl = std::hypot(f.x, f.y);
    f = fl > 0.0f ? Vector2{f.x / fl, f.y / fl} : Vector2{1.0f, 0.0f};
    Vector2 fs = iso_offset(f);
    const float sl = std::hypot(fs.x, fs.y);
    fs = sl > 0.0f ? Vector2{fs.x / sl, fs.y / sl} : Vector2{1.0f, 0.0f};
    const Vector2 base = on_terrain(map, c);
    const uint32_t h = s.id * 2654435761u;
    const bool glint = manned && std::fmod(static_cast<float>(GetTime()) + static_cast<float>(s.id % 7), 3.0f) < 0.25f;
    auto slit = [&](Vector2 at) {
        DrawRectangleRec({std::round(at.x - 1.5f), std::round(at.y), 3.0f, 1.0f}, lit({24, 20, 16, 255}));
        if (glint) DrawRectangleRec({std::round(at.x), std::round(at.y), 1.0f, 1.0f}, {226, 236, 255, 255});
    };
    switch (s.post) {
        case engine::PostKind::Stump: {
            constexpr float r = 4.5f;
            constexpr float tall = 10.0f;
            DrawEllipse(static_cast<int>(base.x + 2.0f), static_cast<int>(base.y + 1.0f), r + 2.5f, (r + 2.5f) * 0.45f, lit({0, 0, 0, 60}));
            for (const float side : {-1.0f, 1.0f}) {  // roots
                fill_triangle({base.x + side * (r + 2.5f), base.y + 1.0f}, {base.x + side * (r - 1.0f), base.y - 3.0f},
                              {base.x + side * (r - 1.5f), base.y + 1.5f}, {84, 62, 42, 255});
            }
            DrawEllipse(static_cast<int>(base.x), static_cast<int>(base.y), r, r * 0.5f, lit({80, 60, 42, 255}));
            DrawRectangleRec({base.x - r, base.y - tall, r * 2.0f, tall}, lit({96, 72, 50, 255}));
            DrawRectangleRec({base.x - r, base.y - tall, r * 0.7f, tall}, lit({116, 90, 62, 255}));  // lit on the left
            DrawRectangleRec({base.x + r * 0.45f, base.y - tall, r * 0.55f, tall}, lit({74, 56, 40, 255}));
            for (int i = 0; i < 4; ++i) {  // bark ridges
                const float x = base.x - r + 1.5f + static_cast<float>(i) * 2.2f + static_cast<float>((h >> (i * 3)) & 1u);
                DrawLineV({x, base.y - tall + 2.0f}, {x, base.y - 1.0f}, lit({62, 46, 32, 255}));
            }
            DrawEllipse(static_cast<int>(base.x), static_cast<int>(base.y - tall), r, r * 0.5f, lit({176, 140, 94, 255}));
            DrawEllipseLines(static_cast<int>(base.x), static_cast<int>(base.y - tall), r * 0.6f, r * 0.3f, lit({140, 108, 72, 255}));
            if (fs.y > -0.3f) slit({base.x + fs.x * r * 0.6f, base.y - tall * 0.62f + fs.y * 1.5f});
            break;
        }
        case engine::PostKind::Hide: {
            static constexpr Color kLeaves[4] = {{70, 88, 44, 255}, {94, 108, 56, 255}, {112, 96, 62, 255}, {58, 72, 38, 255}};
            DrawEllipse(static_cast<int>(base.x + 1.0f), static_cast<int>(base.y + 1.0f), 9.0f, 4.0f, lit({0, 0, 0, 60}));
            for (int i = 0; i < 14; ++i) {  // branches heaped into a low dome, the back ones first
                const float a = (static_cast<float>(i) / 14.0f) * 3.14159f;
                const float ring = i % 2 == 0 ? 1.0f : 0.55f;
                const Vector2 p{base.x - std::cos(a) * 7.0f * ring, base.y - 2.0f - std::sin(a) * 5.0f * ring - (1.0f - ring) * 2.0f};
                disc(p, 2.4f + static_cast<float>((h >> i) & 1u), kLeaves[(h >> (i * 2)) % 4]);
            }
            for (int i = 0; i < 3; ++i) {  // twigs sticking out
                const float x = base.x - 5.0f + static_cast<float>(i) * 5.0f;
                DrawLineV({x, base.y - 6.0f}, {x + (i - 1) * 2.0f, base.y - 10.0f}, lit({92, 70, 48, 255}));
            }
            if (fs.y > -0.3f) slit({base.x + fs.x * 4.0f, base.y - 4.0f + fs.y * 1.0f});
            break;
        }
        case engine::PostKind::Tree: {
            // The ladder up the trunk.
            const Vector2 foot{base.x - 3.0f, base.y};
            for (const float dx : {0.0f, 3.0f}) DrawLineV({foot.x + dx, foot.y}, {foot.x + dx, foot.y - kTreePostZ}, lit({126, 96, 60, 255}));
            for (float y = 3.0f; y < kTreePostZ; y += 4.0f) DrawLineV({foot.x, foot.y - y}, {foot.x + 3.0f, foot.y - y}, lit({146, 112, 70, 255}));
            // The platform of poles.
            const Vector2 q[4] = {on_terrain(map, {c.x - 0.2f, c.y - 0.2f}, kTreePostZ), on_terrain(map, {c.x + 0.2f, c.y - 0.2f}, kTreePostZ),
                                  on_terrain(map, {c.x + 0.2f, c.y + 0.2f}, kTreePostZ), on_terrain(map, {c.x - 0.2f, c.y + 0.2f}, kTreePostZ)};
            fill_quad(q[0], q[1], q[2], q[3], {138, 108, 70, 255});
            for (int i = 0; i < 4; ++i) DrawLineV(q[i], q[(i + 1) % 4], lit({92, 70, 46, 255}));
            // Branches tied round its back and sides.
            for (int i = 0; i < 7; ++i) {
                const float a = 3.14159f + static_cast<float>(i) / 6.0f * 3.14159f;
                disc({(q[0].x + q[2].x) * 0.5f + std::cos(a) * 9.0f, (q[0].y + q[2].y) * 0.5f - 2.0f + std::sin(a) * 4.0f}, 2.6f,
                     i % 2 == 0 ? Color{64, 90, 44, 255} : Color{84, 108, 54, 255});
            }
            break;
        }
    }
}

// A village's bus stop, as the collective farms built them: a concrete
// shelter, its back wall a mosaic towards the road (a sun over the wheat,
// the blue sky, a dove), its sides, a bench in it, the flat roof slab over
// it; a kiosk beside some.
void WorldRenderer::draw_bus_stop(const engine::TileMap& map, const BusStop& b) const {
    const Vector2 f = b.facing;
    const Vector2 side{-f.y, f.x};
    const uint32_t h = tile_hash(static_cast<int>(b.ground.x * 7.0f), static_cast<int>(b.ground.y * 7.0f));
    constexpr float kHalf = 0.3f;   // along the road, tiles
    constexpr float kDeep = 0.2f;  // back wall to its sides' ends
    constexpr float kTall = 18.0f;
    constexpr float kRoof = 0.13f;  // how far the roof slab reaches out over the bench
    const Color concrete{190, 186, 176, 255};
    const Vector2 back{b.ground.x - f.x * kDeep * 0.5f, b.ground.y - f.y * kDeep * 0.5f};
    const Vector2 w0{back.x - side.x * kHalf, back.y - side.y * kHalf};
    const Vector2 w1{back.x + side.x * kHalf, back.y + side.y * kHalf};
    auto fwd = [&](Vector2 p, float t) { return Vector2{p.x + f.x * t, p.y + f.y * t}; };
    // Its sides: the one farther from us first, the back wall, the bench, the near side.
    auto side_wall = [&](Vector2 at) {
        const Face s{on_terrain(map, at), on_terrain(map, fwd(at, kDeep * 0.7f))};
        face_fill(s, 0.0f, 1.0f, 0.0f, kTall, shade(concrete, 0.8f));
        face_line(s, 0.0f, kTall, 1.0f, kTall, shade(concrete, 1.1f));
    };
    const bool w0_far = w0.x + w0.y < w1.x + w1.y;
    side_wall(w0_far ? w0 : w1);
    const Face wall{on_terrain(map, w0), on_terrain(map, w1)};
    const bool facing_us = f.x + f.y > 0.0f;  // the mosaic's side seen
    face_fill(wall, 0.0f, 1.0f, 0.0f, kTall, facing_us ? concrete : shade(concrete, 0.78f));
    if (facing_us) {
        const float px = wall.px();
        for (float u = 2.0f * px; u < 1.0f - 2.0f * px; u += 2.0f * px) {
            for (float v = 2.0f; v < kTall - 2.0f; v += 2.0f) {
                const float su = (u - 0.72f) * wall.pixels() / 2.0f;
                const float sv = (v - 12.0f) / 2.0f;
                const float roll = hash_unit(tile_hash(static_cast<int>(u * 97.0f) + static_cast<int>(h % 97u), static_cast<int>(v)));
                Color tile;
                if (su * su + sv * sv < 5.0f) {
                    tile = roll < 0.5f ? Color{250, 206, 60, 255} : Color{236, 150, 44, 255};  // the sun
                } else if (v < 7.0f) {
                    tile = roll < 0.6f ? Color{214, 170, 70, 255} : roll < 0.85f ? Color{190, 140, 50, 255} : Color{110, 150, 70, 255};  // the wheat
                } else {
                    tile = roll < 0.55f ? Color{70, 130, 196, 255} : roll < 0.85f ? Color{104, 160, 214, 255} : Color{236, 236, 230, 255};  // the sky
                }
                face_fill(wall, u, u + 2.0f * px, v, v + 2.0f, tile);
            }
        }
        // A dove in the sky.
        const Vector2 dove = wall.at(0.3f, 11.0f);
        DrawLineV({dove.x - 2.0f, dove.y}, {dove.x + 2.0f, dove.y - 1.0f}, lit({248, 248, 244, 255}));
        DrawLineV({dove.x, dove.y}, {dove.x + 1.0f, dove.y - 2.0f}, lit({248, 248, 244, 255}));
    } else {
        face_line(wall, 0.2f, 3.0f, 0.5f, 5.0f, shade(concrete, 0.6f));  // a stain down it
    }
    // The bench along it, inside.
    const Face bench{on_terrain(map, fwd(w0, 0.07f)), on_terrain(map, fwd(w1, 0.07f))};
    face_fill(bench, 0.08f, 0.92f, 4.0f, 5.5f, {150, 112, 70, 255});
    face_fill(bench, 0.1f, 0.14f, 0.0f, 4.0f, {110, 104, 96, 255});
    face_fill(bench, 0.86f, 0.9f, 0.0f, 4.0f, {110, 104, 96, 255});
    side_wall(w0_far ? w1 : w0);
    // The roof slab, out over the bench.
    const Vector2 r0 = on_terrain(map, {w0.x - side.x * 0.04f, w0.y - side.y * 0.04f}, kTall);
    const Vector2 r1 = on_terrain(map, {w1.x + side.x * 0.04f, w1.y + side.y * 0.04f}, kTall);
    const Vector2 r2 = on_terrain(map, fwd({w1.x + side.x * 0.04f, w1.y + side.y * 0.04f}, kRoof), kTall);
    const Vector2 r3 = on_terrain(map, fwd({w0.x - side.x * 0.04f, w0.y - side.y * 0.04f}, kRoof), kTall);
    fill_quad({r0.x, r0.y + 2.0f}, {r1.x, r1.y + 2.0f}, {r2.x, r2.y + 2.0f}, {r3.x, r3.y + 2.0f}, shade(concrete, 0.7f));
    fill_quad(r0, r1, r2, r3, shade(concrete, 1.08f));
    if (!b.kiosk) return;
    // A kiosk beside it: a blue and white booth, its window, the awning.
    const Vector2 k0{b.ground.x + side.x * 0.62f - 0.1f, b.ground.y + side.y * 0.62f - 0.1f};
    const Box box = box_on(map, {k0.x, k0.y, 0.2f, 0.2f}, 11.0f);
    face_fill(box.lit_face, 0.0f, 1.0f, 0.0f, 11.0f, {214, 220, 226, 255});
    face_fill(box.shade_face, 0.0f, 1.0f, 0.0f, 11.0f, {160, 168, 176, 255});
    face_fill(box.lit_face, 0.0f, 1.0f, 8.0f, 11.0f, {50, 96, 180, 255});
    face_fill(box.shade_face, 0.0f, 1.0f, 8.0f, 11.0f, {40, 76, 146, 255});
    face_fill(box.lit_face, 0.2f, 0.8f, 3.5f, 7.0f, {70, 90, 104, 255});
    face_fill(box.shade_face, 0.2f, 0.8f, 3.5f, 7.0f, {56, 74, 86, 255});
    fill_quad(box.top[0], box.top[1], box.top[2], box.top[3], {120, 124, 128, 255});
}

// A direction finder's aerial up a cell tower: a cross-arm with its four
// dipoles over the top platform, the set's box under it with the side's
// tape, its cable down the mast.
void WorldRenderer::draw_tower_aerial(const engine::TileMap& map, const engine::Structure& s, float damage) const {
    if (stage_of(damage) >= 3) return;  // came down with the mast
    const Vector2 foot = on_terrain(map, to_vector2(s.center));
    const Vector2 at{foot.x, foot.y - 80.0f * building_scale(s)};
    const Color metal = lit({46, 48, 46, 255});
    const Vector2 ends[4] = {{at.x - 8.0f, at.y + 2.0f}, {at.x + 8.0f, at.y - 2.0f}, {at.x - 5.0f, at.y - 3.0f}, {at.x + 5.0f, at.y + 3.0f}};
    DrawLineEx(ends[0], ends[1], 1.2f, metal);
    DrawLineEx(ends[2], ends[3], 1.2f, metal);
    for (const Vector2& e : ends) DrawLineEx({e.x, e.y - 6.0f}, {e.x, e.y + 5.0f}, 1.0f, lit({30, 32, 30, 255}));
    DrawRectangleRec({at.x - 2.0f, at.y + 1.0f, 4.0f, 4.0f}, lit({86, 94, 70, 255}));
    DrawRectangleRec({at.x - 1.0f, at.y + 5.0f, 2.0f, 2.0f}, theme::player_color(s.antenna));
    DrawLineV({at.x + 1.0f, at.y + 5.0f}, {foot.x + 1.0f, foot.y - 12.0f}, ColorAlpha({30, 30, 30, 255}, 0.6f));
}

// A soldier in our pixel art (see soldiers.h), his pose as he's doing;
// the mortar's tube on its plate, the AGS on its tripod set up in front of
// a crew; a rear trooper's load on his back.
void WorldRenderer::draw_soldier(const engine::Unit& u, Vector2 feet, Vector2 facing) const {
    using engine::UnitTypeId;
    // Down in a trench, a pit: drawn that much lower, cut off at its near rim.
    const float sunk = sink_of(u.id);
    const float cut = sunk > 0.5f ? feet.y + 1.5f : 1e9f;
    feet.y += sunk;
    Vector2 fs = iso_offset(facing);
    {
        const float l = std::hypot(fs.x, fs.y);
        fs = l > 0.0f ? Vector2{fs.x / l, fs.y / l} : Vector2{1.0f, 0.0f};
    }
    const float side = fs.x >= 0.0f ? 1.0f : -1.0f;  // which way he faces on screen
    const UnitTypeId type = u.type;
    const auto [pose, frame] = soldier_pose(u);
    const bool crew_set = pose == soldiers::Pose::Crew;
    const float x = feet.x;

    // Things in front of him on the ground: a mortar's tube on its plate, an AGS on its tripod.
    auto ahead_px = [&](float px) { return Vector2{x + fs.x * px, feet.y + fs.y * px}; };
    auto crew_weapon = [&] {
        // Fired: a mortar's tube driven down into its plate; an AGS shaking on its tripod through its burst.
        const auto fired = shot_at_.find(u.id);
        const float since = fired != shot_at_.end() ? static_cast<float>(GetTime() - fired->second) : 99.0f;
        if (type == UnitTypeId::Mortar && crew_set) {
            const float jolt = since < 0.18f ? 1.0f - since / 0.18f : 0.0f;
            const Vector2 plate = {ahead_px(7.0f).x, ahead_px(7.0f).y + jolt};
            DrawEllipse(static_cast<int>(plate.x), static_cast<int>(plate.y), 3.5f, 1.6f, lit({54, 56, 50, 255}));
            const Vector2 muzzle{plate.x + fs.x * 3.0f, plate.y - 11.0f + jolt * 1.5f};
            DrawLineEx({plate.x + fs.x * 4.0f + 1.0f, plate.y}, {plate.x + fs.x * 1.5f, plate.y - 6.0f}, 1.0f, lit({46, 48, 44, 255}));
            DrawLineEx(plate, muzzle, 2.6f, lit({66, 72, 58, 255}));
            DrawLineEx({plate.x - 0.6f, plate.y}, {muzzle.x - 0.6f, muzzle.y}, 0.8f, lit({100, 108, 88, 255}));
        }
        if (type == UnitTypeId::Ags && crew_set) {
            const float shake = since < 0.4f && std::fmod(since, 0.08f) < 0.04f ? 1.2f : 0.0f;
            const Vector2 base = {ahead_px(7.0f).x - fs.x * shake, ahead_px(7.0f).y - fs.y * shake};
            for (const float k : {-1.0f, 1.0f, 0.0f}) {
                DrawLineEx({base.x, base.y - 4.0f}, {base.x + k * 3.5f - fs.x * (k == 0.0f ? 3.0f : 0.0f), base.y + (k == 0.0f ? 1.0f : 0.5f)},
                           1.0f, lit({50, 52, 48, 255}));
            }
            const Vector2 body{base.x, base.y - 5.0f};
            DrawLineEx({body.x - fs.x * 3.0f, body.y - fs.y * 1.5f}, {body.x + fs.x * 4.0f, body.y + fs.y * 2.0f}, 3.2f, lit({64, 70, 56, 255}));
            DrawLineEx({body.x + fs.x * 4.0f, body.y + fs.y * 2.0f}, {body.x + fs.x * 7.0f, body.y + fs.y * 3.2f}, 1.4f, lit({40, 42, 38, 255}));
            disc({body.x - fs.x * 0.5f + side * 0.5f, body.y + 2.0f}, 2.2f, {58, 66, 48, 255});  // the drum
        }
    };
    const bool toward_us = fs.y > 0.0f;  // what's ahead of him is nearer us than he is
    if (!toward_us) crew_weapon();
    const SpriteSheet& sheet = soldier_sheet(kit_of(type), u.owner);
    draw_sprite(sheet, feet, facing, soldiers::first_frame(pose) + frame, WHITE, cut);  // (a rear trooper's load: in his pose)
    if (toward_us) crew_weapon();
}

// --- Vehicles, drawn by hand ---------------------------------------------------

// A vehicle's own frame on screen: points by how far along it (tiles, +
// ahead), across it (tiles, + to its left) and up (pixels). It is drawn
// rigid, level with the ground under its middle.
struct Frame {
    Vector2 o;  // its middle on the ground, on screen
    Vector2 F;  // a tile ahead, on screen
    Vector2 S;  // a tile to its left, on screen
    Vector2 f;  // ahead, on the ground
    Vector2 s;  // to its left, on the ground
    float k = 1.0f;  // drawn this much bigger than life
    // Sunk this many pixels into a bog or the water: what's below that is
    // hidden, the rest drawn that much lower, on the surface.
    float sink = 0.0f;
    Vector2 at(float a, float c, float z = 0.0f) const {
        const float zz = sink > 0.0f ? std::max(z - sink, 0.0f) : z;
        return {o.x + F.x * a + S.x * c, o.y + F.y * a + S.y * c - zz * k};
    }
    // Turned to `dir` about the point (a, c) of this frame: a turret on its hull.
    Frame turned(Vector2 dir, float a, float c) const {
        const Vector2 left{-dir.y, dir.x};
        const Vector2 F2 = iso_offset(dir);
        const Vector2 S2 = iso_offset(left);
        return {at(a, c), {F2.x * k, F2.y * k}, {S2.x * k, S2.y * k}, dir, left, k, sink};
    }
    // Whether a side facing `n` (along, across) is turned towards the viewer, and how it's lit.
    Vector2 ground_of(float na, float nc) const { return {f.x * na + s.x * nc, f.y * na + s.y * nc}; }
};

Frame make_frame(const engine::TileMap& map, Vector2 ground, Vector2 f) {
    const Vector2 left{-f.y, f.x};
    return {on_terrain(map, ground), iso_offset(f), iso_offset(left), f, left};
}

// A solid of the vehicle: a polygon (along, across) standing from z0 up to
// z1, its top the polygon `top` (the same corners moved: a sloped front, a
// rounded turret). Its sides turned towards the viewer are drawn, each shaded
// by the way it faces (lit on the right, as the houses are), then its top.
// Whether what's at height z (and below) has sunk out of sight.
bool under(const Frame& fr, float z) { return fr.sink > 0.0f && z <= fr.sink + 0.2f; }

void solid(const Frame& fr, const Vector2* base, const Vector2* top, int n, float z0, float z1, Color color) {
    if (under(fr, z1)) return;  // sunk out of sight
    Vector2 mid{0.0f, 0.0f};
    for (int i = 0; i < n; ++i) mid = {mid.x + base[i].x / static_cast<float>(n), mid.y + base[i].y / static_cast<float>(n)};
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        Vector2 nrm{base[j].y - base[i].y, -(base[j].x - base[i].x)};
        const Vector2 half{(base[i].x + base[j].x) * 0.5f - mid.x, (base[i].y + base[j].y) * 0.5f - mid.y};
        if (nrm.x * half.x + nrm.y * half.y < 0.0f) nrm = {-nrm.x, -nrm.y};
        const Vector2 g = fr.ground_of(nrm.x, nrm.y);
        const float len = std::hypot(g.x, g.y);
        if (len <= 0.0f) continue;
        const Vector2 gn{g.x / len, g.y / len};
        if (gn.x + gn.y <= 0.02f) continue;  // turned away
        const float k = 0.54f + 0.3f * gn.x - 0.14f * gn.y;
        fill_quad(fr.at(base[i].x, base[i].y, z0), fr.at(base[j].x, base[j].y, z0), fr.at(top[j].x, top[j].y, z1),
                  fr.at(top[i].x, top[i].y, z1), shade(color, k));
    }
    const Vector2 t0 = fr.at(top[0].x, top[0].y, z1);
    const Color lid = shade(color, 1.1f);  // tops catch the most light
    for (int i = 1; i + 1 < n; ++i) fill_triangle(t0, fr.at(top[i].x, top[i].y, z1), fr.at(top[i + 1].x, top[i + 1].y, z1), lid);
}

// A box of the vehicle from a0 to a1 along it and c0 to c1 across, z0 to z1
// up; its top drawn in by `front`, `back` and `sides` (tiles): a sloped plate.
void block(const Frame& fr, float a0, float a1, float c0, float c1, float z0, float z1, Color color, float front = 0.0f,
           float back = 0.0f, float sides = 0.0f) {
    const Vector2 base[4] = {{a1, c1}, {a1, c0}, {a0, c0}, {a0, c1}};
    const Vector2 top[4] = {{a1 - front, c1 - sides}, {a1 - front, c0 + sides}, {a0 + back, c0 + sides}, {a0 + back, c1 - sides}};
    solid(fr, base, top, 4, z0, z1, color);
}

// A round solid: a turret, a cupola, a drum; `taper` draws its top in.
void round_solid(const Frame& fr, float a, float c, float ra, float rc, float z0, float z1, Color color, float taper = 0.0f,
                 int sides = 10) {
    Vector2 base[16];
    Vector2 top[16];
    sides = std::min(sides, 16);
    for (int i = 0; i < sides; ++i) {
        const float t = static_cast<float>(i) * 6.2831853f / static_cast<float>(sides);
        base[i] = {a + std::cos(t) * ra, c + std::sin(t) * rc};
        top[i] = {a + std::cos(t) * ra * (1.0f - taper), c + std::sin(t) * rc * (1.0f - taper)};
    }
    solid(fr, base, top, sides, z0, z1, color);
}

// A patch of paint lying on a top at height z: camouflage, a marking.
void patch(const Frame& fr, float a, float c, float ra, float rc, float z, Color color, uint32_t seed) {
    constexpr int kPoints = 7;
    Vector2 ring[kPoints];
    for (int i = 0; i < kPoints; ++i) {
        const float t = static_cast<float>(i) * 6.2831853f / kPoints;
        const float k = 0.75f + 0.5f * hash_unit(tile_hash(static_cast<int>(seed & 0xFFFF) + i * 7, i * 11));
        ring[i] = fr.at(a + std::cos(t) * ra * k, c + std::sin(t) * rc * k, z);
    }
    const Vector2 mid = fr.at(a, c, z);
    for (int i = 0; i < kPoints; ++i) fill_triangle(mid, ring[i], ring[(i + 1) % kPoints], color);
}

// A wheel standing along the vehicle at (a, c), `r` pixels high: the tyre, the hub.
void wheel(const Frame& fr, float a, float c, float r, Color tyre, Color hub) {
    constexpr int kPoints = 12;
    const float ra = r / 32.0f;  // tiles along the hull
    Vector2 ring[kPoints];
    for (int i = 0; i < kPoints; ++i) {
        const float t = static_cast<float>(i) * 6.2831853f / kPoints;
        ring[i] = fr.at(a + std::cos(t) * ra, c, r + std::sin(t) * r);
    }
    const Vector2 mid = fr.at(a, c, r);
    for (int i = 0; i < kPoints; ++i) fill_triangle(mid, ring[i], ring[(i + 1) % kPoints], tyre);
    for (int i = 0; i < kPoints; ++i) {
        const Vector2 p{mid.x + (ring[i].x - mid.x) * 0.5f, mid.y + (ring[i].y - mid.y) * 0.5f};
        const Vector2 q{mid.x + (ring[(i + 1) % kPoints].x - mid.x) * 0.5f, mid.y + (ring[(i + 1) % kPoints].y - mid.y) * 0.5f};
        fill_triangle(mid, p, q, hub);
    }
}

// Tracks along both sides from a0 to a1: the far one, then (after the
// hull, see near_track) the near one with its road wheels and the links
// running round when it drives.
bool left_is_near(const Frame& fr) { return fr.s.x + fr.s.y > 0.0f; }
void track(const Frame& fr, float a0, float a1, float c_in, float c_out, float high, int wheels, bool near, bool moving) {
    const float side = left_is_near(fr) == near ? 1.0f : -1.0f;
    const Color steel{50, 50, 46, 255};
    block(fr, a0, a1, side > 0 ? c_in : -c_out, side > 0 ? c_out : -c_in, 0.0f, high, steel, 0.04f, 0.04f);
    if (!near) return;
    const float c = side * c_out;
    for (int i = 0; i < wheels; ++i) {
        const float a = a0 + 0.08f + (a1 - a0 - 0.16f) * static_cast<float>(i) / static_cast<float>(wheels - 1);
        wheel(fr, a, c, high * 0.42f, {40, 40, 38, 255}, {104, 106, 96, 255});
    }
    // The links along the top, running back as it drives.
    const float run = moving ? std::fmod(static_cast<float>(GetTime()) * 1.6f, 0.08f) : 0.0f;
    for (float a = a0 + 0.04f + run; a < a1 - 0.04f; a += 0.08f) {
        DrawLineV(fr.at(a, side * c_in, high), fr.at(a, side * c_out, high), lit({30, 30, 28, 255}));
    }
}

// A gun barrel from `from` along the frame's front: a round tube, the lit
// line along its top, bands, a muzzle; drawn a little up when elevated.
void barrel(const Frame& fr, float a0, float a1, float c, float z, float rise, float width, Color color, bool brake = false) {
    if (fr.sink > 0.0f && std::max(z, z + rise) <= fr.sink) return;  // under the water
    const Vector2 p0 = fr.at(a0, c, z);
    const Vector2 p1 = fr.at(a1, c, z + rise);
    DrawLineEx(p0, p1, width, lit(color));
    DrawLineEx({p0.x, p0.y - width * 0.3f}, {p1.x, p1.y - width * 0.3f}, std::max(0.8f, width * 0.35f), lit(shade(color, 1.35f)));
    if (brake) {
        const Vector2 m0 = fr.at(a1 - 0.06f, c, z + rise * (1.0f - 0.06f / (a1 - a0)));
        DrawLineEx(m0, p1, width * 1.7f, lit(shade(color, 0.8f)));
    }
    disc(p1, width * 0.45f, {20, 20, 20, 255});
}

// A barrel from a0 out to a1 laid `deg` degrees up about a0: shorter seen
// from above, its muzzle up.
struct Laid {
    float a1;
    float rise;  // pixels
};
Laid laid(float a0, float a1, float deg) {
    const float t = deg * 0.0174533f;
    return {a0 + (a1 - a0) * std::cos(t), (a1 - a0) * std::sin(t) * kZPerTile};
}

// A camouflage net over a vehicle or a gun, `rx` along and `ry` across it
// (tiles), up to `z` over its middle and down to the ground round it where
// it's pegged: a mesh with strips of cloth in greens and browns all over it.
// `amount` 0..1 while the crew puts it up.
void camo_net(const Frame& fr, float z, float rx, float ry, float amount, uint32_t seed) {
    if (amount <= 0.0f) return;
    constexpr int kSides = 20;
    const Vector2 mid = fr.at(0.0f, 0.0f, z);
    const Color net = ColorAlpha({70, 84, 52, 255}, 0.72f * amount);
    const float rim = z * 0.5f;
    for (int i = 0; i < kSides; ++i) {
        const float t0 = static_cast<float>(i) * 6.2831853f / kSides;
        const float t1 = static_cast<float>(i + 1) * 6.2831853f / kSides;
        fill_triangle(mid, fr.at(std::cos(t0) * rx, std::sin(t0) * ry, rim), fr.at(std::cos(t1) * rx, std::sin(t1) * ry, rim), net);
    }
    for (int i = 0; i < 6; ++i) {  // pegged out
        const float t = (static_cast<float>(i) + 0.5f) * 6.2831853f / 6.0f;
        DrawLineEx(fr.at(std::cos(t) * rx, std::sin(t) * ry, rim), fr.at(std::cos(t) * rx * 1.25f, std::sin(t) * ry * 1.25f, 0.0f), 1.0f,
                   ColorAlpha(lit({46, 50, 36, 255}), 0.8f * amount));
    }
    constexpr Color kCloth[] = {{50, 68, 36, 255}, {84, 98, 56, 255}, {104, 90, 60, 255}, {130, 122, 86, 255}};
    const int n = static_cast<int>(70.0f * amount);
    for (int i = 0; i < n; ++i) {
        const uint32_t h = tile_hash(static_cast<int>(seed & 0xFFFF) + i * 7, i * 13 + 5);
        const float rr = std::sqrt(hash_unit(h));
        const float t = hash_unit(h >> 8) * 6.2831853f;
        const Vector2 p = fr.at(std::cos(t) * rx * rr, std::sin(t) * ry * rr, z - (z - rim) * rr * rr);
        DrawRectangleRec({std::round(p.x - 1.0f), std::round(p.y), 2.0f + static_cast<float>((h >> 20) % 2), 1.0f + static_cast<float>((h >> 22) % 2)},
                         ColorAlpha(lit(kCloth[(h >> 16) % 4]), amount));
    }
}

engine::VehicleModel model_of(const engine::Unit& u);  // with the vehicles' pixel art, below
Vector2 turret_ring_of(engine::VehicleModel m);  // along and across the hull
bool raises_gun(engine::VehicleModel m);         // a self-propelled howitzer
float radar_at_of(engine::VehicleModel m);       // where an AA gun's radar turns, along its turret
Vector2 ring_at(const Frame& fr, engine::VehicleModel m);  // where on the screen the turret stands

// A vehicle, drawn by hand in its own frame (see Frame): tracks with their
// road wheels or tyred wheels, the hull with its sloped plates, a turret
// turned where it aims, the gun; painted olive with the side's colour in it
// and camouflage patches, the side's stripe round the turret.
// The mud a tank sank into, heaped round it in lumps: grey, each lump lit
// on its upper left and shaded below, a darker soup under the hull. `front`:
// the lumps on the near side (drawn over the tank), else the far ones and the
// soup (drawn under it).
void mud_halo(const Frame& fr, Vector2 size, bool front, uint32_t seed) {
    const Color mud{74, 72, 62, 255};
    const float len = size.x * 0.56f * kVehicleScale;
    const float wid = (size.y + 0.1f) * kVehicleScale;
    if (!front) {  // the soup under it, filling the ring
        constexpr int kSides = 24;
        const Vector2 o = fr.at(0.0f, 0.0f);
        const Color soup = ColorAlpha(shade(mud, 0.6f), 0.6f);
        for (int i = 0; i < kSides; ++i) {
            const float t0 = static_cast<float>(i) * 6.2831853f / kSides;
            const float t1 = static_cast<float>(i + 1) * 6.2831853f / kSides;
            fill_triangle(o, fr.at(std::cos(t0) * len, std::sin(t0) * wid), fr.at(std::cos(t1) * len, std::sin(t1) * wid), soup);
        }
    }
    constexpr int kLumps = 72;  // two rings of them, the outer one looser
    for (int i = 0; i < kLumps; ++i) {
        const float t = static_cast<float>(i) * 6.2831853f / (kLumps / 2) + hash_unit(tile_hash(static_cast<int>(seed & 0xFFFF) + i, 3)) * 0.2f;
        const float out = (i % 2 == 0 ? 0.96f : 1.1f) + 0.08f * hash_unit(tile_hash(static_cast<int>(seed >> 8) + i, 7));
        const float a = std::cos(t) * len * out;
        const float c = std::sin(t) * wid * out;
        const Vector2 g = fr.ground_of(a, c);  // which way from the middle, on the ground
        const bool near = g.x + g.y > 0.0f;
        if (near != front) continue;
        const Vector2 p = fr.at(a, c);
        const float r = 1.3f + 1.0f * hash_unit(tile_hash(static_cast<int>(seed >> 4) + i * 13, 11));
        const float tone = 0.9f + 0.2f * hash_unit(tile_hash(i, static_cast<int>(seed & 0xFF)));
        disc({p.x + r * 0.3f, p.y + r * 0.3f}, r, shade(mud, 0.62f * tone));
        disc({p.x, p.y - r * 0.2f}, r * 0.85f, shade(mud, tone));
        disc({p.x - r * 0.3f, p.y - r * 0.5f}, r * 0.4f, shade(mud, 1.3f * tone));
    }
}

void WorldRenderer::draw_vehicle(const engine::TileMap& map, const engine::Unit& u, Vector2 ground,
                                 Vector2 facing) const {
    using engine::UnitTypeId;
    Vector2 hull_dir = to_vector2(u.hull);
    {
        const float l = std::hypot(hull_dir.x, hull_dir.y);
        hull_dir = l > 0.0f ? Vector2{hull_dir.x / l, hull_dir.y / l} : facing;
    }
    const UnitTypeId type = u.type;
    // Its hull goes where it drives, its turret turns on its own: tanks, IFVs, SPGs, AA guns.
    const bool hull_leads = drawn_as_armor(engine::unit_type(type));
    const bool gun_leads = engine::unit_type(type).family == engine::Family::Gun;  // no hull apart from the gun
    Frame fr = make_frame(map, ground, hull_leads || gun_leads ? hull_dir : facing);
    // Down in its pit (a caponier, a gun's emplacement): drawn that much
    // lower, cut off at the pit's near rim (its bank drawn over that).
    float cut = 1e9f;
    if (const float pit = sink_of(u.id); pit > 0.5f) {
        const Vector2 f = fr.f;
        cut = fr.o.y + 16.0f * (0.45f * std::fabs(f.x + f.y) + 0.24f * std::fabs(f.x - f.y)) + 1.0f;
        fr.o.y += pit;
    }
    // Alive: the engine's shudder on the idle, a sway on the move; a jolt
    // when it's hit; the gun's recoil when it fires rocking it back (a towed
    // gun thrown back whole).
    float since = 99.0f;  // since it fired
    if (const auto seen = vehicles_seen_.find(u.id); seen != vehicles_seen_.end()) {
        const VehicleSeen& v = seen->second;
        since = v.recoil;
        if (v.jolt < 0.3f) {
            const float k = (1.0f - v.jolt / 0.3f) * 1.8f * std::sin(v.jolt * 70.0f);
            fr.o = {fr.o.x + v.jolt_dir.x * k, fr.o.y + v.jolt_dir.y * k};
        }
    }
    // How firing throws it back (pixels: the hull; the turret on top of
    // it). A tank rocks back, its turret more. An SPG, the heavier gun,
    // harder and longer (its barrel recoils in its cradle besides: its
    // sprite). A rapid-fire gun (an autocannon, an AA gun, a machine gun)
    // jitters as long as its burst lasts. A rocket off a launcher shoves
    // it. A towed gun hops on its spades.
    const engine::WeaponDef& weapon = engine::weapon_of(u);
    const bool heavy = raises_gun(engine::unit_type(type).model);
    const bool rapid = !engine::unit_type(type).tank && !heavy && !gun_leads && weapon.damage > 0 &&
                       weapon.damage_type == engine::DamageType::Bullet;  // an autocannon, an AA gun, a machine gun
    float hull_k = 0.0f;
    float turret_k = 0.0f;
    float hop = 0.0f;
    if (gun_leads) {
        if (since < 0.15f) {
            hull_k = 1.0f - since / 0.15f;
            hop = std::sin(since / 0.15f * 3.14159f) * 1.8f;
        }
    } else if (heavy) {
        if (since < 0.45f) {
            const float k = 1.0f - since / 0.45f;
            hull_k = 2.2f * k * k;
            turret_k = 0.6f * k * k;
        }
    } else if (rapid) {
        if (since < 0.3f && std::fmod(since, 0.066f) < 0.033f) {
            hull_k = 0.5f;
            turret_k = 1.0f;
        }
    } else if (type == UnitTypeId::Mlrs) {
        if (since < 0.18f) hull_k = 2.0f * (1.0f - since / 0.18f);
    } else if (since < 0.25f) {
        hull_k = 1.0f - since / 0.25f;
        turret_k = 1.5f * hull_k;
    }
    Vector2 back{};  // a screen pixel back from where the gun points
    {
        const Vector2 d = iso_offset(facing);
        const float l = std::hypot(d.x, d.y);
        if (l > 0.0f) back = {-d.x / l, -d.y / l};
    }
    const Vector2 kick{back.x * hull_k, back.y * hull_k};
    const Vector2 turret_kick{back.x * turret_k, back.y * turret_k};
    if (gun_leads) {
        fr.o = {fr.o.x + kick.x, fr.o.y + kick.y - hop};
    } else {
        const float t = static_cast<float>(GetTime()) + static_cast<float>(u.id % 97) * 0.37f;
        const bool shudder = u.moving ? std::sin(t * 14.0f) > 0.35f : std::sin(t * 50.0f) > 0.8f;
        fr.o = {fr.o.x + kick.x, fr.o.y + kick.y - (shudder ? 1.0f : 0.0f)};
    }
    const Color team = theme::player_color(u.owner);
    const Color olive{98, 104, 70, 255};
    const Color paint = mix(olive, team, 0.3f);
    const Color dark = shade(paint, 0.62f);
    const Color metal{58, 60, 54, 255};
    const uint32_t h = static_cast<uint32_t>(u.id) * 2654435761u;
    auto camo = [&](float a0, float a1, float c0, float c1, float z, int n) {
        for (int i = 0; i < n; ++i) {
            const uint32_t hi = tile_hash(static_cast<int>(h >> 8) + i * 13, i * 7);
            patch(fr, a0 + (a1 - a0) * hash_unit(hi), c0 + (c1 - c0) * hash_unit(hi >> 16), 0.07f, 0.05f, z,
                  i % 2 == 0 ? shade(paint, 0.72f) : mix(paint, {150, 132, 92, 255}, 0.45f), hi);
        }
    };

    const engine::UnitTypeDef& type_def = engine::unit_type(type);
    if (drawn_as_armor(type_def)) {
        const engine::VehicleModel model = model_of(u);
        const int wear = wear_of(u.hp, type_def.max_hp);
        // In a bog a tank sinks, deeper and deeper (drawn from a sunk sprite
        // once baked); an amphibious IFV sits in it to its fenders.
        const engine::TilePos under = map.clamp_tile({static_cast<int32_t>(std::floor(ground.x)), static_cast<int32_t>(std::floor(ground.y))});
        const bool bog = map.terrain(under) == engine::Terrain::Swamp;
        // To its fenders, then to its deck, then its hull's gone and only the turret shows.
        const int depth = !bog                                       ? 0
                          : type_def.floats || u.mired * 3 < engine::kBogLimit ? 1
                          : u.mired * 3 < engine::kBogLimit * 2      ? 3
                                                                     : 4;
        const int era = kit(model, u.owner);
        const SpriteSheet* hull_sheet = sheet(SpritePart::Hull, armor_variant(model, era, wear, depth), u.owner);
        const SpriteSheet* turret_sheet = sheet(SpritePart::Turret, armor_variant(model, era, wear, depth), u.owner);
        int shown_depth = depth;
        if (bog && (!hull_sheet || !turret_sheet)) {
            hull_sheet = sheet(SpritePart::Hull, armor_variant(model, era, wear, 0), u.owner);
            turret_sheet = sheet(SpritePart::Turret, armor_variant(model, era, wear, 0), u.owner);
            shown_depth = 0;
        }
        if (hull_sheet && turret_sheet) {
            const uint32_t seed = static_cast<uint32_t>(u.id) * 2654435761u;
            if (bog) mud_halo(fr, armor_size(model), false, seed);  // the mud behind and under it
            const int frame = u.moving ? static_cast<int>(GetTime() * 10.0) % 2 : 0;
            draw_sprite(*hull_sheet, fr.o, fr.f, frame, WHITE, cut);
            const Vector2 ring = ring_at(fr, model);
            // The gun as it's laid. A tank's after a shot: the muzzle jumps
            // up, dips, settles, the turret rocking on its ring a moment. A
            // howitzer's raised to fire.
            const auto seen = vehicles_seen_.find(u.id);
            const float elev = seen != vehicles_seen_.end() && seen->second.elev >= 0.0f ? seen->second.elev : 1.0f;
            int turret_frame = 0;
            Vector2 sway{};
            if (type_def.tank) {
                const float t = seen != vehicles_seen_.end() ? seen->second.recoil : 1.0f;
                const int jump = t < 0.07f ? 1 : t < 0.14f ? 0 : t < 0.22f ? -1 : 0;
                turret_frame = kTankLadder[std::clamp(static_cast<int>(std::lround(elev)) + jump, 0, 4)];
                const Vector2 d = iso_offset(facing);
                const float dl = std::hypot(d.x, d.y);
                if (t < 0.6f && dl > 0.0f) {
                    const float k = 1.4f * std::exp(-6.0f * t) * std::sin(t * 42.0f);
                    sway = {-d.y / dl * k, d.x / dl * k * 0.5f};
                }
            }
            // A howitzer set up: its barrel apart from the turret, laid, recoiling after a shot.
            const SpriteSheet* barrel = nullptr;
            int barrel_frame = 0;
            if (raises_gun(model) && u.deployed && wear < 4) {
                barrel = sheet(SpritePart::Barrel, armor_variant(model, 0, wear >= 3 ? 3 : 0, shown_depth), u.owner);
                if (barrel) {
                    turret_frame = 1;  // the turret without its gun
                    barrel_frame = (std::clamp(static_cast<int>(std::lround(elev)), 1, kHowitzerFrames - 1) - 1) * kRecoilStates +
                                   recoil_state(since);
                }
            }
            const Vector2 on_ring{ring.x + turret_kick.x + sway.x, ring.y + turret_kick.y + sway.y};
            const bool front = barrel && gun_toward_viewer(facing, barrel->dirs);
            if (barrel && !front) draw_sprite(*barrel, on_ring, facing, barrel_frame, WHITE, cut);
            draw_sprite(*turret_sheet, on_ring, facing, turret_frame, WHITE, cut);
            if (barrel && front) draw_sprite(*barrel, on_ring, facing, barrel_frame, WHITE, cut);
            if (const SpriteSheet* radar = sheet(SpritePart::Radar, armor_variant(model, era, wear, depth), u.owner)) {
                // An AA gun's search radar turning round and round on its turret.
                const Vector2 r = turret_ring_of(model);
                const Frame tf = fr.turned(facing, r.x * kVehicleScale, r.y * kVehicleScale);
                const float spin = static_cast<float>(GetTime()) * 2.2f + static_cast<float>(u.id);
                const Vector2 at = tf.at(radar_at_of(model) * kVehicleScale, 0.0f);
                draw_sprite(*radar, {at.x + turret_kick.x, at.y + turret_kick.y}, {std::cos(spin), std::sin(spin)}, 0);
            }
            if (bog) mud_halo(fr, armor_size(model), true, seed);  // and in front of it
            if (seen != vehicles_seen_.end() && seen->second.camo > 0.0f) {
                const Vector2 size = armor_size(model);
                camo_net(fr, armor_muzzle(model).y + 5.0f, size.x * 0.62f * kVehicleScale, size.y * 1.35f * kVehicleScale,
                         seen->second.camo, seed);
            }
            return;
        }
        if (!type_def.tank) return;  // not baked yet
    }
    if (hull_leads) {
        const bool tank = engine::unit_type(type).tank;
        const float track_h = 5.0f;
        const int wheels = type == UnitTypeId::Spg ? 7 : 6;
        track(fr, -0.5f, 0.48f, 0.19f, 0.29f, track_h, wheels, false, u.moving);
        // The hull: a low box, the front plate sloped.
        const float deck = 8.0f;
        block(fr, -0.48f, 0.36f, -0.2f, 0.2f, 2.0f, deck, paint);
        block(fr, 0.36f, 0.5f, -0.2f, 0.2f, 2.0f, deck, shade(paint, 1.06f), 0.14f);
        // Fenders over the tracks.
        for (const float sgn : {-1.0f, 1.0f}) {
            block(fr, -0.5f, 0.46f, sgn > 0 ? 0.2f : -0.3f, sgn > 0 ? 0.3f : -0.2f, track_h, track_h + 0.8f, shade(paint, 0.9f));
        }
        camo(-0.4f, 0.3f, -0.17f, 0.17f, deck, 3);
        if (tank) {
            // The engine deck's grille, two fuel drums across the back.
            for (const float a : {-0.4f, -0.34f, -0.28f}) DrawLineV(fr.at(a, -0.14f, deck), fr.at(a, 0.14f, deck), lit(shade(paint, 0.7f)));
            for (const float c : {-0.1f, 0.1f}) round_solid(fr, -0.47f, c, 0.04f, 0.08f, deck, deck + 2.5f, {86, 84, 70, 255}, 0.0f, 8);
        }
        track(fr, -0.5f, 0.48f, 0.19f, 0.29f, track_h, wheels, true, u.moving);

        // The turret, turned where it aims.
        const float ta = tank ? -0.04f : type == UnitTypeId::Spg ? -0.16f : type == UnitTypeId::Shilka ? -0.06f : 0.0f;
        const Frame tf = fr.turned(facing, ta, 0.0f);
        const bool gun_front = tf.f.x + tf.f.y > 0.0f;  // the gun comes towards us: drawn over the turret
        float gun_z = deck + 3.0f;
        float gun_a0 = 0.15f;
        float gun_a1 = 0.8f;
        float gun_w = 2.6f;
        float rise = 0.0f;
        bool brake = false;
        auto guns = [&] {
            if (type == UnitTypeId::Shilka) {  // four barrels in two pairs
                for (const float c : {-0.09f, -0.05f, 0.05f, 0.09f}) barrel(tf, 0.18f, 0.5f, c, deck + 4.5f, 2.0f, 1.2f, metal);
                return;
            }
            barrel(tf, gun_a0, gun_a1, 0.0f, gun_z, rise, gun_w, tank ? shade(paint, 0.8f) : metal, brake);
            if (tank) {  // the thermal sleeve's bands, the fume extractor
                for (const float k : {0.35f, 0.62f}) {
                    const float a = gun_a0 + (gun_a1 - gun_a0) * k;
                    DrawLineEx(tf.at(a - 0.02f, 0.0f, gun_z), tf.at(a + 0.02f, 0.0f, gun_z), gun_w + 1.2f, lit(shade(paint, 0.7f)));
                }
            }
        };
        if (tank) {
            gun_z = deck + 3.2f;
        } else if (type == UnitTypeId::Spg) {
            gun_a0 = 0.16f;
            gun_a1 = u.deployed ? 0.62f : 0.72f;
            gun_w = 2.4f;
            gun_z = deck + 3.0f;
            rise = u.deployed ? 12.0f : 0.0f;
            brake = true;
        }
        if (!gun_front) guns();
        if (tank) {
            // A low round turret, the side's stripe round it, the commander's cupola and its machine gun.
            round_solid(tf, 0.0f, 0.0f, 0.2f, 0.17f, deck, deck + 2.2f, paint, 0.0f, 12);
            round_solid(tf, 0.0f, 0.0f, 0.2f, 0.17f, deck + 2.2f, deck + 3.2f, team, 0.02f, 12);
            round_solid(tf, 0.0f, 0.0f, 0.196f, 0.167f, deck + 3.2f, deck + 5.0f, paint, 0.3f, 12);
            camo(-0.1f, 0.08f, -0.08f, 0.08f, deck + 5.0f, 2);
            round_solid(tf, -0.06f, 0.07f, 0.045f, 0.045f, deck + 5.0f, deck + 6.5f, shade(paint, 0.92f), 0.2f, 8);
            DrawLineEx(tf.at(-0.06f, 0.07f, deck + 7.0f), tf.at(0.06f, 0.07f, deck + 7.5f), 1.0f, lit({34, 34, 34, 255}));
        } else if (type == UnitTypeId::Spg) {
            block(tf, -0.18f, 0.17f, -0.17f, 0.17f, deck, deck + 2.0f, team, 0.0f, 0.0f, 0.0f);
            block(tf, -0.18f, 0.17f, -0.17f, 0.17f, deck + 2.0f, deck + 5.5f, paint, 0.05f, 0.02f, 0.03f);
            camo(-0.12f, 0.1f, -0.12f, 0.12f, deck + 5.5f, 2);
        } else {  // Shilka: a big flat turret, its radar dish turning at the back
            block(tf, -0.2f, 0.18f, -0.19f, 0.19f, deck, deck + 2.0f, team);
            block(tf, -0.2f, 0.18f, -0.19f, 0.19f, deck + 2.0f, deck + 6.0f, paint, 0.03f, 0.0f, 0.02f);
            const Vector2 mast = tf.at(-0.16f, 0.0f, deck + 6.0f);
            DrawLineEx(mast, {mast.x, mast.y - 5.0f}, 1.2f, lit(metal));
            const float spin = std::cos(static_cast<float>(GetTime()) * 2.5f);
            DrawEllipse(static_cast<int>(mast.x), static_cast<int>(mast.y - 7.0f), 1.0f + 5.0f * std::fabs(spin), 3.2f, lit({86, 92, 80, 255}));
            DrawEllipseLines(static_cast<int>(mast.x), static_cast<int>(mast.y - 7.0f), 1.0f + 5.0f * std::fabs(spin), 3.2f, lit(metal));
        }
        if (gun_front) guns();
        if (u.camouflaged) {  // nets and branches over it
            fill_ground_ellipse(fr.at(0.0f, 0.0f, deck + 3.0f), 0.6f, {64, 88, 52, 170});
            draw_ground_ellipse(fr.at(0.0f, 0.0f, deck + 3.0f), 0.6f, {46, 66, 38, 200});
        }
        return;
    }

    if (const std::optional<TruckModel> truck = truck_model(type, u.owner)) {
        const int wear = wear_of(u.hp, type_def.max_hp);
        const auto seen = units_seen_.find(u.id);
        const int load = seen != units_seen_.end() ? truck_load(static_cast<Cargo>(seen->second.cargo)) : 0;
        const SpriteSheet* body = sheet(SpritePart::TruckBody, truck_variant(*truck, wear, 0, load), u.owner);
        if (!body) body = sheet(SpritePart::TruckBody, truck_variant(*truck, wear, 0, 0), u.owner);
        if (body) {
            const int frame = u.deployed ? 1 : 0;  // set up: masts raised, jacks down, the launcher up
            const bool rolling = u.moving && static_cast<int>(GetTime() * 10.0) % 2 == 1;
            draw_sprite(*body, fr.o, fr.f, rolling ? 2 : frame, WHITE, cut);  // (2: on the move, the wheels turned)
            if (truck_turns_top(*truck)) {
                if (const SpriteSheet* top = sheet(SpritePart::TruckTop, truck_variant(*truck, wear, 0, 0), u.owner)) {
                    // A radar set up turns round and round; packed, it lies along the truck.
                    const float spin = static_cast<float>(GetTime()) * 1.8f + static_cast<float>(u.id);
                    const Vector2 dir = u.deployed ? Vector2{std::cos(spin), std::sin(spin)} : fr.f;
                    const Vector2 ring = truck_top_ring_of(*truck);
                    draw_sprite(*top, fr.at(ring.x * kVehicleScale, ring.y * kVehicleScale), dir, frame, WHITE, cut);
                }
            }
            return;
        }
    }
    if (has_gun_look(type_def.model)) {
        const int wear = wear_of(u.hp, type_def.max_hp);
        if (const SpriteSheet* gun = sheet(SpritePart::Gun, small_variant(type_def.model, wear), u.owner)) {
            // Set up: the trails spread, the barrel (its own sprite) laid for
            // the range, recoiling in its cradle after a shot.
            const auto seen = vehicles_seen_.find(u.id);
            const float elev = seen != vehicles_seen_.end() && seen->second.elev >= 0.0f ? seen->second.elev : 1.0f;
            const SpriteSheet* barrel =
                u.deployed && wear < 4 ? sheet(SpritePart::Barrel, armor_variant(type_def.model, 0, wear >= 3 ? 3 : 0, 0), u.owner) : nullptr;
            draw_sprite(*gun, fr.o, fr.f, barrel ? 1 : 0, WHITE, cut);
            if (barrel) {
                const int laid_at = std::clamp(static_cast<int>(std::lround(elev)), 1, kHowitzerFrames - 1);
                draw_sprite(*barrel, fr.o, fr.f, (laid_at - 1) * kRecoilStates + recoil_state(since), WHITE, cut);
            }
            if (seen != vehicles_seen_.end() && seen->second.camo > 0.0f) {  // nets and branches over it
                camo_net(fr, 12.0f, 0.72f, 0.58f, seen->second.camo, static_cast<uint32_t>(u.id) * 2654435761u);
            }
            return;
        }
    }
    if (type == UnitTypeId::Howitzer) {
        // A D-30: set up, its three trails spread round it, the wheels up,
        // the barrel over its shield rising to fire; packed, the trails
        // folded under the barrel, which points back the way it's towed.
        const Color gun_metal = shade(paint, 0.78f);
        if (u.deployed) {
            for (const float deg : {180.0f, 60.0f, -60.0f}) {
                const float t = deg * 0.0174533f;
                const Frame trail = fr.turned({fr.f.x * std::cos(t) - fr.f.y * std::sin(t), fr.f.y * std::cos(t) + fr.f.x * std::sin(t)}, 0.0f, 0.0f);
                block(trail, 0.04f, 0.46f, -0.025f, 0.025f, 0.0f, 2.2f, dark);
            }
            block(fr, -0.08f, 0.08f, -0.08f, 0.08f, 0.0f, 6.0f, gun_metal, 0.0f, 0.0f, 0.02f);
            for (const float c : {-0.18f, 0.18f}) wheel(fr, -0.02f, c, 3.0f, {36, 36, 34, 255}, {96, 98, 88, 255});
            block(fr, 0.06f, 0.09f, -0.15f, 0.15f, 3.0f, 10.0f, paint);  // the shield
            barrel(fr, -0.1f, 0.62f, 0.0f, 7.0f, u.engaged ? 14.0f : 6.0f, 2.8f, gun_metal, true);
            if (u.camouflaged) {
                fill_ground_ellipse(fr.at(0.0f, 0.0f, 8.0f), 0.55f, {64, 88, 52, 170});
                draw_ground_ellipse(fr.at(0.0f, 0.0f, 8.0f), 0.55f, {46, 66, 38, 200});
            }
        } else {
            block(fr, 0.02f, 0.5f, -0.04f, 0.04f, 0.5f, 2.5f, dark);  // the trails folded together: the towing bar
            for (const float c : {-0.17f, 0.17f}) wheel(fr, 0.0f, c, 3.5f, {36, 36, 34, 255}, {96, 98, 88, 255});
            block(fr, -0.06f, 0.06f, -0.14f, 0.14f, 3.0f, 9.0f, paint);
            barrel(fr, 0.0f, -0.62f, 0.0f, 7.0f, 0.0f, 2.8f, gun_metal, true);
        }
        return;
    }

    // Wheeled: a Ural truck, a BTR-type command vehicle.
    const bool btr = type == UnitTypeId::FieldHq;
    const Color tyre{34, 34, 32, 255};
    const Color hub{92, 94, 84, 255};
    const float near = left_is_near(fr) ? 1.0f : -1.0f;
    if (btr) {
        const float axles[4] = {0.34f, 0.12f, -0.12f, -0.34f};
        for (const float a : axles) wheel(fr, a, -near * 0.2f, 3.4f, tyre, hub);
        block(fr, -0.5f, 0.36f, -0.21f, 0.21f, 3.0f, 10.0f, paint, 0.0f, 0.04f, 0.03f);
        block(fr, 0.36f, 0.54f, -0.21f, 0.21f, 3.0f, 10.0f, shade(paint, 1.06f), 0.16f, 0.0f, 0.03f);
        for (const float a : axles) wheel(fr, a, near * 0.21f, 3.4f, tyre, hub);
        camo(-0.4f, 0.3f, -0.15f, 0.15f, 10.0f, 3);
        round_solid(fr, 0.12f, 0.0f, 0.07f, 0.07f, 10.0f, 11.5f, team, 0.0f, 8);
        round_solid(fr, 0.12f, 0.0f, 0.068f, 0.068f, 11.5f, 13.5f, paint, 0.3f, 8);
        DrawLineEx(fr.at(0.18f, 0.0f, 12.5f), fr.at(0.34f, 0.0f, 12.8f), 1.2f, lit(metal));
        for (const float c : {-0.15f, 0.15f}) {  // whip aerials
            const Vector2 foot = fr.at(-0.36f, c, 10.0f);
            DrawLineEx(foot, {foot.x + 2.0f * c * 10.0f, foot.y - 28.0f}, 1.0f, lit({30, 32, 30, 255}));
        }
        return;
    }
    // A Ural: three axles, a bonneted cab, what's on the back by what it is.
    const float axles[3] = {0.34f, -0.14f, -0.36f};
    for (const float a : axles) wheel(fr, a, -near * 0.17f, 3.4f, tyre, hub);
    block(fr, -0.5f, 0.46f, -0.13f, 0.13f, 2.6f, 4.6f, {44, 44, 40, 255});  // the frame
    // The bonnet, the cab with its windscreen and side window.
    block(fr, 0.3f, 0.52f, -0.14f, 0.14f, 4.0f, 9.5f, paint, 0.03f);
    block(fr, 0.08f, 0.3f, -0.19f, 0.19f, 4.0f, 13.0f, paint, 0.0f, 0.0f, 0.01f);
    {
        const Vector2 fw = fr.ground_of(1.0f, 0.0f);
        if (fw.x + fw.y > 0.0f) {
            fill_quad(fr.at(0.3f, -0.15f, 8.8f), fr.at(0.3f, 0.15f, 8.8f), fr.at(0.3f, 0.15f, 12.2f), fr.at(0.3f, -0.15f, 12.2f),
                      {74, 92, 104, 255});
        }
        const float c = near * 0.19f;
        fill_quad(fr.at(0.13f, c, 8.8f), fr.at(0.26f, c, 8.8f), fr.at(0.26f, c, 12.0f), fr.at(0.13f, c, 12.0f), {80, 98, 110, 255});
    }
    DrawRectangleRec({fr.at(0.19f, 0.0f, 13.0f).x - 2.0f, fr.at(0.19f, 0.0f, 13.0f).y - 1.0f, 4.0f, 1.2f}, lit(team));  // a marking on the roof
    const float bed0 = -0.5f;
    const float bed1 = 0.06f;
    switch (type) {
        case UnitTypeId::FuelTanker: {  // a silver tank, a red band, a ladder
            block(fr, bed0, bed1, -0.16f, 0.16f, 4.6f, 11.5f, {176, 178, 172, 255}, 0.02f, 0.02f, 0.07f);
            block(fr, bed0 + 0.2f, bed0 + 0.26f, -0.165f, 0.165f, 7.0f, 8.2f, {176, 60, 50, 255});
            break;
        }
        case UnitTypeId::AmmoTruck: {  // a tarpaulin over the crates
            block(fr, bed0, bed1, -0.2f, 0.2f, 4.6f, 7.0f, dark);
            block(fr, bed0, bed1, -0.2f, 0.2f, 7.0f, 12.0f, {92, 100, 66, 255}, 0.0f, 0.0f, 0.05f);
            break;
        }
        case UnitTypeId::Mlrs: {  // the launcher: forty tubes, raised to fire
            block(fr, bed0, bed1, -0.2f, 0.2f, 4.6f, 6.0f, dark);
            const float up = u.deployed ? 6.0f : 0.0f;
            const Vector2 base[4] = {{bed1 - 0.02f, 0.17f}, {bed1 - 0.02f, -0.17f}, {bed0 + 0.02f, -0.17f}, {bed0 + 0.02f, 0.17f}};
            const Vector2 top[4] = {{bed1 - 0.02f, 0.17f}, {bed1 - 0.02f, -0.17f}, {bed0 + 0.02f, -0.17f}, {bed0 + 0.02f, 0.17f}};
            solid(fr, base, top, 4, 6.0f + up, 11.0f + up, {74, 82, 60, 255});
            const Vector2 bk = fr.ground_of(-1.0f, 0.0f);
            if (bk.x + bk.y > 0.0f) {  // the tube ends
                for (int row = 0; row < 3; ++row) {
                    for (int k = 0; k < 6; ++k) {
                        disc(fr.at(bed0 + 0.02f, -0.14f + 0.056f * static_cast<float>(k), 7.2f + up + 1.4f * static_cast<float>(row)), 0.8f,
                             {24, 26, 22, 255});
                    }
                }
            }
            break;
        }
        case UnitTypeId::DfStation:
        case UnitTypeId::AirRadar: {  // a box body; a mast with a loop, or a radar turning
            const Color box{112, 118, 96, 255};
            block(fr, bed0, bed1, -0.2f, 0.2f, 4.6f, 14.0f, box);
            DrawLineV(fr.at(bed0 + 0.1f, near * 0.2f, 5.0f), fr.at(bed0 + 0.1f, near * 0.2f, 13.0f), lit(shade(box, 0.6f)));
            const Vector2 foot = fr.at(bed0 + 0.2f, 0.0f, 14.0f);
            if (type == UnitTypeId::AirRadar) {
                DrawLineEx(foot, {foot.x, foot.y - 8.0f}, 1.5f, lit(metal));
                const float spin = static_cast<float>(GetTime()) * 1.8f + static_cast<float>(u.id);
                const Frame rf = fr.turned({std::cos(spin), std::sin(spin)}, bed0 + 0.2f, 0.0f);
                fill_quad(rf.at(0.0f, -0.28f, 22.0f), rf.at(0.0f, 0.28f, 22.0f), rf.at(0.03f, 0.28f, 30.0f), rf.at(0.03f, -0.28f, 30.0f),
                          {150, 156, 146, 255});
                for (int k = 1; k < 6; ++k) {
                    const float c = -0.28f + 0.56f * static_cast<float>(k) / 6.0f;
                    DrawLineV(rf.at(0.0f, c, 22.0f), rf.at(0.03f, c, 30.0f), lit({96, 100, 92, 255}));
                }
            } else if (u.deployed) {
                const Vector2 top{foot.x, foot.y - 26.0f};
                DrawLineEx(foot, top, 1.5f, lit(metal));
                DrawEllipseLines(static_cast<int>(top.x), static_cast<int>(top.y - 4.0f), 3.0f, 5.0f, lit(metal));
            } else {
                DrawLineEx(foot, fr.at(0.2f, 0.0f, 14.5f), 1.5f, lit(metal));
            }
            break;
        }
        default: {  // a supply truck: the open bed, its load on it
            block(fr, bed0, bed1, -0.2f, 0.2f, 4.6f, 8.0f, paint);
            block(fr, bed0 + 0.02f, bed1 - 0.02f, -0.18f, 0.18f, 5.0f, 7.6f, shade(paint, 0.55f));
            if (u.carrying > 0) {
                const float load = 2.0f + 5.0f * std::min(1.0f, static_cast<float>(u.carrying) / static_cast<float>(engine::kTruckCapacity));
                block(fr, bed0 + 0.04f, bed1 - 0.04f, -0.16f, 0.16f, 6.0f, 8.0f + load, cargo_color(u.carrying_type), 0.0f, 0.0f, 0.02f);
            }
            break;
        }
    }
    for (const float a : axles) wheel(fr, a, near * 0.17f, 3.4f, tyre, hub);
}

// A road wheel standing along the hull at (a, c), `r` pixels high (`up` off the ground: a return roller): the
// rubber tyre, the dished face in shadow at its rim, the hub lit, its cap.
void road_wheel(const Frame& fr, float a, float c, float r, Color face, float up = 0.0f) {
    if (fr.sink > 0.0f && r + up <= fr.sink) return;  // sunk out of sight
    constexpr int kPoints = 14;
    const float ra = r / 32.0f;
    const Vector2 mid = fr.at(a, c, r + up);
    auto ring = [&](float k, Color color) {
        Vector2 prev{};
        for (int i = 0; i <= kPoints; ++i) {
            const float t = static_cast<float>(i) * 6.2831853f / kPoints;
            const Vector2 p = fr.at(a + std::cos(t) * ra * k, c, r + up + std::sin(t) * r * k);
            if (i > 0) fill_triangle(mid, prev, p, color);
            prev = p;
        }
    };
    ring(1.0f, {26, 26, 24, 255});
    ring(0.78f, shade(face, 0.62f));
    ring(0.62f, face);
    ring(0.34f, shade(face, 1.4f));
    ring(0.14f, shade(face, 0.5f));
}

// --- Tanks in pixel art ------------------------------------------------------------

// The real tanks of both alliances, each drawn from what sets it apart: its
// size, its running gear, its skirts, its back, the shape of its turret, its
// gun, its reactive armor, its paint. Drawn a pixel to a world pixel into a
// texture, then made pixel art (see pixelate): the hull and the turret apart,
// so the turret turns on its own.
enum class TurretShape : uint8_t {
    Dome,     // the T-62's smooth frying pan
    Cast,     // the T-64's, T-72's, T-80's rounded cast turret
    Welded,   // the T-90M's angular welded one, a box on its back
    Wedge,    // Leopard 2A6, K2, Type 99A: an arrowhead of armor in front
    Flat,     // the Abrams': wide and low, faceted, a long bustle with its rack
    Modular,  // the Type 10's: boxy, bolted-on modules
    Leo1,     // the Leopard 1A5's, with its wedged add-on front
    Merkava,  // the Merkava's: long and low, a sharp nose, far back on the hull, chains under its bustle
};
enum class Skirt : uint8_t { None, Rubber, Full };  // fenders only; a rubber skirt over the upper run; armored, the full length
enum class Rear : uint8_t { Drums, Turbine, Plain };  // fuel drums and a log; a gas turbine's grilles; stowage
enum class EraKind : uint8_t {
    Soviet,    // Kontakt-1, then Kontakt-5, then Relikt, as researched
    SovietK1,  // Kontakt-1 only (a T-64BV, a T-62M)
    Nozh,      // the Ukrainian Nozh, then Duplet
    Tusk,      // the Abrams' TUSK kit
    Fy,        // the Chinese FY series
    None,      // composite armor, nothing bolted on
};
enum class Camo : uint8_t { None, ThreeTone, TwoTone, Digital };

struct TankLook {
    float length;  // the hull, tiles
    float width;   // half of it, to the tracks' outer edge
    float deck;    // the hull's roof, pixels up
    float glacis;  // tiles of the sloped front plate
    int wheels;
    float wheel_r;  // pixels
    bool rollers;   // return rollers above the road wheels
    bool gap;       // a wider gap after the first road wheel (the T-55/62's)
    Skirt skirt;
    Rear rear;
    TurretShape turret;
    float turret_at;  // the turret ring, along the hull
    float turret_r;   // the turret's size, tiles
    float turret_h;   // pixels
    float bustle;     // its rear bustle, tiles
    float gun;        // how far the gun reaches from the ring, tiles
    float gun_w;      // pixels
    float evacuator;  // where along the gun its fume extractor is, 0..1 (below 0: none)
    bool eyebrows;    // the T-62M's BDD armor over the turret front
    EraKind era;
    Camo camo;
    Color paint;
    Color camo1;
    Color camo2;
};

constexpr Color kRussianOlive{100, 104, 58, 255};
constexpr Color kUkrainianGreen{86, 100, 56, 255};
constexpr Color kNatoGreen{80, 94, 62, 255};
constexpr Color kNatoBrown{106, 86, 60, 255};
constexpr Color kNatoBlack{42, 42, 36, 255};

constexpr TankLook kT72B3{.length = 1.03f, .width = 0.29f, .deck = 7.5f, .glacis = 0.17f, .wheels = 6, .wheel_r = 3.0f,
                          .rollers = false, .gap = false, .skirt = Skirt::Rubber, .rear = Rear::Drums, .turret = TurretShape::Cast,
                          .turret_at = -0.04f, .turret_r = 0.2f, .turret_h = 4.8f, .bustle = 0.0f, .gun = 0.86f, .gun_w = 1.8f,
                          .evacuator = 0.5f, .eyebrows = false, .era = EraKind::Soviet, .camo = Camo::None,
                          .paint = kRussianOlive, .camo1 = kRussianOlive, .camo2 = kRussianOlive};

constexpr TankLook kTankLooks[] = {
    kT72B3,  // Standard: stands for its side's usual tank, see look_of
    // T-64BV: small and low, six small road wheels with return rollers, Kontakt-1.
    {.length = 0.95f, .width = 0.28f, .deck = 7.0f, .glacis = 0.16f, .wheels = 6, .wheel_r = 2.2f, .rollers = true,
     .gap = false, .skirt = Skirt::Rubber, .rear = Rear::Drums, .turret = TurretShape::Cast, .turret_at = -0.02f,
     .turret_r = 0.19f, .turret_h = 4.6f, .bustle = 0.0f, .gun = 0.84f, .gun_w = 1.8f, .evacuator = 0.5f, .eyebrows = false,
     .era = EraKind::SovietK1, .camo = Camo::None, .paint = kUkrainianGreen, .camo1 = kUkrainianGreen, .camo2 = kUkrainianGreen},
    // T-64BM Bulat: the T-64 with Nozh.
    {.length = 0.96f, .width = 0.28f, .deck = 7.0f, .glacis = 0.16f, .wheels = 6, .wheel_r = 2.2f, .rollers = true,
     .gap = false, .skirt = Skirt::Rubber, .rear = Rear::Drums, .turret = TurretShape::Cast, .turret_at = -0.02f,
     .turret_r = 0.2f, .turret_h = 4.8f, .bustle = 0.0f, .gun = 0.85f, .gun_w = 1.8f, .evacuator = 0.5f, .eyebrows = false,
     .era = EraKind::Nozh, .camo = Camo::None, .paint = kUkrainianGreen, .camo1 = kUkrainianGreen, .camo2 = kUkrainianGreen},
    // Leopard 1A5: long and high, seven big road wheels under a short skirt, the wedged turret, the 105 mm gun.
    {.length = 1.0f, .width = 0.3f, .deck = 8.0f, .glacis = 0.2f, .wheels = 7, .wheel_r = 3.0f, .rollers = true, .gap = false,
     .skirt = Skirt::Full, .rear = Rear::Plain, .turret = TurretShape::Leo1, .turret_at = 0.0f, .turret_r = 0.19f,
     .turret_h = 5.2f, .bustle = 0.05f, .gun = 0.82f, .gun_w = 1.6f, .evacuator = 0.45f, .eyebrows = false, .era = EraKind::None,
     .camo = Camo::ThreeTone, .paint = kNatoGreen, .camo1 = kNatoBrown, .camo2 = kNatoBlack},
    // Leopard 2A6: long and flat, seven road wheels behind a full skirt, the arrowhead turret, the long L/55.
    {.length = 1.1f, .width = 0.31f, .deck = 8.0f, .glacis = 0.14f, .wheels = 7, .wheel_r = 3.0f, .rollers = true, .gap = false,
     .skirt = Skirt::Full, .rear = Rear::Plain, .turret = TurretShape::Wedge, .turret_at = -0.02f, .turret_r = 0.2f,
     .turret_h = 5.4f, .bustle = 0.12f, .gun = 1.0f, .gun_w = 1.7f, .evacuator = 0.4f, .eyebrows = false, .era = EraKind::None,
     .camo = Camo::ThreeTone, .paint = kNatoGreen, .camo1 = kNatoBrown, .camo2 = kNatoBlack},
    // M1A1 Abrams: wide and heavy, the flat faceted turret with its long bustle, a gas turbine, desert tan.
    {.length = 1.1f, .width = 0.32f, .deck = 7.5f, .glacis = 0.12f, .wheels = 7, .wheel_r = 2.9f, .rollers = true, .gap = false,
     .skirt = Skirt::Full, .rear = Rear::Turbine, .turret = TurretShape::Flat, .turret_at = -0.04f, .turret_r = 0.22f,
     .turret_h = 4.4f, .bustle = 0.14f, .gun = 0.9f, .gun_w = 1.7f, .evacuator = 0.72f, .eyebrows = false, .era = EraKind::Tusk,
     .camo = Camo::None, .paint = {172, 152, 108, 255}, .camo1 = {172, 152, 108, 255}, .camo2 = {172, 152, 108, 255}},
    // Type 10: compact, five road wheels, the boxy modular turret.
    {.length = 0.98f, .width = 0.29f, .deck = 7.5f, .glacis = 0.14f, .wheels = 5, .wheel_r = 3.2f, .rollers = true, .gap = false,
     .skirt = Skirt::Full, .rear = Rear::Plain, .turret = TurretShape::Modular, .turret_at = -0.02f, .turret_r = 0.2f,
     .turret_h = 5.0f, .bustle = 0.1f, .gun = 0.9f, .gun_w = 1.6f, .evacuator = 0.5f, .eyebrows = false, .era = EraKind::None,
     .camo = Camo::TwoTone, .paint = {86, 98, 64, 255}, .camo1 = {112, 94, 66, 255}, .camo2 = {112, 94, 66, 255}},
    // K2 Black Panther: six road wheels behind a full skirt, a sloped arrowhead turret, the L/55.
    {.length = 1.06f, .width = 0.3f, .deck = 7.6f, .glacis = 0.16f, .wheels = 6, .wheel_r = 3.0f, .rollers = true, .gap = false,
     .skirt = Skirt::Full, .rear = Rear::Plain, .turret = TurretShape::Wedge, .turret_at = -0.02f, .turret_r = 0.2f,
     .turret_h = 5.0f, .bustle = 0.12f, .gun = 1.0f, .gun_w = 1.7f, .evacuator = 0.45f, .eyebrows = false, .era = EraKind::None,
     .camo = Camo::ThreeTone, .paint = {78, 90, 60, 255}, .camo1 = {114, 96, 68, 255}, .camo2 = {40, 40, 34, 255}},
    // Merkava Mk4: the engine in front, a long glacis, the turret far back, long and sharp, chains under its bustle.
    {.length = 1.12f, .width = 0.3f, .deck = 8.0f, .glacis = 0.3f, .wheels = 6, .wheel_r = 3.2f, .rollers = true, .gap = false,
     .skirt = Skirt::Full, .rear = Rear::Plain, .turret = TurretShape::Merkava, .turret_at = -0.14f, .turret_r = 0.19f,
     .turret_h = 4.6f, .bustle = 0.16f, .gun = 0.98f, .gun_w = 1.7f, .evacuator = 0.5f, .eyebrows = false, .era = EraKind::None,
     .camo = Camo::None, .paint = {148, 144, 120, 255}, .camo1 = {148, 144, 120, 255}, .camo2 = {148, 144, 120, 255}},
    // T-62M: five big road wheels, the first one apart, bare wheels under fenders, the dome turret with its BDD eyebrows.
    {.length = 0.95f, .width = 0.28f, .deck = 7.0f, .glacis = 0.18f, .wheels = 5, .wheel_r = 3.5f, .rollers = false, .gap = true,
     .skirt = Skirt::None, .rear = Rear::Drums, .turret = TurretShape::Dome, .turret_at = 0.02f, .turret_r = 0.2f,
     .turret_h = 4.4f, .bustle = 0.0f, .gun = 0.8f, .gun_w = 1.8f, .evacuator = 0.62f, .eyebrows = true, .era = EraKind::SovietK1,
     .camo = Camo::None, .paint = {96, 100, 58, 255}, .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    kT72B3,
    // T-80BVM: the T-72's size, return rollers, a gas turbine at the back.
    {.length = 1.04f, .width = 0.29f, .deck = 7.2f, .glacis = 0.17f, .wheels = 6, .wheel_r = 2.9f, .rollers = true, .gap = false,
     .skirt = Skirt::Rubber, .rear = Rear::Turbine, .turret = TurretShape::Cast, .turret_at = -0.04f, .turret_r = 0.2f,
     .turret_h = 4.8f, .bustle = 0.0f, .gun = 0.86f, .gun_w = 1.8f, .evacuator = 0.5f, .eyebrows = false, .era = EraKind::Soviet,
     .camo = Camo::None, .paint = {98, 104, 60, 255}, .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // T-90M: the angular welded turret with a box on its back.
    {.length = 1.04f, .width = 0.3f, .deck = 7.5f, .glacis = 0.17f, .wheels = 6, .wheel_r = 3.0f, .rollers = false, .gap = false,
     .skirt = Skirt::Rubber, .rear = Rear::Plain, .turret = TurretShape::Welded, .turret_at = -0.04f, .turret_r = 0.21f,
     .turret_h = 5.0f, .bustle = 0.1f, .gun = 0.88f, .gun_w = 1.8f, .evacuator = 0.5f, .eyebrows = false, .era = EraKind::Soviet,
     .camo = Camo::None, .paint = {88, 100, 58, 255}, .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // Type 99A: long, six road wheels, an arrowhead turret, digital camouflage.
    {.length = 1.08f, .width = 0.3f, .deck = 7.6f, .glacis = 0.16f, .wheels = 6, .wheel_r = 3.1f, .rollers = true, .gap = false,
     .skirt = Skirt::Full, .rear = Rear::Plain, .turret = TurretShape::Wedge, .turret_at = -0.03f, .turret_r = 0.21f,
     .turret_h = 5.0f, .bustle = 0.1f, .gun = 0.92f, .gun_w = 1.8f, .evacuator = 0.5f, .eyebrows = false, .era = EraKind::Fy,
     .camo = Camo::Digital, .paint = {96, 112, 76, 255}, .camo1 = {64, 82, 54, 255}, .camo2 = {134, 144, 106, 255}},
    // Karrar: Iran's, on the T-72's hull, an angular welded turret, sand.
    {.length = 1.03f, .width = 0.29f, .deck = 7.5f, .glacis = 0.17f, .wheels = 6, .wheel_r = 3.0f, .rollers = false, .gap = false,
     .skirt = Skirt::Rubber, .rear = Rear::Drums, .turret = TurretShape::Welded, .turret_at = -0.04f, .turret_r = 0.2f,
     .turret_h = 5.0f, .bustle = 0.08f, .gun = 0.86f, .gun_w = 1.8f, .evacuator = 0.5f, .eyebrows = false, .era = EraKind::Soviet,
     .camo = Camo::None, .paint = {146, 134, 94, 255}, .camo1 = {146, 134, 94, 255}, .camo2 = {146, 134, 94, 255}},
};
static_assert(std::size(kTankLooks) == static_cast<size_t>(engine::VehicleModel::Bmp2));  // the tanks, then the IFVs and APCs

// The real IFVs and APCs, SPGs and AA guns of both alliances, drawn as the
// tanks are, each from what sets it apart: tracks or wheels, the shape of
// its hull, its hatches, doors and windows, its turret or weapon station,
// its gun and its missiles, its radars, its paint; the hull and the turret apart.
enum class HullShape : uint8_t {
    Bmp,      // BMP-1, BMP-2: low, the long ribbed nose, the engine at the front right, two doors at the back
    Bmp3,     // BMP-3, ZBD-04A: a blunter nose, the back raised over the engine
    Box,      // M113, Boragh: a tall box, a steep front
    Bradley,  // Bradley, Marder, Type 89, K21: tall, the glacis sloped over the engine, armored skirts
    Mtlb,     // MT-LB: long, flat and low, a cab across the front
    Btr,      // BTR-82A: a boat on eight wheels, its sides leaning in, a door in the side
    Btr4,     // BTR-4E: eight wheels, the cab and the engine in front, a tall box behind
    Stryker,  // eight wheels, slab sides, the front sloped
    Ratel,    // six big wheels, a tall box, the driver's windows across the front
    Namer,    // on a Merkava's hull: the long glacis over the engine in front
    Truck,    // a truck's cab and a platform behind it (CAESAR, Pantsir)
};
enum class TurretKind : uint8_t {
    Bmp2,    // a two-man cone, the long thin 30 mm
    Bmp1,    // a small one-man cone, the short fat 73 mm with the missile's rail over it
    Bmp3,    // wide and flat: the 100 mm with the 30 mm beside it
    Zbd,     // the ZBD-04A's: the BMP-3's, armor bolted on its sides
    Btr,     // the BTR-82A's small cone
    Box,     // an angular two-man turret (Bradley, Type 89, K21, Ratel)
    Marder,  // the Marder's, low, the gun on top of it outside
    Module,  // the BTR-4E's flat module on the roof, a grenade launcher beside the gun
    Rws,     // a remote weapon station: a machine gun on a small mount
    Cupola,  // the commander's cupola, a machine gun behind a shield
    Mini,    // the MT-LB's tiny one-man turret
    SpgRound,  // a howitzer's rounded turret (2S1, 2S3), the gun raised to fire
    SpgBox,    // a howitzer's big angular turret with its bustle (M109, PzH 2000, K9, Msta-S, PLZ-05)
    OpenGun,   // the 2S7's huge gun on its open mount
    TruckGun,  // CAESAR's gun on its cradle
    Shilka,    // the wide flat turret, four barrels, the radar dish behind
    AaTwin,    // the guns outside either side, the search radar on top (Gepard, Type 87, K30, PGZ-09)
    Tunguska,  // the big turret: twin guns and missiles either side, the radar on top
    Pantsir,   // radars on top, twin guns and missile tubes either side
};
enum class Launcher : uint8_t {
    None,
    Roof,  // a tube on the roof: Konkurs (BMP-2); Barrier's pair beside the module (BTR-4E)
    Rail,  // Malyutka on its rail over the gun (BMP-1)
    Gun,   // fired through the gun (BMP-3, ZBD-04A): nothing outside
    Box,   // the twin TOW box on the turret's side (Bradley)
    Side,  // tubes on the turret's side: Milan (Marder), Jyu-MAT (Type 89)
};

struct VehicleLook {
    HullShape shape;
    float length;  // the hull, tiles
    float width;   // half of it, to the tracks' or the wheels' outer edge
    float deck;    // the roof, pixels up
    float nose;    // tiles the front's upper plate slopes back over
    float nose_z;  // where it meets the lower plate, pixels up
    float cab;     // a cab over the front of the roof, this high (the MT-LB's; its turret on it), pixels
    bool wheeled;
    int wheels;     // road wheels a side, or axles
    float wheel_r;  // pixels
    bool rollers;   // return rollers
    Skirt skirt;
    TurretKind turret;
    float turret_at;  // along the hull
    float turret_c;   // across it: off to the left (to the right below 0)
    float turret_r;   // its size, tiles
    float turret_h;   // pixels
    float gun;        // how far the gun reaches from the ring, tiles
    float gun_w;      // pixels
    Launcher launcher;
    float engine_at;  // where along the hull its engine is
    bool doors;       // two doors at the back, else a ramp
    Camo camo;
    Color paint;
    Color camo1;
    Color camo2;
};

constexpr Color kOliveDrab{86, 94, 62, 255};
constexpr Color kSand{150, 134, 98, 255};

// In the order of engine::VehicleModel, from the BMP-2.
constexpr VehicleLook kVehicleLooks[] = {
    // BMP-2: the two-man cone turret with the long 30 mm, Konkurs on its roof.
    {.shape = HullShape::Bmp, .length = 1.0f, .width = 0.25f, .deck = 8.0f, .nose = 0.36f, .nose_z = 3.5f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.5f, .rollers = true, .skirt = Skirt::Rubber, .turret = TurretKind::Bmp2,
     .turret_at = 0.02f, .turret_c = 0.0f, .turret_r = 0.15f, .turret_h = 4.4f, .gun = 0.62f, .gun_w = 1.3f,
     .launcher = Launcher::Roof, .engine_at = 0.08f, .doors = true, .camo = Camo::None, .paint = kRussianOlive,
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // BMP-1: the same hull, a small one-man turret forward, the stubby 73 mm, Malyutka on its rail.
    {.shape = HullShape::Bmp, .length = 1.0f, .width = 0.24f, .deck = 7.8f, .nose = 0.36f, .nose_z = 3.5f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.5f, .rollers = true, .skirt = Skirt::Rubber, .turret = TurretKind::Bmp1,
     .turret_at = 0.06f, .turret_c = 0.0f, .turret_r = 0.12f, .turret_h = 3.8f, .gun = 0.3f, .gun_w = 2.6f,
     .launcher = Launcher::Rail, .engine_at = 0.08f, .doors = true, .camo = Camo::None, .paint = {96, 102, 60, 255},
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // BMP-3: the blunt nose, the back raised over the engine, the flat turret with the 100 mm and the 30 mm.
    {.shape = HullShape::Bmp3, .length = 1.05f, .width = 0.26f, .deck = 8.4f, .nose = 0.24f, .nose_z = 4.5f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.7f, .rollers = false, .skirt = Skirt::Rubber, .turret = TurretKind::Bmp3,
     .turret_at = 0.1f, .turret_c = 0.0f, .turret_r = 0.17f, .turret_h = 4.0f, .gun = 0.55f, .gun_w = 2.2f,
     .launcher = Launcher::Gun, .engine_at = -0.36f, .doors = true, .camo = Camo::None, .paint = {92, 100, 58, 255},
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // BTR-82A: the boat on eight wheels in pairs, the small turret forward with its 30 mm.
    {.shape = HullShape::Btr, .length = 1.13f, .width = 0.23f, .deck = 11.0f, .nose = 0.2f, .nose_z = 6.0f, .cab = 0.0f,
     .wheeled = true, .wheels = 4, .wheel_r = 3.0f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::Btr,
     .turret_at = 0.2f, .turret_c = 0.0f, .turret_r = 0.1f, .turret_h = 3.6f, .gun = 0.5f, .gun_w = 1.2f,
     .launcher = Launcher::None, .engine_at = -0.38f, .doors = false, .camo = Camo::None, .paint = {100, 106, 62, 255},
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // MT-LB: long, flat and low, the cab across the front with the tiny turret on it.
    {.shape = HullShape::Mtlb, .length = 0.95f, .width = 0.23f, .deck = 7.0f, .nose = 0.14f, .nose_z = 4.0f, .cab = 2.6f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.6f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::Mini,
     .turret_at = 0.2f, .turret_c = -0.1f, .turret_r = 0.055f, .turret_h = 2.6f, .gun = 0.22f, .gun_w = 1.0f,
     .launcher = Launcher::None, .engine_at = 0.02f, .doors = true, .camo = Camo::None, .paint = {92, 98, 60, 255},
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // ZBD-04A: the BMP-3's turret and guns on a hull of its own, armor bolted on, digital camouflage.
    {.shape = HullShape::Bmp3, .length = 1.06f, .width = 0.26f, .deck = 8.6f, .nose = 0.24f, .nose_z = 4.6f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.7f, .rollers = true, .skirt = Skirt::Rubber, .turret = TurretKind::Zbd,
     .turret_at = 0.06f, .turret_c = 0.0f, .turret_r = 0.17f, .turret_h = 4.2f, .gun = 0.55f, .gun_w = 2.2f,
     .launcher = Launcher::Gun, .engine_at = 0.26f, .doors = true, .camo = Camo::Digital, .paint = {96, 112, 76, 255},
     .camo1 = {64, 82, 54, 255}, .camo2 = {134, 144, 106, 255}},
    // Ratel 20: six big wheels, a tall box with the driver's windows, a small turret with the 20 mm, the veld's brown.
    {.shape = HullShape::Ratel, .length = 1.06f, .width = 0.2f, .deck = 12.0f, .nose = 0.16f, .nose_z = 8.0f, .cab = 0.0f,
     .wheeled = true, .wheels = 3, .wheel_r = 3.4f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::Box,
     .turret_at = 0.06f, .turret_c = 0.0f, .turret_r = 0.11f, .turret_h = 3.6f, .gun = 0.42f, .gun_w = 1.4f,
     .launcher = Launcher::None, .engine_at = -0.38f, .doors = false, .camo = Camo::None, .paint = {150, 128, 92, 255},
     .camo1 = kSand, .camo2 = kSand},
    // Boragh: Iran's, a lengthened BMP-1's running gear under a box, a cupola with a 12.7 mm, sand.
    {.shape = HullShape::Box, .length = 1.02f, .width = 0.24f, .deck = 9.5f, .nose = 0.22f, .nose_z = 5.0f, .cab = 0.0f,
     .wheeled = false, .wheels = 7, .wheel_r = 2.3f, .rollers = true, .skirt = Skirt::Rubber, .turret = TurretKind::Cupola,
     .turret_at = 0.14f, .turret_c = 0.08f, .turret_r = 0.06f, .turret_h = 2.6f, .gun = 0.3f, .gun_w = 1.2f,
     .launcher = Launcher::None, .engine_at = 0.12f, .doors = true, .camo = Camo::None, .paint = {146, 134, 94, 255},
     .camo1 = kSand, .camo2 = kSand},
    // BTR-4E Bucephalus: eight wheels evenly, the cab and the engine in front, the module with the 30 mm and Barrier.
    {.shape = HullShape::Btr4, .length = 1.15f, .width = 0.24f, .deck = 11.5f, .nose = 0.26f, .nose_z = 7.0f, .cab = 0.0f,
     .wheeled = true, .wheels = 4, .wheel_r = 3.1f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::Module,
     .turret_at = -0.04f, .turret_c = 0.0f, .turret_r = 0.12f, .turret_h = 3.0f, .gun = 0.5f, .gun_w = 1.4f,
     .launcher = Launcher::Roof, .engine_at = 0.18f, .doors = true, .camo = Camo::None, .paint = kUkrainianGreen,
     .camo1 = kUkrainianGreen, .camo2 = kUkrainianGreen},
    // M113A3: the small aluminium box, five road wheels, the trim vane on its front, the .50 behind its shield.
    {.shape = HullShape::Box, .length = 0.74f, .width = 0.21f, .deck = 10.5f, .nose = 0.12f, .nose_z = 6.5f, .cab = 0.0f,
     .wheeled = false, .wheels = 5, .wheel_r = 2.5f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::Cupola,
     .turret_at = 0.02f, .turret_c = -0.06f, .turret_r = 0.065f, .turret_h = 2.8f, .gun = 0.3f, .gun_w = 1.2f,
     .launcher = Launcher::None, .engine_at = 0.08f, .doors = false, .camo = Camo::None, .paint = kOliveDrab,
     .camo1 = kOliveDrab, .camo2 = kOliveDrab},
    // M2A2 Bradley ODS: tall and wide, armored skirts, the turret off to the right, the 25 mm, the TOW box on its left.
    {.shape = HullShape::Bradley, .length = 0.98f, .width = 0.29f, .deck = 10.0f, .nose = 0.28f, .nose_z = 5.5f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.8f, .rollers = true, .skirt = Skirt::Full, .turret = TurretKind::Box,
     .turret_at = 0.0f, .turret_c = -0.05f, .turret_r = 0.16f, .turret_h = 5.0f, .gun = 0.44f, .gun_w = 1.6f,
     .launcher = Launcher::Box, .engine_at = 0.14f, .doors = false, .camo = Camo::ThreeTone, .paint = kNatoGreen,
     .camo1 = kNatoBrown, .camo2 = kNatoBlack},
    // Marder 1A3: long, skirts, a low turret with the 20 mm on top of it outside, Milan at its side.
    {.shape = HullShape::Bradley, .length = 1.0f, .width = 0.26f, .deck = 9.4f, .nose = 0.3f, .nose_z = 5.0f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.8f, .rollers = true, .skirt = Skirt::Full, .turret = TurretKind::Marder,
     .turret_at = 0.0f, .turret_c = 0.0f, .turret_r = 0.14f, .turret_h = 3.6f, .gun = 0.45f, .gun_w = 1.4f,
     .launcher = Launcher::Side, .engine_at = 0.14f, .doors = false, .camo = Camo::ThreeTone, .paint = kNatoGreen,
     .camo1 = kNatoBrown, .camo2 = kNatoBlack},
    // Stryker: eight wheels, slab sides, the machine gun on its remote station.
    {.shape = HullShape::Stryker, .length = 1.03f, .width = 0.22f, .deck = 11.5f, .nose = 0.2f, .nose_z = 7.0f, .cab = 0.0f,
     .wheeled = true, .wheels = 4, .wheel_r = 3.0f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::Rws,
     .turret_at = 0.02f, .turret_c = 0.05f, .turret_r = 0.06f, .turret_h = 2.6f, .gun = 0.3f, .gun_w = 1.2f,
     .launcher = Launcher::None, .engine_at = 0.2f, .doors = false, .camo = Camo::None, .paint = {82, 90, 62, 255},
     .camo1 = kOliveDrab, .camo2 = kOliveDrab},
    // Type 89: long and low, skirts, the 35 mm, Jyu-MAT at both sides of the turret, two colours.
    {.shape = HullShape::Bradley, .length = 1.0f, .width = 0.26f, .deck = 9.0f, .nose = 0.26f, .nose_z = 5.0f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.8f, .rollers = true, .skirt = Skirt::Full, .turret = TurretKind::Box,
     .turret_at = 0.02f, .turret_c = 0.0f, .turret_r = 0.16f, .turret_h = 4.6f, .gun = 0.55f, .gun_w = 1.6f,
     .launcher = Launcher::Side, .engine_at = 0.14f, .doors = true, .camo = Camo::TwoTone, .paint = {86, 98, 64, 255},
     .camo1 = {112, 94, 66, 255}, .camo2 = {112, 94, 66, 255}},
    // K21: angular, skirts, the big two-man turret with the 40 mm, three colours.
    {.shape = HullShape::Bradley, .length = 1.02f, .width = 0.27f, .deck = 9.5f, .nose = 0.3f, .nose_z = 5.5f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.9f, .rollers = false, .skirt = Skirt::Full, .turret = TurretKind::Box,
     .turret_at = 0.02f, .turret_c = 0.0f, .turret_r = 0.17f, .turret_h = 5.0f, .gun = 0.55f, .gun_w = 1.8f,
     .launcher = Launcher::None, .engine_at = 0.14f, .doors = false, .camo = Camo::ThreeTone, .paint = {78, 90, 60, 255},
     .camo1 = {114, 96, 68, 255}, .camo2 = {40, 40, 34, 255}},
    // Namer: a Merkava's hull, the long glacis, big wheels behind skirts, a weapon station on the roof.
    {.shape = HullShape::Namer, .length = 1.1f, .width = 0.29f, .deck = 10.5f, .nose = 0.36f, .nose_z = 5.0f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 3.2f, .rollers = true, .skirt = Skirt::Full, .turret = TurretKind::Rws,
     .turret_at = -0.08f, .turret_c = 0.0f, .turret_r = 0.07f, .turret_h = 2.6f, .gun = 0.32f, .gun_w = 1.3f,
     .launcher = Launcher::None, .engine_at = 0.3f, .doors = false, .camo = Camo::None, .paint = {148, 144, 120, 255},
     .camo1 = {148, 144, 120, 255}, .camo2 = {148, 144, 120, 255}},
    // 2S1 Gvozdika: the MT-LB's low hull, seven small road wheels, the rounded turret at the back with the 122 mm.
    {.shape = HullShape::Bradley, .length = 1.06f, .width = 0.23f, .deck = 7.4f, .nose = 0.2f, .nose_z = 4.6f, .cab = 0.0f,
     .wheeled = false, .wheels = 7, .wheel_r = 2.4f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::SpgRound,
     .turret_at = -0.2f, .turret_c = 0.0f, .turret_r = 0.17f, .turret_h = 5.0f, .gun = 0.52f, .gun_w = 2.2f,
     .launcher = Launcher::None, .engine_at = 0.3f, .doors = true, .camo = Camo::None, .paint = kRussianOlive,
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // 2S3 Akatsiya: a long hull, six road wheels with rollers, the big rounded turret, the 152 mm.
    {.shape = HullShape::Bradley, .length = 1.08f, .width = 0.26f, .deck = 8.4f, .nose = 0.24f, .nose_z = 4.8f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.8f, .rollers = true, .skirt = Skirt::None, .turret = TurretKind::SpgRound,
     .turret_at = -0.12f, .turret_c = 0.0f, .turret_r = 0.21f, .turret_h = 6.6f, .gun = 0.66f, .gun_w = 2.4f,
     .launcher = Launcher::None, .engine_at = 0.3f, .doors = true, .camo = Camo::None, .paint = {96, 102, 60, 255},
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // 2S19 Msta-S: a tank's hull, the huge angular turret, the long 152 mm.
    {.shape = HullShape::Bradley, .length = 1.04f, .width = 0.29f, .deck = 7.6f, .nose = 0.18f, .nose_z = 2.4f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 3.0f, .rollers = true, .skirt = Skirt::Rubber, .turret = TurretKind::SpgBox,
     .turret_at = -0.06f, .turret_c = 0.0f, .turret_r = 0.25f, .turret_h = 7.0f, .gun = 0.86f, .gun_w = 2.4f,
     .launcher = Launcher::None, .engine_at = -0.35f, .doors = false, .camo = Camo::None, .paint = {92, 100, 58, 255},
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // 2S7 Pion: a long hull with the crew's cab in front, the huge 203 mm on its open mount at the back, a spade.
    {.shape = HullShape::Mtlb, .length = 1.14f, .width = 0.27f, .deck = 7.0f, .nose = 0.16f, .nose_z = 4.0f, .cab = 2.8f,
     .wheeled = false, .wheels = 7, .wheel_r = 2.8f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::OpenGun,
     .turret_at = -0.28f, .turret_c = 0.0f, .turret_r = 0.12f, .turret_h = 3.4f, .gun = 1.2f, .gun_w = 3.0f,
     .launcher = Launcher::None, .engine_at = 0.1f, .doors = false, .camo = Camo::None, .paint = {98, 104, 58, 255},
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // PLZ-05: long, seven road wheels, the big angular turret, digital camouflage.
    {.shape = HullShape::Bradley, .length = 1.08f, .width = 0.28f, .deck = 8.2f, .nose = 0.24f, .nose_z = 4.6f, .cab = 0.0f,
     .wheeled = false, .wheels = 7, .wheel_r = 2.8f, .rollers = true, .skirt = Skirt::Full, .turret = TurretKind::SpgBox,
     .turret_at = -0.14f, .turret_c = 0.0f, .turret_r = 0.22f, .turret_h = 7.0f, .gun = 0.92f, .gun_w = 2.4f,
     .launcher = Launcher::None, .engine_at = 0.3f, .doors = true, .camo = Camo::Digital, .paint = {96, 112, 76, 255},
     .camo1 = {64, 82, 54, 255}, .camo2 = {134, 144, 106, 255}},
    // M109A6 Paladin: the tall box hull, seven road wheels, the flat-sided turret with its bustle.
    {.shape = HullShape::Box, .length = 0.94f, .width = 0.25f, .deck = 9.0f, .nose = 0.18f, .nose_z = 5.0f, .cab = 0.0f,
     .wheeled = false, .wheels = 7, .wheel_r = 2.5f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::SpgBox,
     .turret_at = -0.1f, .turret_c = 0.0f, .turret_r = 0.2f, .turret_h = 6.4f, .gun = 0.66f, .gun_w = 2.4f,
     .launcher = Launcher::None, .engine_at = 0.2f, .doors = true, .camo = Camo::ThreeTone, .paint = kNatoGreen,
     .camo1 = kNatoBrown, .camo2 = kNatoBlack},
    // PzH 2000: long and big, skirts, the huge turret far back, the long 155 mm.
    {.shape = HullShape::Bradley, .length = 1.12f, .width = 0.29f, .deck = 8.6f, .nose = 0.28f, .nose_z = 4.8f, .cab = 0.0f,
     .wheeled = false, .wheels = 7, .wheel_r = 2.9f, .rollers = true, .skirt = Skirt::Full, .turret = TurretKind::SpgBox,
     .turret_at = -0.2f, .turret_c = 0.0f, .turret_r = 0.23f, .turret_h = 7.2f, .gun = 0.96f, .gun_w = 2.4f,
     .launcher = Launcher::None, .engine_at = 0.3f, .doors = false, .camo = Camo::ThreeTone, .paint = kNatoGreen,
     .camo1 = kNatoBrown, .camo2 = kNatoBlack},
    // CAESAR: a six-wheeled truck, the 155 mm on its cradle over the back, a spade.
    {.shape = HullShape::Truck, .length = 1.16f, .width = 0.2f, .deck = 8.6f, .nose = 0.0f, .nose_z = 0.0f, .cab = 0.0f,
     .wheeled = true, .wheels = 3, .wheel_r = 3.3f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::TruckGun,
     .turret_at = -0.34f, .turret_c = 0.0f, .turret_r = 0.08f, .turret_h = 3.0f, .gun = 0.95f, .gun_w = 2.2f,
     .launcher = Launcher::None, .engine_at = 0.46f, .doors = false, .camo = Camo::ThreeTone, .paint = {150, 140, 104, 255},
     .camo1 = {112, 100, 70, 255}, .camo2 = {86, 90, 64, 255}},
    // K9 Thunder: skirts, six road wheels, the big turret, the long 155 mm.
    {.shape = HullShape::Bradley, .length = 1.06f, .width = 0.28f, .deck = 8.4f, .nose = 0.26f, .nose_z = 4.8f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.9f, .rollers = true, .skirt = Skirt::Full, .turret = TurretKind::SpgBox,
     .turret_at = -0.16f, .turret_c = 0.0f, .turret_r = 0.22f, .turret_h = 7.0f, .gun = 0.92f, .gun_w = 2.4f,
     .launcher = Launcher::None, .engine_at = 0.3f, .doors = false, .camo = Camo::ThreeTone, .paint = {78, 90, 60, 255},
     .camo1 = {114, 96, 68, 255}, .camo2 = {40, 40, 34, 255}},
    // ZSU-23-4 Shilka: a low hull, six road wheels, the wide flat turret, four barrels, the radar dish behind.
    {.shape = HullShape::Box, .length = 0.96f, .width = 0.25f, .deck = 7.2f, .nose = 0.2f, .nose_z = 4.0f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.7f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::Shilka,
     .turret_at = -0.02f, .turret_c = 0.0f, .turret_r = 0.2f, .turret_h = 5.0f, .gun = 0.36f, .gun_w = 1.0f,
     .launcher = Launcher::None, .engine_at = -0.3f, .doors = false, .camo = Camo::None, .paint = kRussianOlive,
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // 2K22 Tunguska: long, the big turret, twin 30 mm and the missiles either side, the radar on top.
    {.shape = HullShape::Bradley, .length = 1.1f, .width = 0.27f, .deck = 8.0f, .nose = 0.22f, .nose_z = 4.6f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.7f, .rollers = true, .skirt = Skirt::None, .turret = TurretKind::Tunguska,
     .turret_at = -0.04f, .turret_c = 0.0f, .turret_r = 0.21f, .turret_h = 6.0f, .gun = 0.42f, .gun_w = 1.3f,
     .launcher = Launcher::None, .engine_at = -0.34f, .doors = false, .camo = Camo::None, .paint = {92, 100, 58, 255},
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // Pantsir-S1: an eight-wheeled truck, the turret with its radars, guns and missiles over the back.
    {.shape = HullShape::Truck, .length = 1.2f, .width = 0.21f, .deck = 9.2f, .nose = 0.0f, .nose_z = 0.0f, .cab = 0.0f,
     .wheeled = true, .wheels = 4, .wheel_r = 3.3f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::Pantsir,
     .turret_at = -0.28f, .turret_c = 0.0f, .turret_r = 0.16f, .turret_h = 6.0f, .gun = 0.36f, .gun_w = 1.2f,
     .launcher = Launcher::None, .engine_at = 0.48f, .doors = false, .camo = Camo::None, .paint = {98, 104, 62, 255},
     .camo1 = kRussianOlive, .camo2 = kRussianOlive},
    // PGZ-09: skirts, the turret with the 35 mm guns outside it, the radar on top, digital camouflage.
    {.shape = HullShape::Bradley, .length = 1.08f, .width = 0.28f, .deck = 8.0f, .nose = 0.24f, .nose_z = 4.6f, .cab = 0.0f,
     .wheeled = false, .wheels = 6, .wheel_r = 2.8f, .rollers = true, .skirt = Skirt::Full, .turret = TurretKind::AaTwin,
     .turret_at = -0.04f, .turret_c = 0.0f, .turret_r = 0.2f, .turret_h = 6.0f, .gun = 0.5f, .gun_w = 1.4f,
     .launcher = Launcher::None, .engine_at = -0.34f, .doors = false, .camo = Camo::Digital, .paint = {96, 112, 76, 255},
     .camo1 = {64, 82, 54, 255}, .camo2 = {134, 144, 106, 255}},
    // Gepard: a Leopard 1's hull, the turret with the 35 mm guns outside it, the search radar behind, the tracking one in front.
    {.shape = HullShape::Bradley, .length = 1.04f, .width = 0.29f, .deck = 8.0f, .nose = 0.22f, .nose_z = 4.4f, .cab = 0.0f,
     .wheeled = false, .wheels = 7, .wheel_r = 3.0f, .rollers = true, .skirt = Skirt::Full, .turret = TurretKind::AaTwin,
     .turret_at = -0.04f, .turret_c = 0.0f, .turret_r = 0.2f, .turret_h = 6.0f, .gun = 0.52f, .gun_w = 1.4f,
     .launcher = Launcher::None, .engine_at = -0.34f, .doors = false, .camo = Camo::ThreeTone, .paint = kNatoGreen,
     .camo1 = kNatoBrown, .camo2 = kNatoBlack},
    // Type 87: a Type 74's hull, five road wheels, the same kind of turret, two colours.
    {.shape = HullShape::Bradley, .length = 0.98f, .width = 0.27f, .deck = 7.8f, .nose = 0.2f, .nose_z = 4.2f, .cab = 0.0f,
     .wheeled = false, .wheels = 5, .wheel_r = 3.2f, .rollers = false, .skirt = Skirt::Full, .turret = TurretKind::AaTwin,
     .turret_at = -0.04f, .turret_c = 0.0f, .turret_r = 0.19f, .turret_h = 5.8f, .gun = 0.5f, .gun_w = 1.4f,
     .launcher = Launcher::None, .engine_at = -0.32f, .doors = false, .camo = Camo::TwoTone, .paint = {86, 98, 64, 255},
     .camo1 = {112, 94, 66, 255}, .camo2 = {112, 94, 66, 255}},
    // K30 Biho: a K200's box hull, five road wheels, the small turret with its guns either side and the radar.
    {.shape = HullShape::Box, .length = 0.98f, .width = 0.26f, .deck = 8.6f, .nose = 0.2f, .nose_z = 5.0f, .cab = 0.0f,
     .wheeled = false, .wheels = 5, .wheel_r = 2.7f, .rollers = false, .skirt = Skirt::None, .turret = TurretKind::AaTwin,
     .turret_at = -0.06f, .turret_c = 0.0f, .turret_r = 0.17f, .turret_h = 5.0f, .gun = 0.44f, .gun_w = 1.3f,
     .launcher = Launcher::None, .engine_at = 0.24f, .doors = false, .camo = Camo::ThreeTone, .paint = {78, 90, 60, 255},
     .camo1 = {114, 96, 68, 255}, .camo2 = {40, 40, 34, 255}},
};
static_assert(std::size(kVehicleLooks) ==
              static_cast<size_t>(engine::VehicleModel::K30) + 1 - static_cast<size_t>(engine::VehicleModel::Bmp2));

bool has_vehicle_look(engine::VehicleModel m) { return m >= engine::VehicleModel::Bmp2 && m <= engine::VehicleModel::K30; }
// A self-propelled howitzer: its gun raised to fire when it's set up (its turret's second frame).
bool raises_gun(engine::VehicleModel m) { return m >= engine::VehicleModel::Gvozdika && m <= engine::VehicleModel::K9; }
const VehicleLook& vehicle_look_of(engine::VehicleModel m) {
    return kVehicleLooks[static_cast<size_t>(m) - static_cast<size_t>(engine::VehicleModel::Bmp2)];
}
// Where an IFV's turret stands: on its roof, or on the cab (the MT-LB's).
float vehicle_turret_z(const VehicleLook& l) { return l.deck + l.cab; }

// The real vehicle an armored unit is.
engine::VehicleModel model_of(const engine::Unit& u) { return engine::unit_type(u.type).model; }
const TankLook& look_of(engine::VehicleModel m) { return kTankLooks[static_cast<size_t>(m)]; }

// What a tank's drawing and an IFV's have in common: its size, its roof,
// where its turret stands and how high, its wheels.
struct Dims {
    float length;
    float width;
    float deck;
    float turret_z;
    float turret_h;
    float wheel_r;
    Vector2 ring;  // the turret's, along and across the hull
};
Dims dims_of(engine::VehicleModel m) {
    if (has_vehicle_look(m)) {
        const VehicleLook& l = vehicle_look_of(m);
        return {l.length, l.width, l.deck, vehicle_turret_z(l), l.turret_h, l.wheel_r, {l.turret_at, l.turret_c}};
    }
    const TankLook& t = look_of(m);
    return {t.length, t.width, t.deck, t.deck, t.turret_h, t.wheel_r, {t.turret_at, 0.0f}};
}
Vector2 turret_ring_of(engine::VehicleModel m) { return dims_of(m).ring; }
float track_half(engine::UnitTypeId type) {
    const engine::UnitTypeDef& def = engine::unit_type(type);
    return drawn_as_armor(def) ? (dims_of(def.model).width - 0.05f) * kVehicleScale : 0.24f * kVehicleScale;
}
Vector2 armor_size(engine::VehicleModel m) {
    const Dims d = dims_of(m);
    return {d.length, d.width};
}
// An armored vehicle's sprite sheets by its reactive armor (an IFV's kit: slat cages 1, missiles 2), wear and how far it's sunk.
int armor_variant(engine::VehicleModel model, int era, int wear, int sink) { return ((static_cast<int>(model) * 4 + era) * 6 + wear) * 5 + sink; }
Vector2 armor_muzzle(engine::VehicleModel m) {
    if (has_vehicle_look(m)) {
        const VehicleLook& l = vehicle_look_of(m);
        return {(l.turret_at + l.gun) * kVehicleScale, (vehicle_turret_z(l) + l.turret_h * 0.55f) * kVehicleScale};
    }
    const TankLook& t = look_of(m);
    return {(t.turret_at + t.gun) * kVehicleScale, (t.deck + t.turret_h * 0.66f) * kVehicleScale};
}
Vector2 armor_engine(engine::VehicleModel m) {
    if (has_vehicle_look(m)) {
        const VehicleLook& l = vehicle_look_of(m);
        return {l.engine_at * kVehicleScale, (l.deck + 1.0f) * kVehicleScale};
    }
    const TankLook& t = look_of(m);
    const float back = t.rear == Rear::Turbine || t.turret == TurretShape::Merkava ? -0.3f : -0.35f;
    return {t.length * back * kVehicleScale, (t.deck + 1.0f) * kVehicleScale};
}
Vector2 ring_at(const Frame& fr, engine::VehicleModel m) {
    const Vector2 r = turret_ring_of(m);
    return fr.at(r.x * kVehicleScale, r.y * kVehicleScale);
}

// A polygon solid with its top drawn in towards its middle by `taper`.
void poly_solid(const Frame& fr, const Vector2* pts, int n, float z0, float z1, Color color, float taper = 0.0f) {
    Vector2 mid{0.0f, 0.0f};
    for (int i = 0; i < n; ++i) mid = {mid.x + pts[i].x / static_cast<float>(n), mid.y + pts[i].y / static_cast<float>(n)};
    Vector2 top[16];
    for (int i = 0; i < n && i < 16; ++i) top[i] = {pts[i].x + (mid.x - pts[i].x) * taper, pts[i].y + (mid.y - pts[i].y) * taper};
    solid(fr, pts, top, std::min(n, 16), z0, z1, color);
}

// The paint's pattern on a top at height z: NATO's three colours, two, the Chinese digital squares.
template <typename Look>  // a TankLook, an VehicleLook
void camouflage(const Frame& fr, const Look& look, float a0, float a1, float c0, float c1, float z, uint32_t seed, int n) {
    if (look.camo == Camo::None) return;
    for (int i = 0; i < n; ++i) {
        const uint32_t hi = tile_hash(static_cast<int>(seed & 0xFFFF) + i * 13, i * 7 + 3);
        const float a = a0 + (a1 - a0) * hash_unit(hi);
        const float c = c0 + (c1 - c0) * hash_unit(hi >> 16);
        if (look.camo == Camo::Digital) {
            const float s = 0.025f + 0.015f * static_cast<float>(i % 2);
            fill_quad(fr.at(a, c, z), fr.at(a + s, c, z), fr.at(a + s, c + s, z), fr.at(a, c + s, z), i % 2 == 0 ? look.camo1 : look.camo2);
        } else {
            const Color color = look.camo == Camo::ThreeTone && i % 3 == 2 ? look.camo2 : look.camo1;
            patch(fr, a, c, 0.065f, 0.045f, z, color, hi);
        }
    }
}

// How worn a tank is: 0 whole, 1 scratched, 2 battered, 3 barely going, 4 a
// burnt-out wreck. A piece of it (a skirt plate, an ERA box) is torn off the
// more likely the more worn it is.
bool torn(uint32_t key, int wear) { return static_cast<int>(tile_hash(static_cast<int>(key & 0xFFFF), static_cast<int>(key >> 16) + 7) % 5) < wear; }
int wear_of(int32_t hp, int32_t max_hp) {
    const int32_t pct = hp * 100 / std::max(1, max_hp);
    return pct > 75 ? 0 : pct > 50 ? 1 : pct > 25 ? 2 : 3;
}

// Soot on a top: dark patches where it burned, more and bigger the more worn.
void scorch(const Frame& fr, float a0, float a1, float c0, float c1, float z, int wear, uint32_t seed) {
    for (int i = 0; i < wear * 2; ++i) {
        const uint32_t hi = tile_hash(static_cast<int>(seed & 0xFFFF) + i * 17, i * 5 + 11);
        const float a = a0 + (a1 - a0) * hash_unit(hi);
        const float c = c0 + (c1 - c0) * hash_unit(hi >> 16);
        const float r = 0.04f + 0.02f * static_cast<float>(wear) * hash_unit(hi >> 8);
        patch(fr, a, c, r, r * 0.8f, z, i % 3 == 0 ? Color{64, 44, 32, 255} : Color{28, 26, 24, 255}, hi);
    }
}

// The hull: the tracks with their road wheels (and return rollers, and the
// sprocket and idler), the skirt or the fenders, the hull and its sloped
// glacis, what's on its back, the reactive armor its side has on it, the
// side's stripe. `frame` moves the track links on.
void draw_tank_hull(const Frame& fr, const TankLook& look, int frame, int era, Color team, int wear) {
    const float a0 = -look.length * 0.5f;
    const float a1 = look.length * 0.5f;
    const float w = look.width;
    const float hw = w - 0.09f;  // the hull's side, inside the tracks
    const float near = left_is_near(fr) ? 1.0f : -1.0f;
    const Color paint = look.paint;
    const Color rubber{30, 30, 27, 255};
    const Color steel{64, 66, 58, 255};
    const bool soviet = look.era == EraKind::Soviet || look.era == EraKind::SovietK1 || look.era == EraKind::Nozh;
    const int k1 = look.era == EraKind::SovietK1 ? std::min(era, 1) : era;  // what this one takes of the line
    auto side_block = [&](float sgn, float b0, float b1, float c0, float c1, float z0, float z1, Color color, float front = 0.0f) {
        block(fr, b0, b1, sgn > 0 ? c0 : -c1, sgn > 0 ? c1 : -c0, z0, z1, color, front);
    };
    // The far track and skirt, the hull, then the near ones over it.
    side_block(-near, a0 - 0.01f, a1 - 0.03f, hw - 0.02f, w, 0.0f, 5.5f, rubber, 0.03f);
    if (look.skirt == Skirt::Full) side_block(-near, a0 + 0.02f, a1 - 0.08f, hw, w + 0.01f, 3.0f, look.deck - 0.4f, shade(paint, 0.85f));
    block(fr, a0, a1 - look.glacis, -hw, hw, 2.0f, look.deck, paint);
    block(fr, a1 - look.glacis, a1, -hw, hw, 2.0f, look.deck, shade(paint, 1.08f), look.glacis * 0.92f);
    const bool deck_under = under(fr, look.deck);  // sunk in a bog over its deck: only what stands on it shows
    if (!deck_under) {
        camouflage(fr, look, a0 + 0.05f, a1 - look.glacis - 0.05f, -hw + 0.03f, hw - 0.06f, look.deck, 0x51u + static_cast<uint32_t>(look.length * 100.0f), 5);
        scorch(fr, a0 + 0.04f, a1 - look.glacis - 0.04f, -hw + 0.03f, hw - 0.06f, look.deck, wear, 0x33u + static_cast<uint32_t>(look.length * 50.0f));
    }

    // Reactive armor on the glacis: bricks (Kontakt-1), plates (Kontakt-5, Relikt, FY), Nozh's angled rows.
    const float g0 = a1 - look.glacis;
    auto glacis_at = [&](float t, float c) { return fr.at(g0 + look.glacis * t, c, look.deck - (look.deck - 2.0f) * t * 0.9f); };
    const bool bricks = (look.era == EraKind::Soviet || look.era == EraKind::SovietK1) && k1 == 1;
    const bool plates = (look.era == EraKind::Soviet && era >= 2) || (look.era == EraKind::Fy && era >= 1);
    if (bricks) {
        for (int row = 0; row < 2; ++row) {
            for (int k = 0; k < 5; ++k) {
                const float c = -hw + 0.03f + (2.0f * hw - 0.06f) * static_cast<float>(k) / 5.0f;
                const float t = 0.2f + 0.38f * static_cast<float>(row);
                if (torn(0x100u + static_cast<uint32_t>(row * 8 + k), wear)) continue;
                if (under(fr, look.deck - (look.deck - 2.0f) * (0.34f + 0.38f * static_cast<float>(row)) * 0.9f)) continue;
                const Color brick = shade(paint, 0.95f + 0.1f * static_cast<float>((k + row) % 2));
                const Vector2 q[4] = {glacis_at(t, c), glacis_at(t, c + 0.065f), glacis_at(t + 0.28f, c + 0.065f), glacis_at(t + 0.28f, c)};
                fill_quad(q[0], q[1], q[2], q[3], brick);
                DrawLineV(q[0], q[1], lit(shade(brick, 1.4f)));
                DrawLineV(q[3], q[2], lit(shade(brick, 0.5f)));
            }
        }
    }
    if (plates || (look.era == EraKind::Nozh && era >= 1)) {
        const int n = look.era == EraKind::Nozh ? 4 : 3;
        for (int k = 0; k < n; ++k) {
            const float c = -hw + (2.0f * hw) * static_cast<float>(k) / static_cast<float>(n);
            const float cw = 2.0f * hw / static_cast<float>(n) - 0.01f;
            if (torn(0x200u + static_cast<uint32_t>(k), wear)) continue;
            if (under(fr, look.deck - (look.deck - 2.0f) * 0.45f * 0.9f)) continue;
            const Color slab = shade(paint, 1.0f + 0.08f * static_cast<float>(k % 2) + (era == 3 ? 0.06f : 0.0f));
            const Vector2 q[4] = {glacis_at(0.05f, c), glacis_at(0.05f, c + cw), glacis_at(0.85f, c + cw), glacis_at(0.85f, c)};
            fill_quad(q[0], q[1], q[2], q[3], slab);
            DrawLineV(q[0], q[1], lit(shade(slab, 1.4f)));
            DrawLineV(q[3], q[2], lit(shade(slab, 0.5f)));
            DrawLineV(q[1], q[2], lit(shade(slab, 0.6f)));
            if (look.era == EraKind::Nozh) DrawLineV(lerp(q[0], q[3], 0.5f), lerp(q[1], q[2], 0.2f), lit(shade(slab, 0.55f)));
        }
    }
    if (soviet && !deck_under) {  // the splash guard across the glacis
        DrawLineV(fr.at(g0, -hw, look.deck), glacis_at(0.5f, 0.0f), lit(shade(paint, 0.7f)));
        DrawLineV(glacis_at(0.5f, 0.0f), fr.at(g0, hw, look.deck), lit(shade(paint, 0.7f)));
    }
    if (!deck_under) disc(fr.at(g0 - 0.06f, 0.0f, look.deck), 1.4f, shade(paint, 0.62f));  // the driver's hatch
    if (!under(fr, look.deck - (look.deck - 2.0f) * 0.55f * 0.9f)) {
        for (const float c : {-hw + 0.03f, hw - 0.03f}) disc(glacis_at(0.55f, c), 1.0f, {210, 206, 170, 255});  // headlights
    }

    // Its back: fuel drums and the unditching log; a gas turbine's grilles; stowage.
    switch (look.rear) {
        case Rear::Drums: {
            if (wear >= 4) break;
            for (int k = 0; k < 4 && !deck_under; ++k) {
                const float a = a0 + 0.08f + 0.05f * static_cast<float>(k);
                DrawLineV(fr.at(a, -hw + 0.05f, look.deck), fr.at(a, hw - 0.05f, look.deck), lit(shade(paint, 0.62f)));
            }
            for (const float c : {-0.1f, 0.1f}) {
                block(fr, a0 - 0.02f, a0 + 0.06f, c - 0.075f, c + 0.075f, look.deck - 1.0f, look.deck + 3.0f, {88, 88, 70, 255}, 0.015f, 0.015f);
            }
            block(fr, a0 - 0.02f, a0 + 0.03f, -hw + 0.01f, hw - 0.01f, look.deck + 3.0f, look.deck + 5.0f, {112, 84, 54, 255}, 0.01f, 0.01f);
            break;
        }
        case Rear::Turbine: {
            block(fr, a0 + 0.02f, a0 + 0.22f, -hw + 0.05f, hw - 0.05f, look.deck, look.deck + 0.8f, shade(paint, 0.7f));
            for (int k = 0; k < 6 && !under(fr, look.deck + 0.8f); ++k) {
                const float a = a0 + 0.04f + 0.03f * static_cast<float>(k);
                DrawLineV(fr.at(a, -hw + 0.06f, look.deck + 0.8f), fr.at(a, hw - 0.06f, look.deck + 0.8f), lit({34, 34, 30, 255}));
            }
            const Vector2 bk = fr.ground_of(-1.0f, 0.0f);
            if (bk.x + bk.y > 0.0f && !under(fr, 6.0f)) {  // the exhaust in the back plate
                fill_quad(fr.at(a0, -hw * 0.6f, 3.0f), fr.at(a0, hw * 0.6f, 3.0f), fr.at(a0, hw * 0.6f, 6.0f), fr.at(a0, -hw * 0.6f, 6.0f),
                          {30, 30, 28, 255});
            }
            break;
        }
        case Rear::Plain: {
            for (int k = 0; k < 3 && !deck_under; ++k) {
                const float a = a0 + 0.06f + 0.05f * static_cast<float>(k);
                DrawLineV(fr.at(a, -hw + 0.05f, look.deck), fr.at(a, hw - 0.05f, look.deck), lit(shade(paint, 0.66f)));
            }
            block(fr, a0 - 0.02f, a0 + 0.04f, -hw + 0.03f, hw - 0.03f, look.deck - 2.0f, look.deck + 1.0f, shade(paint, 0.85f));
            break;
        }
    }
    // Fenders and their stowage boxes, where there's no skirt.
    if (look.skirt == Skirt::None) {
        for (const float sgn : {-1.0f, 1.0f}) {
            side_block(sgn, a0, a1 - 0.06f, hw, w + 0.01f, 5.5f, 6.2f, shade(paint, 0.82f));
            side_block(sgn, a0 + 0.1f, a0 + 0.36f, hw + 0.01f, w, 6.2f, 8.5f, shade(paint, 0.92f));
        }
    }

    // The near track: the links along the ground run, the sprocket at the
    // back, the idler at the front, the road wheels, the return rollers.
    const float c = near * w;
    const bool slipped = wear >= 4;
    if (!slipped) {
        side_block(near, a0 - 0.01f, a1 - 0.03f, hw - 0.02f, w, 0.0f, 5.5f, rubber, 0.03f);
        for (float a = a0 + 0.03f * static_cast<float>(frame); a < a1 - 0.04f && !under(fr, 1.4f); a += 0.06f) {
            DrawLineV(fr.at(a, c + near * 0.005f, 0.2f), fr.at(a, c + near * 0.005f, 1.4f), lit({58, 58, 52, 255}));
        }
    } else {
        // Slipped off its wheels: still wrapped round the sprocket at the back,
        // then sagging off it and lying flat along the ground beside the wheels,
        // its links showing, the loose end at the front twisted outward.
        side_block(near, a0 - 0.01f, a0 + 0.14f, hw - 0.02f, w, 0.0f, 5.5f, rubber, 0.03f);
        const float out0 = w + 0.005f;
        const float out1 = w + 0.1f;
        side_block(near, a0 + 0.12f, a1 - 0.12f, out0, out1, 0.0f, 1.2f, rubber);
        const Vector2 sag_q[4] = {fr.at(a0 + 0.12f, near * hw, 5.0f), fr.at(a0 + 0.12f, near * w, 5.0f),
                                  fr.at(a0 + 0.26f, near * out1, 1.2f), fr.at(a0 + 0.26f, near * out0, 1.2f)};
        if (!under(fr, 5.0f)) fill_quad(sag_q[0], sag_q[1], sag_q[2], sag_q[3], shade(rubber, 1.1f));  // sagging off the sprocket
        for (float a = a0 + 0.16f; a < a1 - 0.14f && !under(fr, 1.2f); a += 0.05f) {
            DrawLineV(fr.at(a, near * out0, 1.2f), fr.at(a, near * out1, 1.2f), lit({62, 62, 56, 255}));
        }
        const Vector2 end0 = fr.at(a1 - 0.12f, near * out0, 0.6f);
        const Vector2 end1 = fr.at(a1 - 0.12f, near * out1, 0.6f);
        const Vector2 tip0 = fr.at(a1 + 0.02f, near * (out1 + 0.05f), 0.3f);
        const Vector2 tip1 = fr.at(a1 + 0.0f, near * (out1 + 0.14f), 0.3f);
        if (!under(fr, 0.6f)) {
            fill_quad(end0, end1, tip1, tip0, shade(rubber, 0.9f));  // the loose end
            DrawLineV(lerp(end0, tip0, 0.5f), lerp(end1, tip1, 0.5f), lit({62, 62, 56, 255}));
        }
    }
    road_wheel(fr, a0 + 0.05f, c + near * 0.004f, 2.4f, steel);
    road_wheel(fr, a1 - 0.07f, c + near * 0.004f, 2.2f, steel);
    const float span = look.length - 0.3f;
    const float slots = static_cast<float>(look.wheels - 1) + (look.gap ? 0.6f : 0.0f);
    for (int i = 0; i < look.wheels; ++i) {
        const float t = (static_cast<float>(i) + (look.gap && i > 0 ? 0.6f : 0.0f)) / slots;
        float drop = 0.0f;
        if (wear >= 4) {  // a wreck: some wheels torn off, some hanging askew
            if (torn(0xA00u + static_cast<uint32_t>(i), 2)) continue;
            if (torn(0xB00u + static_cast<uint32_t>(i), 2)) drop = -1.2f;
        }
        road_wheel(fr, a1 - 0.15f - span * t, c + near * (0.008f + (drop < 0.0f ? 0.02f : 0.0f)), look.wheel_r, {74, 76, 64, 255}, drop);
    }
    if (look.rollers && look.skirt != Skirt::Full) {
        for (int i = 0; i < 3; ++i) {
            if (wear >= 4 && torn(0xC00u + static_cast<uint32_t>(i), 2)) continue;
            road_wheel(fr, a1 - 0.25f - (span - 0.2f) * static_cast<float>(i) / 2.0f, c + near * 0.004f, 1.0f, steel, 3.6f);
        }
    }
    for (float a = a0 + 0.04f * static_cast<float>(frame); a < (slipped ? a0 + 0.12f : a1 - 0.04f) && !under(fr, 5.5f); a += 0.08f) {
        DrawLineV(fr.at(a, near * (hw - 0.02f), 5.5f), fr.at(a, near * w, 5.5f), lit({26, 26, 24, 255}));
    }

    // The near skirt: rubber plates over the upper run, or the armored skirt
    // the full length in panels; its reactive armor; the side's stripe.
    float stripe_z0 = 4.6f;
    float stripe_z1 = 6.0f;
    if (look.skirt == Skirt::Rubber) {
        const float s0 = a0 + 0.06f;
        const float len = look.length - 0.4f;
        for (int k = 0; k < 5; ++k) {
            const float b0 = s0 + len * static_cast<float>(k) / 5.0f;
            if (torn(0x300u + static_cast<uint32_t>(k), wear)) continue;
            side_block(near, b0 + 0.004f, b0 + len / 5.0f - 0.004f, hw, w + 0.01f + 0.006f * static_cast<float>(k % 2), 4.0f, 6.6f,
                       shade(paint, 0.84f + 0.05f * static_cast<float>(k % 2)));
        }
    } else if (look.skirt == Skirt::Full) {
        const float s0 = a0 + 0.02f;
        const float len = look.length - 0.1f;
        const int panels = 6;
        for (int k = 0; k < panels; ++k) {
            const float b0 = s0 + len * static_cast<float>(k) / static_cast<float>(panels);
            if (torn(0x400u + static_cast<uint32_t>(k), wear - 1)) continue;  // armored: harder to tear off
            side_block(near, b0 + 0.003f, b0 + len / static_cast<float>(panels) - 0.003f, hw, w + 0.012f, 3.0f, look.deck - 0.4f,
                       shade(paint, 0.86f + 0.05f * static_cast<float>(k % 2)));
        }
        stripe_z0 = 4.0f;
        stripe_z1 = 5.6f;
        if (look.era == EraKind::Tusk && era >= 1) {  // TUSK: reactive tiles over the skirt, in a grid
            for (int k = 0; k < 10; ++k) {
                for (int row = 0; row < 2; ++row) {
                    if (torn(0x500u + static_cast<uint32_t>(k * 2 + row), wear)) continue;
                    const float b = a0 + 0.1f + (look.length - 0.3f) * static_cast<float>(k) / 10.0f;
                    side_block(near, b, b + (look.length - 0.3f) / 10.0f - 0.006f, hw, w + 0.024f, 3.4f + 2.2f * static_cast<float>(row),
                               5.4f + 2.2f * static_cast<float>(row), shade(paint, 0.92f + 0.08f * static_cast<float>((k + row) % 2)));
                }
            }
        }
    }
    const bool skirt_boxes = (look.era == EraKind::Soviet || look.era == EraKind::SovietK1) && k1 >= 1 && look.skirt == Skirt::Rubber;
    const bool covered = (look.era == EraKind::Soviet && era == 3) || (look.era == EraKind::Nozh && era >= 1);
    if (covered) {  // Relikt or Nozh the length of the skirt
        const int n = 8;
        for (int k = 0; k < n; ++k) {
            const float b = a0 + 0.06f + (look.length - 0.4f) * static_cast<float>(k) / static_cast<float>(n);
            if (torn(0x600u + static_cast<uint32_t>(k), wear)) continue;
            side_block(near, b, b + (look.length - 0.4f) / static_cast<float>(n) - 0.006f, hw, w + 0.025f, 3.6f, 6.9f,
                       shade(paint, 0.94f + 0.08f * static_cast<float>(k % 2)));
            if (!under(fr, 6.2f)) disc(fr.at(b + 0.02f, near * (w + 0.027f), 6.2f), 0.5f, shade(paint, 1.5f));
        }
    }
    if (skirt_boxes || covered || (look.era == EraKind::Fy && era >= 2)) {
        for (int k = 0; k < 3; ++k) {
            const float b = a1 - 0.32f + 0.085f * static_cast<float>(k);
            if (torn(0x700u + static_cast<uint32_t>(k), wear)) continue;
            side_block(near, b, b + 0.078f, hw, w + 0.03f, 3.4f, 7.2f, shade(paint, 0.96f + 0.08f * static_cast<float>(k % 2)));
            if (!under(fr, 6.4f)) disc(fr.at(b + 0.02f, near * (w + 0.032f), 6.4f), 0.5f, shade(paint, 1.5f));
            if (!under(fr, 4.2f)) disc(fr.at(b + 0.058f, near * (w + 0.032f), 4.2f), 0.5f, shade(paint, 0.5f));
        }
    }
    const float sc = near * (w + (covered ? 0.03f : look.skirt == Skirt::None ? -0.08f : 0.016f));
    if (wear < 4 && !under(fr, stripe_z1)) fill_quad(fr.at(a0 + 0.2f, sc, stripe_z0), fr.at(a0 + 0.5f, sc, stripe_z0), fr.at(a0 + 0.5f, sc, stripe_z1),
              fr.at(a0 + 0.2f, sc, stripe_z1), team);
}

// The turret: its body by its shape, the side's band round it, the reactive
// armor its side has on it, sights, cupolas and machine guns, smoke
// grenade launchers, stowage; the gun with its sleeve, fume extractor and
// muzzle. The whip aerial is added after, a pixel thin (see bake_sprites).
Vector2 aerial_foot(const TankLook& look) { return {-look.turret_r * 0.6f, -look.turret_r * 0.45f}; }

void draw_tank_turret(const Frame& tf, const TankLook& look, Color team, int era, int wear, float pitch = 0.0f) {
    const float z0 = look.deck;
    const float r = look.turret_r;
    const float h = look.turret_h;
    const float b = look.bustle;
    const Color paint = look.paint;
    const bool gun_front = tf.f.x + tf.f.y > 0.0f;
    const float gun_z = z0 + h * 0.66f;
    const bool western = look.turret == TurretShape::Wedge || look.turret == TurretShape::Flat ||
                         look.turret == TurretShape::Modular || look.turret == TurretShape::Leo1;
    auto gun = [&] {
        if (tf.sink > 0.0f && gun_z <= tf.sink) return;  // under the water
        const float g0 = r * 0.8f;
        const Color tube = shade(paint, 0.6f);
        // Laid as it's aimed; a burnt-out wreck's gun hanging down to the ground.
        const Laid lay = laid(g0, look.gun, wear >= 4 ? 0.0f : pitch);
        const float end = lay.a1;
        const float sag = wear >= 4 ? -(gun_z - 1.5f) : lay.rise;
        barrel(tf, g0, end, 0.0f, gun_z, sag, look.gun_w, tube);
        for (const float k : {0.3f, 0.6f, 0.82f}) {  // the thermal sleeve's bands
            const float a = g0 + (end - g0) * k;
            const float z = gun_z + sag * k;
            DrawLineEx(tf.at(a - 0.015f, 0.0f, z), tf.at(a + 0.015f, 0.0f, z), look.gun_w + 0.8f, lit(shade(paint, 0.48f)));
        }
        if (look.evacuator >= 0.0f) {
            const float a = g0 + (end - g0) * look.evacuator;
            const float z = gun_z + sag * look.evacuator;
            DrawLineEx(tf.at(a - 0.035f, 0.0f, z), tf.at(a + 0.035f, 0.0f, z), look.gun_w + 1.4f, lit(shade(paint, 0.72f)));
        }
    };
    if (!gun_front) gun();
    const Color low = shade(paint, 0.95f);
    const Color roof = shade(paint, 1.06f);
    auto layers = [&](const Vector2* pts, int n, float taper) {
        poly_solid(tf, pts, n, z0, z0 + h * 0.35f, low);
        poly_solid(tf, pts, n, z0 + h * 0.35f, z0 + h * 0.6f, team);
        poly_solid(tf, pts, n, z0 + h * 0.6f, z0 + h, roof, taper);
    };
    switch (look.turret) {
        case TurretShape::Dome: {
            round_solid(tf, 0.0f, 0.0f, r, r * 0.92f, z0, z0 + h * 0.35f, low, 0.0f, 12);
            round_solid(tf, 0.0f, 0.0f, r, r * 0.92f, z0 + h * 0.35f, z0 + h * 0.6f, team, 0.02f, 12);
            round_solid(tf, 0.0f, 0.0f, r * 0.98f, r * 0.9f, z0 + h * 0.6f, z0 + h, roof, 0.45f, 12);
            if (look.eyebrows) {  // BDD armor over the front, either side of the gun
                for (const float sgn : {-1.0f, 1.0f}) {
                    const Vector2 p[4] = {{r * 0.95f, sgn * r * 0.2f}, {r * 0.55f, sgn * r * 0.85f}, {r * 0.35f, sgn * r * 0.8f}, {r * 0.7f, sgn * r * 0.15f}};
                    poly_solid(tf, p, 4, z0 + h * 0.2f, z0 + h * 0.75f, shade(paint, 1.02f), 0.1f);
                }
            }
            break;
        }
        case TurretShape::Cast: {
            round_solid(tf, 0.0f, 0.0f, r, r * 0.875f, z0, z0 + h * 0.34f, low, 0.0f, 10);
            round_solid(tf, 0.0f, 0.0f, r, r * 0.875f, z0 + h * 0.34f, z0 + h * 0.66f, team, 0.01f, 10);
            round_solid(tf, 0.0f, 0.0f, r * 0.985f, r * 0.86f, z0 + h * 0.66f, z0 + h, roof, 0.3f, 10);
            break;
        }
        case TurretShape::Welded: {
            const Vector2 p[8] = {{r, r * 0.45f}, {r * 0.6f, r}, {-r * 0.75f, r * 0.95f}, {-r, r * 0.6f},
                                  {-r, -r * 0.6f}, {-r * 0.75f, -r * 0.95f}, {r * 0.6f, -r}, {r, -r * 0.45f}};
            layers(p, 8, 0.12f);
            block(tf, -r - b, -r + 0.01f, -r * 0.7f, r * 0.7f, z0 + h * 0.2f, z0 + h * 0.85f, shade(paint, 0.9f));  // the box on its back
            break;
        }
        case TurretShape::Wedge: {
            const Vector2 p[7] = {{r * 1.35f, 0.0f}, {r * 0.55f, r * 0.95f}, {-r, r * 0.95f}, {-r - b, r * 0.75f},
                                  {-r - b, -r * 0.75f}, {-r, -r * 0.95f}, {r * 0.55f, -r * 0.95f}};
            layers(p, 7, 0.08f);
            DrawLineV(tf.at(r * 0.55f, -r * 0.9f, z0 + h), tf.at(r * 0.55f, r * 0.9f, z0 + h), lit(shade(paint, 0.62f)));  // the wedge's seam
            break;
        }
        case TurretShape::Flat: {
            const Vector2 p[8] = {{r * 1.05f, r * 0.35f}, {r * 0.75f, r}, {-r * 0.8f, r}, {-r - b, r * 0.85f},
                                  {-r - b, -r * 0.85f}, {-r * 0.8f, -r}, {r * 0.75f, -r}, {r * 1.05f, -r * 0.35f}};
            layers(p, 8, 0.06f);
            for (int k = 0; k < 4; ++k) {  // the bustle rack
                const float a = -r - b + 0.02f + (b + 0.1f) * static_cast<float>(k) / 3.0f;
                DrawLineV(tf.at(a, -r * 0.8f, z0 + h + 1.5f), tf.at(a, r * 0.8f, z0 + h + 1.5f), lit(shade(paint, 0.55f)));
            }
            DrawLineV(tf.at(-r - b + 0.02f, -r * 0.8f, z0 + h + 1.5f), tf.at(-r * 0.6f, -r * 0.8f, z0 + h + 1.5f), lit(shade(paint, 0.55f)));
            DrawLineV(tf.at(-r - b + 0.02f, r * 0.8f, z0 + h + 1.5f), tf.at(-r * 0.6f, r * 0.8f, z0 + h + 1.5f), lit(shade(paint, 0.55f)));
            break;
        }
        case TurretShape::Modular: {
            const Vector2 p[6] = {{r, r * 0.82f}, {-r * 0.9f, r * 0.92f}, {-r - b, r * 0.7f}, {-r - b, -r * 0.7f},
                                  {-r * 0.9f, -r * 0.92f}, {r, -r * 0.82f}};
            layers(p, 6, 0.08f);
            for (const float k : {0.45f, -0.1f}) {  // the modules' seams
                DrawLineV(tf.at(r * k, -r * 0.85f, z0 + h), tf.at(r * k, r * 0.85f, z0 + h), lit(shade(paint, 0.62f)));
            }
            break;
        }
        case TurretShape::Merkava: {
            const Vector2 p[7] = {{r * 1.7f, 0.0f}, {r * 0.4f, r * 0.95f}, {-r, r * 0.95f}, {-r - b, r * 0.8f},
                                  {-r - b, -r * 0.8f}, {-r, -r * 0.95f}, {r * 0.4f, -r * 0.95f}};
            layers(p, 7, 0.1f);
            for (int k = 0; k < 7 && !under(tf, z0 + 0.5f); ++k) {  // the chains hanging under the bustle, a ball at the end of each
                const float c = -r * 0.75f + r * 1.5f * static_cast<float>(k) / 6.0f;
                const Vector2 top = tf.at(-r - b + 0.01f, c, z0 + 0.5f);
                DrawLineV(top, {top.x, top.y + 3.0f}, lit({52, 52, 46, 255}));
                disc({top.x, top.y + 3.5f}, 0.8f, {60, 60, 52, 255});
            }
            break;
        }
        case TurretShape::Leo1: {
            const Vector2 p[8] = {{r * 1.1f, r * 0.3f}, {r * 0.7f, r * 0.95f}, {-r * 0.6f, r * 0.95f}, {-r - b, r * 0.6f},
                                  {-r - b, -r * 0.6f}, {-r * 0.6f, -r * 0.95f}, {r * 0.7f, -r * 0.95f}, {r * 1.1f, -r * 0.3f}};
            layers(p, 8, 0.1f);
            break;
        }
    }
    camouflage(tf, look, -r * 0.8f, r * 0.6f, -r * 0.7f, r * 0.6f, z0 + h, 0x77u + static_cast<uint32_t>(r * 1000.0f), 3);
    scorch(tf, -r * 0.8f, r * 0.6f, -r * 0.7f, r * 0.6f, z0 + h, wear, 0x99u + static_cast<uint32_t>(r * 700.0f));

    // Reactive armor on the turret, by its kind and how far its side has got.
    const int k1 = look.era == EraKind::SovietK1 ? std::min(era, 1) : era;
    for (const float sgn : {-1.0f, 1.0f}) {
        // Smoke grenade launchers forward on either side; a stowage box towards the back.
        for (int k = 0; k < 3 && !under(tf, z0 + h * 0.66f); ++k) {
            disc(tf.at(r * 0.1f - 0.03f * static_cast<float>(k), sgn * r * 0.95f, z0 + h * 0.66f), 0.9f, {50, 52, 44, 255});
        }
        if (!western) block(tf, -r * 0.85f, -r * 0.25f, sgn > 0 ? r * 0.75f : -r, sgn > 0 ? r : -r * 0.75f, z0 + 1.0f, z0 + h * 0.75f, shade(paint, 0.9f));
        const bool wedges = (look.era == EraKind::Soviet && era >= 2) && !torn(0x800u + (sgn > 0 ? 1u : 0u), wear);
        if (wedges) {  // Kontakt-5's (and Relikt's) wedges, their bricks in chevrons
            const Vector2 base[4] = {{r, sgn * 0.035f}, {r * 0.65f, sgn * r * 0.85f}, {r * 0.1f, sgn * r * 0.95f}, {r * 0.3f, sgn * 0.035f}};
            const Vector2 top[4] = {{r * 0.85f, sgn * 0.035f}, {r * 0.5f, sgn * r * 0.75f}, {r * 0.15f, sgn * r * 0.8f}, {r * 0.3f, sgn * 0.035f}};
            solid(tf, base, top, 4, z0 + h * 0.25f, z0 + h * 0.96f, shade(paint, 1.04f));
            for (int k = 0; k < 3; ++k) {
                const float d = 0.035f * static_cast<float>(k);
                DrawLineV(tf.at(r * 0.85f - d, sgn * 0.04f, z0 + h * 0.96f), tf.at(r * 0.55f - d, sgn * r * 0.8f, z0 + h * 0.96f), lit(shade(paint, 1.45f)));
                DrawLineV(tf.at(r * 0.78f - d, sgn * 0.04f, z0 + h * 0.96f), tf.at(r * 0.48f - d, sgn * r * 0.8f, z0 + h * 0.96f), lit(shade(paint, 0.55f)));
            }
            if (era == 3) {  // Relikt: modules down the sides as well
                block(tf, -r * 0.5f, r * 0.1f, sgn > 0 ? r * 0.8f : -r * 1.08f, sgn > 0 ? r * 1.08f : -r * 0.8f, z0 + h * 0.3f, z0 + h * 0.92f, shade(paint, 1.02f));
            }
        }
        if (look.era == EraKind::Nozh && era >= 1) {  // Nozh: flat angled modules across the front and down the sides (Duplet thicker)
            const float t = era >= 2 ? 0.05f : 0.035f;
            const Vector2 p[4] = {{r * 1.02f, sgn * 0.035f}, {r * 0.6f, sgn * r * 0.98f}, {r * 0.6f - t, sgn * (r * 0.98f - t)}, {r * 1.02f - t, sgn * 0.035f}};
            poly_solid(tf, p, 4, z0 + h * 0.2f, z0 + h * 0.9f, shade(paint, 1.03f));
            block(tf, -r * 0.6f, r * 0.55f, sgn > 0 ? r * 0.85f : -r * 1.05f, sgn > 0 ? r * 1.05f : -r * 0.85f, z0 + h * 0.25f, z0 + h * 0.85f, shade(paint, 0.98f));
        }
        if (look.era == EraKind::Fy && era >= 1) {  // FY: two rows of boxes on the wedge's faces
            for (int row = 0; row < 2; ++row) {
                for (int k = 0; k < 3; ++k) {
                    const float t = 0.15f + 0.25f * static_cast<float>(k);
                    const float a = r * 1.35f - (r * 0.8f) * t;
                    const float c = sgn * r * 0.95f * t;
                    block(tf, a - 0.02f, a + 0.02f, c - 0.02f, c + 0.02f, z0 + h * (0.3f + 0.3f * static_cast<float>(row)),
                          z0 + h * (0.55f + 0.3f * static_cast<float>(row)), shade(paint, 1.02f + 0.06f * static_cast<float>(k % 2)));
                }
            }
        }
    }
    if ((look.era == EraKind::Soviet || look.era == EraKind::SovietK1) && k1 == 1) {
        // Kontakt-1: rows of small boxes round the front of the turret roof.
        for (int k = 0; k < 9; ++k) {
            const float t = -1.25f + 2.5f * static_cast<float>(k) / 8.0f;
            if (std::fabs(t) < 0.18f) continue;  // the gun
            const float a = std::cos(t) * r * 0.75f;
            const float c = std::sin(t) * r * 0.7f;
            if (torn(0x900u + static_cast<uint32_t>(k), wear)) continue;
            block(tf, a - 0.025f, a + 0.025f, c - 0.022f, c + 0.022f, z0 + h * 0.7f, z0 + h * 1.12f, shade(paint, 1.0f + 0.08f * static_cast<float>(k % 2)));
        }
    }
    // Hatches, sights, machine guns.
    if (western) {
        block(tf, -r * 0.35f, -r * 0.05f, r * 0.3f, r * 0.62f, z0 + h, z0 + h + 1.2f, shade(paint, 0.9f));  // the commander's hatch
        const Vector2 post = tf.at(-r * 0.1f, -r * 0.45f, z0 + h);
        DrawLineV(post, {post.x, post.y - 2.5f}, lit(shade(paint, 0.6f)));  // the commander's sight on its post
        block(tf, -r * 0.2f, 0.0f, -r * 0.58f, -r * 0.32f, z0 + h + 2.2f, z0 + h + 4.2f, shade(paint, 0.85f));
        block(tf, r * 0.25f, r * 0.55f, -r * 0.8f, -r * 0.5f, z0 + h, z0 + h + 1.6f, shade(paint, 0.85f));  // the gunner's sight
        if (look.turret == TurretShape::Flat) {  // the loader's machine gun (and TUSK's shield)
            DrawLineEx(tf.at(-r * 0.3f, -r * 0.6f, z0 + h + 2.5f), tf.at(r * 0.1f, -r * 0.6f, z0 + h + 2.8f), 1.0f, lit({34, 34, 32, 255}));
            if (era >= 1) block(tf, 0.0f, 0.02f, -r * 0.8f, -r * 0.4f, z0 + h, z0 + h + 3.5f, shade(paint, 0.85f));
        }
    } else {
        round_solid(tf, -r * 0.3f, r * 0.35f, 0.05f, 0.05f, z0 + h, z0 + h + 1.5f, shade(paint, 0.94f), 0.2f, 10);  // the cupola
        DrawLineEx(tf.at(-r * 0.3f, r * 0.35f, z0 + h + 2.2f), tf.at(r * 0.35f, r * 0.35f, z0 + h + 2.5f), 1.0f, lit({34, 34, 32, 255}));  // its MG
        block(tf, 0.0f, r * 0.35f, -r * 0.55f, -r * 0.25f, z0 + h, z0 + h + 2.0f, shade(paint, 0.88f));  // the gunner's sight
        fill_quad(tf.at(r * 0.35f, -r * 0.52f, z0 + h + 0.5f), tf.at(r * 0.35f, -r * 0.28f, z0 + h + 0.5f),
                  tf.at(r * 0.35f, -r * 0.28f, z0 + h + 1.6f), tf.at(r * 0.35f, -r * 0.52f, z0 + h + 1.6f), {70, 96, 110, 255});
    }
    const Vector2 af = aerial_foot(look);
    disc(tf.at(af.x, af.y, z0 + h), 1.0f, shade(paint, 0.6f));  // the aerial's base
    if (gun_front) gun();
}

// --- IFVs and APCs in pixel art ------------------------------------------------------

// Where a wheeled one's axles are along the hull: in pairs with a gap (the
// BTR-82A's, the Stryker's), the Ratel's three, else evenly.
float axle_at(const VehicleLook& look, int i) {
    const float h = look.length * 0.5f;
    switch (look.shape) {
        case HullShape::Btr: {
            constexpr float k[4] = {0.7f, 0.34f, -0.36f, -0.72f};
            return h * k[i % 4];
        }
        case HullShape::Stryker: {
            constexpr float k[4] = {0.68f, 0.36f, -0.42f, -0.72f};
            return h * k[i % 4];
        }
        case HullShape::Ratel: {
            constexpr float k[3] = {0.62f, -0.32f, -0.7f};
            return h * k[i % 3];
        }
        default: return h * (0.7f - 1.42f * static_cast<float>(i) / static_cast<float>(std::max(1, look.wheels - 1)));
    }
}

// A big tyre standing along the hull at (a, c), `r` pixels high: the
// rubber, its tread's face, the dished rim in the vehicle's paint, the hub;
// burnt out, the bare rim alone, down on the ground.
void tyre(const Frame& fr, float a, float c, float r, Color paint, bool burnt, float lift = 0.0f, float turn = 0.0f) {
    const float rr = (burnt ? r * 0.62f : r) + lift;  // how high its middle is
    if (fr.sink > 0.0f && rr * 2.0f <= fr.sink) return;  // sunk out of sight
    constexpr int kPoints = 16;
    const float ra = r / 32.0f;
    const Vector2 mid = fr.at(a, c, rr);
    auto ring = [&](float k, Color color) {
        Vector2 prev{};
        for (int i = 0; i <= kPoints; ++i) {
            const float t = static_cast<float>(i) * 6.2831853f / kPoints;
            const Vector2 p = fr.at(a + std::cos(t) * ra * k, c, rr + std::sin(t) * r * k);
            if (i > 0) fill_triangle(mid, prev, p, color);
            prev = p;
        }
    };
    if (!burnt) {
        ring(1.0f, {24, 24, 22, 255});
        ring(0.84f, {42, 42, 38, 255});  // the tread's face
        ring(0.58f, shade(paint, 0.72f));  // the rim
    } else {
        ring(0.62f, {70, 46, 34, 255});  // the bare rim, burnt and rusting
    }
    ring(0.36f, burnt ? Color{96, 62, 40, 255} : shade(paint, 1.05f));
    ring(0.14f, {30, 30, 28, 255});
    if (!burnt) {  // the tread's blocks round its edge, turned as it rolls
        for (int i = 0; i < 8; ++i) {
            const float t = turn + static_cast<float>(i) * 0.785f;
            DrawPixelV(fr.at(a + std::cos(t) * ra * 0.92f, c, rr + std::sin(t) * r * 0.92f), lit({62, 62, 56, 255}));
        }
    }
}

// How a hull's shape cuts its box: its lower front plate (tiles back under
// the nose), its sides narrowing to a boat's keel and leaning in over it,
// its roof drawn in at the back.
struct HullCut {
    float lower;
    float keel;
    float lean;
    float back;
};
HullCut cut_of(HullShape s) {
    switch (s) {
        case HullShape::Bmp: return {0.2f, 0.0f, 0.02f, 0.02f};
        case HullShape::Bmp3: return {0.12f, 0.0f, 0.02f, 0.01f};
        case HullShape::Box: return {0.04f, 0.0f, 0.0f, 0.01f};
        case HullShape::Bradley: return {0.08f, 0.0f, 0.0f, 0.01f};
        case HullShape::Mtlb: return {0.08f, 0.0f, 0.0f, 0.01f};
        case HullShape::Btr: return {0.18f, 0.06f, 0.05f, 0.1f};
        case HullShape::Btr4: return {0.1f, 0.04f, 0.02f, 0.02f};
        case HullShape::Stryker: return {0.08f, 0.02f, 0.0f, 0.01f};
        case HullShape::Ratel: return {0.1f, 0.02f, 0.01f, 0.02f};
        case HullShape::Namer: return {0.08f, 0.0f, 0.02f, 0.02f};
    }
    return {};
}

// The hull: the far track or wheels; the hull, its lower plate and its
// upper one (a boat's keel and leaning sides); on the roof the engine's
// grilles, the hatches, the driver's periscopes or windows, a BMP's ribbed
// nose, a trim vane; its doors or ramp, a BTR's side door, the side's
// stripe; the near track with its wheels, or the near tyres; the skirts;
// slat cages over it once researched (`kit` & 1). Worn, pieces torn off;
// burnt out, the tyres gone to the rims, a track slipped off.
void draw_truck_chassis(const Frame& fr, const VehicleLook& look, Color team, int wear);  // with the trucks, below

void draw_vehicle_hull(const Frame& fr, const VehicleLook& look, int frame, int kit, Color team, int wear) {
    if (look.shape == HullShape::Truck) {
        draw_truck_chassis(fr, look, team, wear);
        return;
    }
    const float a0 = -look.length * 0.5f;
    const float a1 = look.length * 0.5f;
    const float w = look.width;
    const bool tracked = !look.wheeled;
    const float hw = tracked ? w - 0.08f : w - 0.03f;  // the hull's side: inside the tracks, over the wheels
    const float near = left_is_near(fr) ? 1.0f : -1.0f;
    const Color paint = look.paint;
    const Color rubber{30, 30, 27, 255};
    const Color steel{64, 66, 58, 255};
    const bool burnt = wear >= 4;
    const float track_top = look.wheel_r * 1.9f;
    const float belly = tracked ? 2.0f : look.wheel_r;
    const HullCut cut = cut_of(look.shape);
    const float tw = hw - cut.lean;  // the roof's half width
    const float roof0 = a0 + cut.back;
    const float roof1 = a1 - look.nose;
    const uint32_t seed = static_cast<uint32_t>(look.length * 1000.0f) + static_cast<uint32_t>(look.deck * 10.0f);
    auto side_block = [&](float sgn, float b0, float b1, float c0, float c1, float z0, float z1, Color color, float front = 0.0f) {
        block(fr, b0, b1, sgn > 0 ? c0 : -c1, sgn > 0 ? c1 : -c0, z0, z1, color, front);
    };
    auto facing = [&](float na, float nc) {
        const Vector2 g = fr.ground_of(na, nc);
        return g.x + g.y > 0.02f;
    };
    // On the upper front plate: t from its lower edge (0) up to the roof (1).
    auto glacis_z = [&](float t) { return look.nose_z + (look.deck - look.nose_z) * t; };
    auto glacis = [&](float t, float c) { return fr.at(a1 - look.nose * t, c, glacis_z(t)); };
    // A window (or its armored cover) on the upper front plate, from t0 to t1 up it.
    auto window = [&](float t0, float t1, float c0, float c1, Color glass) {
        if (under(fr, glacis_z(t1))) return;
        const Vector2 q[4] = {glacis(t0, c0), glacis(t0, c1), glacis(t1, c1), glacis(t1, c0)};
        fill_quad(q[0], q[1], q[2], q[3], glass);
        DrawLineV(q[3], q[2], lit(shade(paint, 0.5f)));
        DrawLineV(q[0], q[1], lit(shade(glass, 1.35f)));
    };

    // The far track or wheels.
    if (tracked) {
        side_block(-near, a0 + 0.01f, a1 - 0.03f, hw - 0.01f, w, 0.0f, track_top, rubber, 0.03f);
    } else {
        for (int i = 0; i < look.wheels; ++i) tyre(fr, axle_at(look, i), -near * (w - 0.05f), look.wheel_r, paint, burnt);
    }

    // The hull: the lower part, its front plate sloping back under the nose
    // (a boat's sides in towards its keel); the upper, its glacis, its sides
    // leaning in over the wheels (a BTR's), its back drawn in.
    {
        const float kw = hw - cut.keel;
        const Vector2 base[4] = {{a1 - cut.lower, kw}, {a1 - cut.lower, -kw}, {a0 + 0.02f, -kw}, {a0 + 0.02f, kw}};
        const Vector2 top[4] = {{a1, hw}, {a1, -hw}, {a0, -hw}, {a0, hw}};
        solid(fr, base, top, 4, belly, look.nose_z, shade(paint, 0.9f));
    }
    {
        const Vector2 base[4] = {{a1, hw}, {a1, -hw}, {a0, -hw}, {a0, hw}};
        const Vector2 top[4] = {{roof1, tw}, {roof1, -tw}, {roof0, -tw}, {roof0, tw}};
        solid(fr, base, top, 4, look.nose_z, look.deck, paint);
    }
    const bool deck_under = under(fr, look.deck);
    if (!deck_under) {
        camouflage(fr, look, roof0 + 0.04f, roof1 - 0.04f, -tw + 0.03f, tw - 0.06f, look.deck, 0x61u + seed, 6);
        scorch(fr, roof0 + 0.03f, roof1 - 0.03f, -tw + 0.03f, tw - 0.06f, look.deck, wear, 0x43u + seed);
    }

    // What its roof and its front have by its kind.
    auto lid = [&](float a, float c, float ra, float rc, float z, Color color) {  // a round lid lying on the roof
        constexpr int kPoints = 10;
        Vector2 ring[kPoints];
        for (int i = 0; i < kPoints; ++i) {
            const float t = static_cast<float>(i) * 6.2831853f / kPoints;
            ring[i] = fr.at(a + std::cos(t) * ra, c + std::sin(t) * rc, z);
        }
        const Vector2 mid = fr.at(a, c, z);
        for (int i = 0; i < kPoints; ++i) fill_triangle(mid, ring[i], ring[(i + 1) % kPoints], color);
    };
    auto hatch = [&](float a, float c, float r) {
        if (deck_under) return;
        lid(a, c, r + 0.012f, r + 0.012f, look.deck, shade(paint, 0.58f));
        lid(a, c, r, r, look.deck, shade(paint, 1.1f));
    };
    auto rect_hatch = [&](float b0, float b1, float c0, float c1) {  // a big square lid: an M113's, a Bradley's
        if (deck_under) return;
        const Vector2 q[4] = {fr.at(b0, c0, look.deck), fr.at(b1, c0, look.deck), fr.at(b1, c1, look.deck), fr.at(b0, c1, look.deck)};
        fill_quad(q[0], q[1], q[2], q[3], shade(paint, 1.08f));
        for (int i = 0; i < 4; ++i) DrawLineV(q[i], q[(i + 1) % 4], lit(shade(paint, 0.6f)));
    };
    switch (look.shape) {
        case HullShape::Bmp: {
            // The ribbed nose, the trim vane folded on it, the four hatches over the men.
            for (int k = 1; k < 7; ++k) {
                const float t = static_cast<float>(k) / 7.0f;
                if (under(fr, glacis_z(t))) continue;
                DrawLineV(glacis(t, -tw + 0.02f), glacis(t, tw - 0.02f), lit(shade(paint, k % 2 == 1 ? 0.68f : 1.28f)));
            }
            window(0.06f, 0.42f, -tw + 0.05f, tw - 0.05f, shade(paint, 1.06f));  // the trim vane
            for (const float a : {roof0 + 0.1f, roof0 + 0.24f}) {
                for (const float c : {-tw * 0.5f, tw * 0.5f}) hatch(a, c, 0.035f);
            }
            hatch(roof1 - 0.06f, tw * 0.55f, 0.03f);  // the driver's
            break;
        }
        case HullShape::Bmp3: {
            // The back raised over the engine, two hatches before it; the driver's in the middle up front.
            block(fr, roof0, roof0 + 0.34f, -tw, tw, look.deck, look.deck + 1.4f, shade(paint, 0.97f), 0.03f);
            if (!under(fr, look.deck + 1.4f)) {
                for (int k = 0; k < 4; ++k) {  // its grilles
                    const float a = roof0 + 0.06f + 0.05f * static_cast<float>(k);
                    DrawLineV(fr.at(a, -tw + 0.05f, look.deck + 1.4f), fr.at(a, tw - 0.05f, look.deck + 1.4f), lit(shade(paint, 0.62f)));
                }
            }
            for (const float c : {-tw * 0.5f, tw * 0.5f}) hatch(roof0 + 0.42f, c, 0.035f);
            hatch(roof1 - 0.05f, 0.0f, 0.03f);
            break;
        }
        case HullShape::Box: {
            // A steep front with the trim vane over it, the big hatch over the men.
            window(0.08f, 0.85f, -tw + 0.03f, tw - 0.03f, shade(paint, 1.08f));
            if (!under(fr, glacis_z(0.5f))) DrawLineV(glacis(0.5f, -tw + 0.04f), glacis(0.5f, tw - 0.04f), lit(shade(paint, 0.7f)));
            rect_hatch(roof0 + 0.05f, roof0 + 0.24f, -tw * 0.7f, tw * 0.7f);
            hatch(roof1 - 0.05f, tw * 0.55f, 0.028f);
            break;
        }
        case HullShape::Bradley:
        case HullShape::Namer: {
            rect_hatch(roof0 + 0.04f, roof0 + 0.2f, -tw * 0.6f, tw * 0.6f);  // the cargo hatch
            hatch(roof1 - 0.05f, tw * 0.55f, 0.03f);
            if (look.shape == HullShape::Namer) {  // the commander's raised position
                block(fr, -0.06f, 0.08f, tw * 0.25f, tw * 0.75f, look.deck, look.deck + 1.6f, shade(paint, 0.95f), 0.02f, 0.02f, 0.01f);
            }
            break;
        }
        case HullShape::Mtlb: {
            // The cab across the front, its two windows; two hatches over the men behind.
            const float c0 = roof1 - 0.3f;
            block(fr, c0, roof1 + 0.02f, -tw, tw, look.deck, look.deck + look.cab, shade(paint, 1.02f), 0.04f);
            if (facing(1.0f, 0.0f) && !under(fr, look.deck + look.cab)) {
                for (const float c : {-tw * 0.55f, tw * 0.45f}) {
                    auto at = [&](float t, float cc) { return fr.at(roof1 + 0.02f - 0.04f * t, cc, look.deck + look.cab * t); };
                    fill_quad(at(0.25f, c - 0.06f), at(0.25f, c + 0.06f), at(0.8f, c + 0.06f), at(0.8f, c - 0.06f), {70, 96, 110, 255});
                }
            }
            for (const float c : {-tw * 0.5f, tw * 0.5f}) hatch(roof0 + 0.14f, c, 0.035f);
            break;
        }
        case HullShape::Btr: {
            // The driver's and the commander's windows with their armored covers down; hatches on top.
            for (const float c : {-0.06f, 0.06f}) window(0.55f, 0.92f, c - 0.045f, c + 0.045f, shade(paint, 0.82f));
            for (const float c : {-tw * 0.45f, tw * 0.45f}) hatch(-0.04f, c, 0.03f);
            break;
        }
        case HullShape::Btr4:
        case HullShape::Ratel: {
            // The cab's windows across the front.
            const int n = look.shape == HullShape::Ratel ? 3 : 2;
            for (int k = 0; k < n; ++k) {
                const float c = -tw + 0.04f + (2.0f * tw - 0.08f) * (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
                const float half = (2.0f * tw - 0.08f) / static_cast<float>(n) * 0.4f;
                window(0.45f, 0.9f, c - half, c + half, {70, 96, 110, 255});
            }
            for (const float c : {-tw * 0.45f, tw * 0.45f}) hatch(roof0 + 0.2f, c, 0.032f);
            break;
        }
        case HullShape::Stryker: {
            hatch(roof1 - 0.05f, tw * 0.55f, 0.03f);
            rect_hatch(roof0 + 0.05f, roof0 + 0.22f, -tw * 0.55f, tw * 0.55f);
            break;
        }
    }
    // The engine's grilles: over it at the front right (a BMP's, a Bradley's), or across the back.
    if (!deck_under) {
        const bool front = look.engine_at > 0.0f;
        const float c0 = front ? -tw + 0.03f : -tw + 0.05f;
        const float c1 = front ? -0.02f : tw - 0.05f;
        for (int k = 0; k < 4; ++k) {
            const float a = look.engine_at - 0.06f + 0.04f * static_cast<float>(k);
            if (a > roof1 - 0.01f || a < roof0 + 0.01f) continue;  // on the roof only
            if (look.shape == HullShape::Bmp3 && a < roof0 + 0.35f) continue;  // (its own, raised)
            DrawLineV(fr.at(a, c0, look.deck), fr.at(a, c1, look.deck), lit(shade(paint, 0.62f)));
        }
    }
    // Headlights low on the front.
    if (!under(fr, look.nose_z + 0.5f)) {
        for (const float c : {-hw + 0.03f, hw - 0.03f}) disc(fr.at(a1 + 0.005f, c, look.nose_z - 0.4f), 0.9f, {210, 206, 170, 255});
    }

    // Its back: two doors (a BMP's bulging: its fuel tanks), or the ramp with its hinge.
    if (facing(-1.0f, 0.0f) && !under(fr, look.deck - 0.8f)) {
        auto back = [&](float c, float z) {
            const float t = std::clamp((z - look.nose_z) / (look.deck - look.nose_z), 0.0f, 1.0f);
            return fr.at(a0 - 0.004f + (z > look.nose_z ? cut.back * t : 0.0f), c, z);
        };
        const float z0 = belly + 0.8f;
        const float z1 = look.deck - 0.8f;
        if (look.doors) {
            for (const float c : {-tw * 0.48f, tw * 0.48f}) {
                const float cw = tw * 0.4f;
                const Vector2 q[4] = {back(c - cw, z0), back(c + cw, z0), back(c + cw, z1), back(c - cw, z1)};
                fill_quad(q[0], q[1], q[2], q[3], shade(paint, look.shape == HullShape::Bmp ? 1.04f : 0.8f));
                for (int i = 0; i < 4; ++i) DrawLineV(q[i], q[(i + 1) % 4], lit(shade(paint, 0.55f)));
                disc(lerp(q[0], q[2], 0.5f), 0.6f, shade(paint, 0.45f));  // its handle
            }
        } else {
            const Vector2 q[4] = {back(-tw * 0.85f, z0), back(tw * 0.85f, z0), back(tw * 0.85f, z1), back(-tw * 0.85f, z1)};
            fill_quad(q[0], q[1], q[2], q[3], shade(paint, 0.84f));
            for (int i = 0; i < 4; ++i) DrawLineV(q[i], q[(i + 1) % 4], lit(shade(paint, 0.55f)));
            DrawLineV(lerp(q[0], q[3], 0.08f), lerp(q[1], q[2], 0.08f), lit(shade(paint, 0.45f)));  // the hinge
        }
    }
    // A BTR's door in the side between its second and third axles; firing ports along it.
    const float side_c = near * hw;
    if (look.shape == HullShape::Btr && !under(fr, look.deck - 1.2f)) {
        const float b0 = axle_at(look, 2) + 0.08f;
        const float b1 = axle_at(look, 1) - 0.08f;
        auto at = [&](float a, float z) {
            const float t = std::clamp((z - look.nose_z) / (look.deck - look.nose_z), 0.0f, 1.0f);
            return fr.at(a, near * (hw - cut.lean * t), z);
        };
        const Vector2 q[4] = {at(b0, look.nose_z - 1.0f), at(b1, look.nose_z - 1.0f), at(b1, look.deck - 1.2f), at(b0, look.deck - 1.2f)};
        fill_quad(q[0], q[1], q[2], q[3], shade(paint, 0.84f));
        for (int i = 0; i < 4; ++i) DrawLineV(q[i], q[(i + 1) % 4], lit(shade(paint, 0.5f)));
    }
    if ((look.shape == HullShape::Bmp || look.shape == HullShape::Btr || look.shape == HullShape::Bmp3) && !under(fr, look.nose_z + 1.5f)) {
        for (int k = 0; k < 3; ++k) {
            const float a = roof0 + 0.12f + 0.12f * static_cast<float>(k);
            disc(fr.at(a, near * (hw + 0.004f), look.nose_z + 1.5f), 0.7f, shade(paint, 0.45f));
        }
    }

    // The near track with its wheels (a wreck's slipped off them, some torn
    // off), or the near tyres; the skirts over the track.
    if (tracked) {
        const float c = near * w;
        if (!burnt) {
            side_block(near, a0 + 0.01f, a1 - 0.03f, hw - 0.01f, w, 0.0f, track_top, rubber, 0.03f);
            for (float a = a0 + 0.03f + 0.028f * static_cast<float>(frame); a < a1 - 0.05f && !under(fr, 1.2f); a += 0.055f) {
                DrawLineV(fr.at(a, c + near * 0.005f, 0.2f), fr.at(a, c + near * 0.005f, 1.2f), lit({58, 58, 52, 255}));
            }
        } else {
            side_block(near, a0 + 0.01f, a0 + 0.12f, hw - 0.01f, w, 0.0f, track_top, rubber, 0.03f);
            const float out0 = w + 0.005f;
            const float out1 = w + 0.09f;
            side_block(near, a0 + 0.1f, a1 - 0.1f, out0, out1, 0.0f, 1.0f, rubber);
            for (float a = a0 + 0.14f; a < a1 - 0.12f && !under(fr, 1.0f); a += 0.045f) {
                DrawLineV(fr.at(a, near * out0, 1.0f), fr.at(a, near * out1, 1.0f), lit({62, 62, 56, 255}));
            }
        }
        road_wheel(fr, a1 - 0.05f, c + near * 0.004f, look.wheel_r * 0.85f, steel);  // the sprocket, the idler
        road_wheel(fr, a0 + 0.05f, c + near * 0.004f, look.wheel_r * 0.8f, steel);
        const float span = look.length - 0.26f;
        for (int i = 0; i < look.wheels; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(look.wheels - 1);
            float drop = 0.0f;
            if (burnt) {
                if (torn(0xA10u + static_cast<uint32_t>(i), 2)) continue;
                if (torn(0xB10u + static_cast<uint32_t>(i), 2)) drop = -1.0f;
            }
            road_wheel(fr, a1 - 0.13f - span * t, c + near * (0.008f + (drop < 0.0f ? 0.02f : 0.0f)), look.wheel_r, {74, 76, 64, 255}, drop);
        }
        if (look.rollers && look.skirt != Skirt::Full) {
            for (int i = 0; i < 3; ++i) {
                if (burnt && torn(0xC10u + static_cast<uint32_t>(i), 2)) continue;
                road_wheel(fr, a1 - 0.22f - (span - 0.2f) * static_cast<float>(i) / 2.0f, c + near * 0.004f, 0.8f, steel, track_top - 1.8f);
            }
        }
        for (float a = a0 + 0.035f * static_cast<float>(frame); a < (burnt ? a0 + 0.1f : a1 - 0.04f) && !under(fr, track_top); a += 0.07f) {
            DrawLineV(fr.at(a, near * (hw - 0.01f), track_top), fr.at(a, near * w, track_top), lit({26, 26, 24, 255}));
        }
        // A skirt of `n` panels side by side from s0: each run of them in one
        // piece, the seams drawn on it, a torn-off panel a gap; bolts on the armored ones.
        auto skirt = [&](int n, float s0, float len, float z0, float z1, float out, uint32_t key, int tear, bool bolts) {
            auto at = [&](int k) { return s0 + len * static_cast<float>(k) / static_cast<float>(n); };
            int k = 0;
            while (k < n) {
                if (torn(key + static_cast<uint32_t>(k), tear)) {
                    ++k;
                    continue;
                }
                int e = k;
                while (e + 1 < n && !torn(key + static_cast<uint32_t>(e + 1), tear)) ++e;
                side_block(near, at(k), at(e + 1), hw, out, z0, z1, shade(paint, 0.86f));
                if (!under(fr, z1)) {
                    for (int j = k + 1; j <= e; ++j) {
                        DrawLineV(fr.at(at(j), near * out, z0 + 0.3f), fr.at(at(j), near * out, z1 - 0.2f), lit(shade(paint, 0.68f)));
                    }
                    for (int j = k; j <= e && bolts; ++j) {
                        for (const float a : {at(j) + 0.02f, at(j + 1) - 0.025f}) {
                            disc(fr.at(a, near * (out + 0.002f), (z0 + z1) * 0.5f + 0.6f), 0.45f, shade(paint, 1.4f));
                        }
                    }
                }
                k = e + 1;
            }
        };
        if (look.skirt == Skirt::Rubber) {  // flaps over the upper run
            skirt(5, a0 + 0.08f, look.length - 0.3f, track_top - 1.4f, track_top + 0.6f, w + 0.01f, 0x310u, wear, false);
        } else if (look.skirt == Skirt::Full) {  // armored panels, bolted on
            skirt(5, a0 + 0.04f, look.length - 0.1f, 2.4f, track_top + 1.8f, w + 0.012f, 0x410u, wear - 1, true);
        }
    } else {
        for (int i = 0; i < look.wheels; ++i) tyre(fr, axle_at(look, i), near * (w - 0.05f), look.wheel_r, paint, burnt, 0.0f, 0.39f * static_cast<float>(frame));
    }

    // The side's stripe: on the skirt, or on the hull's side.
    const bool on_skirt = tracked && look.skirt == Skirt::Full;
    const float sc = on_skirt ? near * (w + 0.016f) : side_c + near * 0.004f;
    const float s0 = on_skirt ? track_top - 0.4f : (tracked ? std::max(track_top, look.nose_z) + 0.5f : look.wheel_r * 2.0f + 0.8f);
    const float s1 = s0 + 1.4f;
    if (wear < 4 && !under(fr, s1)) {
        fill_quad(fr.at(a0 + 0.14f, sc, s0), fr.at(a0 + 0.38f, sc, s0), fr.at(a0 + 0.38f, sc, s1), fr.at(a0 + 0.14f, sc, s1), team);
    }

    // The 2S7's spade at the back, raised.
    if (look.turret == TurretKind::OpenGun && !under(fr, look.deck)) {
        block(fr, a0 - 0.04f, a0 + 0.01f, -0.18f, 0.18f, 2.0f, look.deck - 0.5f, shade(paint, 0.8f), 0.0f, 0.02f);
    }
    // Slat cages, once researched: bars stood off its side and its back.
    if ((kit & 1) != 0 && !burnt) {
        const Color bar{56, 58, 50, 255};
        const float z0 = tracked ? track_top + 0.6f : look.wheel_r * 1.8f;
        const float z1 = look.deck + 0.6f;
        if (!under(fr, z1)) {
            const float out = near * (w + 0.03f);
            const float b0 = a0 + 0.02f;
            const float b1 = roof1 - 0.02f;
            int k = 0;
            for (float a = b0; a <= b1; a += 0.03f, ++k) {
                if (torn(0xD00u + static_cast<uint32_t>(k), wear)) continue;
                DrawLineV(fr.at(a, out, z0), fr.at(a, out, z1), lit(shade(bar, k % 2 == 0 ? 1.0f : 1.3f)));
            }
            for (const float z : {z0 + 0.4f, z1}) DrawLineV(fr.at(b0, out, z), fr.at(b1, out, z), lit(bar));
            if (facing(-1.0f, 0.0f)) {
                const float b = a0 - 0.03f;
                for (float c = -w; c <= w; c += 0.03f) DrawLineV(fr.at(b, c, z0), fr.at(b, c, z1), lit(bar));
                for (const float z : {z0 + 0.4f, z1}) DrawLineV(fr.at(b, -w, z), fr.at(b, w, z), lit(bar));
            }
        }
    }
}

// An IFV's turret or weapon station: its body by its kind, the side's band
// round it, hatches and sights, smoke grenade launchers; its gun (the
// BMP-3's two), its missiles once researched (`kit` & 2). Burnt out, the gun
// hangs down. The whip aerial is added after, a pixel thin (see bake_sprites).
Vector2 vehicle_aerial_foot(const VehicleLook& look) { return {-look.turret_r * 0.55f, look.turret_r * 0.5f}; }

// A radar's dish standing on its post, its face towards `a` (along the turret).
void radar_dish(const Frame& tf, float a, float c, float z, float rad, float thin, bool burnt) {
    if (under(tf, z)) return;
    const Vector2 foot = tf.at(a, c, z - rad - 1.0f);
    DrawLineEx(foot, tf.at(a, c, z), 1.2f, lit({58, 60, 54, 255}));
    constexpr int kPoints = 12;
    Vector2 ring[kPoints];
    for (int i = 0; i < kPoints; ++i) {
        const float t = static_cast<float>(i) * 6.2831853f / kPoints;
        ring[i] = tf.at(a, c + std::cos(t) * rad * 0.016f, z + std::sin(t) * rad * thin);
    }
    const Vector2 mid = tf.at(a, c, z);
    const Color face = burnt ? Color{40, 38, 34, 255} : Color{150, 156, 146, 255};
    for (int i = 0; i < kPoints; ++i) fill_triangle(mid, ring[i], ring[(i + 1) % kPoints], face);
    for (int i = 0; i < kPoints; ++i) DrawLineV(ring[i], ring[(i + 1) % kPoints], lit(shade(face, 0.55f)));
}

// A search radar's flat face leaning back on its post (from `foot` up to `z0`): the Tunguska's bar, the Pantsir's panel.
void radar_panel(const Frame& tf, float a, float half, float foot, float z0, float z1, float lean, bool burnt) {
    if (under(tf, z1)) return;
    DrawLineEx(tf.at(a, 0.0f, foot), tf.at(a, 0.0f, z0), 1.2f, lit({58, 60, 54, 255}));
    const Vector2 q[4] = {tf.at(a, -half, z0), tf.at(a, half, z0), tf.at(a - lean, half, z1), tf.at(a - lean, -half, z1)};
    const Color face = burnt ? Color{40, 38, 34, 255} : Color{150, 156, 146, 255};
    fill_quad(q[0], q[1], q[2], q[3], face);
    for (int i = 0; i < 4; ++i) DrawLineV(q[i], q[(i + 1) % 4], lit({70, 72, 64, 255}));
}

// An AA gun's search radar, turning on its own: whether it has one, where
// along its turret it turns, and the radar alone at its post's foot.
bool has_radar(const VehicleLook& l) {
    return l.turret == TurretKind::Shilka || l.turret == TurretKind::AaTwin || l.turret == TurretKind::Tunguska || l.turret == TurretKind::Pantsir;
}
float radar_at(const VehicleLook& l) {
    switch (l.turret) {
        case TurretKind::Shilka: return -l.turret_r * 0.75f;
        case TurretKind::Tunguska: return -l.turret_r * 0.7f;
        default: return -l.turret_r * 0.6f;
    }
}
float radar_at_of(engine::VehicleModel m) { return has_vehicle_look(m) ? radar_at(vehicle_look_of(m)) : 0.0f; }
void draw_aa_radar(const Frame& tf, const VehicleLook& look, int wear) {
    const float z0 = vehicle_turret_z(look);
    const float r = look.turret_r;
    const float h = look.turret_h;
    const bool burnt = wear >= 4;
    switch (look.turret) {
        case TurretKind::Shilka: radar_dish(tf, 0.0f, 0.0f, z0 + h + 4.0f, 3.6f, 0.9f, burnt); break;
        case TurretKind::AaTwin: radar_dish(tf, 0.0f, 0.0f, z0 + h + 4.2f, 4.0f, 0.55f, burnt); break;
        case TurretKind::Tunguska: radar_panel(tf, 0.0f, r * 0.7f, z0 + h, z0 + h + 2.0f, z0 + h + 4.0f, 0.05f, burnt); break;
        case TurretKind::Pantsir: radar_panel(tf, 0.0f, r * 0.8f, z0 + h, z0 + h + 1.0f, z0 + h + 5.0f, 0.12f, burnt); break;
        default: break;
    }
}

// `part`: 0 all of it; 1 a howitzer's turret without its gun, 2 the gun
// alone, laid at `frame`, `recoil` of its length back in its cradle.
void draw_vehicle_turret(const Frame& tf, const VehicleLook& look, Color team, int kit, int wear, int frame = 0, bool radar = true,
                         int part = 0, float recoil = 0.0f) {
    const float z0 = vehicle_turret_z(look);
    const float r = look.turret_r;
    const float h = look.turret_h;
    const Color paint = look.paint;
    const Color low = shade(paint, 0.95f);
    const Color roof = shade(paint, 1.06f);
    const Color metal{58, 60, 54, 255};
    const Color launcher{84, 94, 62, 255};
    const bool missiles = (kit & 2) != 0 && wear < 4;
    const bool burnt = wear >= 4;
    const bool gun_front = tf.f.x + tf.f.y > 0.0f;
    float gun_z = z0 + h * 0.55f;
    float g0 = r * 0.75f;
    switch (look.turret) {
        case TurretKind::Marder:
            gun_z = z0 + h + 1.3f;  // on top, outside
            g0 = -r * 0.2f;
            break;
        case TurretKind::Cupola:
            gun_z = z0 + h + 0.9f;
            g0 = r * 0.2f;
            break;
        case TurretKind::Rws:
            gun_z = z0 + h * 0.75f;
            g0 = 0.0f;
            break;
        case TurretKind::Bmp1:
            gun_z = z0 + h * 0.45f;
            g0 = r * 0.6f;
            break;
        case TurretKind::SpgRound:
        case TurretKind::SpgBox:
            gun_z = z0 + h * 0.5f;
            g0 = r * 0.8f;
            break;
        case TurretKind::OpenGun:
            gun_z = z0 + h * 0.9f;
            g0 = -r * 1.2f;
            break;
        case TurretKind::TruckGun:
            gun_z = z0 + h * 0.8f;
            g0 = -r * 1.5f;
            break;
        case TurretKind::Shilka:
            gun_z = z0 + h * 0.62f;
            g0 = r * 0.9f;
            break;
        default: break;
    }
    const bool twin = look.turret == TurretKind::Bmp3 || look.turret == TurretKind::Zbd;
    const bool howitzer = look.turret == TurretKind::SpgRound || look.turret == TurretKind::SpgBox ||
                          look.turret == TurretKind::OpenGun || look.turret == TurretKind::TruckGun;
    // Guns outside either side (the AA guns'): the far ones before the turret, the near ones after it.
    const bool side_guns = look.turret == TurretKind::AaTwin || look.turret == TurretKind::Tunguska || look.turret == TurretKind::Pantsir;
    const float near = left_is_near(tf) ? 1.0f : -1.0f;
    auto guns_at = [&](float sgn) {
        const float z = look.turret == TurretKind::Pantsir ? z0 + h * 0.35f : z0 + h * 0.58f;
        if (under(tf, z)) return;
        const float sag = burnt ? -(z - 1.5f) * 0.6f : 0.0f;
        const float c = sgn * r * (look.turret == TurretKind::AaTwin ? 1.2f : 1.18f);
        block(tf, -r * 0.5f, r * 0.55f, sgn > 0.0f ? r * 0.98f : -c - r * 0.14f, sgn > 0.0f ? c + r * 0.14f : -r * 0.98f,
              z - 1.6f, z + 1.4f, shade(paint, 0.9f));  // its housing
        barrel(tf, r * 0.5f, r * 0.5f + look.gun, c, z, sag, look.gun_w, metal, true);
        if (look.turret != TurretKind::AaTwin) barrel(tf, r * 0.5f, r * 0.45f + look.gun, c + sgn * 0.03f, z + 0.9f, sag, look.gun_w, metal);
    };
    auto gun = [&] {
        if (tf.sink > 0.0f && gun_z <= tf.sink) return;  // under
        if (look.turret == TurretKind::Shilka) {  // four barrels in two pairs, one over the other
            const float sag = burnt ? -(gun_z - 1.5f) * 0.6f : 0.0f;
            for (const float c : {-0.035f, 0.035f}) {
                for (const float dz : {0.0f, 1.3f}) barrel(tf, g0, g0 + look.gun, c, gun_z + dz, sag, look.gun_w, metal);
            }
            return;
        }
        if (side_guns) return;
        // A howitzer's gun laid to fire (set up) for the range, thrown back
        // in its cradle after a shot (in a turret the breech goes in; on an
        // open mount it shows, going back); a burnt-out wreck's hanging down.
        const float deg = howitzer && !burnt ? kHowitzerPitch[std::clamp(frame, 0, kHowitzerFrames - 1)] : 0.0f;
        const float length = look.gun - g0;
        const float slide = howitzer && !burnt ? recoil * length : 0.0f;
        const bool open = look.turret == TurretKind::OpenGun || look.turret == TurretKind::TruckGun;
        const float u0 = open ? -slide : 0.0f;  // along it from its trunnions: where it shows from, to
        const float u1 = length - slide;
        const float ct = std::cos(deg * 0.0174533f);
        const float st = std::sin(deg * 0.0174533f) * kZPerTile;
        const float z_from = gun_z + u0 * st;
        const float sag = burnt ? -(gun_z - 1.5f) * 0.8f : (u1 - u0) * st;
        const Color tube = look.gun_w >= 2.0f ? shade(paint, 0.62f) : metal;
        const bool brake = look.turret == TurretKind::Bmp2 || look.turret == TurretKind::Btr || look.turret == TurretKind::Module || howitzer;
        barrel(tf, g0 + u0 * ct, g0 + u1 * ct, twin ? 0.012f : 0.0f, z_from, sag, look.gun_w, tube, brake);
        if (howitzer) {  // its fume extractor, a band round it
            const float band = length * 0.4f - slide;
            const float a = g0 + band * ct;
            const float z = burnt ? gun_z + sag * 0.4f : gun_z + band * st;
            DrawLineEx(tf.at(a - 0.03f, 0.0f, z), tf.at(a + 0.03f, 0.0f, z), look.gun_w + 1.2f, lit(shade(tube, 0.85f)));
        }
        if (twin) barrel(tf, g0, look.gun + 0.04f, -0.04f, gun_z + 0.3f, sag, 1.1f, metal, true);  // the 30 mm beside the 100 mm
        if (look.turret == TurretKind::Module) barrel(tf, r * 0.5f, r * 1.4f, r * 0.7f, gun_z - 0.4f, 0.0f, 1.6f, metal);  // the grenade launcher
        if (look.turret == TurretKind::Bmp1 && !burnt) {  // the Malyutka's rail over it
            DrawLineEx(tf.at(g0, 0.0f, z0 + h + 0.2f), tf.at(look.gun * 0.8f, 0.0f, z0 + h + 0.2f), 1.0f, lit(metal));
        }
    };
    if (part == 2) {  // the gun alone
        gun();
        return;
    }
    if (!gun_front && part == 0) gun();
    if (side_guns) guns_at(-near);
    auto layers = [&](const Vector2* pts, int n, float taper) {
        poly_solid(tf, pts, n, z0, z0 + h * 0.35f, low);
        poly_solid(tf, pts, n, z0 + h * 0.35f, z0 + h * 0.6f, team);
        poly_solid(tf, pts, n, z0 + h * 0.6f, z0 + h, roof, taper);
    };
    // A radar's dish standing on a post, its face towards `a` (along the turret).
    auto dish = [&](float a, float c, float z, float rad, float thin) { radar_dish(tf, a, c, z, rad, thin, burnt); };
    bool sides = true;  // smoke grenade launchers on its sides
    switch (look.turret) {
        case TurretKind::Bmp2:
        case TurretKind::Bmp1:
        case TurretKind::Btr: {  // a cone
            round_solid(tf, 0.0f, 0.0f, r, r * 0.92f, z0, z0 + h * 0.3f, low, 0.0f, 12);
            round_solid(tf, 0.0f, 0.0f, r, r * 0.92f, z0 + h * 0.3f, z0 + h * 0.55f, team, 0.03f, 12);
            round_solid(tf, 0.0f, 0.0f, r * 0.97f, r * 0.89f, z0 + h * 0.55f, z0 + h, roof, look.turret == TurretKind::Bmp1 ? 0.5f : 0.4f, 12);
            block(tf, r * 0.05f, r * 0.4f, -r * 0.55f, -r * 0.3f, z0 + h * 0.85f, z0 + h + 1.2f, shade(paint, 0.85f));  // the gunner's sight
            sides = look.turret != TurretKind::Bmp1;
            break;
        }
        case TurretKind::Bmp3:
        case TurretKind::Zbd: {
            const Vector2 p[8] = {{r, r * 0.55f},   {r * 0.55f, r},   {-r * 0.8f, r},   {-r, r * 0.65f},
                                  {-r, -r * 0.65f}, {-r * 0.8f, -r}, {r * 0.55f, -r}, {r, -r * 0.55f}};
            layers(p, 8, 0.12f);
            if (look.turret == TurretKind::Zbd) {  // armor bolted on its sides
                for (const float sgn : {-1.0f, 1.0f}) {
                    if (torn(0x820u + (sgn > 0.0f ? 1u : 0u), wear)) continue;
                    block(tf, -r * 0.6f, r * 0.5f, sgn > 0.0f ? r * 0.95f : -r * 1.12f, sgn > 0.0f ? r * 1.12f : -r * 0.95f, z0 + h * 0.15f,
                          z0 + h * 0.8f, shade(paint, 1.03f));
                }
            }
            block(tf, r * 0.1f, r * 0.45f, -r * 0.8f, -r * 0.45f, z0 + h, z0 + h + 1.8f, shade(paint, 0.86f));  // the gunner's sight
            block(tf, -r * 0.5f, -r * 0.2f, r * 0.35f, r * 0.65f, z0 + h, z0 + h + 1.2f, shade(paint, 0.9f));  // the commander's
            break;
        }
        case TurretKind::Box: {
            const float b = r * 0.35f;  // its bustle
            const Vector2 p[8] = {{r * 1.05f, r * 0.5f},  {r * 0.7f, r * 0.92f},  {-r * 0.85f, r * 0.92f},  {-r - b, r * 0.7f},
                                  {-r - b, -r * 0.7f}, {-r * 0.85f, -r * 0.92f}, {r * 0.7f, -r * 0.92f}, {r * 1.05f, -r * 0.5f}};
            layers(p, 8, 0.1f);
            block(tf, -r * 0.45f, -r * 0.1f, r * 0.25f, r * 0.6f, z0 + h, z0 + h + 1.2f, shade(paint, 0.9f));  // the commander's hatch
            block(tf, r * 0.2f, r * 0.55f, -r * 0.75f, -r * 0.45f, z0 + h, z0 + h + 1.8f, shade(paint, 0.85f));  // the gunner's sight
            break;
        }
        case TurretKind::Marder: {
            const Vector2 p[8] = {{r, r * 0.45f},   {r * 0.5f, r},   {-r * 0.7f, r},   {-r, r * 0.55f},
                                  {-r, -r * 0.55f}, {-r * 0.7f, -r}, {r * 0.5f, -r}, {r, -r * 0.45f}};
            layers(p, 8, 0.22f);
            block(tf, -r * 0.35f, r * 0.25f, -r * 0.22f, r * 0.22f, z0 + h, z0 + h + 1.6f, shade(paint, 0.9f));  // the gun's mount on the roof
            break;
        }
        case TurretKind::Module: {
            const Vector2 p[6] = {{r * 1.1f, r * 0.45f}, {-r * 0.8f, r * 0.9f}, {-r, r * 0.6f},
                                  {-r, -r * 0.6f},       {-r * 0.8f, -r * 0.9f}, {r * 1.1f, -r * 0.45f}};
            layers(p, 6, 0.12f);
            block(tf, r * 0.1f, r * 0.45f, -r * 0.85f, -r * 0.5f, z0 + h, z0 + h + 1.8f, shade(paint, 0.85f));  // the sight
            break;
        }
        case TurretKind::Rws: {
            round_solid(tf, 0.0f, 0.0f, r, r, z0, z0 + h * 0.35f, team, 0.0f, 10);  // its ring in the side's colour
            block(tf, -r * 0.7f, r * 0.7f, -r * 0.6f, r * 0.6f, z0 + h * 0.35f, z0 + h * 0.95f, low, 0.1f);  // the mount
            block(tf, -r * 0.5f, r * 0.3f, r * 0.6f, r * 1.2f, z0 + h * 0.45f, z0 + h * 0.9f, shade(paint, 0.8f));  // the ammunition box
            block(tf, 0.0f, r * 0.55f, -r * 1.2f, -r * 0.6f, z0 + h * 0.6f, z0 + h * 1.25f, shade(paint, 0.7f));  // the sight
            sides = false;
            break;
        }
        case TurretKind::Cupola: {
            round_solid(tf, 0.0f, 0.0f, r, r, z0, z0 + h * 0.5f, team, 0.0f, 10);
            round_solid(tf, 0.0f, 0.0f, r * 0.95f, r * 0.95f, z0 + h * 0.5f, z0 + h, low, 0.15f, 10);
            // The shield in front of the gun.
            block(tf, r * 0.9f, r * 1.1f, -r * 1.3f, r * 1.3f, z0 + h * 0.6f, z0 + h + 2.6f, shade(paint, 0.9f));
            sides = false;
            break;
        }
        case TurretKind::Mini: {
            round_solid(tf, 0.0f, 0.0f, r, r, z0, z0 + h * 0.45f, team, 0.0f, 10);
            round_solid(tf, 0.0f, 0.0f, r * 0.96f, r * 0.96f, z0 + h * 0.45f, z0 + h, roof, 0.45f, 10);
            sides = false;
            break;
        }
        case TurretKind::SpgRound: {  // rounded, the commander's cupola with its machine gun
            round_solid(tf, 0.0f, 0.0f, r, r * 0.9f, z0, z0 + h * 0.35f, low, 0.0f, 12);
            round_solid(tf, 0.0f, 0.0f, r, r * 0.9f, z0 + h * 0.35f, z0 + h * 0.6f, team, 0.02f, 12);
            round_solid(tf, 0.0f, 0.0f, r * 0.98f, r * 0.88f, z0 + h * 0.6f, z0 + h, roof, 0.25f, 12);
            round_solid(tf, -r * 0.2f, r * 0.45f, 0.045f, 0.045f, z0 + h - 0.4f, z0 + h + 1.2f, shade(paint, 0.92f), 0.2f, 8);
            DrawLineEx(tf.at(-r * 0.2f, r * 0.45f, z0 + h + 1.8f), tf.at(r * 0.3f, r * 0.45f, z0 + h + 2.1f), 1.0f, lit({34, 34, 32, 255}));
            break;
        }
        case TurretKind::SpgBox: {  // big and angular, a bustle, hatches and the machine gun on the roof
            const float b = r * 0.45f;
            const Vector2 p[8] = {{r, r * 0.62f},  {r * 0.78f, r * 0.9f},  {-r * 0.9f, r * 0.9f},  {-r - b, r * 0.8f},
                                  {-r - b, -r * 0.8f}, {-r * 0.9f, -r * 0.9f}, {r * 0.78f, -r * 0.9f}, {r, -r * 0.62f}};
            layers(p, 8, 0.05f);
            if (!under(tf, z0 + h)) {
                for (const float k : {0.3f, -0.4f}) DrawLineV(tf.at(r * k, -r * 0.85f, z0 + h), tf.at(r * k, r * 0.85f, z0 + h), lit(shade(paint, 0.7f)));
            }
            block(tf, -r * 0.55f, -r * 0.15f, r * 0.3f, r * 0.7f, z0 + h, z0 + h + 1.2f, shade(paint, 0.9f));  // the commander's hatch
            DrawLineEx(tf.at(-r * 0.35f, r * 0.5f, z0 + h + 2.0f), tf.at(r * 0.15f, r * 0.5f, z0 + h + 2.3f), 1.0f, lit({34, 34, 32, 255}));
            block(tf, r * 0.3f, r * 0.65f, -r * 0.75f, -r * 0.45f, z0 + h, z0 + h + 1.6f, shade(paint, 0.85f));  // the sight
            break;
        }
        case TurretKind::OpenGun: {  // the mount: a low deck, the cradle's sides, the recoil cylinders
            block(tf, -r * 1.4f, r * 0.8f, -r * 1.3f, r * 1.3f, z0, z0 + h * 0.3f, low);
            for (const float sgn : {-1.0f, 1.0f}) {
                block(tf, -r * 0.8f, r * 0.4f, sgn > 0.0f ? r * 0.35f : -r * 0.6f, sgn > 0.0f ? r * 0.6f : -r * 0.35f, z0 + h * 0.3f, z0 + h * 1.1f,
                      team);
            }
            sides = false;
            break;
        }
        case TurretKind::TruckGun: {  // the cradle on its pivot
            block(tf, -r * 1.2f, r * 0.9f, -r * 1.0f, r * 1.0f, z0, z0 + h * 0.5f, low);
            block(tf, -r * 0.6f, r * 0.6f, -r * 0.7f, r * 0.7f, z0 + h * 0.5f, z0 + h * 0.9f, team);
            sides = false;
            break;
        }
        case TurretKind::Shilka: {  // wide and flat, the radar dish on its post behind
            const Vector2 p[8] = {{r * 0.9f, r * 0.7f},  {r * 0.6f, r},  {-r * 0.8f, r},  {-r, r * 0.8f},
                                  {-r, -r * 0.8f}, {-r * 0.8f, -r}, {r * 0.6f, -r}, {r * 0.9f, -r * 0.7f}};
            layers(p, 8, 0.08f);
            block(tf, r * 0.55f, r * 0.95f, -r * 0.35f, r * 0.35f, z0 + h * 0.3f, z0 + h * 0.95f, shade(paint, 0.9f));  // the guns' mantlet
            if (radar) dish(-r * 0.75f, 0.0f, z0 + h + 4.0f, 3.6f, 0.9f);
            sides = false;
            break;
        }
        case TurretKind::AaTwin: {  // the turret between its guns; the search radar on top behind, the tracking one in front
            const Vector2 p[8] = {{r * 0.95f, r * 0.55f},  {r * 0.6f, r * 0.85f},  {-r * 0.8f, r * 0.85f},  {-r, r * 0.6f},
                                  {-r, -r * 0.6f}, {-r * 0.8f, -r * 0.85f}, {r * 0.6f, -r * 0.85f}, {r * 0.95f, -r * 0.55f}};
            layers(p, 8, 0.1f);
            if (radar) dish(-r * 0.6f, 0.0f, z0 + h + 4.2f, 4.0f, 0.55f);
            round_solid(tf, r * 0.7f, 0.0f, 0.05f, 0.05f, z0 + h * 0.6f, z0 + h + 0.6f, shade(paint, 0.8f), 0.2f, 8);  // the tracking radar
            sides = false;
            break;
        }
        case TurretKind::Tunguska: {  // big; missiles either side behind the guns; the radar on top
            const Vector2 p[8] = {{r, r * 0.6f},  {r * 0.65f, r * 0.9f},  {-r * 0.8f, r * 0.9f},  {-r * 1.1f, r * 0.7f},
                                  {-r * 1.1f, -r * 0.7f}, {-r * 0.8f, -r * 0.9f}, {r * 0.65f, -r * 0.9f}, {r, -r * 0.6f}};
            layers(p, 8, 0.06f);
            for (const float sgn : {-1.0f, 1.0f}) {
                for (int k = 0; k < 2; ++k) {
                    const float c0 = sgn * (r * 0.92f + 0.03f * static_cast<float>(k));
                    block(tf, -r * 0.9f, -r * 0.1f, std::min(c0, c0 + sgn * 0.028f), std::max(c0, c0 + sgn * 0.028f), z0 + h * 0.65f,
                          z0 + h * 0.95f, {84, 94, 62, 255});
                }
            }
            if (radar) radar_panel(tf, -r * 0.7f, r * 0.7f, z0 + h, z0 + h + 2.0f, z0 + h + 4.0f, 0.05f, burnt);  // the search radar: a bar
            dish(r * 0.55f, 0.0f, z0 + h + 1.6f, 2.2f, 0.9f);  // the tracking radar
            sides = false;
            break;
        }
        case TurretKind::Pantsir: {  // the radars on top: the search one flat, the tracking one round; missile tubes either side
            const Vector2 p[6] = {{r, r * 0.7f}, {-r * 0.9f, r * 0.8f}, {-r * 1.1f, r * 0.5f}, {-r * 1.1f, -r * 0.5f}, {-r * 0.9f, -r * 0.8f}, {r, -r * 0.7f}};
            layers(p, 6, 0.05f);
            for (const float sgn : {-1.0f, 1.0f}) {
                for (int k = 0; k < 3; ++k) {
                    const float c0 = sgn * (r * 0.9f + 0.028f * static_cast<float>(k));
                    block(tf, -r * 0.8f, r * 0.6f, std::min(c0, c0 + sgn * 0.026f), std::max(c0, c0 + sgn * 0.026f), z0 + h * 0.6f,
                          z0 + h * 0.95f, {84, 94, 62, 255});
                }
            }
            if (radar) radar_panel(tf, -r * 0.6f, r * 0.8f, z0 + h, z0 + h + 1.0f, z0 + h + 5.0f, 0.12f, burnt);  // the search radar: a panel
            dish(r * 0.6f, 0.0f, z0 + h + 1.2f, 2.6f, 0.9f);
            sides = false;
            break;
        }
    }
    camouflage(tf, look, -r * 0.7f, r * 0.5f, -r * 0.6f, r * 0.5f, z0 + h, 0x71u + static_cast<uint32_t>(r * 1000.0f), 2);
    scorch(tf, -r * 0.7f, r * 0.5f, -r * 0.6f, r * 0.5f, z0 + h, wear, 0x93u + static_cast<uint32_t>(r * 700.0f));
    if (sides && !under(tf, z0 + h * 0.6f)) {
        for (const float sgn : {-1.0f, 1.0f}) {
            for (int k = 0; k < 3; ++k) disc(tf.at(r * 0.2f - 0.025f * static_cast<float>(k), sgn * r * 0.95f, z0 + h * 0.6f), 0.8f, {50, 52, 44, 255});
        }
    }
    // The missiles, once researched.
    if (missiles) {
        switch (look.launcher) {
            case Launcher::Roof:
                if (look.turret == TurretKind::Module) {  // Barrier: two tubes beside the module
                    for (int k = 0; k < 2; ++k) {
                        const float c = r * (0.95f + 0.26f * static_cast<float>(k));
                        block(tf, -r * 0.7f, r * 0.95f, c, c + r * 0.22f, z0 + h * 0.3f, z0 + h * 0.85f, launcher);
                    }
                } else {  // Konkurs on the roof
                    block(tf, -r * 0.1f, r * 0.1f, -r * 0.62f, -r * 0.48f, z0 + h, z0 + h + 0.5f, metal);  // its mount
                    block(tf, -r * 0.55f, r * 1.05f, -r * 0.72f, -r * 0.38f, z0 + h + 0.5f, z0 + h + 1.9f, launcher);
                }
                break;
            case Launcher::Rail:  // the Malyutka on its rail
                block(tf, r * 0.6f, look.gun * 0.78f, -0.012f, 0.012f, z0 + h + 0.4f, z0 + h + 1.4f, launcher);
                break;
            case Launcher::Box:  // the twin TOW box on the left
                block(tf, -r * 0.75f, r * 0.45f, r * 0.95f, r * 1.4f, z0 + h * 0.25f, z0 + h * 0.95f, shade(paint, 0.94f), 0.03f);
                break;
            case Launcher::Side:
                if (look.turret == TurretKind::Marder) {  // Milan on a post at the right
                    block(tf, -r * 0.1f, r * 0.05f, -r * 1.15f, -r * 1.0f, z0 + h * 0.5f, z0 + h + 0.8f, metal);
                    block(tf, -r * 0.4f, r * 0.9f, -r * 1.25f, -r * 0.95f, z0 + h + 0.8f, z0 + h + 2.0f, launcher);
                } else {  // Jyu-MAT at both sides
                    for (const float sgn : {-1.0f, 1.0f}) {
                        block(tf, -r * 0.5f, r * 0.75f, sgn > 0.0f ? r * 0.95f : -r * 1.2f, sgn > 0.0f ? r * 1.2f : -r * 0.95f,
                              z0 + h * 0.4f, z0 + h * 0.85f, launcher);
                    }
                }
                break;
            default: break;
        }
    }
    const Vector2 af = vehicle_aerial_foot(look);
    if (!under(tf, z0 + h)) disc(tf.at(af.x, af.y, z0 + h), 0.9f, shade(paint, 0.6f));  // the aerial's base
    if (side_guns) guns_at(near);
    if (gun_front && part == 0) gun();
}

// --- Trucks in pixel art ----------------------------------------------------------------

// The axes' own trucks and wheeled vehicles, each a real one for its job
// (the job and its numbers are the same for both sides): the Democratic
// axis's KrAZ-6322 (the supply truck, BM-21 "Bastion" on it), the HEMTT
// tanker, the MAN HX with the ammunition, Humvees (the command post's
// shelter, the Prophet direction finder, the Sentinel radar on its
// trailer); the Authoritarian axis's KamAZ-5350 (the supply truck, the
// Zhitel direction finder's box on it), Urals (the tanker, the ammunition,
// BM-21 Grad, the P-18 radar), the command vehicle on a BTR-80. Drawn as the
// tanks are; a radar's antenna apart, turning on its own.
enum class TruckModel : uint8_t {
    KrazCargo,
    HemttTanker,
    ManAmmo,
    KrazGrad,
    HmmwvCommand,
    HmmwvProphet,
    HmmwvSentinel,
    KamazCargo,
    UralTanker,
    UralAmmo,
    UralGrad,
    Btr80Command,
    KamazZhitel,
    UralRadar,
    Count,
};
enum class Chassis : uint8_t {
    Kraz,   // KrAZ-6322: a long square bonnet, a big cab, three axles
    Ural,   // Ural-4320: a shorter bonnet between round fenders, three axles
    Kamaz,  // KamAZ-5350: the cab over the engine, three axles
    ManHx,  // MAN HX: an angular cab over the engine, four axles
    Hemtt,  // HEMTT: a low cab over the front, the engine behind it, four axles
    Hmmwv,  // HMMWV: low and wide, four wheels
    Btr80,  // a BTR-80's hull: the command vehicle
};
enum class Body : uint8_t {
    Open,      // a cargo bed, its load on it
    Tarp,      // a tarpaulin on hoops over the crates
    Tank,      // a fuel tank on its cradle, a walkway, a ladder
    Launcher,  // the rocket launcher: forty tubes on a platform, raised to fire
    Box,       // a box body: a direction finder's (its masts), a radar's
    Shelter,   // a Humvee's shelter on its back
    Trailer,   // a Humvee towing the radar on its trailer
    Platform,  // a flat platform: a gun's (its spade at the back), a turret's
    None,      // (the BTR-80)
};
enum class Top : uint8_t {
    None,
    Yagi,      // the P-18's array of rods on its mast
    Sentinel,  // the Sentinel's flat square face
};

struct TruckLook {
    Chassis chassis;
    Body body;
    Top top;
    float length;   // tiles
    float width;    // half of it, to the wheels' outer edge
    float wheel_r;  // pixels
    bool mast;      // a direction finder's mast, raised when set up
    int whips;      // whip aerials
    Camo camo;
    Color paint;
    Color camo1;
    Color camo2;
};

constexpr TruckLook kTruckLooks[] = {
    {Chassis::Kraz, Body::Open, Top::None, 1.16f, 0.2f, 3.4f, false, 0, Camo::None, kUkrainianGreen, kUkrainianGreen, kUkrainianGreen},
    {Chassis::Hemtt, Body::Tank, Top::None, 1.24f, 0.21f, 3.5f, false, 0, Camo::ThreeTone, kNatoGreen, kNatoBrown, kNatoBlack},
    {Chassis::ManHx, Body::Tarp, Top::None, 1.2f, 0.21f, 3.3f, false, 0, Camo::ThreeTone, kNatoGreen, kNatoBrown, kNatoBlack},
    {Chassis::Kraz, Body::Launcher, Top::None, 1.16f, 0.2f, 3.4f, false, 0, Camo::None, kUkrainianGreen, kUkrainianGreen, kUkrainianGreen},
    {Chassis::Hmmwv, Body::Shelter, Top::None, 0.72f, 0.2f, 2.4f, false, 2, Camo::ThreeTone, kNatoGreen, kNatoBrown, kNatoBlack},
    {Chassis::Hmmwv, Body::Shelter, Top::None, 0.72f, 0.2f, 2.4f, true, 1, Camo::ThreeTone, kNatoGreen, kNatoBrown, kNatoBlack},
    {Chassis::Hmmwv, Body::Trailer, Top::Sentinel, 0.72f, 0.2f, 2.4f, false, 1, Camo::ThreeTone, kNatoGreen, kNatoBrown, kNatoBlack},
    {Chassis::Kamaz, Body::Open, Top::None, 1.1f, 0.2f, 3.3f, false, 0, Camo::None, kRussianOlive, kRussianOlive, kRussianOlive},
    {Chassis::Ural, Body::Tank, Top::None, 1.1f, 0.2f, 3.3f, false, 0, Camo::None, kRussianOlive, kRussianOlive, kRussianOlive},
    {Chassis::Ural, Body::Tarp, Top::None, 1.1f, 0.2f, 3.3f, false, 0, Camo::None, kRussianOlive, kRussianOlive, kRussianOlive},
    {Chassis::Ural, Body::Launcher, Top::None, 1.1f, 0.2f, 3.3f, false, 0, Camo::None, kRussianOlive, kRussianOlive, kRussianOlive},
    {Chassis::Btr80, Body::None, Top::None, 1.1f, 0.23f, 3.0f, false, 4, Camo::None, kRussianOlive, kRussianOlive, kRussianOlive},
    {Chassis::Kamaz, Body::Box, Top::None, 1.1f, 0.2f, 3.3f, true, 0, Camo::None, kRussianOlive, kRussianOlive, kRussianOlive},
    {Chassis::Ural, Body::Box, Top::Yagi, 1.1f, 0.2f, 3.3f, false, 0, Camo::None, kRussianOlive, kRussianOlive, kRussianOlive},
};
static_assert(std::size(kTruckLooks) == static_cast<size_t>(TruckModel::Count));

const TruckLook& truck_look_of(TruckModel m) { return kTruckLooks[static_cast<size_t>(m)]; }

// Which truck a unit is, by its job and its side; none for the rest.
std::optional<TruckModel> truck_model(engine::UnitTypeId type, engine::PlayerId owner) {
    const bool ours = engine::axis_of(owner) == engine::Axis::Democratic;
    switch (type) {
        case engine::UnitTypeId::Truck: return ours ? TruckModel::KrazCargo : TruckModel::KamazCargo;
        case engine::UnitTypeId::FuelTanker: return ours ? TruckModel::HemttTanker : TruckModel::UralTanker;
        case engine::UnitTypeId::AmmoTruck: return ours ? TruckModel::ManAmmo : TruckModel::UralAmmo;
        case engine::UnitTypeId::Mlrs: return ours ? TruckModel::KrazGrad : TruckModel::UralGrad;
        case engine::UnitTypeId::FieldHq: return ours ? TruckModel::HmmwvCommand : TruckModel::Btr80Command;
        case engine::UnitTypeId::DfStation: return ours ? TruckModel::HmmwvProphet : TruckModel::KamazZhitel;
        case engine::UnitTypeId::AirRadar: return ours ? TruckModel::HmmwvSentinel : TruckModel::UralRadar;
        default: return std::nullopt;
    }
}

// A truck's sprite sheets by its wear, how far it's sunk, its load (a Cargo).
int truck_variant(TruckModel model, int wear, int sink, int load) {
    return ((static_cast<int>(model) * 6 + wear) * 5 + sink) * 16 + load;
}

// Where a truck's parts are: its axles, its cab (along, its back and its
// front; its roof), the bonnet in front of it (0: the cab over the engine),
// the bed's floor and front.
struct ChassisDims {
    std::array<float, 4> axles;
    int n;
    float cab0;
    float cab1;
    float cab_z;
    float bonnet;
    float bonnet_z;
    float floor;
    float bed1;
};
ChassisDims chassis_of(const TruckLook& l) {
    const float h = l.length * 0.5f;
    switch (l.chassis) {
        case Chassis::Kraz: return {{h * 0.72f, -h * 0.42f, -h * 0.76f, 0.0f}, 3, h - 0.56f, h - 0.3f, 14.5f, 0.3f, 10.0f, 8.0f, h - 0.58f};
        case Chassis::Ural: return {{h * 0.7f, -h * 0.36f, -h * 0.76f, 0.0f}, 3, h - 0.46f, h - 0.24f, 13.5f, 0.24f, 9.5f, 7.6f, h - 0.48f};
        case Chassis::Kamaz: return {{h * 0.74f, -h * 0.38f, -h * 0.74f, 0.0f}, 3, h - 0.26f, h, 14.0f, 0.0f, 0.0f, 8.0f, h - 0.29f};
        case Chassis::ManHx: return {{h * 0.78f, h * 0.44f, -h * 0.44f, -h * 0.76f}, 4, h - 0.28f, h, 15.0f, 0.0f, 0.0f, 8.4f, h - 0.31f};
        case Chassis::Hemtt: return {{h * 0.78f, h * 0.46f, -h * 0.42f, -h * 0.76f}, 4, h - 0.24f, h, 12.5f, 0.0f, 0.0f, 8.4f, h - 0.44f};
        case Chassis::Hmmwv: return {{h * 0.62f, -h * 0.62f, 0.0f, 0.0f}, 2, h - 0.5f, h - 0.24f, 10.0f, 0.24f, 6.8f, 6.6f, h - 0.52f};
        case Chassis::Btr80: return {{}, 0, 0.0f, 0.0f, 11.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    }
    return {};
}

// The BTR-80 the command vehicle is: the BTR-82A's hull in its paint.
VehicleLook btr80_of(const TruckLook& l) {
    VehicleLook btr = vehicle_look_of(engine::VehicleModel::Btr82a);
    btr.paint = l.paint;
    btr.camo = l.camo;
    return btr;
}

// How high a truck stands (its cab's roof, the top of its box), where its
// engine is (along, up), where its radar turns (along, up).
float truck_height(const TruckLook& l) {
    const ChassisDims ch = chassis_of(l);
    return l.body == Body::Box ? std::max(ch.cab_z, ch.floor + 7.5f) : ch.cab_z;
}
Vector2 truck_engine(const TruckLook& l) {
    const ChassisDims ch = chassis_of(l);
    const float h = l.length * 0.5f;
    if (l.chassis == Chassis::Btr80) return {-h * 0.7f * kVehicleScale, 12.0f * kVehicleScale};
    return {(h - 0.12f) * kVehicleScale, (ch.bonnet > 0.0f ? ch.bonnet_z : ch.cab_z * 0.6f) * kVehicleScale};
}
// Where a truck burns: its engine (along, up), its cab (along, up), its bed
// (the middle, half its length, its top).
struct TruckSpots {
    Vector2 engine;
    Vector2 cab;
    float bed_mid;
    float bed_half;
    float bed_z;
};
TruckSpots truck_spots(const TruckLook& l) {
    const ChassisDims ch = chassis_of(l);
    const float h = l.length * 0.5f;
    const float k = kVehicleScale;
    const float b0 = -h + 0.02f;
    const float z = l.body == Body::Tank ? ch.floor + 6.2f : l.body == Body::Tarp ? ch.floor + 2.0f : ch.floor + 2.4f;
    return {truck_engine(l), {(ch.cab0 + ch.cab1) * 0.5f * k, ch.cab_z * k}, (b0 + ch.bed1) * 0.5f * k, (ch.bed1 - b0) * 0.5f * k, z * k};
}

Vector2 plane_extent(engine::VehicleModel m);  // half an aircraft's length, half its span, tiles (below)

std::vector<FlameSpot> vehicle_flames(engine::UnitTypeId type, engine::PlayerId owner, bool wreck, bool blown, Cargo cargo, float age, uint32_t seed) {
    std::vector<FlameSpot> spots;
    int most = 2;  // a wreck: all ablaze its first 24 s, dying down to 45 s
    if (wreck) {
        if (age > 45.0f) return spots;
        most = age > 34.0f ? 0 : age > 24.0f ? 1 : 2;
    }
    auto add = [&](float a, float c, float z, int size) {
        spots.push_back({a, c, z, std::min(size, most), seed + static_cast<uint32_t>(spots.size()) * 7919u});
    };
    const engine::UnitTypeDef& def = engine::unit_type(type);
    if (const std::optional<TruckModel> model = truck_model(type, owner)) {
        const TruckLook& look = truck_look_of(*model);
        if (look.chassis == Chassis::Btr80) {
            const Vector2 e = truck_engine(look);
            add(e.x, 0.04f, e.y, wreck ? 2 : 1);
            return spots;
        }
        const TruckSpots t = truck_spots(look);
        if (!wreck) {  // running, burning low: the engine, the tarpaulin
            add(t.engine.x, 0.03f, t.engine.y, 1);
            if (look.body == Body::Tarp) add(t.bed_mid + t.bed_half * 0.3f, -0.05f, t.bed_z + 4.0f, 0);
            return spots;
        }
        add(t.engine.x, 0.0f, t.engine.y, 2);
        add(t.cab.x, 0.05f, t.cab.y, 1);
        if (cargo == Cargo::Fuel || cargo == Cargo::Drums) {  // the fuel: flames the length of it
            for (const float k : {-0.6f, 0.0f, 0.55f}) add(t.bed_mid + t.bed_half * k, 0.02f * k, t.bed_z + 1.0f, 2);
            if (age < 20.0f) {  // the pool of it burning round it, going out
                const int pool = age < 8.0f ? 1 : 0;
                for (int i = 0; i < 8; ++i) {
                    const float a = static_cast<float>(i) * 0.785f + rand01(seed, i) * 0.5f;
                    const float d = 0.5f + 0.35f * rand01(seed, i + 10);
                    if (age > 12.0f && i % 2 == 1) continue;
                    spots.push_back({std::cos(a) * d, std::sin(a) * d, 0.0f, pool, seed + 991u * static_cast<uint32_t>(i + 1)});
                }
            }
        } else {
            add(t.bed_mid + t.bed_half * 0.45f, -0.04f, t.bed_z - 1.0f, 1);
            add(t.bed_mid - t.bed_half * 0.5f, 0.05f, t.bed_z - 1.0f, cargo == Cargo::None || is_stone(cargo) ? 0 : 1);
        }
        return spots;
    }
    if (drawn_as_armor(def)) {
        const Vector2 e = armor_engine(def.model);
        const Dims d = dims_of(def.model);
        const float k = kVehicleScale;
        if (!wreck) {  // running, burning low: its engine deck
            add(e.x, 0.05f, e.y, 1);
            add(e.x + 0.08f, -0.07f, e.y, 0);
            return spots;
        }
        add(e.x, 0.02f, e.y, 2);
        add(e.x + 0.1f, -0.1f, e.y, 1);
        add(d.ring.x * k, d.ring.y * k, (blown ? d.deck : d.turret_z + d.turret_h) * k, blown ? 2 : 1);  // the turret, or the hole it left
        add(d.length * 0.3f * k, -0.06f, (d.deck + 0.5f) * k, 0);  // the driver's hatch
        return spots;
    }
    if (wreck && has_plane_look(def.model) && cargo == Cargo::Fuel) {  // crashed: its pieces burning, its fuel round them
        const Vector2 half = plane_extent(def.model);
        const float l = half.x * kVehicleScale;
        const float span = half.y * kVehicleScale;
        add(0.0f, 0.02f, 2.0f, 2);          // the middle, burnt out
        add(-0.18f * l, -0.05f, 1.5f, 2);
        add(0.8f * l, 0.04f, 2.5f, 1);      // the nose
        add(-0.85f * l, -0.03f, 2.5f, 1);   // the tail
        add(0.05f * l, 0.9f * span, 0.5f, 1);  // the wing torn off
        add(0.0f, -0.55f * span, 0.5f, 0);
        if (age < 20.0f) {  // the fuel burning round them, going out
            const int pool = age < 8.0f ? 1 : 0;
            for (int i = 0; i < 9; ++i) {
                if (age > 12.0f && i % 2 == 1) continue;
                const float a = static_cast<float>(i) * 0.7f + rand01(seed, 50 + i) * 0.5f;
                const float d = 0.4f + 0.4f * rand01(seed, 60 + i);
                spots.push_back({std::cos(a) * d * 1.3f, std::sin(a) * d, 0.0f, pool, seed + 773u * static_cast<uint32_t>(i + 1)});
            }
        }
        return spots;
    }
    if (wreck && gun_or_plane(def)) add(0.0f, 0.0f, 3.0f, 1);
    return spots;
}
Vector2 truck_top_ring(const TruckLook& l) {
    const float h = l.length * 0.5f;
    return l.top == Top::Sentinel ? Vector2{-h - 0.22f, 0.0f} : Vector2{-h + 0.26f, 0.0f};
}
float truck_top_z(const TruckLook& l) { return l.top == Top::Sentinel ? 5.0f : chassis_of(l).floor + 7.5f; }

// The rocket launcher: forty tubes in a pack pointing ahead over the cab,
// hinged at its back; raised to fire (frame 1). Their mouths at the ends.
void grad_pack(const Frame& fr, const TruckLook& look, const ChassisDims& ch, int frame, int wear) {
    const float h = look.length * 0.5f;
    const float back = -h + 0.1f;
    const float front = ch.bed1 + 0.02f;
    const float z0 = ch.floor + 1.8f;
    const float rise = frame == 1 ? 6.5f : 0.0f;
    const float tall = 3.2f;
    const float hw = 0.12f;
    const Color paint = wear >= 4 ? look.paint : shade(look.paint, 0.92f);
    auto z = [&](float a) { return z0 + rise * (a - back) / (front - back); };
    auto facing = [&](float na, float nc) {
        const Vector2 g = fr.ground_of(na, nc);
        return g.x + g.y > 0.02f;
    };
    if (under(fr, z0 + tall)) return;
    block(fr, back + 0.02f, back + 0.16f, -0.05f, 0.05f, ch.floor + 0.4f, z0 + 0.3f, {48, 50, 44, 255});  // its pivot
    const float near = left_is_near(fr) ? 1.0f : -1.0f;
    auto quad = [&](Vector2 p0, Vector2 p1, Vector2 p2, Vector2 p3, Color color) { fill_quad(p0, p1, p2, p3, color); };
    // The end facing us with the tubes' mouths.
    auto mouths = [&](float a) {
        const float zb = z(a);
        quad(fr.at(a, -hw, zb), fr.at(a, hw, zb), fr.at(a, hw, zb + tall), fr.at(a, -hw, zb + tall), {40, 42, 36, 255});
        for (int row = 0; row < 3; ++row) {
            for (int k = 0; k < 6; ++k) {
                const float c = -hw + 0.02f + (2.0f * hw - 0.04f) * static_cast<float>(k) / 5.0f;
                disc(fr.at(a, c, zb + 0.6f + 1.0f * static_cast<float>(row)), 0.55f, {18, 18, 16, 255});
            }
        }
    };
    if (facing(-1.0f, 0.0f)) mouths(back);
    if (facing(1.0f, 0.0f)) mouths(front);
    const float sc = near * hw;
    quad(fr.at(back, sc, z(back)), fr.at(front, sc, z(front)), fr.at(front, sc, z(front) + tall), fr.at(back, sc, z(back) + tall),
         shade(paint, 0.78f));
    for (int row = 1; row < 3; ++row) {  // the rows of tubes along its side
        const float dz = tall * static_cast<float>(row) / 3.0f;
        DrawLineV(fr.at(back, sc, z(back) + dz), fr.at(front, sc, z(front) + dz), lit(shade(paint, 0.6f)));
    }
    quad(fr.at(back, -hw, z(back) + tall), fr.at(front, -hw, z(front) + tall), fr.at(front, hw, z(front) + tall),
         fr.at(back, hw, z(back) + tall), shade(paint, 1.05f));
    for (int k = 1; k < 6; ++k) {  // the tubes along its top
        const float c = -hw + 2.0f * hw * static_cast<float>(k) / 6.0f;
        DrawLineV(fr.at(back, c, z(back) + tall), fr.at(front, c, z(front) + tall), lit(shade(paint, 0.75f)));
    }
    scorch(fr, back, front, -hw, hw, z0 + tall, wear, 0x5A1u);
}

// A truck's body: the far wheels, the frame; its bed or body, its cab, its
// bonnet, whatever is nearer drawn over; the near wheels. The cab's
// windscreen and side window (cracked when battered, black burnt out), the
// grille, the bumper, the side's colour on the door; the bed by the job:
// its load, the tarpaulin, the tank, the launcher, a box with its masts
// (raised when set up, frame 1), a Humvee's shelter, the radar's trailer.
// Whip aerials to add after, a pixel thin, in `whips`. Burnt out, the tyres
// gone to the rims, the tarpaulin burnt off its hoops, the glass gone.
struct Whip {
    Vector2 foot;
    int length;
};

// A truck's load on its bed from a0 (its back) to a1 along it, `c` either
// side, on its floor at z: sacks of flour and grain; boards, a pallet of
// bricks, bags of cement; logs, their cut ends to the back, stakes holding
// them; stone; green ammunition boxes stacked; long shell crates, a row of
// shells standing; drums. Burnt (a wreck's): charred, what's left of it.
void draw_cargo(const Frame& fr, Cargo cargo, float a0, float a1, float c, float z, bool burnt, uint32_t seed) {
    enum Kind : uint8_t { Box, Sack, Drum, Log, Shell, Stone, AmmoBox, ShellCrate };
    struct Item {
        int layer;
        float a0, a1, c0, c1, z0, z1;
        Color color;
        Kind kind;
    };
    std::vector<Item> items;
    auto rnd = [&](int i) { return rand01(seed, i); };
    auto charred = [&](Color col) { return burnt ? shade(mix(col, {44, 36, 30, 255}, 0.75f), 0.85f) : col; };
    const float len = a1 - a0;
    const float wid = 2.0f * c;
    auto cell = [&](int i, int n, int j, int m, float fill_a, float fill_c) {  // a grid cell's bounds, a little in from its neighbours
        const float da = len / static_cast<float>(n);
        const float dc = wid / static_cast<float>(m);
        const float ma = a0 + da * (static_cast<float>(i) + 0.5f);
        const float mc = -c + dc * (static_cast<float>(j) + 0.5f);
        return std::array<float, 4>{ma - da * fill_a * 0.5f, ma + da * fill_a * 0.5f, mc - dc * fill_c * 0.5f, mc + dc * fill_c * 0.5f};
    };
    switch (cargo) {
        case Cargo::Sacks: {  // two layers, lying across; burlap and white flour sacks, a box of tins
            for (int layer = 0; layer < (burnt ? 1 : 2); ++layer) {
                for (int i = 0; i < 4; ++i) {
                    for (int j = 0; j < 3 - layer; ++j) {
                        if (burnt && rnd(i * 7 + j) < 0.5f) continue;
                        const auto b = cell(i, 4, j + (layer ? 0 : 0), 3 - layer, 0.9f, 0.88f);
                        const float lift = static_cast<float>(layer) * 2.2f;
                        const Color col = (i + j + layer) % 3 == 0 ? Color{214, 208, 190, 255} : Color{176, 156, 112, 255};
                        items.push_back({layer, b[0], b[1], b[2], b[3], z + lift, z + lift + 2.3f - (burnt ? 1.0f : 0.0f), charred(col), Sack});
                    }
                }
            }
            break;
        }
        case Cargo::Boards: {  // boards along the back, a pallet of bricks and bags of cement at the front
            const float split = a0 + len * 0.55f;
            for (int layer = 0; layer < (burnt ? 1 : 3); ++layer) {
                for (int j = 0; j < 5; ++j) {
                    const float dc = wid / 5.0f;
                    const float c0 = -c + dc * static_cast<float>(j) + dc * 0.06f;
                    const float lift = static_cast<float>(layer) * 1.1f;
                    items.push_back({layer, a0 - 0.02f, split, c0, c0 + dc * 0.88f, z + lift, z + lift + 1.0f,
                                     charred(shade({190, 150, 98, 255}, 0.9f + 0.2f * rnd(20 + layer * 5 + j))), Box});
                }
            }
            if (!burnt) {
                items.push_back({0, split + 0.02f, split + (a1 - split) * 0.55f, -c + 0.01f, c - 0.01f, z, z + 0.8f, {150, 116, 76, 255}, Box});  // the pallet
                items.push_back({1, split + 0.03f, split + (a1 - split) * 0.53f, -c + 0.03f, c - 0.03f, z + 0.8f, z + 4.2f, {160, 84, 60, 255}, Box});  // bricks
                for (int j = 0; j < 2; ++j) {
                    const auto b = cell(0, 1, j, 2, 1.0f, 0.85f);
                    items.push_back({0, split + (a1 - split) * 0.6f, a1 - 0.01f, b[2], b[3], z, z + 2.0f, {150, 148, 142, 255}, Sack});  // cement
                    items.push_back({1, split + (a1 - split) * 0.62f, a1 - 0.03f, b[2] + 0.01f, b[3] - 0.01f, z + 2.0f, z + 4.0f, {160, 158, 150, 255}, Sack});
                }
            }
            break;
        }
        case Cargo::LogsSome:
        case Cargo::LogsMost:
        case Cargo::Logs: {  // five logs, three and two, longer than the bed; stakes up its sides (two, three as it's loaded)
            const float r = wid / 6.0f;
            const float rz = 2.4f;
            const int logs = cargo == Cargo::LogsSome ? 2 : cargo == Cargo::LogsMost ? 3 : 5;
            for (int layer = 0; layer < (burnt || logs <= 3 ? 1 : 2); ++layer) {
                const int n = layer == 0 ? std::min(3, logs) : 2;
                for (int j = 0; j < n; ++j) {
                    const float mc = -c + r + (layer == 0 ? 2.0f * r * static_cast<float>(j) : r + 2.0f * r * static_cast<float>(j)) + (layer == 0 ? 0.0f : 0.0f);
                    const float lift = static_cast<float>(layer) * rz * 1.7f;
                    const float over = 0.05f + 0.06f * rnd(40 + layer * 3 + j);  // out over the back
                    items.push_back({layer, a0 - over, a1 - 0.02f, mc - r * 0.95f, mc + r * 0.95f, z + lift, z + lift + rz * 2.0f,
                                     charred(shade({104, 80, 56, 255}, 0.9f + 0.2f * rnd(50 + layer * 3 + j))), Log});
                }
            }
            for (const float side : {-1.0f, 1.0f}) {  // the stakes
                for (int k = 0; k < 3; ++k) {
                    const float a = a0 + len * (0.15f + 0.35f * static_cast<float>(k));
                    items.push_back({0, a - 0.012f, a + 0.012f, side * c - 0.012f, side * c + 0.012f, z, z + 8.0f, charred({92, 70, 50, 255}), Box});
                }
            }
            break;
        }
        case Cargo::StoneSome:
        case Cargo::StoneMost:
        case Cargo::Stone: {  // a heap of broken stone, higher in the middle (lower as it's loaded)
            const int stones = cargo == Cargo::StoneSome ? 5 : cargo == Cargo::StoneMost ? 9 : 12;
            for (int i = 0; i < stones; ++i) {
                const float ma = a0 + len * (0.1f + 0.8f * rnd(60 + i));
                const float mc = -c * 0.85f + wid * 0.85f * rnd(70 + i);
                const float s = 0.035f + 0.03f * rnd(80 + i);
                const float middle = 1.0f - std::fabs(mc) / c;
                const float lift = middle * 2.0f * rnd(90 + i);
                items.push_back({i < 6 ? 0 : 1, ma - s, ma + s, mc - s * 0.8f, mc + s * 0.8f, z + lift, z + lift + 1.5f + 2.0f * rnd(100 + i),
                                 charred(shade({140, 136, 128, 255}, 0.8f + 0.35f * rnd(110 + i))), Stone});
            }
            break;
        }
        case Cargo::Crates: {  // green ammunition boxes, three by two, two high
            if (burnt) break;
            for (int layer = 0; layer < 2; ++layer) {
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 2; ++j) {
                        if (layer == 1 && i == 0 && j == 1) continue;
                        const auto b = cell(i, 3, j, 2, 0.9f, 0.9f);
                        const float lift = static_cast<float>(layer) * 2.6f;
                        items.push_back({layer, b[0], b[1], b[2], b[3], z + lift, z + lift + 2.5f, shade({88, 100, 64, 255}, 0.92f + 0.14f * rnd(120 + i * 3 + j)), AmmoBox});
                    }
                }
            }
            break;
        }
        case Cargo::Shells: {  // at the back the shells standing in rows; long wooden shell crates stacked at the front, the lids off the top ones
            if (burnt) break;
            const float split = a0 + len * 0.42f;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 4; ++j) {
                    const float ma = a0 + (split - a0) * (static_cast<float>(i) + 0.5f) / 3.0f;
                    const float mc = -c + wid * (static_cast<float>(j) + 0.5f) / 4.0f;
                    items.push_back({0, ma - 0.028f, ma + 0.028f, mc - 0.028f, mc + 0.028f, z, z + 5.2f, {112, 112, 80, 255}, Shell});
                }
            }
            for (int layer = 0; layer < 2; ++layer) {
                for (int j = 0; j < 3; ++j) {
                    const float dc = wid / 3.0f;
                    const float c0 = -c + dc * static_cast<float>(j) + dc * 0.07f;
                    const float lift = static_cast<float>(layer) * 2.2f;
                    items.push_back({layer, split + 0.02f, a1, c0, c0 + dc * 0.86f, z + lift, z + lift + 2.1f,
                                     shade({170, 130, 84, 255}, 0.9f + 0.15f * rnd(130 + j + layer * 3)), layer == 1 ? ShellCrate : Box});
                }
            }
            break;
        }
        case Cargo::Drums: {  // drums standing, two rows; blue, red, green; burnt, some knocked over
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 2; ++j) {
                    if (burnt && rnd(140 + i * 2 + j) < 0.3f) continue;
                    const auto b = cell(i, 4, j, 2, 0.9f, 0.9f);
                    static constexpr Color kDrums[3] = {{60, 90, 130, 255}, {150, 60, 46, 255}, {70, 100, 70, 255}};
                    items.push_back({0, b[0], b[1], b[2], b[3], z, z + 4.4f, charred(kDrums[(i + j * 2) % 3]), Drum});
                }
            }
            break;
        }
        default: break;
    }
    // Back to front, bottom to top.
    std::sort(items.begin(), items.end(), [&](const Item& p, const Item& q) {
        if (p.layer != q.layer) return p.layer < q.layer;
        return fr.at((p.a0 + p.a1) * 0.5f, (p.c0 + p.c1) * 0.5f).y < fr.at((q.a0 + q.a1) * 0.5f, (q.c0 + q.c1) * 0.5f).y;
    });
    const Vector2 back = fr.ground_of(-1.0f, 0.0f);
    const bool back_near = back.x + back.y > 0.02f;
    for (const Item& it : items) {
        const float ma = (it.a0 + it.a1) * 0.5f;
        const float mc = (it.c0 + it.c1) * 0.5f;
        switch (it.kind) {
            case Box: block(fr, it.a0, it.a1, it.c0, it.c1, it.z0, it.z1, it.color); break;
            case ShellCrate: {  // its lid off: the two shells in it, olive, their noses
                block(fr, it.a0, it.a1, it.c0, it.c1, it.z0, it.z1, it.color);
                for (const float t : {0.3f, 0.7f}) {
                    const float cc = it.c0 + (it.c1 - it.c0) * t;
                    DrawLineEx(fr.at(it.a0 + 0.02f, cc, it.z1), fr.at(it.a1 - 0.03f, cc, it.z1), 1.2f, lit({112, 112, 80, 255}));
                    DrawPixelV(fr.at(it.a1 - 0.025f, cc, it.z1), lit({160, 160, 130, 255}));
                    DrawPixelV(fr.at(it.a0 + 0.04f, cc, it.z1), lit({196, 160, 70, 255}));
                }
                break;
            }
            case AmmoBox:
                block(fr, it.a0, it.a1, it.c0, it.c1, it.z0, it.z1, it.color);
                DrawLineV(fr.at(ma, it.c0 + 0.005f, it.z1), fr.at(ma, it.c1 - 0.005f, it.z1), lit(shade(it.color, 0.45f)));  // its lid's seam
                DrawLineV(fr.at(it.a0 + 0.01f, it.c1, it.z1 - 0.9f), fr.at(it.a1 - 0.01f, it.c1, it.z1 - 0.9f), lit(shade(it.color, 0.55f)));  // its band
                DrawPixelV(fr.at(it.a0 + (it.a1 - it.a0) * 0.3f, it.c1, it.z0 + 1.2f), lit({206, 180, 70, 255}));             // its marking
                break;
            case Sack: block(fr, it.a0, it.a1, it.c0, it.c1, it.z0, it.z1, it.color, 0.012f, 0.012f, 0.018f); break;
            case Stone: block(fr, it.a0, it.a1, it.c0, it.c1, it.z0, it.z1, it.color, 0.01f * rnd(200 + static_cast<int>(ma * 100.0f)), 0.012f, 0.012f); break;
            case Drum:
                round_solid(fr, ma, mc, (it.a1 - it.a0) * 0.5f, (it.c1 - it.c0) * 0.5f, it.z0, it.z1, it.color, 0.0f, 10);
                DrawLineV(fr.at(ma - (it.a1 - it.a0) * 0.3f, mc, it.z1), fr.at(ma + (it.a1 - it.a0) * 0.3f, mc, it.z1), lit(shade(it.color, 0.7f)));  // its bung
                break;
            case Shell:
                round_solid(fr, ma, mc, (it.a1 - it.a0) * 0.5f, (it.c1 - it.c0) * 0.5f, it.z0, it.z1 - 1.4f, it.color, 0.0f, 8);
                round_solid(fr, ma, mc, (it.a1 - it.a0) * 0.45f, (it.c1 - it.c0) * 0.45f, it.z1 - 1.4f, it.z1, {150, 150, 110, 255}, 0.75f, 8);  // its nose
                round_solid(fr, ma, mc, (it.a1 - it.a0) * 0.52f, (it.c1 - it.c0) * 0.52f, it.z0 + 0.8f, it.z0 + 1.4f, {196, 160, 70, 255}, 0.0f, 8);  // the band
                break;
            case Log: {
                const float r = (it.c1 - it.c0) * 0.5f;
                const float rz = (it.z1 - it.z0) * 0.5f;
                block(fr, it.a0, it.a1, it.c0, it.c1, it.z0, it.z1, it.color, 0.0f, 0.0f, r * 0.45f);
                if (back_near) {  // its cut end: pale wood, the rings, the bark round it
                    constexpr int kN = 10;
                    Vector2 ring[kN];
                    for (int k = 0; k < kN; ++k) {
                        const float t = static_cast<float>(k) * 6.2831853f / kN;
                        ring[k] = fr.at(it.a0, mc + std::cos(t) * r, it.z0 + rz + std::sin(t) * rz);
                    }
                    const Vector2 mid = fr.at(it.a0, mc, it.z0 + rz);
                    const Color cut = burnt ? Color{40, 34, 30, 255} : Color{206, 172, 122, 255};
                    for (int k = 0; k < kN; ++k) fill_triangle(mid, ring[k], ring[(k + 1) % kN], cut);
                    for (int k = 0; k < kN; ++k) DrawLineV(ring[k], ring[(k + 1) % kN], lit(shade(it.color, 0.8f)));
                    if (!burnt) DrawPixelV(mid, lit({150, 116, 76, 255}));
                }
                break;
            }
        }
    }
}

void draw_truck_body(const Frame& fr, const TruckLook& look, int frame, Color team, int wear, int load, std::vector<Whip>& whips) {
    if (look.chassis == Chassis::Btr80) {  // its hull, its mast folded along the roof, its aerials
        const VehicleLook btr = btr80_of(look);
        draw_vehicle_hull(fr, btr, 0, 0, team, wear);
        const float h = btr.length * 0.5f;
        if (!under(fr, btr.deck + 1.0f)) {
            block(fr, -h + 0.16f, h - 0.34f, -0.05f, -0.02f, btr.deck, btr.deck + 1.0f, shade(look.paint, 0.8f));
        }
        if (wear < 3) {
            for (const Vector2 p : {Vector2{-h + 0.14f, 0.15f}, Vector2{-h + 0.14f, -0.15f}, Vector2{h - 0.38f, 0.14f}, Vector2{h - 0.38f, -0.14f}}) {
                whips.push_back({fr.at(p.x, p.y, btr.deck), 16});
            }
        }
        return;
    }
    const ChassisDims ch = chassis_of(look);
    const float h = look.length * 0.5f;
    const float w = look.width;
    const float near = left_is_near(fr) ? 1.0f : -1.0f;
    const Color paint = look.paint;
    const bool burnt = wear >= 4;
    const bool humvee = look.chassis == Chassis::Hmmwv;
    const Color glass = burnt ? Color{28, 26, 24, 255} : wear >= 2 ? Color{56, 72, 80, 255} : Color{70, 96, 110, 255};
    const Color dark{44, 44, 40, 255};
    const uint32_t seed = static_cast<uint32_t>(look.length * 1000.0f) + static_cast<uint32_t>(look.body) * 77u;
    auto facing = [&](float na, float nc) {
        const Vector2 g = fr.ground_of(na, nc);
        return g.x + g.y > 0.02f;
    };
    const bool front_near = facing(1.0f, 0.0f);
    const float tyre_c = w - 0.05f;

    // The far wheels, the frame.
    for (int i = 0; i < ch.n; ++i) tyre(fr, ch.axles[static_cast<size_t>(i)], -near * tyre_c, look.wheel_r, paint, burnt);
    block(fr, -h + 0.03f, h - 0.06f, -0.1f, 0.1f, look.wheel_r * 0.8f, look.wheel_r * 0.8f + 1.8f, dark);

    const float low = humvee ? 2.8f : 4.2f;  // the bottom of the cab and the bonnet
    auto cab = [&] {
        const float slope = ch.bonnet > 0.0f ? 0.01f : 0.025f;
        block(fr, ch.cab0, ch.cab1, -w + 0.02f, w - 0.02f, low, ch.cab_z, paint, slope, 0.0f, 0.015f);
        if (under(fr, ch.cab_z)) return;
        camouflage(fr, look, ch.cab0 + 0.03f, ch.cab1 - 0.04f, -w + 0.05f, w - 0.06f, ch.cab_z, 0x33u + seed, 2);
        scorch(fr, ch.cab0 + 0.02f, ch.cab1 - 0.03f, -w + 0.05f, w - 0.06f, ch.cab_z, wear, 0x71u + seed);
        // The windscreen in two panes on the front, the side window, the door.
        auto front = [&](float c, float z) { return fr.at(ch.cab1 - slope * (z - low) / (ch.cab_z - low), c, z); };
        const float g0 = ch.cab_z - (humvee ? 3.2f : 4.4f);
        const float g1 = ch.cab_z - 0.9f;
        if (front_near) {
            for (const float s : {-1.0f, 1.0f}) {
                const float c0 = s * 0.01f;
                const float c1 = s * (w - 0.06f);
                fill_quad(front(c0, g0), front(c1, g0), front(c1, g1), front(c0, g1), glass);
                if (wear >= 2 && !burnt) DrawLineV(front(c0 + s * 0.02f, g0 + 0.5f), front(c1 - s * 0.02f, g1 - 0.4f), lit({120, 140, 150, 255}));
            }
        }
        const float sc = near * (w - 0.02f - 0.015f * 0.5f);
        fill_quad(fr.at(ch.cab0 + 0.04f, sc, g0), fr.at(ch.cab1 - 0.05f, sc, g0), fr.at(ch.cab1 - 0.05f, sc, g1), fr.at(ch.cab0 + 0.04f, sc, g1), glass);
        DrawLineV(fr.at(ch.cab0 + 0.03f, sc, low + 0.6f), fr.at(ch.cab0 + 0.03f, sc, g1), lit(shade(paint, 0.6f)));
        if (wear < 4) {  // the side's colour on the door
            const float d0 = ch.cab0 + 0.05f;
            const float d1 = ch.cab1 - 0.06f;
            fill_quad(fr.at(d0, sc, low + 1.2f), fr.at(d1, sc, low + 1.2f), fr.at(d1, sc, low + 2.6f), fr.at(d0, sc, low + 2.6f), team);
        }
        if (ch.bonnet <= 0.0f) {  // a cab over the engine: its bumper, its lights
            block(fr, ch.cab1 - 0.02f, ch.cab1 + 0.012f, -w + 0.01f, w - 0.01f, 2.8f, low + 0.2f, {50, 50, 46, 255});
            if (front_near) {
                for (const float c : {-w + 0.05f, w - 0.05f}) disc(front(c, low + 1.2f), 0.9f, burnt ? Color{30, 28, 26, 255} : Color{210, 206, 170, 255});
            }
        }
        if (look.chassis == Chassis::Hemtt) {  // the engine behind the cab, its louvres
            block(fr, ch.cab0 - 0.18f, ch.cab0, -w + 0.04f, w - 0.04f, low, ch.cab_z + 0.6f, shade(paint, 0.95f), 0.0f, 0.02f, 0.02f);
            for (int k = 0; k < 4 && !under(fr, ch.cab_z); ++k) {
                const float z = low + 2.0f + 1.8f * static_cast<float>(k);
                DrawLineV(fr.at(ch.cab0 - 0.16f, near * (w - 0.04f), z), fr.at(ch.cab0 - 0.02f, near * (w - 0.04f), z), lit(shade(paint, 0.6f)));
            }
        }
    };
    auto bonnet = [&] {
        if (ch.bonnet <= 0.0f) return;
        const float b0 = ch.cab1 - 0.01f;
        const float b1 = ch.cab1 + ch.bonnet;
        const float bw = humvee ? w - 0.02f : w - 0.06f;
        block(fr, b0, b1, -bw, bw, low, ch.bonnet_z, paint, humvee ? 0.06f : 0.03f, 0.0f, humvee ? 0.02f : 0.01f);
        if (!under(fr, ch.bonnet_z)) {
            camouflage(fr, look, b0 + 0.02f, b1 - 0.05f, -bw + 0.03f, bw - 0.04f, ch.bonnet_z, 0x93u + seed, 2);
            scorch(fr, b0 + 0.02f, b1 - 0.04f, -bw + 0.03f, bw - 0.04f, ch.bonnet_z, wear, 0x47u + seed);
        }
        if (!humvee) {  // the fenders over the front wheels: a Ural's round ones, a KrAZ's square
            const float ra = look.wheel_r / 32.0f * 1.35f;
            const float a = ch.axles[0];
            const float taper = look.chassis == Chassis::Ural ? 0.3f : 0.0f;
            for (const float s : {-1.0f, 1.0f}) {
                const Vector2 base[4] = {{a + ra, s * (w + 0.005f)}, {a + ra, s * (w - 0.09f)}, {a - ra, s * (w - 0.09f)}, {a - ra, s * (w + 0.005f)}};
                Vector2 top[4];
                for (int i = 0; i < 4; ++i) top[i] = {a + (base[i].x - a) * (1.0f - taper), base[i].y};
                solid(fr, base, top, 4, look.wheel_r * 2.0f - 0.2f, look.wheel_r * 2.0f + 1.0f, shade(paint, 0.92f));
            }
        }
        if (front_near && !under(fr, ch.bonnet_z - 0.8f)) {  // the grille, the lights
            const float gw = bw - 0.03f;
            auto at = [&](float c, float z) { return fr.at(b1 + 0.004f - (humvee ? 0.06f : 0.03f) * (z - low) / (ch.bonnet_z - low), c, z); };
            fill_quad(at(-gw, low + 0.8f), at(gw, low + 0.8f), at(gw, ch.bonnet_z - 0.8f), at(-gw, ch.bonnet_z - 0.8f), {36, 36, 32, 255});
            for (int k = 1; k < 6; ++k) {
                const float c = -gw + 2.0f * gw * static_cast<float>(k) / 6.0f;
                DrawLineV(at(c, low + 1.0f), at(c, ch.bonnet_z - 1.0f), lit(shade(paint, 0.8f)));
            }
            for (const float c : {-bw + 0.02f, bw - 0.02f}) disc(at(c, low + 1.4f), 0.9f, burnt ? Color{30, 28, 26, 255} : Color{210, 206, 170, 255});
        }
        block(fr, b1 - 0.02f, b1 + 0.012f, -w + 0.01f, w - 0.01f, 2.8f, low + 0.2f, {50, 50, 46, 255});  // the bumper
    };
    auto trailer = [&] {  // the radar's trailer behind a Humvee: its drawbar, its wheels
        if (look.body != Body::Trailer) return;
        const float t0 = -h - 0.38f;
        const float t1 = -h - 0.06f;
        DrawLineEx(fr.at(t1, 0.0f, 3.2f), fr.at(-h + 0.02f, 0.0f, 3.2f), 1.4f, lit(dark));
        tyre(fr, (t0 + t1) * 0.5f, -near * (w - 0.06f), 2.0f, paint, burnt);
        block(fr, t0, t1, -w + 0.05f, w - 0.05f, 2.8f, 5.0f, shade(paint, 0.9f));
        tyre(fr, (t0 + t1) * 0.5f, near * (w - 0.06f), 2.0f, paint, burnt);
    };
    auto bed = [&] {
        const float b0 = -h + 0.02f;
        const float b1 = ch.bed1;
        switch (look.body) {
            case Body::Open: {
                block(fr, b0, b1, -w + 0.01f, w - 0.01f, ch.floor - 1.0f, ch.floor + 2.4f, paint);
                block(fr, b0 + 0.02f, b1 - 0.02f, -w + 0.03f, w - 0.03f, ch.floor - 0.6f, ch.floor + 2.25f, shade(paint, 0.55f));  // inside
                if (!under(fr, ch.floor + 1.6f)) {
                    for (const float z : {ch.floor - 0.1f, ch.floor + 1.0f}) {  // the boards
                        DrawLineV(fr.at(b0 + 0.01f, near * (w - 0.01f), z), fr.at(b1 - 0.01f, near * (w - 0.01f), z), lit(shade(paint, 0.7f)));
                    }
                }
                if (load > 0 && !under(fr, ch.floor + 2.0f)) {  // the load (on a wreck, what's left of it)
                    draw_cargo(fr, static_cast<Cargo>(load), b0 + 0.04f, b1 - 0.04f, w - 0.05f, ch.floor - 0.4f, burnt, seed + static_cast<uint32_t>(load) * 13u);
                }
                break;
            }
            case Body::Tarp: {
                block(fr, b0, b1, -w + 0.01f, w - 0.01f, ch.floor - 1.0f, ch.floor + 1.4f, paint);
                const float top = ch.floor + 6.4f;
                const auto cargo = static_cast<Cargo>(load);
                const bool open = cargo != Cargo::None && !burnt;  // loaded: the tarpaulin rolled up off the back, the load showing
                const float split = open ? b0 + (b1 - b0) * 0.48f : b0;
                const Color tarp = mix(paint, {128, 122, 86, 255}, 0.35f);
                auto cover = [&] {
                    if (!burnt) {
                        // Burning (at its worst, still running): the tarpaulin melting, sagging between its hoops, burnt through.
                        const bool melted = wear == 3;
                        const float sag = melted ? 2.4f : 0.0f;
                        const Color cloth = melted ? mix(tarp, {58, 48, 36, 255}, 0.55f) : tarp;
                        block(fr, split + 0.01f, b1 - 0.01f, -w + 0.015f, w - 0.015f, ch.floor + 1.4f, top - sag, cloth, 0.01f, 0.01f, melted ? 0.075f : 0.05f);
                        if (!under(fr, top)) {
                            for (int k = 1; k < 5; ++k) {  // its hoops showing through (standing up out of it, melted)
                                const float a = split + (b1 - split) * static_cast<float>(k) / 5.0f;
                                if (melted) {
                                    DrawLineV(fr.at(a, -w + 0.07f, top), fr.at(a, w - 0.07f, top), lit({40, 36, 32, 255}));
                                    for (const float side : {-1.0f, 1.0f}) DrawLineV(fr.at(a, side * (w - 0.05f), top - sag), fr.at(a, side * (w - 0.07f), top), lit({40, 36, 32, 255}));
                                    continue;
                                }
                                DrawLineV(fr.at(a, -w + 0.07f, top), fr.at(a, w - 0.07f, top), lit(shade(tarp, 0.78f)));
                                DrawLineV(fr.at(a, near * (w - 0.015f), ch.floor + 1.6f), fr.at(a, near * (w - 0.06f), top - 0.2f), lit(shade(tarp, 0.8f)));
                            }
                            if (melted) {
                                for (int k = 0; k < 4; ++k) {  // burnt through: black holes, charred round them
                                    const float a = split + 0.06f + (b1 - split - 0.12f) * (0.1f + 0.27f * static_cast<float>(k));
                                    const float c = (rand01(seed, 40 + k) - 0.5f) * (w - 0.1f);
                                    patch(fr, a, c, 0.075f, 0.06f, top - sag, {50, 36, 26, 255}, seed + 50u + static_cast<uint32_t>(k));
                                    patch(fr, a, c, 0.05f, 0.04f, top - sag, {20, 18, 16, 255}, seed + 60u + static_cast<uint32_t>(k));
                                }
                                for (int k = 0; k < 6; ++k) {  // run down its side
                                    const float a = split + 0.03f + (b1 - split - 0.06f) * static_cast<float>(k) / 5.0f;
                                    DrawLineV(fr.at(a, near * (w - 0.015f), top - sag - 0.3f), fr.at(a, near * (w - 0.015f), top - sag - 1.5f - 2.5f * rand01(seed, 70 + k)),
                                              lit({36, 30, 24, 255}));
                                }
                            }
                        }
                        if (open) block(fr, split - 0.03f, split + 0.02f, -w + 0.02f, w - 0.02f, top - 2.4f, top - 0.2f, shade(tarp, 0.9f), 0.008f, 0.008f, 0.0f);  // rolled up
                    } else if (!under(fr, top)) {  // burnt off its hoops
                        for (int k = 0; k <= 5; ++k) {
                            const float a = b0 + 0.01f + (b1 - b0 - 0.02f) * static_cast<float>(k) / 5.0f;
                            const Vector2 l0 = fr.at(a, -w + 0.02f, ch.floor + 1.4f);
                            const Vector2 l1 = fr.at(a, -w + 0.07f, top);
                            const Vector2 r1 = fr.at(a, w - 0.07f, top);
                            const Vector2 r0 = fr.at(a, w - 0.02f, ch.floor + 1.4f);
                            for (const auto& [p, q] : {std::pair{l0, l1}, std::pair{l1, r1}, std::pair{r1, r0}}) DrawLineV(p, q, lit({34, 32, 28, 255}));
                        }
                    }
                };
                auto freight = [&] {
                    if (!open || under(fr, ch.floor + 2.0f)) return;
                    draw_cargo(fr, cargo, b0 + 0.03f, split - 0.04f, w - 0.04f, ch.floor + 0.2f, false, seed + static_cast<uint32_t>(load) * 13u);
                    for (int k = 0; k < 2; ++k) {  // the bare hoops over it
                        const float a = b0 + 0.03f + (split - b0 - 0.06f) * static_cast<float>(k);
                        const Vector2 l0 = fr.at(a, -w + 0.02f, ch.floor + 1.4f);
                        const Vector2 l1 = fr.at(a, -w + 0.07f, top);
                        const Vector2 r1 = fr.at(a, w - 0.07f, top);
                        const Vector2 r0 = fr.at(a, w - 0.02f, ch.floor + 1.4f);
                        for (const auto& [p, q] : {std::pair{l0, l1}, std::pair{l1, r1}, std::pair{r1, r0}}) DrawLineV(p, q, lit(shade(paint, 0.6f)));
                    }
                };
                if (facing(1.0f, 0.0f)) {  // the front nearer: the load behind the tarpaulin
                    freight();
                    cover();
                } else {
                    cover();
                    freight();
                }
                break;
            }
            case Body::Tank: {
                block(fr, b0, b1, -w + 0.04f, w - 0.04f, ch.floor - 1.0f, ch.floor + 0.4f, dark);  // the cradle
                const float zm = ch.floor + 3.2f;
                const float zt = ch.floor + 6.2f;
                {
                    const Vector2 base[4] = {{b1 - 0.015f, w - 0.07f}, {b1 - 0.015f, -w + 0.07f}, {b0 + 0.015f, -w + 0.07f}, {b0 + 0.015f, w - 0.07f}};
                    const Vector2 top[4] = {{b1, w - 0.02f}, {b1, -w + 0.02f}, {b0, -w + 0.02f}, {b0, w - 0.02f}};
                    solid(fr, base, top, 4, ch.floor + 0.3f, zm, shade(paint, 0.94f));
                }
                {
                    const Vector2 base[4] = {{b1, w - 0.02f}, {b1, -w + 0.02f}, {b0, -w + 0.02f}, {b0, w - 0.02f}};
                    const Vector2 top[4] = {{b1 - 0.02f, w - 0.08f}, {b1 - 0.02f, -w + 0.08f}, {b0 + 0.02f, -w + 0.08f}, {b0 + 0.02f, w - 0.08f}};
                    solid(fr, base, top, 4, zm, zt, paint);
                }
                if (!under(fr, zt)) {
                    camouflage(fr, look, b0 + 0.04f, b1 - 0.04f, -w + 0.1f, w - 0.12f, zt, 0x17u + seed, 3);
                    scorch(fr, b0 + 0.03f, b1 - 0.03f, -w + 0.1f, w - 0.12f, zt, wear, 0x29u + seed);
                    // The walkway along its top, the manhole, the ladder at the back.
                    fill_quad(fr.at(b0 + 0.03f, -0.025f, zt), fr.at(b1 - 0.03f, -0.025f, zt), fr.at(b1 - 0.03f, 0.025f, zt),
                              fr.at(b0 + 0.03f, 0.025f, zt), shade(paint, 0.7f));
                    round_solid(fr, (b0 + b1) * 0.5f, 0.0f, 0.04f, 0.04f, zt, zt + 0.8f, shade(paint, 1.05f), 0.1f, 8);
                    if (facing(-1.0f, 0.0f)) {
                        for (const float c : {-0.03f, 0.03f}) DrawLineV(fr.at(b0 - 0.005f, c, ch.floor + 0.4f), fr.at(b0 - 0.005f, c, zt), lit(dark));
                    }
                    if (wear < 4) {  // its warning band
                        const float sc = near * (w - 0.021f);
                        fill_quad(fr.at(b0 + 0.06f, sc, zm - 0.5f), fr.at(b1 - 0.06f, sc, zm - 0.5f), fr.at(b1 - 0.06f, sc, zm + 0.5f),
                                  fr.at(b0 + 0.06f, sc, zm + 0.5f), shade(paint, 1.45f));
                    }
                }
                break;
            }
            case Body::Launcher: {
                block(fr, b0, b1, -w + 0.02f, w - 0.02f, ch.floor - 1.0f, ch.floor + 0.6f, shade(paint, 0.85f));  // the platform
                // Its jacks at the back: down on the ground to fire.
                for (const float c : {-w + 0.03f, w - 0.03f}) {
                    block(fr, b0 + 0.01f, b0 + 0.05f, c - 0.015f, c + 0.015f, frame == 1 ? 0.0f : 2.4f, ch.floor - 1.0f, dark);
                }
                grad_pack(fr, look, ch, frame, wear);
                break;
            }
            case Body::Box: {
                const float top = ch.floor + 7.5f;
                block(fr, b0, b1, -w + 0.01f, w - 0.01f, ch.floor - 0.8f, top, shade(paint, 1.02f), 0.0f, 0.0f, 0.005f);
                if (!under(fr, top)) {
                    camouflage(fr, look, b0 + 0.04f, b1 - 0.04f, -w + 0.04f, w - 0.05f, top, 0x21u + seed, 3);
                    scorch(fr, b0 + 0.03f, b1 - 0.03f, -w + 0.04f, w - 0.05f, top, wear, 0x39u + seed);
                    block(fr, b1 - 0.12f, b1 - 0.03f, -0.08f, 0.08f, top, top + 1.4f, shade(paint, 0.85f));  // the air conditioner
                    const float sc = near * (w - 0.01f);
                    fill_quad(fr.at(b1 - 0.2f, sc, top - 3.0f), fr.at(b1 - 0.1f, sc, top - 3.0f), fr.at(b1 - 0.1f, sc, top - 1.2f),
                              fr.at(b1 - 0.2f, sc, top - 1.2f), glass);  // a window
                    if (facing(-1.0f, 0.0f)) {  // the door
                        const Vector2 q[4] = {fr.at(b0 - 0.004f, -0.06f, ch.floor), fr.at(b0 - 0.004f, 0.06f, ch.floor),
                                              fr.at(b0 - 0.004f, 0.06f, top - 1.0f), fr.at(b0 - 0.004f, -0.06f, top - 1.0f)};
                        fill_quad(q[0], q[1], q[2], q[3], shade(paint, 0.8f));
                        for (int i = 0; i < 4; ++i) DrawLineV(q[i], q[(i + 1) % 4], lit(shade(paint, 0.55f)));
                    }
                    if (look.mast) {  // the direction finder's two masts: raised when set up, else lying along the roof
                        for (const float a : {b0 + 0.06f, b1 - 0.2f}) {
                            if (frame == 1 && !burnt) {
                                const Vector2 foot = fr.at(a, 0.0f, top);
                                const Vector2 tip{foot.x, foot.y - 26.0f};
                                DrawLineEx(foot, tip, 1.6f, lit({70, 72, 64, 255}));
                                DrawLineEx({tip.x - 4.0f, tip.y + 1.0f}, {tip.x + 4.0f, tip.y + 1.0f}, 1.0f, lit({70, 72, 64, 255}));
                                DrawLineEx({tip.x - 3.0f, tip.y + 5.0f}, {tip.x + 3.0f, tip.y + 5.0f}, 1.0f, lit({70, 72, 64, 255}));
                            } else {
                                block(fr, a - 0.02f, a + 0.14f, -0.02f, 0.02f, top, top + 0.8f, {70, 72, 64, 255});
                            }
                        }
                    }
                }
                break;
            }
            case Body::Shelter: {
                const float top = ch.floor + 5.2f;
                block(fr, b0, b1, -w + 0.03f, w - 0.03f, ch.floor - 0.6f, top, shade(paint, 1.03f), 0.0f, 0.0f, 0.005f);
                if (!under(fr, top)) {
                    camouflage(fr, look, b0 + 0.03f, b1 - 0.03f, -w + 0.05f, w - 0.06f, top, 0x51u + seed, 2);
                    scorch(fr, b0 + 0.02f, b1 - 0.02f, -w + 0.05f, w - 0.06f, top, wear, 0x63u + seed);
                    if (facing(-1.0f, 0.0f)) {
                        const Vector2 q[4] = {fr.at(b0 - 0.004f, -0.07f, ch.floor), fr.at(b0 - 0.004f, 0.07f, ch.floor),
                                              fr.at(b0 - 0.004f, 0.07f, top - 0.8f), fr.at(b0 - 0.004f, -0.07f, top - 0.8f)};
                        fill_quad(q[0], q[1], q[2], q[3], shade(paint, 0.8f));
                        for (int i = 0; i < 4; ++i) DrawLineV(q[i], q[(i + 1) % 4], lit(shade(paint, 0.55f)));
                    }
                    if (look.mast && frame == 1 && !burnt) {  // the direction finder's mast, up, its loop at the top
                        const Vector2 foot = fr.at(b1 - 0.04f, 0.0f, top);
                        const Vector2 tip{foot.x, foot.y - 30.0f};
                        DrawLineEx(foot, tip, 1.6f, lit({70, 72, 64, 255}));
                        DrawEllipseLines(static_cast<int>(tip.x), static_cast<int>(tip.y - 3.0f), 3.0f, 3.5f, lit({70, 72, 64, 255}));
                    } else if (look.mast) {
                        block(fr, b0 + 0.04f, b1 - 0.02f, 0.03f, 0.06f, top, top + 0.8f, {70, 72, 64, 255});
                    }
                }
                break;
            }
            case Body::Platform: {
                block(fr, b0, b1, -w + 0.02f, w - 0.02f, ch.floor - 1.0f, ch.floor + 0.6f, shade(paint, 0.88f));
                block(fr, b0 - 0.03f, b0 + 0.02f, -0.14f, 0.14f, 1.2f, ch.floor - 0.6f, dark);  // the spade, raised
                break;
            }
            case Body::Trailer: {  // a Humvee's open back
                block(fr, b0, b1, -w + 0.02f, w - 0.02f, ch.floor - 1.0f, ch.floor + 1.2f, paint);
                block(fr, b0 + 0.02f, b1 - 0.02f, -w + 0.04f, w - 0.04f, ch.floor - 0.6f, ch.floor + 1.1f, shade(paint, 0.55f));
                break;
            }
            case Body::None: break;
        }
        // Whip aerials: on a shelter's back corners, a box's.
        if (look.whips > 0 && wear < 3) {
            const float top = look.body == Body::Shelter ? ch.floor + 5.2f : ch.cab_z;
            for (int k = 0; k < look.whips; ++k) {
                const float c = (k % 2 == 0 ? 1.0f : -1.0f) * (w - 0.05f);
                const float a = look.body == Body::Shelter ? b0 + 0.04f : ch.cab0 + 0.04f;
                whips.push_back({fr.at(a, c, top), 14});
            }
        }
    };
    // Back to front as seen: whatever is nearer drawn over.
    if (front_near) {
        trailer();
        bed();
        cab();
        bonnet();
    } else {
        bonnet();
        cab();
        bed();
        trailer();
    }
    for (int i = 0; i < ch.n; ++i) tyre(fr, ch.axles[static_cast<size_t>(i)], near * tyre_c, look.wheel_r, paint, burnt, 0.0f, frame == 2 ? 0.39f : 0.0f);
}

// A radar's antenna, turning on its own: the P-18's array of rods on its
// mast (lowered on the roof when packed), the Sentinel's flat square face
// (lying flat when packed); frame 1 set up.
void draw_truck_top(const Frame& tf, const TruckLook& look, int frame, int wear) {
    const float z0 = truck_top_z(look);
    const Color metal{70, 72, 64, 255};
    if (under(tf, z0 + 1.0f)) return;
    switch (look.top) {
        case Top::Yagi: {
            const float up = frame == 1 ? 10.0f : 1.2f;
            if (frame == 1) DrawLineEx(tf.at(0.0f, 0.0f, z0), tf.at(0.0f, 0.0f, z0 + up), 1.6f, lit(metal));
            DrawLineEx(tf.at(0.0f, -0.26f, z0 + up), tf.at(0.0f, 0.26f, z0 + up), 1.4f, lit(metal));
            const float rod = frame == 1 ? 3.0f : 0.6f;
            for (int k = 0; k < 8; ++k) {
                const float c = -0.24f + 0.48f * static_cast<float>(k) / 7.0f;
                for (const float a : {-0.03f, 0.04f}) DrawLineV(tf.at(a, c, z0 + up - rod), tf.at(a, c, z0 + up + rod), lit(shade(metal, 1.3f)));
            }
            break;
        }
        case Top::Sentinel: {
            round_solid(tf, 0.0f, 0.0f, 0.05f, 0.05f, z0, z0 + 1.0f, shade(look.paint, 0.8f), 0.0f, 8);
            const bool front = tf.f.x + tf.f.y > 0.0f;
            const Color face = wear >= 4 ? Color{40, 38, 34, 255} : front ? Color{150, 156, 146, 255} : shade(look.paint, 0.9f);
            Vector2 q[4];
            if (frame == 1) {
                q[0] = tf.at(0.03f, -0.13f, z0 + 1.4f);
                q[1] = tf.at(0.03f, 0.13f, z0 + 1.4f);
                q[2] = tf.at(-0.03f, 0.13f, z0 + 9.5f);
                q[3] = tf.at(-0.03f, -0.13f, z0 + 9.5f);
            } else {
                q[0] = tf.at(0.12f, -0.13f, z0 + 1.2f);
                q[1] = tf.at(0.12f, 0.13f, z0 + 1.2f);
                q[2] = tf.at(-0.12f, 0.13f, z0 + 1.2f);
                q[3] = tf.at(-0.12f, -0.13f, z0 + 1.2f);
            }
            fill_quad(q[0], q[1], q[2], q[3], face);
            for (int i = 0; i < 4; ++i) DrawLineV(q[i], q[(i + 1) % 4], lit(shade(face, 0.55f)));
            for (int k = 1; k < 4; ++k) DrawLineV(lerp(q[0], q[3], static_cast<float>(k) / 4.0f), lerp(q[1], q[2], static_cast<float>(k) / 4.0f), lit(shade(face, 0.8f)));
            break;
        }
        case Top::None: break;
    }
}

void draw_truck_chassis(const Frame& fr, const VehicleLook& look, Color team, int wear) {
    const TruckLook truck{look.wheels == 4 ? Chassis::ManHx : Chassis::Kamaz, Body::Platform, Top::None, look.length, look.width,
                          look.wheel_r, false, 0, look.camo, look.paint, look.camo1, look.camo2};
    std::vector<Whip> whips;
    draw_truck_body(fr, truck, 0, team, wear, 0, whips);
}

Cargo cargo_of(const engine::World& world, const engine::Unit& u) {
    if (u.carrying <= 0) return Cargo::None;
    switch (u.type) {
        case engine::UnitTypeId::FuelTanker: return Cargo::Fuel;
        case engine::UnitTypeId::AmmoTruck: {  // for the guns, shells; for the tanks, the infantry, boxes of rounds
            const engine::Unit* served = u.serves != 0 ? world.find_unit(u.serves) : nullptr;
            if (!served) return (u.id & 1u) != 0 ? Cargo::Shells : Cargo::Crates;
            const engine::UnitTypeDef& def = engine::unit_type(served->type);
            const bool guns = engine::is_tube_artillery(def) || served->type == engine::UnitTypeId::Mortar || served->type == engine::UnitTypeId::Mlrs;
            return guns ? Cargo::Shells : Cargo::Crates;
        }
        case engine::UnitTypeId::Truck:
            switch (u.carrying_type) {
                case engine::Resource::Food: return Cargo::Sacks;
                case engine::Resource::Ammo: return (u.id & 1u) != 0 ? Cargo::Shells : Cargo::Crates;
                case engine::Resource::Fuel: return Cargo::Drums;
                case engine::Resource::Materials: {
                    if (u.order != engine::Order::Collect) return Cargo::Boards;  // off the train: building materials
                    // Taking in the rear troops' work: at a quarry stone, else logs; the bed filling as they bring it.
                    const engine::TileMap& map = world.map();
                    const int fill = u.carrying * 3 < engine::kTruckCapacity ? 0 : u.carrying * 3 < engine::kTruckCapacity * 2 ? 1 : 2;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int x = u.order_goal.x + dx;
                            const int y = u.order_goal.y + dy;
                            if (map.contains_tile(x, y) && map.terrain(x, y) == engine::Terrain::Rock) {
                                return fill == 0 ? Cargo::StoneSome : fill == 1 ? Cargo::StoneMost : Cargo::Stone;
                            }
                        }
                    }
                    return fill == 0 ? Cargo::LogsSome : fill == 1 ? Cargo::LogsMost : Cargo::Logs;
                }
                default: return Cargo::None;
            }
        default: return Cargo::None;
    }
}
int truck_load(Cargo c) { return c == Cargo::Fuel ? 0 : static_cast<int>(c); }  // (a tanker's tank looks the same full)
int wreck_load(Cargo c) {  // the rounds and the fuel went up with it; the rest lies there burnt
    return c == Cargo::Sacks || c == Cargo::Boards || is_logs(c) || is_stone(c) || c == Cargo::Drums ? static_cast<int>(c) : 0;
}
bool truck_turns_top(TruckModel m) { return truck_look_of(m).top != Top::None; }
Vector2 truck_top_ring_of(TruckModel m) { return truck_top_ring(truck_look_of(m)); }
Vector2 truck_engine_of(TruckModel m) { return truck_engine(truck_look_of(m)); }

// --- Towed howitzers and aircraft in pixel art -------------------------------------------

// The towed howitzers of both axes: the D-30 (both), Msta-B and Giatsint-B,
// M777 and FH70; each from its carriage (the D-30's three trails round its
// pivot, two split trails, the M777's four legs), its barrel, its shield,
// FH70's little engine. Frame 0 packed to be towed (the trails closed, the
// barrel over them towards the tow), 1 set up with the barrel raised to fire.
struct GunLook {
    int trails;      // 3: round a pivot (the D-30); 2: split behind; 4: two behind, two short ahead (the M777)
    float barrel;    // tiles, from the trunnions
    float barrel_w;  // pixels
    float trunnion;  // how high the barrel's pivot is, pixels
    bool shield;
    bool apu;        // an engine on the carriage (FH70's)
    float wheel_r;
    float axle;      // half the track, tiles
    Camo camo;
    Color paint;
    Color camo1;
    Color camo2;
};

constexpr GunLook kGunLooks[] = {
    // D-30: three trails spread round it, the wheels lifted, the shield, towed by its muzzle.
    {3, 0.72f, 2.6f, 7.0f, true, false, 3.2f, 0.17f, Camo::None, {96, 102, 64, 255}, kRussianOlive, kRussianOlive},
    // 2A65 Msta-B: split trails, a shield, the long 152 mm.
    {2, 0.95f, 2.6f, 8.0f, true, false, 3.4f, 0.2f, Camo::None, {92, 100, 58, 255}, kRussianOlive, kRussianOlive},
    // 2A36 Giatsint-B: split trails, the very long 152 mm, a small shield.
    {2, 1.14f, 2.6f, 8.0f, true, false, 3.4f, 0.2f, Camo::None, {98, 104, 58, 255}, kRussianOlive, kRussianOlive},
    // M777: low, four legs, no shield, the 155 mm.
    {4, 0.9f, 2.4f, 6.0f, false, false, 2.8f, 0.2f, Camo::None, {150, 138, 100, 255}, kSand, kSand},
    // FH70: split trails, a shield, its engine on the trails, three colours.
    {2, 0.95f, 2.6f, 8.0f, true, true, 3.4f, 0.2f, Camo::ThreeTone, kNatoGreen, kNatoBrown, kNatoBlack},
};
static_assert(std::size(kGunLooks) == static_cast<size_t>(engine::VehicleModel::Fh70) + 1 - static_cast<size_t>(engine::VehicleModel::D30));

bool has_gun_look(engine::VehicleModel m) { return m >= engine::VehicleModel::D30 && m <= engine::VehicleModel::Fh70; }
const GunLook& gun_look_of(engine::VehicleModel m) {
    return kGunLooks[static_cast<size_t>(m) - static_cast<size_t>(engine::VehicleModel::D30)];
}
float gun_barrel_of(engine::VehicleModel m) { return has_gun_look(m) ? gun_look_of(m).barrel : 0.6f; }

// `part` (set up): 0 all of it; 1 the carriage without the barrel, 2 the
// barrel alone (its cradle and shield with it), laid at `frame`, `recoil`
// of its length back in its cradle.
void draw_towed_gun(const Frame& fr, const GunLook& look, int frame, Color team, int wear, int part = 0, float recoil = 0.0f) {
    const Color paint = look.paint;
    const Color dark = shade(paint, 0.62f);
    const bool burnt = wear >= 4;
    const bool set_up = frame >= 1;
    const float near = left_is_near(fr) ? 1.0f : -1.0f;
    auto trail = [&](float deg, float from, float len, float z0) {  // a trail from `from` along the frame turned by `deg`
        const float t = deg * 0.0174533f;
        const Vector2 d{fr.f.x * std::cos(t) - fr.f.y * std::sin(t), fr.f.y * std::cos(t) + fr.f.x * std::sin(t)};
        const Frame tf = fr.turned(d, 0.0f, 0.0f);
        block(tf, from, from + len, -0.024f, 0.024f, z0, z0 + 2.0f, dark);
        block(tf, from + len - 0.05f, from + len, -0.04f, 0.04f, 0.0f, z0 + 0.6f, shade(dark, 0.8f));  // its spade
    };
    const float z = look.trunnion;
    // The barrel: laid to fire for the range, about its trunnions, the
    // breech going down behind them; burnt out, down on its trails.
    auto barrel_of = [&] {
        const float a0 = set_up ? -0.12f : -0.1f;
        if (burnt || !set_up) {
            barrel(fr, a0, look.barrel, 0.0f, z, burnt ? -z * 0.6f : 0.0f, look.barrel_w, shade(paint, 0.78f), true);
        } else {  // back in its cradle after a shot, the breech out behind it
            const float t = kHowitzerPitch[std::clamp(frame, 1, kHowitzerFrames - 1)] * 0.0174533f;
            const float slide = recoil * (look.barrel - a0);
            barrel(fr, (a0 - slide) * std::cos(t), (look.barrel - slide) * std::cos(t), 0.0f, z + (a0 - slide) * std::sin(t) * kZPerTile,
                   (look.barrel - a0) * std::sin(t) * kZPerTile, look.barrel_w, shade(paint, 0.78f), true);
        }
        block(fr, -0.14f, 0.12f, -0.05f, 0.05f, z - 1.4f, z + 1.2f, shade(paint, 0.9f));  // the cradle, the recoil cylinders
    };
    auto shield = [&] {
        if (!look.shield) return;
        block(fr, 0.08f, 0.11f, -0.16f, 0.16f, 3.0f, z + 3.0f, paint);
        if (wear < 4 && !under(fr, z + 3.0f)) {
            const float c = near * 0.1f;
            fill_quad(fr.at(0.11f, c - 0.03f, z + 1.2f), fr.at(0.11f, c + 0.03f, z + 1.2f), fr.at(0.11f, c + 0.03f, z + 2.4f),
                      fr.at(0.11f, c - 0.03f, z + 2.4f), team);
        }
    };
    auto wheels = [&](float lift) {
        for (const float s : {-near, near}) tyre(fr, 0.0f, s * look.axle, look.wheel_r, paint, burnt, lift);
    };
    if (!set_up) {
        // Packed: the trails closed into one bar towards the tow, the barrel over them.
        block(fr, 0.02f, 0.55f, -0.04f, 0.04f, 1.2f, 3.2f, dark);
        block(fr, 0.52f, 0.6f, -0.012f, 0.012f, 1.6f, 2.6f, {40, 40, 36, 255});  // the lunette
        tyre(fr, 0.0f, -near * look.axle, look.wheel_r, paint, burnt, 0.0f);
        block(fr, -0.06f, 0.06f, -look.axle + 0.03f, look.axle - 0.03f, look.wheel_r - 0.6f, look.wheel_r + 0.6f, dark);  // the axle
        if (look.apu) block(fr, 0.1f, 0.22f, -0.07f, 0.07f, 3.0f, 5.6f, shade(paint, 0.9f));
        barrel_of();
        shield();
        tyre(fr, 0.0f, near * look.axle, look.wheel_r, paint, burnt, 0.0f);
        return;
    }
    // Set up: the trails spread, the barrel raised over them.
    if (part == 2) {
        barrel_of();
        shield();
        return;
    }
    switch (look.trails) {
        case 3:  // round its pivot, its wheels lifted off the ground
            for (const float deg : {180.0f, 60.0f, -60.0f}) trail(deg, 0.04f, 0.46f, 0.0f);
            block(fr, -0.08f, 0.08f, -0.08f, 0.08f, 0.0f, z - 1.0f, shade(paint, 0.85f), 0.0f, 0.0f, 0.02f);  // the pivot
            wheels(2.0f);
            break;
        case 4:  // two behind, two short ones ahead
            for (const float deg : {154.0f, -154.0f}) trail(deg, 0.02f, 0.5f, 0.3f);
            for (const float deg : {40.0f, -40.0f}) trail(deg, 0.02f, 0.2f, 0.3f);
            block(fr, -0.1f, 0.1f, -0.1f, 0.1f, 0.3f, z - 1.2f, shade(paint, 0.85f));
            wheels(1.2f);
            break;
        default:  // two spread behind, the wheels down
            for (const float deg : {152.0f, -152.0f}) trail(deg, 0.02f, 0.58f, 1.0f);
            if (look.apu) block(fr, -0.3f, -0.18f, -0.07f, 0.07f, 1.0f, 4.0f, shade(paint, 0.9f));  // its engine where the trails meet
            block(fr, -0.06f, 0.06f, -look.axle + 0.03f, look.axle - 0.03f, look.wheel_r - 0.6f, look.wheel_r + 0.6f, dark);
            wheels(0.0f);
            break;
    }
    camouflage(fr, look, -0.1f, 0.1f, -0.08f, 0.08f, z + 1.2f, 0x3Bu, 2);
    if (part == 1) return;
    barrel_of();
    shield();
}

// The attack aircraft: the Su-25 (both), the Su-34, the A-10C; from above,
// as it flies: its wings, its tail (one fin or two), its engines (the
// Su-25's in their nacelles along it, the A-10's high on its back), the
// canopy, its paint, the side's colour on its fin.
struct PlaneLook {
    float length;   // tiles
    float span;     // half of it
    float sweep;    // how far back the wingtips are from the roots, tiles
    float chord;    // the wing's depth at the root, tiles
    bool twin_tail;
    bool canards;
    float engines;  // across, the nacelles alongside it (0: inside)
    bool high;      // the engines high on its back, behind the wings (the A-10's)
    Camo camo;
    Color paint;
    Color camo1;
    Color camo2;
};

constexpr PlaneLook kPlaneLooks[] = {
    {1.0f, 0.47f, 0.12f, 0.2f, false, false, 0.07f, false, Camo::TwoTone, {122, 126, 108, 255}, {86, 96, 70, 255}, {86, 96, 70, 255}},
    {1.2f, 0.44f, 0.26f, 0.34f, true, true, 0.0f, false, Camo::TwoTone, {112, 124, 134, 255}, {84, 96, 108, 255}, {84, 96, 108, 255}},
    {0.95f, 0.55f, 0.0f, 0.2f, true, false, 0.09f, true, Camo::None, {112, 116, 114, 255}, {112, 116, 114, 255}, {112, 116, 114, 255}},
};
static_assert(std::size(kPlaneLooks) == static_cast<size_t>(engine::VehicleModel::A10) + 1 - static_cast<size_t>(engine::VehicleModel::Su25));

bool has_plane_look(engine::VehicleModel m) { return m >= engine::VehicleModel::Su25 && m <= engine::VehicleModel::A10; }
const PlaneLook& plane_look_of(engine::VehicleModel m) {
    return kPlaneLooks[static_cast<size_t>(m) - static_cast<size_t>(engine::VehicleModel::Su25)];
}
Vector2 plane_extent(engine::VehicleModel m) { return {plane_look_of(m).length * 0.5f, plane_look_of(m).span}; }

void draw_plane(const Frame& fr, const PlaneLook& look, Color team, int wear) {
    const Color paint = look.paint;
    const float l = look.length * 0.5f;
    const float s = look.span;
    const Color wing = shade(paint, 0.94f);
    // The wings and the tailplanes, thin, a little below the fuselage's top.
    const float root = l * 0.12f;
    for (const float sgn : {-1.0f, 1.0f}) {
        const Vector2 w[4] = {{root, sgn * 0.05f}, {root - look.sweep, sgn * s}, {root - look.sweep - look.chord * 0.45f, sgn * s},
                              {root - look.chord, sgn * 0.05f}};
        poly_solid(fr, w, 4, 1.6f, 2.2f, wing);
        const float t0 = -l + 0.12f;
        const Vector2 t[4] = {{t0 + 0.08f, sgn * 0.04f}, {t0 - 0.02f + (look.sweep > 0.0f ? -0.02f : 0.0f), sgn * s * 0.42f},
                              {t0 - 0.08f, sgn * s * 0.42f}, {t0 - 0.06f, sgn * 0.04f}};
        poly_solid(fr, t, 4, 1.8f, 2.3f, wing);
        if (look.canards) {
            const Vector2 c[3] = {{l * 0.55f, sgn * 0.04f}, {l * 0.4f, sgn * 0.14f}, {l * 0.36f, sgn * 0.04f}};
            poly_solid(fr, c, 3, 2.0f, 2.4f, wing);
        }
    }
    camouflage(fr, look, root - look.chord, root - look.sweep * 0.5f, -s * 0.8f, s * 0.8f, 2.2f, 0x2Fu, 6);
    // The engines in their nacelles alongside it, or high on its back.
    if (look.engines > 0.0f) {
        for (const float sgn : {-1.0f, 1.0f}) {
            const float a = look.high ? -l * 0.45f : -l * 0.1f;
            const float z0 = look.high ? 2.8f : 0.6f;
            round_solid(fr, a, sgn * look.engines, look.high ? 0.12f : 0.18f, 0.04f, z0, z0 + 2.4f, shade(paint, 0.88f), 0.2f, 10);
        }
    }
    // The fuselage: a long rounded body, the nose drawn to a point.
    const Vector2 body[7] = {{l, 0.0f},  {l * 0.7f, 0.05f},  {-l * 0.6f, 0.05f},  {-l, 0.025f},
                             {-l, -0.025f}, {-l * 0.6f, -0.05f}, {l * 0.7f, -0.05f}};
    poly_solid(fr, body, 7, 0.4f, 3.4f, paint, 0.3f);
    scorch(fr, -l * 0.6f, l * 0.6f, -0.04f, 0.04f, 3.4f, wear, 0x77u);
    // The canopy.
    if (!under(fr, 3.4f)) {
        const Color glass = wear >= 4 ? Color{28, 26, 24, 255} : Color{70, 96, 110, 255};
        round_solid(fr, l * 0.5f, 0.0f, 0.08f, 0.026f, 3.0f, 4.2f, glass, 0.3f, 8);
    }
    // The fin, or two; the side's colour at the top.
    for (const float c : look.twin_tail ? std::initializer_list<float>{-0.07f, 0.07f} : std::initializer_list<float>{0.0f}) {
        const Vector2 base[4] = {{-l + 0.22f, c + 0.008f}, {-l + 0.22f, c - 0.008f}, {-l + 0.02f, c - 0.008f}, {-l + 0.02f, c + 0.008f}};
        const Vector2 top[4] = {{-l + 0.08f, c + 0.006f}, {-l + 0.08f, c - 0.006f}, {-l, c - 0.006f}, {-l, c + 0.006f}};
        solid(fr, base, top, 4, 2.4f, 8.0f, shade(paint, 0.9f));
        if (wear < 4) block(fr, -l + 0.0f, -l + 0.08f, c - 0.007f, c + 0.007f, 7.0f, 8.4f, team);
    }
}

// An aircraft come down: broken apart where it hit, burnt. The middle of it
// burnt out down to its frames, black, ash over it; the nose broken off
// ahead, turned askew; the tail behind, a fin still standing; a wing torn
// off lying away, the marking on it; the other crumpled at its root; a
// tailplane, an engine's pipe, scraps of its skin thrown about. What paint
// is left grey, sooted. `layout`: how it came apart; `rust`: weeks on.
void draw_plane_wreck(const Frame& fr, const PlaneLook& look, Color team, int layout, bool rust) {
    const uint32_t seed = 0x5A17u + static_cast<uint32_t>(layout) * 7919u;
    auto rnd = [&](int i) { return rand01(seed, i); };
    const float l = look.length * 0.5f;
    const float s = look.span;
    const Color paint = rust ? mix(look.paint, {114, 68, 44, 255}, 0.7f) : mix(look.paint, {70, 66, 60, 255}, 0.25f);
    const Color black = rust ? Color{90, 58, 42, 255} : Color{34, 30, 28, 255};
    const Color ash{150, 148, 140, 255};
    const Color steel = rust ? Color{110, 76, 56, 255} : Color{180, 178, 170, 255};
    const float flip = layout == 0 ? 1.0f : -1.0f;  // which wing tore off
    auto turned = [&](float a, float c, float angle) {
        const Vector2 d{fr.f.x * std::cos(angle) - fr.f.y * std::sin(angle), fr.f.y * std::cos(angle) + fr.f.x * std::sin(angle)};
        return fr.turned(d, a, c);
    };
    auto soot = [&](const Frame& f, float a0, float a1, float c0, float c1, float z, int n, int k) {
        for (int i = 0; i < n; ++i) {
            const float a = a0 + (a1 - a0) * rnd(k + i);
            const float c = c0 + (c1 - c0) * rnd(k + 40 + i);
            patch(f, a, c, 0.05f + 0.04f * rnd(k + 80 + i), 0.035f + 0.03f * rnd(k + 120 + i), z, i % 3 == 2 ? Color{64, 44, 32, 255} : black,
                  seed + static_cast<uint32_t>(k + i));
        }
    };
    struct Piece {
        float a, c;
        int kind;
    };
    std::vector<Piece> pieces;
    for (int i = 0; i < 9; ++i) pieces.push_back({(rnd(200 + i) - 0.5f) * l * 2.8f, (rnd(220 + i) - 0.5f) * s * 2.4f, 0});  // scraps
    pieces.push_back({l * 0.08f * flip, -flip * s * 1.2f, 1});                                                          // the tailplane
    pieces.push_back({-l * 0.2f, flip * s * 0.55f + 0.08f, 2});                                                          // an engine's pipe
    pieces.push_back({0.02f * l, flip * (s * 0.95f + 0.1f), 3});                                                        // the wing torn off
    pieces.push_back({-0.95f * l, -0.03f * flip, 4});                                                                   // the tail
    pieces.push_back({0.0f, 0.0f, 5});                                                                                  // the middle
    pieces.push_back({0.9f * l, 0.05f * flip, 6});                                                                      // the nose
    std::sort(pieces.begin(), pieces.end(), [&](const Piece& p, const Piece& q) { return fr.at(p.a, p.c).y < fr.at(q.a, q.c).y; });
    for (const Piece& p : pieces) {
        switch (p.kind) {
            case 0: {  // a scrap of skin, bent up at an edge
                const Frame f = turned(p.a, p.c, rnd(static_cast<int>(p.a * 100.0f) + 300) * 6.28f);
                const float w = 0.03f + 0.04f * rnd(static_cast<int>(p.c * 100.0f) + 310);
                const Vector2 q[4] = {{w, 0.0f}, {w * 0.3f, w * 0.8f}, {-w, w * 0.2f}, {-w * 0.4f, -w * 0.7f}};
                poly_solid(f, q, 4, 0.0f, 0.4f + rnd(static_cast<int>(p.a * 50.0f) + 320), rnd(static_cast<int>(p.c * 70.0f) + 330) < 0.5f ? paint : black);
                break;
            }
            case 1: {  // a tailplane lying on its own
                const Frame f = turned(p.a, p.c, 0.9f + rnd(340) * 1.5f);
                const Vector2 t[4] = {{0.05f, 0.0f}, {-0.02f, s * 0.4f}, {-0.09f, s * 0.4f}, {-0.07f, 0.0f}};
                poly_solid(f, t, 4, 0.0f, 0.5f, paint);
                soot(f, -0.08f, 0.03f, 0.0f, s * 0.35f, 0.5f, 2, 350);
                break;
            }
            case 2: {  // an engine's pipe, silvery, black inside its end
                const Frame f = turned(p.a, p.c, 0.3f + rnd(360) * 0.8f);
                const Vector2 e[6] = {{0.16f, 0.035f}, {-0.16f, 0.04f}, {-0.17f, 0.0f}, {-0.16f, -0.04f}, {0.16f, -0.035f}, {0.17f, 0.0f}};
                poly_solid(f, e, 6, 0.0f, 2.2f, steel, 0.25f);
                patch(f, -0.15f, 0.0f, 0.02f, 0.03f, 2.3f, {20, 18, 16, 255}, seed + 7u);
                soot(f, -0.1f, 0.1f, -0.03f, 0.03f, 2.2f, 2, 370);
                break;
            }
            case 3: {  // the wing torn off, askew: its paint, its marking, the soot, its torn root
                const Frame f = turned(p.a, p.c, flip * (0.35f + 0.5f * rnd(380)));
                const float sg = flip;
                const Vector2 w[4] = {{0.1f, 0.0f}, {0.1f - look.sweep, sg * s * 0.9f}, {0.1f - look.sweep - look.chord * 0.45f, sg * s * 0.9f},
                                      {0.1f - look.chord, 0.0f}};
                poly_solid(f, w, 4, 0.0f, 0.7f, paint);
                const Vector2 m{0.1f - look.sweep * 0.5f - look.chord * 0.4f, sg * s * 0.5f};
                patch(f, m.x, m.y, 0.07f, 0.06f, 0.75f, {236, 234, 226, 255}, seed + 11u);  // the marking
                patch(f, m.x, m.y, 0.05f, 0.042f, 0.8f, team, seed + 12u);
                soot(f, 0.1f - look.chord, 0.1f - look.sweep * 0.3f, 0.0f, sg * s * 0.4f, 0.75f, 4, 390);
                for (int i = 0; i < 4; ++i) {  // its torn root, ragged
                    const float a = 0.1f - look.chord * static_cast<float>(i) / 4.0f;
                    DrawLineV(f.at(a, 0.0f, 0.7f), f.at(a - 0.02f, -sg * 0.03f * rnd(400 + i), 0.5f), lit(black));
                }
                break;
            }
            case 4: {  // the tail: the back of it, a fin standing (bent), the other down
                const Frame f = turned(p.a, p.c, -flip * (0.25f + 0.4f * rnd(410)));
                const Vector2 b[6] = {{0.26f, 0.05f}, {-0.16f, 0.045f}, {-0.22f, 0.02f}, {-0.22f, -0.02f}, {-0.16f, -0.045f}, {0.26f, -0.05f}};
                poly_solid(f, b, 6, 0.0f, 2.2f, paint, 0.25f);
                soot(f, 0.05f, 0.26f, -0.04f, 0.04f, 2.2f, 3, 420);  // the break
                DrawLineV(f.at(0.26f, -0.05f, 1.0f), f.at(0.26f, 0.05f, 1.6f), lit(black));
                const float c0 = look.twin_tail ? 0.07f : 0.0f;
                const Vector2 base[4] = {{0.0f, c0 + 0.008f}, {0.0f, c0 - 0.008f}, {-0.2f, c0 - 0.008f}, {-0.2f, c0 + 0.008f}};
                const Vector2 top[4] = {{-0.14f, c0 + 0.03f}, {-0.14f, c0 + 0.016f}, {-0.22f, c0 + 0.016f}, {-0.22f, c0 + 0.03f}};  // bent over
                solid(f, base, top, 4, 2.0f, 7.0f, shade(paint, 0.9f));
                block(f, -0.22f, -0.14f, c0 + 0.012f, c0 + 0.032f, 6.2f, 7.2f, team);
                if (look.twin_tail) {  // the other one lying flat
                    const Vector2 fin[4] = {{-0.02f, -0.1f}, {-0.2f, -0.1f}, {-0.26f, -0.24f}, {-0.1f, -0.24f}};
                    poly_solid(f, fin, 4, 0.0f, 0.4f, shade(paint, 0.85f));
                }
                break;
            }
            case 5: {  // the middle burnt out: black to its frames, ash on it; the other wing crumpled at the root
                const float sg = -flip;
                const Vector2 w[4] = {{0.05f, 0.0f}, {0.05f - look.sweep * 0.5f, sg * s * 0.5f}, {-look.chord * 0.6f, sg * s * 0.46f}, {-look.chord * 0.9f, 0.0f}};
                poly_solid(fr, w, 4, 0.0f, 0.5f, mix(paint, black, 0.5f));
                soot(fr, -look.chord * 0.8f, 0.0f, 0.0f, sg * s * 0.45f, 0.5f, 3, 430);
                const Vector2 body[6] = {{0.42f * l, 0.055f}, {-0.45f * l, 0.05f}, {-0.5f * l, 0.0f}, {-0.45f * l, -0.05f}, {0.42f * l, -0.055f}, {0.46f * l, 0.0f}};
                poly_solid(fr, body, 6, 0.0f, 1.4f, black, 0.2f);
                for (float a = -0.4f * l; a < 0.4f * l; a += 0.07f) {  // its frames, showing
                    DrawLineV(fr.at(a, -0.045f, 1.4f), fr.at(a + 0.01f, 0.045f, 1.4f), lit(shade(steel, 0.7f)));
                }
                for (int i = 0; i < 6; ++i) patch(fr, (rnd(440 + i) - 0.5f) * 0.8f * l, (rnd(450 + i) - 0.5f) * 0.08f, 0.03f, 0.02f, 1.5f, ash, seed + 20u + static_cast<uint32_t>(i));
                break;
            }
            case 6: {  // the nose broken off, turned: the canopy smashed, soot back from the break
                const Frame f = turned(p.a, p.c, flip * (0.3f + 0.5f * rnd(460)));
                const Vector2 n[5] = {{0.34f, 0.0f}, {0.16f, 0.05f}, {-0.2f, 0.055f}, {-0.2f, -0.055f}, {0.16f, -0.05f}};
                poly_solid(f, n, 5, 0.0f, 2.6f, paint, 0.3f);
                round_solid(f, 0.06f, 0.0f, 0.075f, 0.025f, 2.2f, 3.0f, {26, 24, 22, 255}, 0.3f, 8);  // the canopy, gone
                soot(f, -0.2f, 0.02f, -0.05f, 0.05f, 2.6f, 4, 470);
                DrawLineV(f.at(-0.2f, -0.05f, 1.2f), f.at(-0.2f, 0.05f, 2.0f), lit(black));  // the break
                break;
            }
        }
    }
}

// Makes a drawn frame pixel art: every colour to the nearest of the
// palette (ramps of the paint, the side's colour, steel, rubber, glass),
// a line a pixel darker where one part meets another below or to the right,
// and a dark outline round the whole.
void pixelate(Image& img, const std::vector<Color>& palette) {
    std::vector<int16_t> cache(32768, -1);  // a colour, five bits a channel: its palette entry
    auto* px = static_cast<Color*>(img.data);
    const int w = img.width;
    const int h = img.height;
    auto lum = [](Color c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; };
    for (int i = 0; i < w * h; ++i) {
        if (px[i].a < 100) {
            px[i] = {0, 0, 0, 0};
            continue;
        }
        const size_t key = static_cast<size_t>((px[i].r >> 3) << 10 | (px[i].g >> 3) << 5 | (px[i].b >> 3));
        if (cache[key] >= 0) {
            px[i] = palette[static_cast<size_t>(cache[key])];
            continue;
        }
        int best = 0;
        float best_d = 1e9f;
        for (size_t k = 0; k < palette.size(); ++k) {
            const float dr = static_cast<float>(px[i].r - palette[k].r);
            const float dg = static_cast<float>(px[i].g - palette[k].g);
            const float db = static_cast<float>(px[i].b - palette[k].b);
            const float d = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
            if (d < best_d) {
                best_d = d;
                best = static_cast<int>(k);
            }
        }
        cache[key] = static_cast<int16_t>(best);
        px[i] = palette[static_cast<size_t>(best)];
    }
    std::vector<Color> out(px, px + w * h);
    constexpr Color kOutline{20, 20, 14, 255};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Color c = px[y * w + x];
            auto at = [&](int xx, int yy) { return xx < 0 || yy < 0 || xx >= w || yy >= h ? Color{0, 0, 0, 0} : px[yy * w + xx]; };
            if (c.a == 0) {
                if (at(x - 1, y).a || at(x + 1, y).a || at(x, y - 1).a || at(x, y + 1).a) out[y * w + x] = kOutline;
                continue;
            }
            if (at(x, y - 1).a == 0) {  // the light catching a top edge
                out[y * w + x] = shade(c, 1.32f);
                continue;
            }
            // A part's top edge under a darker one (two pixels of it, not a
            // seam), the part going on below: it catches the light.
            const Color up = at(x, y - 1);
            const Color up2 = at(x, y - 2);
            const Color below = at(x, y + 1);
            if (up.a && up2.a && below.a && lum(c) - lum(up) > 34.0f && lum(c) - lum(up2) > 34.0f &&
                std::fabs(lum(below) - lum(c)) < 14.0f) {
                out[y * w + x] = shade(c, 1.18f);
                continue;
            }
            const Color r = at(x + 1, y);
            const Color d = at(x, y + 1);
            const Color l = at(x - 1, y);
            const bool edge = (r.a && lum(r) - lum(c) > 26.0f) || (d.a && lum(d) - lum(c) > 26.0f) ||
                              (l.a && lum(l) - lum(c) > 26.0f) || (up.a && lum(up) - lum(c) > 26.0f);
            if (edge) out[y * w + x] = shade(c, 0.66f);  // where a lighter face meets this darker one
        }
    }
    std::copy(out.begin(), out.end(), px);
}

// Where to write the baked sprite sheets to look at them (ANCHOR_DUMP_SPRITES), or null.
const char* dump_dir() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // read once, never written
#endif
    return std::getenv("ANCHOR_DUMP_SPRITES");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

void WorldRenderer::bake_trees() const {
    if (!tree_sheets_.empty()) return;
    if (tree_target_.id == 0) {
        tree_target_ = LoadRenderTexture(kTreeW * kTreeVariants, kTreeH);
        SetTextureFilter(tree_target_.texture, TEXTURE_FILTER_POINT);
    }
    const float light = g_light;
    const float zoom = g_zoom;
    g_light = 1.0f;
    g_zoom = 1.0f;  // full detail
    for (int k = 0; k < kTreeKinds; ++k) {
        const auto kind = static_cast<TreeKind>(k);
        const std::vector<Color> palette = tree_palette(kind);
        for (int stage = 0; stage < kTreeStages; ++stage) {
            BeginTextureMode(tree_target_);
            ClearBackground({0, 0, 0, 0});
            for (int v = 0; v < kTreeVariants; ++v) {
                draw_tree_at({kTreeOrigin.x + static_cast<float>(v * kTreeW), kTreeOrigin.y}, tree_variant(kind, v, stage), false);
                g_leaf = 1.0f;
                g_bare = 0.0f;
            }
            EndTextureMode();
            Image img = LoadImageFromTexture(tree_target_.texture);
            ImageFlipVertical(&img);
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            pixelate(img, palette);
            if (const char* dump = dump_dir()) ExportImage(img, TextFormat("%s/trees_%d_%d.png", dump, k, stage));
            SpriteSheet sheet;
            sheet.atlas = LoadTextureFromImage(img);
            SetTextureFilter(sheet.atlas, TEXTURE_FILTER_POINT);
            UnloadImage(img);
            sheet.w = kTreeW;
            sheet.h = kTreeH;
            sheet.dirs = kTreeVariants;
            sheet.frames = 1;
            sheet.origin = kTreeOrigin;
            tree_sheets_[k * kTreeStages + stage] = sheet;
        }
    }
    g_light = light;
    g_zoom = zoom;
}

void WorldRenderer::bake_sprites(const engine::World& world) const {
    for (size_t p = 0; p < world_era_.size(); ++p) {
        const auto player = static_cast<engine::PlayerId>(p);
        world_era_[p] = world.era_level(player);
        world_kit_[p] = (world.has_upgrade(player, engine::UpgradeId::AddOnArmor) ? 1 : 0) |
                        (world.has_upgrade(player, engine::UpgradeId::Atgm) ? 2 : 0);
    }
    constexpr int kW = 128;
    constexpr int kH = 96;
    constexpr int kTallH = 128;  // for a howitzer's gun laid high
    constexpr int kDirs = 32;
    if (bake_target_.id == 0) {
        bake_target_ = LoadRenderTexture(kW * kDirs, kTallH);
        SetTextureFilter(bake_target_.texture, TEXTURE_FILTER_POINT);
    }
    // The colours a vehicle's sprites are made of: ramps of its paint and its
    // camouflage's, of its side's colour; wood, steel, rubber, glass, soot, rust; `more`.
    auto palette_of = [](Color paint, Camo camo, Color camo1, Color camo2, Color team, std::initializer_list<Color> more) {
        std::vector<Color> palette;
        for (const float k : {0.28f, 0.4f, 0.54f, 0.7f, 0.88f, 1.08f, 1.3f, 1.56f}) palette.push_back(shade(paint, k));
        if (camo != Camo::None) {
            for (const Color c : {camo1, camo2}) {
                for (const float k : {0.55f, 0.8f, 1.05f, 1.3f}) palette.push_back(shade(c, k));
            }
        }
        palette.push_back({112, 84, 54, 255});  // wood
        palette.push_back({150, 116, 76, 255});
        for (const float k : {0.6f, 0.85f, 1.1f, 1.3f}) palette.push_back(shade(team, k));
        for (const Color c : {Color{28, 28, 26, 255}, Color{44, 44, 40, 255}, Color{62, 64, 58, 255}, Color{92, 94, 84, 255},
                              Color{124, 126, 112, 255}, Color{70, 96, 110, 255}, Color{210, 206, 170, 255},
                              Color{28, 26, 24, 255}, Color{64, 44, 32, 255}, Color{96, 62, 40, 255}}) {  // and soot, rust
            palette.push_back(c);
        }
        for (const Color c : more) palette.push_back(c);
        return palette;
    };
    // One sheet: every direction (and frame) of a part drawn by `draw` into
    // a frame of its own, made pixel art; the whip aerials `draw` gives added
    // after, a pixel thin, no outline.
    auto bake = [&](SpritePart part, int variant, engine::PlayerId owner, int frames, const std::vector<Color>& palette,
                    const std::string& name, const auto& draw, int frame_h = 96) {
        const Vector2 origin{64.0f, static_cast<float>(frame_h - 34)};
        const float light = g_light;
        g_light = 1.0f;
        Image atlas = GenImageColor(kW * kDirs, frame_h * frames, {0, 0, 0, 0});
        for (int frame = 0; frame < frames; ++frame) {
            BeginTextureMode(bake_target_);
            ClearBackground({0, 0, 0, 0});
            std::vector<Whip> whips;
            for (int d = 0; d < kDirs; ++d) {
                const float a = static_cast<float>(d) * 6.2831853f / kDirs;
                const Vector2 f{std::cos(a), std::sin(a)};
                constexpr float kScale = kVehicleScale;
                const Vector2 F = iso_offset(f);
                const Vector2 S = iso_offset({-f.y, f.x});
                const Frame fr{{origin.x + static_cast<float>(d * kW), origin.y}, {F.x * kScale, F.y * kScale},
                               {S.x * kScale, S.y * kScale}, f, {-f.y, f.x}, kScale};
                draw(fr, frame, whips);
            }
            EndTextureMode();
            Image img = LoadImageFromTexture(bake_target_.texture);
            ImageFlipVertical(&img);
            ImageCrop(&img, {0.0f, 0.0f, static_cast<float>(kW * kDirs), static_cast<float>(frame_h)});
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            pixelate(img, palette);
            for (const Whip& whip : whips) {
                const int x = static_cast<int>(std::lround(whip.foot.x));
                const int y = static_cast<int>(std::lround(whip.foot.y));
                ImageDrawLine(&img, x, y, x, y - whip.length, {36, 36, 32, 255});
            }
            ImageDraw(&atlas, img, {0, 0, static_cast<float>(kW * kDirs), static_cast<float>(frame_h)},
                      {0, static_cast<float>(frame * frame_h), static_cast<float>(kW * kDirs), static_cast<float>(frame_h)}, WHITE);
            UnloadImage(img);
        }
        if (const char* dump = dump_dir()) ExportImage(atlas, TextFormat("%s/%s.png", dump, name.c_str()));  // to look at, for development
        SpriteSheet sheet;
        sheet.atlas = LoadTextureFromImage(atlas);
        SetTextureFilter(sheet.atlas, TEXTURE_FILTER_POINT);
        UnloadImage(atlas);
        sheet.w = kW;
        sheet.h = frame_h;
        sheet.dirs = kDirs;
        sheet.frames = frames;
        sheet.origin = origin;
        sheets_[{{static_cast<int>(part), variant}, owner}] = sheet;
        g_light = light;
    };
    auto team_of = [](int wear, engine::PlayerId owner) {
        return wear == 5 ? Color{110, 64, 40, 255} : wear == 4 ? Color{60, 54, 48, 255} : theme::player_color(owner);
    };
    // Worn: darker, burnt out black and rusty, then rusted over.
    auto worn = [](auto& look, int wear) {
        const Color base = look.paint;
        if (wear == 5) {  // a wreck gone rusty with the weeks
            look.paint = mix(base, {114, 68, 44, 255}, 0.78f);
            look.camo = Camo::None;
        } else if (wear == 4) {  // burnt out: the paint gone black and rusty
            look.paint = mix(base, {46, 40, 34, 255}, 0.75f);
            look.camo = Camo::None;
        } else if (wear == 3) {
            look.paint = mix(base, {52, 48, 42, 255}, 0.3f);
        }
    };

    struct Wanted {
        engine::PlayerId owner;
        engine::VehicleModel model;
        int era;   // reactive armor
        int wear;  // 0 whole .. 3 barely going, 4 a wreck, 5 a rusted one
        int sink;  // 0 on firm ground; in a bog 1 to its fenders, 3 to its deck, 4 its hull gone; 2 its turret's top only
    };
    const engine::TileMap& map = world.map();
    auto terrain_under = [&](Vector2 g) {
        return map.terrain(map.clamp_tile({static_cast<int32_t>(std::floor(g.x)), static_cast<int32_t>(std::floor(g.y))}));
    };
    std::vector<Wanted> wanted;
    for (const engine::Unit& u : world.units()) {
        const engine::UnitTypeDef& def = engine::unit_type(u.type);
        if (!drawn_as_armor(def)) continue;
        const int era = kit(def.model, u.owner);
        // Every wear up front (cheap: a row of directions at once), so a hit never waits for a bake.
        for (int wear = 0; wear <= 5; ++wear) wanted.push_back({u.owner, def.model, wear >= 4 ? 0 : era, wear, 0});  // 5: rusted
        if (terrain_under(to_vector2(u.pos)) == engine::Terrain::Swamp) {  // in a bog: its sunk looks, as it gets worse
            const int now = wear_of(u.hp, def.max_hp);
            for (int wear = now; wear <= std::min(now + 1, 3); ++wear) {
                for (const int sink : {1, 3, 4}) {
                    if (def.floats && sink != 1) continue;  // an amphibious one only sits in it
                    wanted.push_back({u.owner, def.model, era, wear, sink});
                }
            }
        }
        if (dump_dir()) {  // every kind of armor, to look at
            for (int e = 0; e <= 3; ++e) wanted.push_back({u.owner, def.model, e, 0, 0});
        }
    }
    for (const Remains& r : remains_) {
        if (!drawn_as_armor(engine::unit_type(r.type))) continue;
        const engine::VehicleModel model = engine::unit_type(r.type).model;
        const engine::Terrain t = terrain_under(r.ground);
        if (t == engine::Terrain::Water || r.sunk) {  // drowned, or gone under in a bog: not burnt, only its turret's top showing
            wanted.push_back({r.owner, model, kit(model, r.owner), 1, 2});
            continue;
        }
        const int sink = t == engine::Terrain::Swamp ? 1 : 0;
        wanted.push_back({r.owner, model, 0, 4, sink});
        wanted.push_back({r.owner, model, 0, 5, sink});
    }
    for (const auto& [owner, model, era, wear, sink] : wanted) {
        const int variant = armor_variant(model, era, wear, sink);
        // A howitzer's barrel apart, laid and recoiling: one for the paint
        // it has up to battered, one for the worn paint of barely going.
        const bool still = wear >= 4 || sink == 2;
        const int barrel_variant = armor_variant(model, 0, wear >= 3 ? 3 : 0, sink);
        const bool want_barrel = raises_gun(model) && !still && !sheets_.count({{static_cast<int>(SpritePart::Barrel), barrel_variant}, owner});
        const bool want_body = !sheets_.count({{static_cast<int>(SpritePart::Hull), variant}, owner});
        if (!want_body && !want_barrel) continue;
        const bool apc = has_vehicle_look(model);
        TankLook look = look_of(apc ? engine::VehicleModel::Standard : model);
        VehicleLook carrier = vehicle_look_of(apc ? model : engine::VehicleModel::Bmp2);
        if (model == engine::VehicleModel::Bmp2 && engine::axis_of(owner) == engine::Axis::Democratic) {
            carrier.paint = kUkrainianGreen;  // Ukraine's
        }
        worn(look, wear);
        worn(carrier, wear);
        const Dims dims = dims_of(model);
        const Color team = team_of(wear, owner);
        const std::vector<Color> palette = apc ? palette_of(carrier.paint, carrier.camo, carrier.camo1, carrier.camo2, team, {})
                                               : palette_of(look.paint, look.camo, look.camo1, look.camo2, team, {});
        const float sunk = sink == 1   ? 5.0f
                           : sink == 3 ? dims.deck - 0.5f
                           : sink == 4 ? dims.turret_z + dims.turret_h * 0.35f
                           : sink == 2 ? dims.turret_z + dims.turret_h * 0.75f
                                       : 0.0f;
        const int w = std::min(wear, 4);
        if (want_barrel) {
            bake(SpritePart::Barrel, barrel_variant, owner, kBarrelFrames, palette,
                 TextFormat("barrel_%d_%d_%d_%d", static_cast<int>(model), wear >= 3 ? 3 : 0, sink, static_cast<int>(owner)),
                 [&](Frame fr, int frame, std::vector<Whip>&) {
                     fr.sink = sunk;
                     draw_vehicle_turret(fr, carrier, team, era, w, frame / kRecoilStates + 1, false, 2, kRecoilShare[frame % kRecoilStates]);
                 },
                 kTallH);
        }
        if (!want_body) continue;
        bake(SpritePart::Hull, variant, owner, 2, palette,
             TextFormat("sheet_%d_%d_%d_%d_%d_%d", static_cast<int>(SpritePart::Hull), static_cast<int>(model), era, wear, sink, static_cast<int>(owner)),
             [&](Frame fr, int frame, std::vector<Whip>&) {
                 fr.sink = sunk;
                 if (apc) {
                     draw_vehicle_hull(fr, carrier, frame, era, team, w);
                 } else {
                     draw_tank_hull(fr, look, frame, era, team, w);
                 }
             });
        // A tank's gun laid at each angle it's drawn at; a howitzer's
        // turret travelling, and set up without its gun (the barrel apart);
        // a wreck's, a drowned one's, just the one.
        const int turret_frames = still ? 1 : raises_gun(model) ? 2 : apc ? 1 : kTankFrames;
        bake(SpritePart::Turret, variant, owner, turret_frames, palette,
             TextFormat("sheet_%d_%d_%d_%d_%d_%d", static_cast<int>(SpritePart::Turret), static_cast<int>(model), era, wear, sink, static_cast<int>(owner)),
             [&](Frame fr, int frame, std::vector<Whip>& whips) {
                 fr.sink = sunk;
                 if (apc) {
                     draw_vehicle_turret(fr, carrier, team, era, w, 0, false, frame == 1 ? 1 : 0);  // (its radar apart, below)
                 } else {
                     draw_tank_turret(fr, look, team, era, w, kTankPitch[frame]);
                 }
                 // The whip aerial: short, shot away when battered.
                 const Vector2 af = apc ? vehicle_aerial_foot(carrier) : aerial_foot(look);
                 if (wear < 3 || sink == 2) whips.push_back({fr.at(af.x, af.y, dims.turret_z + dims.turret_h), 11});
             });
        if (apc && has_radar(carrier)) {
            bake(SpritePart::Radar, variant, owner, 1, palette,
                 TextFormat("radar_%d_%d_%d_%d", static_cast<int>(model), wear, sink, static_cast<int>(owner)),
                 [&](Frame fr, int, std::vector<Whip>&) {
                     fr.sink = sunk;
                     draw_aa_radar(fr, carrier, w);
                 });
        }
    }

    // Trucks: every wear up front, the load they carry as they get it.
    struct TruckWanted {
        engine::PlayerId owner;
        TruckModel model;
        int wear;
        int sink;  // 0, or 2: drowned, only the top of it showing
        int load;  // what it carries (a Cargo), as its sprite shows it
    };
    std::vector<TruckWanted> trucks;
    for (const engine::Unit& u : world.units()) {
        const std::optional<TruckModel> model = truck_model(u.type, u.owner);
        if (!model) continue;
        for (int wear = 0; wear <= 5; ++wear) trucks.push_back({u.owner, *model, wear, 0, 0});
        if (const int load = truck_load(cargo_of(world, u)); load > 0) {
            const int now = wear_of(u.hp, engine::unit_type(u.type).max_hp);
            for (int wear = now; wear <= std::min(now + 1, 3); ++wear) trucks.push_back({u.owner, *model, wear, 0, load});
        }
    }
    for (const Remains& r : remains_) {
        const std::optional<TruckModel> model = truck_model(r.type, r.owner);
        if (!model) continue;
        if (terrain_under(r.ground) == engine::Terrain::Water) {
            trucks.push_back({r.owner, *model, 1, 2, 0});
            continue;
        }
        trucks.push_back({r.owner, *model, 4, 0, wreck_load(static_cast<Cargo>(r.cargo))});
        trucks.push_back({r.owner, *model, 5, 0, wreck_load(static_cast<Cargo>(r.cargo))});
    }
    // Towed guns and aircraft: every wear up front.
    std::vector<std::pair<engine::PlayerId, engine::VehicleModel>> small;
    auto want_small = [&](engine::PlayerId owner, engine::UnitTypeId type) {
        const engine::VehicleModel model = engine::unit_type(type).model;
        if (!has_gun_look(model) && !has_plane_look(model)) return;
        if (std::find(small.begin(), small.end(), std::pair{owner, model}) == small.end()) small.emplace_back(owner, model);
    };
    for (const engine::Unit& u : world.units()) want_small(u.owner, u.type);
    for (const Remains& r : remains_) want_small(r.owner, r.type);
    for (const auto& [owner, model] : small) {
        const bool plane = has_plane_look(model);
        for (int wear = 0; wear <= 5; ++wear) {
            const int variant = small_variant(model, wear);
            const SpritePart part = plane ? SpritePart::Plane : SpritePart::Gun;
            if (sheets_.count({{static_cast<int>(part), variant}, owner})) continue;
            GunLook gun = gun_look_of(plane ? engine::VehicleModel::D30 : model);
            PlaneLook craft = plane_look_of(plane ? model : engine::VehicleModel::Su25);
            worn(gun, wear);
            worn(craft, wear);
            const Color team = team_of(wear, owner);
            const std::vector<Color> palette = plane ? palette_of(craft.paint, craft.camo, craft.camo1, craft.camo2, team, {})
                                                     : palette_of(gun.paint, gun.camo, gun.camo1, gun.camo2, team, {});
            const int w = std::min(wear, 4);
            // A towed gun: packed; set up without its barrel (apart, laid and recoiling).
            bake(part, variant, owner, plane || wear >= 4 ? 1 : 2, palette,
                 TextFormat("%s_%d_%d_%d", plane ? "plane" : "gun", static_cast<int>(model), wear, static_cast<int>(owner)),
                 [&](Frame fr, int frame, std::vector<Whip>&) {
                     if (plane) {
                         draw_plane(fr, craft, team, w);
                     } else {
                         draw_towed_gun(fr, gun, frame, team, w, frame == 1 ? 1 : 0);
                     }
                 });
            if (plane && wear >= 4) {  // its crashed wreck, burnt (and rusted): broken apart two ways
                PlaneLook left = plane_look_of(model);
                std::vector<Color> wreck = palette_of(left.paint, left.camo, left.camo1, left.camo2, theme::player_color(owner),
                                                      {{150, 148, 140, 255}, {180, 178, 170, 255}, {236, 234, 226, 255}, {90, 92, 90, 255}});
                for (const float k : {0.45f, 0.62f, 0.8f}) wreck.push_back(shade(mix(left.paint, {114, 68, 44, 255}, 0.7f), k));
                bake(part, small_variant(model, wear + 2), owner, 2, wreck,
                     TextFormat("plane_wreck_%d_%d_%d", static_cast<int>(model), wear, static_cast<int>(owner)),
                     [&](Frame fr, int frame, std::vector<Whip>&) { draw_plane_wreck(fr, left, theme::player_color(owner), frame, wear == 5); });
            }
            if (!plane && (wear == 0 || wear == 3)) {
                bake(SpritePart::Barrel, armor_variant(model, 0, wear, 0), owner, kBarrelFrames, palette,
                     TextFormat("barrel_%d_%d_0_%d", static_cast<int>(model), wear, static_cast<int>(owner)),
                     [&](Frame fr, int frame, std::vector<Whip>&) {
                         draw_towed_gun(fr, gun, frame / kRecoilStates + 1, team, w, 2, kRecoilShare[frame % kRecoilStates]);
                     },
                     kTallH);
            }
        }
    }
    for (const auto& [owner, model, wear, sink, load] : trucks) {
        const int variant = truck_variant(model, wear, sink, load);
        if (sheets_.count({{static_cast<int>(SpritePart::TruckBody), variant}, owner})) continue;
        TruckLook look = truck_look_of(model);
        worn(look, wear);
        const Color team = team_of(wear, owner);
        const Color tarp = mix(look.paint, {128, 122, 86, 255}, 0.35f);
        std::vector<Color> palette =
            palette_of(look.paint, look.camo, look.camo1, look.camo2, team,
                       {shade(tarp, 0.7f), tarp, shade(tarp, 1.25f), cargo_color(engine::Resource::Food), cargo_color(engine::Resource::Ammo),
                        cargo_color(engine::Resource::Fuel), cargo_color(engine::Resource::Materials), Color{150, 156, 146, 255}});
        if (load > 0) {  // the load's colours, in their shades
            static constexpr Color kLoads[] = {{176, 156, 112, 255}, {214, 208, 190, 255}, {190, 150, 98, 255}, {104, 80, 56, 255}, {206, 172, 122, 255},
                                               {160, 84, 60, 255},   {150, 148, 142, 255}, {140, 136, 128, 255}, {88, 100, 64, 255},  {170, 130, 84, 255},
                                               {112, 112, 80, 255},  {196, 160, 70, 255},  {60, 90, 130, 255},   {150, 60, 46, 255},  {70, 100, 70, 255}};
            for (const Color c : kLoads) {
                for (const float k : {0.45f, 0.62f, 0.8f, 1.0f, 1.2f}) palette.push_back(shade(c, k));
            }
        }
        const float sunk = sink == 2 ? truck_height(look) - 2.5f : 0.0f;
        const int w = std::min(wear, 4);
        bake(SpritePart::TruckBody, variant, owner, 3, palette,
             TextFormat("truck_%d_%d_%d_%d_%d", static_cast<int>(model), wear, sink, load, static_cast<int>(owner)),
             [&](Frame fr, int frame, std::vector<Whip>& whips) {
                 fr.sink = sunk;
                 draw_truck_body(fr, look, frame, team, w, load, whips);
             });
        if (look.top != Top::None) {
            bake(SpritePart::TruckTop, variant, owner, 2, palette,
                 TextFormat("truck_top_%d_%d_%d_%d", static_cast<int>(model), wear, sink, static_cast<int>(owner)),
                 [&](Frame fr, int frame, std::vector<Whip>&) {
                     fr.sink = sunk;
                     draw_truck_top(fr, look, frame, w);
                 });
        }
    }
}

int WorldRenderer::kit(engine::VehicleModel model, engine::PlayerId owner) const {
    if (!has_vehicle_look(model)) return world_era_[owner % world_era_.size()];
    return model <= engine::VehicleModel::Namer ? world_kit_[owner % world_kit_.size()] : 0;  // an SPG's, an AA gun's: none
}

const WorldRenderer::SpriteSheet* WorldRenderer::sheet(SpritePart part, int variant, engine::PlayerId owner) const {
    const auto it = sheets_.find({{static_cast<int>(part), variant}, owner});
    return it == sheets_.end() ? nullptr : &it->second;
}

// A frame of a sprite sheet with its ground point at `at`: the direction
// nearest to `dir` (on the ground).
// A carousel autoloader (the Soviet line's and its heirs'), or the T-62's
// spent-case ejector: the gun goes to its loading angle to be reloaded. A
// bustle autoloader or a loader loads it where it's laid.
bool loads_at_an_angle(engine::VehicleModel m) {
    using engine::VehicleModel;
    switch (m) {
        case VehicleModel::Standard:
        case VehicleModel::T64BV:
        case VehicleModel::T64BM:
        case VehicleModel::T62M:
        case VehicleModel::T72B3:
        case VehicleModel::T80BVM:
        case VehicleModel::T90M:
        case VehicleModel::Type99A:
        case VehicleModel::Karrar: return true;
        default: return false;
    }
}

float WorldRenderer::gun_elevation(const engine::Unit& u) const {
    const auto it = vehicles_seen_.find(u.id);
    if (it == vehicles_seen_.end() || it->second.elev < 0.0f) return 0.0f;
    const engine::UnitTypeDef& def = engine::unit_type(u.type);
    const int step = static_cast<int>(std::lround(it->second.elev));
    if (def.tank && drawn_as_armor(def)) return kTankPitch[kTankLadder[std::clamp(step, 0, 4)]];
    if ((raises_gun(def.model) || has_gun_look(def.model)) && u.deployed) return kHowitzerPitch[std::clamp(step, 1, kHowitzerFrames - 1)];
    return 0.0f;
}

// The rear troops at their work, as it looks. Each blow of an axe: chips of
// wood flying, the tree shivering; of a pick: chips of stone, a puff of its
// dust, now and then a spark. A tree whose turn it was comes down: over it
// goes from its stump, away from the axe. A load handed in: thrown up into
// the truck's bed (the bed filling), or down by the door of the depot.
bool WorldRenderer::truck_takes_it(const engine::World& world, const engine::Unit& u) {
    const Vector2 at = to_vector2(u.pos);
    float truck = 1e18f;
    for (const engine::Unit& t : world.units()) {
        if (t.owner != u.owner || t.order != engine::Order::Collect) continue;
        const Vector2 park = to_vector2(t.order_point);
        truck = std::min(truck, (park.x - at.x) * (park.x - at.x) + (park.y - at.y) * (park.y - at.y));
    }
    if (truck >= 1e18f) return false;
    float door = 1e18f;
    for (const engine::Structure& s : world.structures()) {
        if (s.owner != u.owner || !s.built) continue;
        if (s.type != engine::StructureType::Headquarters && engine::role_of(s) != engine::StructureType::Warehouse) continue;
        for (const engine::TilePos& tile : s.tiles) {
            const float dx = std::max({static_cast<float>(tile.x) - at.x, 0.0f, at.x - static_cast<float>(tile.x + 1)});
            const float dy = std::max({static_cast<float>(tile.y) - at.y, 0.0f, at.y - static_cast<float>(tile.y + 1)});
            door = std::min(door, dx * dx + dy * dy);
        }
    }
    return truck < door;
}

void WorldRenderer::update_work(const engine::World& world, float dt) {
    const engine::TileMap& map = world.map();
    const double now = GetTime();
    auto unit = [](Vector2 v) {
        const float l = std::hypot(v.x, v.y);
        return l > 0.0f ? Vector2{v.x / l, v.y / l} : Vector2{1.0f, 0.0f};
    };
    for (const engine::Unit& u : world.units()) {
        if (u.type != engine::UnitTypeId::Worker) continue;
        const bool rock = map.contains(u.gather_tile) && map.terrain(u.gather_tile) == engine::Terrain::Rock;
        const Vector2 f = unit(to_vector2(u.facing));
        const Vector2 g = to_vector2(u.pos);
        // The blow.
        int frame = -1;
        if (u.order == engine::Order::Gather && u.work > 0 && !u.moving) {
            const engine::Tick needed = rock ? engine::kQuarryTicks : engine::kChopTicks;
            frame = static_cast<float>(u.work % needed) / static_cast<float>(needed) < 0.5f ? 0 : 1;
        }
        auto [stroke, fresh] = stroke_.try_emplace(u.id, frame);
        if (!fresh && stroke->second == 0 && frame == 1 && !u.inside && shows(world, u)) {
            const Vector2 hit{g.x + f.x * 0.3f, g.y + f.y * 0.3f};
            for (int i = 0; i < (rock ? 5 : 6); ++i) {
                Particle p{};
                p.kind = Particle::Kind::Clod;
                const float a = (fx_random() - 0.5f) * 2.4f;
                const float out = 0.4f + 0.7f * fx_random();
                p.ground = hit;
                p.vel = {(-f.x * std::cos(a) + f.y * std::sin(a)) * out, (-f.y * std::cos(a) - f.x * std::sin(a)) * out};  // back towards him, fanned out
                p.z = rock ? 1.5f : 4.0f + 3.0f * fx_random();
                p.vz = 22.0f + 30.0f * fx_random();
                p.life = 0.55f + 0.35f * fx_random();
                p.size = 0.9f + 0.7f * fx_random();
                p.color = rock ? (i % 2 == 0 ? Color{150, 146, 138, 255} : Color{112, 108, 100, 255})
                               : (i % 3 == 0 ? Color{220, 190, 136, 255} : Color{164, 124, 80, 255});
                particles_.push_back(p);
            }
            if (rock) {  // its dust, a spark off the steel now and then
                Particle d{};
                d.kind = Particle::Kind::Smoke;
                d.ground = hit;
                d.z = 2.0f;
                d.vz = 5.0f;
                d.vel = {0.05f, -0.03f};
                d.life = 1.0f;
                d.size = 1.8f;
                d.grow = 4.0f;
                d.color = {186, 176, 156, 150};
                particles_.push_back(d);
                if (fx_random() < 0.35f) spawn_sparks(hit, 2.0f, 2, {255, 226, 150, 255}, 0.6f);
            } else {
                chopped_at_[u.gather_tile.y * map.width() + u.gather_tile.x] = now;
                if (tree_standing_.find(u.gather_tile.y * map.width() + u.gather_tile.x) == tree_standing_.end()) {
                    tree_standing_[u.gather_tile.y * map.width() + u.gather_tile.x] = 255;  // (to be counted below)
                }
            }
        }
        stroke->second = frame;
        // The load handed in.
        auto [carried, first] = carried_.try_emplace(u.id, u.carrying);
        if (!first && u.carrying < carried->second && !u.inside && shows(world, u)) {
            const engine::Unit* truck = nullptr;
            float best = 3.0f;
            for (const engine::Unit& t : world.units()) {
                if (t.owner != u.owner || t.order != engine::Order::Collect || engine::unit_type(t.type).supplies != engine::Resource::Count) continue;
                const float d = std::hypot(to_float(t.pos.x - u.pos.x), to_float(t.pos.y - u.pos.y));
                if (d < best) {
                    best = d;
                    truck = &t;
                }
            }
            Vector2 to{g.x + f.x * 0.35f, g.y + f.y * 0.35f};
            float z1 = 1.0f;
            if (truck) {  // into its bed, behind the cab
                const Vector2 tf = unit(to_vector2(truck->facing));
                to = {to_float(truck->pos.x) - tf.x * 0.28f, to_float(truck->pos.y) - tf.y * 0.28f};
                z1 = 12.0f;
            }
            Particle p{};
            p.kind = Particle::Kind::Toss;
            p.ground = {g.x + f.x * 0.1f, g.y + f.y * 0.1f};
            p.life = 0.45f;
            p.vel = {(to.x - p.ground.x) / p.life, (to.y - p.ground.y) / p.life};
            p.size = truck ? (rock ? 1.0f : 0.0f) : (rock ? 3.0f : 2.0f);  // (to the door: the armful)
            p.dir = {truck ? 15.0f : 11.0f, z1};
            p.color = WHITE;
            particles_.push_back(p);
            heave_at_[u.id] = now;
        }
        carried->second = u.carrying;
    }
    std::erase_if(stroke_, [&](const auto& e) { return world.find_unit(e.first) == nullptr; });
    std::erase_if(carried_, [&](const auto& e) { return world.find_unit(e.first) == nullptr; });
    std::erase_if(heave_at_, [&](const auto& e) { return world.find_unit(e.first) == nullptr; });
    std::erase_if(chopped_at_, [&](const auto& e) { return now - e.second > 2.0; });

    // Trees coming down on the tiles being cut: as many fewer as stand now.
    for (auto it = tree_standing_.begin(); it != tree_standing_.end();) {
        const int tx = it->first % map.width();
        const int ty = it->first / map.width();
        const bool wood = map.terrain(tx, ty) == engine::Terrain::Forest;
        auto forest = [&](int x, int y) { return map.contains_tile(x, y) && map.terrain(x, y) == engine::Terrain::Forest; };
        const bool along_x = forest(tx - 1, ty) || forest(tx + 1, ty);
        const bool along_y = forest(tx, ty - 1) || forest(tx, ty + 1);
        const int line = along_x && !along_y ? 1 : along_y && !along_x ? 2 : !along_x && !along_y ? 3 : 0;
        const float left = wood ? std::clamp(static_cast<float>(map.resource({tx, ty})) / engine::kForestMaterials, 0.0f, 1.0f) : 0.0f;
        size_t count = 0;
        const std::array<Tree, 3> trees = trees_on_tile(tx, ty, line, 1.0f, count);
        const auto standing = static_cast<uint8_t>(std::ceil(static_cast<float>(count) * left - 0.001f));
        if (it->second != 255 && standing < it->second) {
            for (size_t i = standing; i < std::min<size_t>(it->second, count); ++i) {
                const Tree& t = trees[i];
                const int v = t.kind == TreeKind::Oak && t.size >= 1.3f ? kTreeVariants - 1 - static_cast<int>(t.seed % 2)
                                                                        : static_cast<int>((t.seed >> 3) % kTreeVariants);
                // Away from whoever cut it.
                float side = (t.seed >> 5) % 2 == 0 ? 1.0f : -1.0f;
                for (const engine::Unit& u : world.units()) {
                    if (u.type != engine::UnitTypeId::Worker || u.gather_tile != engine::TilePos{tx, ty}) continue;
                    const Vector2 d = iso_offset({t.ground.x - to_float(u.pos.x), t.ground.y - to_float(u.pos.y)});
                    side = d.x >= 0.0f ? 1.0f : -1.0f;
                    break;
                }
                falling_trees_.push_back({t.ground, static_cast<int>(t.kind) * kTreeStages + tree_stage(map.shred(tx, ty)), v, side});
            }
        }
        it->second = standing;
        it = wood ? std::next(it) : tree_standing_.erase(it);
    }
    // The felled ones: down with a crash (dust, leaves), lying a moment.
    for (FallingTree& f : falling_trees_) {
        const float before = f.age;
        f.age += dt;
        if (before < kTreeFallSeconds && f.age >= kTreeFallSeconds) {
            // Where its crown hit: off to the side on the screen, along the ground.
            const Vector2 across{0.45f * f.side, -0.45f * f.side};
            for (int i = 0; i < 4; ++i) {
                Particle d{};
                d.kind = Particle::Kind::Smoke;
                const float k = 0.3f + 0.25f * static_cast<float>(i);
                d.ground = {f.ground.x + across.x * k, f.ground.y + across.y * k};
                d.z = 2.0f;
                d.vz = 6.0f;
                d.vel = {0.08f, -0.04f};
                d.life = 1.4f + 0.5f * fx_random();
                d.size = 2.5f + fx_random();
                d.grow = 5.0f;
                d.color = {176, 164, 132, 160};
                particles_.push_back(d);
            }
            for (int i = 0; i < 6; ++i) {  // leaves, twigs knocked off
                Particle l{};
                l.kind = Particle::Kind::Clod;
                l.ground = {f.ground.x + across.x * (0.6f + 0.4f * fx_random()), f.ground.y + across.y * (0.6f + 0.4f * fx_random())};
                l.vel = {(fx_random() - 0.5f) * 0.8f, (fx_random() - 0.5f) * 0.8f};
                l.z = 3.0f;
                l.vz = 20.0f + 20.0f * fx_random();
                l.life = 0.9f + 0.5f * fx_random();
                l.size = 1.0f + 0.5f * fx_random();
                l.color = i % 3 == 0 ? Color{110, 84, 56, 255} : Color{82, 124, 58, 255};
                particles_.push_back(l);
            }
        }
    }
    std::erase_if(falling_trees_, [](const FallingTree& f) { return f.age > kTreeFallSeconds + 1.2f; });
}

Vector2 WorldRenderer::muzzle_of(const engine::Unit& u) const {
    const engine::UnitTypeDef& def = engine::unit_type(u.type);
    const float t = gun_elevation(u) * 0.0174533f;
    if (drawn_as_armor(def)) {
        const Vector2 level = armor_muzzle(def.model);
        const float ring = turret_ring_of(def.model).x * kVehicleScale;
        const float gun = level.x - ring;
        return {ring + gun * std::cos(t), level.y + gun * std::sin(t) * kZPerTile};
    }
    if (has_gun_look(def.model)) {
        const float gun = gun_barrel_of(def.model) * kVehicleScale * 0.8f;
        return {gun * std::cos(t), 14.0f + gun * std::sin(t) * kZPerTile};
    }
    return {0.5f, 8.0f};
}

// Smoke grenades fired off a tank's turret: a fan of them arcing out ahead,
// a pop at each launcher. (The screen blooms where they land.)
void WorldRenderer::launch_smoke(const engine::Unit& u) {
    Vector2 f = to_vector2(u.facing);
    const float fl = std::hypot(f.x, f.y);
    f = fl > 0.0f ? Vector2{f.x / fl, f.y / fl} : Vector2{1.0f, 0.0f};
    const Vector2 at = to_vector2(u.pos);
    const float ahead = to_float(engine::kSmokeAhead);
    for (int i = 0; i < 8; ++i) {
        const float spread = (static_cast<float>(i) / 7.0f - 0.5f) * 1.3f;
        const Vector2 d{f.x * std::cos(spread) - f.y * std::sin(spread), f.y * std::cos(spread) + f.x * std::sin(spread)};
        const float side = i < 4 ? -1.0f : 1.0f;
        Particle p{};
        p.kind = Particle::Kind::Spray;  // gone when it lands
        p.ground = {at.x - f.y * side * 0.2f, at.y + f.x * side * 0.2f};
        p.z = 14.0f;
        p.vz = 60.0f + 10.0f * fx_random();
        const float flight = 2.0f * p.vz / 170.0f + 0.05f;
        const float reach = ahead * (0.8f + 0.4f * fx_random());
        p.vel = {d.x * reach / flight, d.y * reach / flight};
        p.life = flight + 0.2f;
        p.size = 1.6f;
        p.color = {70, 72, 64, 255};
        particles_.push_back(p);
    }
    for (const float side : {-1.0f, 1.0f}) {
        const Vector2 pop{at.x - f.y * side * 0.2f, at.y + f.x * side * 0.2f};
        spawn_flash(pop, 14.0f, 3.0f, {255, 236, 190, 255});
        Particle p{};
        p.kind = Particle::Kind::Smoke;
        p.ground = pop;
        p.z = 14.0f;
        p.vz = 6.0f;
        p.life = 0.8f;
        p.size = 2.0f;
        p.grow = 5.0f;
        p.color = {210, 210, 204, 170};
        particles_.push_back(p);
    }
}

// A call for supply over the radio: its waves going out off the aerial.
void WorldRenderer::draw_radio_calls(const engine::World& world, float alpha) const {
    const engine::TileMap& map = world.map();
    for (const engine::Unit& u : world.units()) {
        const auto it = vehicles_seen_.find(u.id);
        if (it == vehicles_seen_.end() || it->second.radio >= 1.8f || u.inside || !shows(world, u)) continue;
        const float age = it->second.radio;
        const Vector2 at = on_terrain(map, unit_ground_pos(u, alpha), 26.0f);
        for (int k = 0; k < 3; ++k) {
            const float phase = std::fmod(age * 1.6f + static_cast<float>(k) / 3.0f, 1.0f);
            const float r = 3.0f + 12.0f * phase;
            const Color c = ColorAlpha({214, 236, 255, 255}, (1.0f - phase) * (1.0f - age / 1.8f) * 0.9f);
            DrawRing(at, r, r + 1.2f, -45.0f, 45.0f, 8, c);
            DrawRing(at, r, r + 1.2f, 135.0f, 225.0f, 8, c);
        }
        DrawCircleV(at, 1.5f, ColorAlpha({255, 255, 255, 255}, 1.0f - age / 1.8f));
    }
}

void WorldRenderer::draw_sprite(const SpriteSheet& sheet, Vector2 at, Vector2 dir, int frame, Color tint, float cut) const {
    float a = std::atan2(dir.y, dir.x);
    if (a < 0.0f) a += 6.2831853f;
    const int d = static_cast<int>(std::lround(a / 6.2831853f * static_cast<float>(sheet.dirs))) % sheet.dirs;
    Rectangle src{static_cast<float>(d * sheet.w), static_cast<float>((frame % sheet.frames) * sheet.h), static_cast<float>(sheet.w),
                  static_cast<float>(sheet.h)};
    Rectangle dst{std::round(at.x - sheet.origin.x), std::round(at.y - sheet.origin.y), static_cast<float>(sheet.w),
                  static_cast<float>(sheet.h)};
    if (cut < dst.y + dst.height) {  // the rest sunk out of sight
        const float keep = std::floor(cut - dst.y);
        if (keep <= 0.0f) return;
        src.height = keep;
        dst.height = keep;
    }
    DrawTexturePro(sheet.atlas, src, dst, {0.0f, 0.0f}, 0.0f, lit(tint));
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
    if (p.weapon.kinetic) {  // an armor-piercing round: its tracer a long white-hot streak
        DrawLineEx({pos.x - dir.x * 24.0f, pos.y - dir.y * 24.0f}, pos, 1.6f, {255, 196, 110, 140});
        DrawLineEx({pos.x - dir.x * 11.0f, pos.y - dir.y * 11.0f}, pos, 2.0f, {255, 244, 214, 230});
        DrawCircleV(pos, 1.6f, {255, 255, 240, 255});
        return;
    }
    if (p.weapon.guided) {  // the wire paying out behind it back to the launcher, wavering
        const Vector2 from = iso::project(origin, to_float(p.origin_height));
        const float wave = static_cast<float>(GetTime()) * 9.0f + static_cast<float>(p.id);
        Vector2 prev = from;
        for (int i = 1; i <= 12; ++i) {
            const float k = static_cast<float>(i) / 12.0f;
            const float sway = std::sin(wave + k * 7.0f) * 1.2f * k * (1.0f - k) * 4.0f;
            const Vector2 q{from.x + (pos.x - from.x) * k - dir.y * sway, from.y + (pos.y - from.y) * k + dir.x * sway + 3.0f * k * (1.0f - k)};
            DrawLineV(prev, q, {214, 212, 200, 90});
            prev = q;
        }
    }
    switch (p.weapon.damage_type) {
        case engine::DamageType::AntiTank:  // rocket with a smoke trail
            DrawLineEx({pos.x - dir.x * 14.0f, pos.y - dir.y * 14.0f}, pos, 2.5f, {180, 180, 170, 150});
            DrawCircleV(pos, 2.5f, {255, 150, 40, 255});
            break;
        case engine::DamageType::Explosive:  // tank shell: fast, a streak
            DrawLineEx({pos.x - dir.x * 12.0f, pos.y - dir.y * 12.0f}, pos, 2.0f, {255, 240, 170, 170});
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
        Vector2 m{def.vehicle ? 0.5f : 0.25f, def.vehicle ? 8.0f : 9.0f};
        if (drawn_as_armor(def) || has_gun_look(def.model)) m = muzzle_of(u);
        Vector2 muzzle = on_terrain(map, {ground.x + facing.x * m.x, ground.y + facing.y * m.x}, m.y);
        if (!def.vehicle) {  // his weapon as he's drawn
            Vector2 at{};
            float z = 0.0f;
            soldier_muzzle(u, ground, facing, at, z);
            muzzle = on_terrain(map, at, z);
        }
        const bool sweep = u.order == engine::Order::Ability && u.order_ability == engine::AbilityId::MgSweep;
        if (engine::weapon_of(u).projectile_speed.raw == 0 || sweep) {  // instant hit: draw the tracer
            const engine::Unit* target = world.find_unit(u.engaged);
            const float lift = target && target->airborne ? flight_lift(*target) : 6.0f;  // up at an aircraft
            DrawLineV(muzzle, on_terrain(map, to_vector2(u.last_shot_at), lift), {255, 235, 160, 140});
        }
    }
}

// Service going on between two vehicles: a tanker's hose sagging into a
// vehicle; an ammunition truck's crates carried over in an arc, one after
// another. (A workshop's welding is its flashes and sparks.)
void WorldRenderer::draw_services(const engine::World& world, float alpha) const {
    const engine::TileMap& map = world.map();
    for (const Service& s : services_) {
        if (s.kind == Service::Kind::Weld) continue;
        const engine::Unit* to = world.find_unit(s.to);
        const engine::Unit* from = world.find_unit(s.from);
        if (!to || !shows(world, *to) || (s.from != 0 && (!from || !shows(world, *from)))) continue;
        // From the truck's side (the depot) to the vehicle's back deck (its filler caps, its ammunition hatch).
        const Vector2 a = from ? unit_ground_pos(*from, alpha) : s.at;
        const Vector2 b = unit_ground_pos(*to, alpha);
        Vector2 back = to_vector2(to->hull);
        const float bl = std::hypot(back.x, back.y);
        back = bl > 0.0f ? Vector2{back.x / bl, back.y / bl} : Vector2{1.0f, 0.0f};
        const Vector2 filler{b.x - back.x * 0.3f, b.y - back.y * 0.3f};
        const Vector2 d{filler.x - a.x, filler.y - a.y};
        const float dl = std::max(0.01f, std::hypot(d.x, d.y));
        const Vector2 p0 = on_terrain(map, {a.x + d.x / dl * 0.3f, a.y + d.y / dl * 0.3f}, 7.0f);
        const Vector2 p1 = on_terrain(map, filler, 9.0f);
        const float fade = std::clamp((1.2f - s.age) / 0.3f, 0.0f, 1.0f);
        if (s.kind == Service::Kind::Hose) {  // black rubber, a glint along it, the nozzle
            Vector2 prev = p0;
            for (int i = 1; i <= 14; ++i) {
                const float t = static_cast<float>(i) / 14.0f;
                const Vector2 q{p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t + 10.0f * 4.0f * t * (1.0f - t)};
                DrawLineEx(prev, q, 2.2f, ColorAlpha(lit({22, 22, 20, 255}), fade));
                DrawLineEx({prev.x, prev.y - 0.8f}, {q.x, q.y - 0.8f}, 0.8f, ColorAlpha(lit({120, 118, 104, 255}), fade));
                prev = q;
            }
            DrawRectangleRec({std::round(p1.x - 1.5f), std::round(p1.y - 1.5f), 3.0f, 3.0f}, ColorAlpha(lit({150, 150, 140, 255}), fade));
            continue;
        }
        for (int k = 0; k < 2; ++k) {  // wooden ammunition crates, one after another
            const float t = std::fmod(static_cast<float>(GetTime()) * 1.2f + static_cast<float>(k) * 0.5f, 1.0f);
            const Vector2 q{p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t - 10.0f * 4.0f * t * (1.0f - t)};
            DrawRectangleRec({std::round(q.x - 3.0f), std::round(q.y - 2.0f), 6.0f, 4.0f}, ColorAlpha(lit({30, 28, 22, 255}), fade));
            DrawRectangleRec({std::round(q.x - 2.0f), std::round(q.y - 1.0f), 4.0f, 2.0f}, ColorAlpha(lit({146, 122, 76, 255}), fade));
            DrawPixelV({std::round(q.x), std::round(q.y - 1.0f)}, ColorAlpha(lit({90, 74, 46, 255}), fade));
        }
    }
}

// Smoke, clods of earth dark against the ground, flames, spray.
// A puff of smoke `r` pixels round, its frame of the sheet for its size
// (see bake_smoke; -1: one of the dense ones, by its seed), tinted `light`
// (the colour its lit top has), `alpha` see-through.
void puff(Vector2 at, float r, Color light, float alpha, uint32_t seed, int frame = -1) {
    if (alpha <= 0.02f || r < 0.5f) return;
    int tier = 0;
    while (tier < 5 && r > kSmokeTiers[tier] * 1.15f) ++tier;
    const FxSheet& s = fx_sheet(static_cast<Fx>(static_cast<int>(Fx::Smoke0) + tier), static_cast<int>(seed % 3u));
    auto up = [](unsigned char c) { return static_cast<unsigned char>(std::min(255, static_cast<int>(c) * 255 / 244)); };
    const Color tint{up(light.r), up(light.g), up(light.b), static_cast<unsigned char>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)};
    draw_fx(s, frame >= 0 ? std::min(frame, kSmokeFrames - 1) : static_cast<int>(seed % 3u), at, tint);
}

// A puff blown out along `dir` (on the screen), `r` pixels round: drawn out along it.
void blown_puff(Vector2 at, float r, Vector2 dir, Color light, float alpha, uint32_t seed, int frame) {
    if (alpha <= 0.02f || g_streaks.empty()) return;
    const int size = r < 3.0f ? 0 : r < 5.5f ? 1 : 2;
    float a = std::atan2(dir.y, dir.x);
    if (a < 0.0f) a += 3.14159265f;  // (the same drawn out either way)
    if (a >= 3.14159265f) a -= 3.14159265f;
    const int turn = static_cast<int>(std::lround(a / 3.14159265f * kStreakTurns)) % kStreakTurns;
    const FxSheet& s = g_streaks[static_cast<size_t>(((seed % 2u) * 3u + static_cast<uint32_t>(size)) * kStreakTurns + static_cast<uint32_t>(turn))];
    auto up = [](unsigned char c) { return static_cast<unsigned char>(std::min(255, static_cast<int>(c) * 255 / 244)); };
    draw_fx(s, std::min(frame, kSmokeFrames - 1), at, {up(light.r), up(light.g), up(light.b), static_cast<unsigned char>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)});
}

// The smoke on the field, as clouds of puffs, drawn back to front. A screen:
// a mound of white, blooming out as its grenades land, thinning out at the
// end. A burning wreck's (a fire's) plume: a column of black leaning off
// with the wind, going grey as it rises. A burst's dust and smoke: a brown
// dome rolling out, a ring of it on the ground, settling.
void WorldRenderer::draw_smoke(const engine::World& world) const {
    const engine::TileMap& map = world.map();
    const float now = static_cast<float>(GetTime());
    struct Puff {
        float depth;
        Vector2 at;
        float r;
        Color light;
        Color dark;
        float alpha;
        uint32_t seed;
        int frame = -1;  // of its sheet: -1 dense
    };
    std::vector<Puff> puffs;
    const Vector2 drift = to_vector2(engine::kPlumeDrift);
    for (const engine::Smoke& s : world.smokes()) {
        const Vector2 c = to_vector2(s.center);
        if (!reveal_ && fog(world, static_cast<int>(c.x), static_cast<int>(c.y)) == kUnexplored) continue;
        const float age = static_cast<float>(world.tick() - std::min(world.tick(), s.made)) / engine::kTicksPerSecond;
        const float left = static_cast<float>(s.clears > world.tick() ? s.clears - world.tick() : 0) / engine::kTicksPerSecond;
        const float span = std::max(0.1f, age + left);
        const float radius = to_float(s.radius);
        const uint32_t seed = tile_hash(static_cast<int>(s.center.x.raw >> 6) ^ static_cast<int>(s.made * 7u),
                                        static_cast<int>(s.center.y.raw >> 6));
        auto rnd = [&](int i, int k) { return hash_unit(tile_hash(static_cast<int>(seed & 0xFFFF) + i * 31, k * 17 + static_cast<int>(seed >> 16))); };
        switch (s.kind) {
            case engine::SmokeKind::Screen: {
                const float bloom = std::clamp((age - 0.3f) / 1.5f, 0.0f, 1.0f);
                const float appear = std::clamp((age - 0.3f) / 0.3f, 0.0f, 1.0f);
                const float end = std::clamp(left / 3.0f, 0.0f, 1.0f);  // breaking up, shrinking away
                if (appear <= 0.0f || end <= 0.0f) break;
                const float r = radius * (0.3f + 0.7f * bloom);
                const float top = 34.0f * (0.45f + 0.55f * bloom);  // how high the mound stands in its middle
                // Its body: big puffs low in it; then smaller ones over its
                // surface, all round it: the bumps of a cumulus.
                const int core = 5 + static_cast<int>(radius * 2.0f);
                const int n = core + 16 + static_cast<int>(radius * 10.0f);
                for (int i = 0; i < n; ++i) {
                    if (rnd(i, 7) > end * 1.2f) continue;  // the last seconds: fewer of them
                    const bool body = i < core;
                    const float a = rnd(i, 1) * 6.2831853f;
                    const float d = std::sqrt(rnd(i, 2)) * r * (body ? 0.5f : 0.9f);
                    const Vector2 g{c.x + std::cos(a) * d, c.y + std::sin(a) * d};
                    const float middle = std::sqrt(std::max(0.0f, 1.0f - (d / std::max(0.01f, r)) * (d / std::max(0.01f, r))));
                    const float z = (body ? 0.35f : 0.75f + 0.25f * rnd(i, 3)) * top * middle + 4.0f + 1.5f * std::sin(now * 0.8f + static_cast<float>(i));
                    const float f = std::clamp(z / (top + 6.0f), 0.0f, 1.0f);  // high up: lighter
                    const float pr = (body ? 17.0f + 6.0f * rnd(i, 4) : 8.0f + 6.0f * rnd(i, 4)) * (0.55f + 0.45f * bloom) *
                                     (0.5f + 0.5f * end) * (0.8f + 0.2f * middle) * (1.0f + 0.06f * std::sin(now * 1.3f + static_cast<float>(i) * 1.7f));
                    puffs.push_back({g.x + g.y + z * 0.01f, on_terrain(map, g, z), pr, mix({200, 202, 204, 255}, {250, 250, 246, 255}, f),
                                     mix({74, 78, 86, 255}, {150, 150, 150, 255}, f), appear, seed + static_cast<uint32_t>(i),
                                     end < 1.0f ? 3 + static_cast<int>((1.0f - end) * 4.99f) : -1});
                }
                break;
            }
            case engine::SmokeKind::Plume: {
                // Thick while it burns; thinning out its last five seconds.
                const float fade = std::min(1.0f, left / 5.0f) * std::clamp(age / 1.5f, 0.0f, 1.0f);
                if (fade <= 0.0f) break;
                const Vector2 base{c.x - drift.x, c.y - drift.y};
                const float dl = std::max(0.01f, std::hypot(drift.x, drift.y));
                const Vector2 lean{drift.x / dl, drift.y / dl};
                const int n = 10 + static_cast<int>(radius * 4.0f);
                for (int i = 0; i < n; ++i) {
                    const float f = std::fmod(static_cast<float>(i) / static_cast<float>(n) + now * 0.07f + rnd(i, 1) * 0.05f, 1.0f);  // rising
                    const float side = (rnd(i, 2) - 0.5f) * radius * 0.5f * (0.4f + f);
                    const Vector2 g{base.x + lean.x * f * radius * 1.8f - lean.y * side, base.y + lean.y * f * radius * 1.8f + lean.x * side};
                    const float z = 6.0f + f * 62.0f;
                    const float pr = (4.0f + 10.0f * f) * (0.8f + 0.4f * rnd(i, 3)) * (0.8f + 0.2f * radius);
                    const float a = std::min(1.0f, f / 0.08f) * (1.0f - std::max(0.0f, f - 0.65f) / 0.35f);
                    puffs.push_back({g.x + g.y + z * 0.01f, on_terrain(map, g, z), pr * (0.6f + 0.4f * a), mix({40, 38, 36, 255}, {130, 128, 124, 255}, f),
                                     mix({20, 18, 16, 255}, {100, 98, 96, 255}, f), std::min(1.0f, 1.4f * a) * fade, seed + static_cast<uint32_t>(i),
                                     f < 0.45f ? -1 : 3 + static_cast<int>((f - 0.45f) / 0.55f * 2.99f)});  // (black smoke: thinning out, no rings)
                }
                break;
            }
            case engine::SmokeKind::Dust: {
                const float k = std::clamp(age / span, 0.0f, 1.0f);
                const float fade = std::pow(1.0f - k, 0.7f) * std::clamp(age / 0.2f, 0.0f, 1.0f);
                if (fade <= 0.0f) break;
                const float r = radius * (0.55f + 0.6f * std::sqrt(k));
                const int ring = 6 + static_cast<int>(radius * 3.0f);
                for (int i = 0; i < ring; ++i) {  // rolling out along the ground
                    const float a = (static_cast<float>(i) + rnd(i, 5)) * 6.2831853f / static_cast<float>(ring);
                    const Vector2 g{c.x + std::cos(a) * r, c.y + std::sin(a) * r};
                    puffs.push_back({g.x + g.y, on_terrain(map, g, 2.5f), 4.5f + 3.0f * rnd(i, 6) + 2.5f * k, {204, 190, 164, 255},
                                     {112, 100, 82, 255}, 0.85f * fade, seed + 97u + static_cast<uint32_t>(i), static_cast<int>(k * 7.99f)});
                }
                const int n = 6 + static_cast<int>(radius * 6.0f);
                for (int i = 0; i < n; ++i) {  // the dome over the burst
                    const float a = rnd(i, 1) * 6.2831853f;
                    const float d = std::sqrt(rnd(i, 2)) * r * 0.6f;
                    const Vector2 g{c.x + std::cos(a) * d + drift.x * k * 0.6f, c.y + std::sin(a) * d + drift.y * k * 0.6f};
                    const float middle = 1.0f - d / std::max(0.01f, r);
                    const float z = 3.0f + middle * 20.0f * (1.0f - 0.3f * k) + rnd(i, 3) * 6.0f;
                    const float f = std::clamp(z / 28.0f, 0.0f, 1.0f);
                    const float pr = (5.0f + 5.0f * rnd(i, 4)) * (0.8f + 0.5f * k) * (0.6f + 0.4f * middle) * (0.7f + 0.3f * radius);
                    puffs.push_back({g.x + g.y + z * 0.01f, on_terrain(map, g, z), pr * (0.5f + 0.5f * fade), mix({170, 156, 132, 255}, {214, 206, 188, 255}, f),
                                     mix({70, 62, 50, 255}, {128, 120, 106, 255}, f), std::min(1.0f, 1.5f * fade), seed + static_cast<uint32_t>(i),
                                     static_cast<int>(k * 7.99f)});
                }
                break;
            }
        }
    }
    std::sort(puffs.begin(), puffs.end(), [](const Puff& a, const Puff& b) { return a.depth < b.depth; });
    for (const Puff& p : puffs) puff(p.at, p.r, p.light, p.alpha, p.seed, p.frame);
}

void WorldRenderer::draw_particles(const engine::TileMap& map) const {
    // Twice: everything, then the fire out of muzzles and the flashes over it (over their own smoke).
    for (int pass = 0; pass < 2; ++pass)
    for (const Particle& p : particles_) {
        const bool bright = p.kind == Particle::Kind::Muzzle || p.kind == Particle::Kind::Flash;
        if (bright != (pass == 1)) continue;
        const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
        const Vector2 g = on_terrain(map, p.ground);
        const Vector2 at{g.x, g.y - p.z};
        switch (p.kind) {
            case Particle::Kind::Smoke: {
                // A puff: dense, breaking up, its rings thinning out, as its life goes; paler, greyer as it goes.
                const float alpha = static_cast<float>(p.color.a) / 255.0f * (t < 0.75f ? 1.0f : 1.0f - (t - 0.75f) / 0.25f) *
                                    std::min(1.0f, p.age * 8.0f);
                const int shade_sum = p.color.r + p.color.g + p.color.b;
                const bool black = shade_sum < 150;
                const Color base = mix(Color{p.color.r, p.color.g, p.color.b, 255}, black ? Color{64, 62, 60, 255} : Color{186, 184, 180, 255}, t * 0.4f);
                const Color light = black ? shade(base, 1.7f) : shade(base, 1.28f);
                // Dark smoke thins out in its clumps (its rings would be black scribbles); the light goes to rings.
                const int frame = std::min(static_cast<int>(t * 7.99f), black ? 4 : shade_sum < 300 ? 5 : kSmokeFrames - 1);
                puff(at, p.size, light, std::min(1.0f, alpha * 1.25f), p.seed, frame);
                break;
            }
            case Particle::Kind::Muzzle: {  // fire out of a muzzle, pointing the way the barrel does
                if (g_muzzles.empty()) break;
                float a = std::atan2(p.dir.y, p.dir.x);
                if (a < 0.0f) a += 6.2831853f;
                const int turn = static_cast<int>(std::lround(a / 6.2831853f * kMuzzleTurns)) % kMuzzleTurns;
                const int shape = std::clamp(static_cast<int>(p.size), 0, kMuzzleShapes - 1);
                const FxSheet& s = g_muzzles[static_cast<size_t>(shape * kMuzzleTurns + turn)];
                draw_fx(s, static_cast<int>(t * static_cast<float>(kMuzzleFrames)), at);
                break;
            }
            case Particle::Kind::Blow: {  // blown out of a muzzle, off the ground: drawn out along the way it goes
                const float alpha = static_cast<float>(p.color.a) / 255.0f * (t < 0.7f ? 1.0f : 1.0f - (t - 0.7f) / 0.3f) * std::min(1.0f, p.age * 10.0f);
                const Color base = mix(Color{p.color.r, p.color.g, p.color.b, 255}, {186, 184, 180, 255}, t * 0.3f);
                blown_puff(at, p.size, iso_offset(p.vel), shade(base, 1.2f), std::min(1.0f, alpha * 1.25f), p.seed, static_cast<int>(t * 7.99f));
                break;
            }
            case Particle::Kind::Clod:
            case Particle::Kind::Spray: {
                const float alpha = t > 0.7f ? (1.0f - t) / 0.3f : 1.0f;
                if (p.z > 0.5f) DrawCircleV({g.x + p.z * 0.25f, g.y}, p.size * 0.7f, ColorAlpha({0, 0, 0, 255}, 0.25f * alpha));  // its shadow
                DrawRectangleRec({at.x - p.size * 0.5f, at.y - p.size * 0.5f, p.size, p.size}, ColorAlpha(p.color, alpha));
                break;
            }
            case Particle::Kind::Flame: {  // a tongue of fire, flickering, smaller as it goes out
                const float size = p.size * (t < 0.7f ? 1.0f : 1.0f - (t - 0.7f) / 0.3f * 0.6f);
                const Fx kind = size < 2.6f ? Fx::Flame0 : size < 4.6f ? Fx::Flame1 : Fx::Flame2;
                const FxSheet& s = fx_sheet(kind, static_cast<int>(p.seed % 3u));
                draw_fx(s, static_cast<int>(p.age * 14.0f + static_cast<float>(p.seed % 8u)) % s.frames, {at.x, at.y + size * 0.6f});
                break;
            }
            case Particle::Kind::Spark: {  // a hot streak along its flight, pixel-sharp: a white head, its colour behind
                const Vector2 v = iso_offset(p.vel);
                const Vector2 head{std::round(at.x), std::round(at.y)};
                const Vector2 tail{std::round(at.x - v.x * 0.04f), std::round(at.y - v.y * 0.04f + p.vz * 0.04f)};
                const Color c = t < 0.5f ? p.color : kHeat[3];
                DrawLineV(tail, head, c);
                DrawRectangleRec({head.x, head.y, 1.0f, 1.0f}, t < 0.6f ? kHeat[0] : kHeat[1]);
                break;
            }
            case Particle::Kind::Flash: {  // a star of rays out of a white-hot heart, gone at once
                const Fx kind = p.size < 5.0f ? Fx::Star0 : p.size < 11.0f ? Fx::Star1 : Fx::Star2;
                const FxSheet& s = fx_sheet(kind, static_cast<int>(p.seed % 3u));
                draw_fx(s, static_cast<int>(t * static_cast<float>(s.frames)), at);
                break;
            }
            case Particle::Kind::Toss: {  // a log turning over as it flies, a sack; up and over, down into the bed
                const float z = p.dir.x + (p.dir.y - p.dir.x) * t + 10.0f * 4.0f * t * (1.0f - t);
                const Vector2 c{g.x, g.y - z};
                if (p.size > 2.5f) {  // blocks of stone
                    for (const float k : {-1.5f, 1.5f}) {
                        DrawRectangleRec({std::round(c.x + k - 2.0f), std::round(c.y - 2.0f), 4.0f, 3.0f}, lit({40, 36, 30, 255}));
                        DrawRectangleRec({std::round(c.x + k - 1.5f), std::round(c.y - 1.5f), 3.0f, 2.0f}, lit({150, 146, 138, 255}));
                    }
                } else if (p.size > 1.5f) {  // a bundle of bars
                    DrawRectangleRec({std::round(c.x - 4.0f), std::round(c.y - 3.0f), 8.0f, 5.0f}, lit({40, 30, 22, 255}));
                    DrawRectangleRec({std::round(c.x - 3.0f), std::round(c.y - 2.0f), 6.0f, 1.5f}, lit({172, 134, 90, 255}));
                    DrawRectangleRec({std::round(c.x - 3.0f), std::round(c.y - 0.5f), 6.0f, 1.5f}, lit({146, 108, 68, 255}));
                } else if (p.size < 0.5f) {
                    const float a = t * 2.8f + static_cast<float>(p.seed % 7u) * 0.3f;
                    const Vector2 d{std::cos(a) * 5.0f, std::sin(a) * 2.2f};
                    DrawLineEx({c.x - d.x, c.y - d.y}, {c.x + d.x, c.y + d.y}, 4.4f, lit({34, 26, 20, 255}));  // (its outline)
                    DrawLineEx({c.x - d.x, c.y - d.y}, {c.x + d.x, c.y + d.y}, 2.6f, lit({112, 84, 58, 255}));
                    DrawLineV({c.x - d.x, c.y - d.y - 1.0f}, {c.x + d.x, c.y + d.y - 1.0f}, lit({140, 108, 74, 255}));
                    for (const float k : {-1.0f, 1.0f}) DrawRectangleRec({std::round(c.x + d.x * k - 1.0f), std::round(c.y + d.y * k - 1.0f), 2.0f, 2.0f}, lit({200, 168, 118, 255}));
                } else {
                    DrawEllipse(static_cast<int>(c.x), static_cast<int>(c.y), 4.4f, 3.6f, lit({40, 34, 24, 255}));
                    DrawEllipse(static_cast<int>(c.x), static_cast<int>(c.y), 3.4f, 2.7f, lit({158, 138, 98, 255}));
                    DrawEllipse(static_cast<int>(c.x - 1.0f), static_cast<int>(c.y - 1.0f), 1.6f, 1.2f, lit({182, 162, 118, 255}));
                }
                break;
            }
            case Particle::Kind::Casing: {
                DrawRectangleRec({std::round(at.x - 1.0f), std::round(at.y - 0.5f), 2.0f, 1.0f}, ColorAlpha(p.color, t > 0.8f ? (1.0f - t) / 0.2f : 1.0f));
                break;
            }
        }
    }
}

// A tank knocked out: its hull burnt out, the skirts and ERA blown off; the
// turret on it, or, where the rounds in the carousel went up (the T-72s,
// T-80s, T-90s, T-62s, the Karrar), blown off and lying upturned beside it,
// a black hole where it sat.
void WorldRenderer::draw_wreck(const engine::TileMap& map, const Remains& r) const {
    const engine::UnitTypeDef& def = engine::unit_type(r.type);
    auto unit = [](Vector2 v) {
        const float l = std::hypot(v.x, v.y);
        return l > 0.0f ? Vector2{v.x / l, v.y / l} : Vector2{1.0f, 0.0f};
    };
    const engine::Terrain under =
        map.terrain(map.clamp_tile({static_cast<int32_t>(std::floor(r.ground.x)), static_cast<int32_t>(std::floor(r.ground.y))}));
    if (const std::optional<TruckModel> truck = truck_model(r.type, r.owner)) {
        draw_truck_wreck(map, r, *truck, under == engine::Terrain::Water);
        return;
    }
    if (has_gun_look(def.model) || has_plane_look(def.model)) {
        // A gun burnt out, its barrel down; an aircraft come down, lying
        // askew in a scorched patch; rusting over the weeks.
        if (under == engine::Terrain::Water) return;  // gone under
        const bool plane = has_plane_look(def.model);
        const SpritePart part = plane ? SpritePart::Plane : SpritePart::Gun;
        if (plane && r.crashed) {  // come down: broken apart where it hit, burnt, its pieces burning a while
            const SpriteSheet* pieces = sheet(part, small_variant(def.model, 6), r.owner);
            const SpriteSheet* rusty = sheet(part, small_variant(def.model, 7), r.owner);
            if (!pieces) return;
            const float fade = std::clamp((kTankWreckLifetime - r.age) / 3.0f, 0.0f, 1.0f);
            const float rust = std::clamp((r.age - 20.0f) / 90.0f, 0.0f, 0.9f);
            const float light = g_light;
            g_light = light * (0.4f + 0.6f * fade);
            const Vector2 at = on_terrain(map, r.ground);
            draw_scorch(at, 2, r.age < 60.0f, fade, r.seed);
            const int layout = static_cast<int>(r.seed % static_cast<uint32_t>(pieces->frames));
            draw_sprite(*pieces, at, unit(r.facing), layout);
            if (rusty && rust > 0.0f) draw_sprite(*rusty, at, unit(r.facing), layout, ColorAlpha(WHITE, rust));
            g_light = light;
            return;
        }
        const SpriteSheet* burnt = sheet(part, small_variant(def.model, 4), r.owner);
        const SpriteSheet* rusted = sheet(part, small_variant(def.model, 5), r.owner);
        if (!burnt) return;
        const float fade = std::clamp((kTankWreckLifetime - r.age) / 3.0f, 0.0f, 1.0f);
        const float rust = std::clamp((r.age - 20.0f) / 90.0f, 0.0f, 0.9f);
        const float light = g_light;
        g_light = light * (0.4f + 0.6f * fade);
        const Vector2 at = on_terrain(map, r.ground);
        Vector2 dir = unit(r.facing);
        if (!plane) draw_scorch(at, 0, r.age < 30.0f, fade, r.seed);
        if (plane) {
            const float t = (hash_unit(tile_hash(static_cast<int>(r.seed & 0xFFFF), 71)) - 0.5f) * 1.6f;
            dir = {dir.x * std::cos(t) - dir.y * std::sin(t), dir.y * std::cos(t) + dir.x * std::sin(t)};
            draw_scorch(at, 1, r.age < 40.0f, fade, r.seed);
        }
        draw_sprite(*burnt, at, dir, 0);
        if (rusted && rust > 0.0f) draw_sprite(*rusted, at, dir, 0, ColorAlpha(WHITE, rust));
        g_light = light;
        return;
    }
    if (under == engine::Terrain::Water) {
        // Drowned with the bridge under it: the water over it, only the top
        // of its turret and its aerial showing, rings on the water round them.
        const int v = armor_variant(def.model, kit(def.model, r.owner), 1, 2);
        const SpriteSheet* turret = sheet(SpritePart::Turret, v, r.owner);
        if (!turret) return;
        const Frame fr = make_frame(map, r.ground, unit(r.hull));
        const Vector2 ring = ring_at(fr, def.model);
        const float t = static_cast<float>(GetTime());
        for (int k = 0; k < 2; ++k) {
            const float grow = std::fmod(t * 0.4f + static_cast<float>(k) * 0.5f, 1.0f);
            DrawEllipseLines(static_cast<int>(ring.x), static_cast<int>(ring.y), 12.0f + 10.0f * grow, 5.0f + 4.0f * grow,
                             ColorAlpha({190, 214, 226, 255}, 0.55f * (1.0f - grow)));
        }
        // Its hull a dark shape under the water, the gun along it.
        // Soft round blots along it and across it, each faint, darker where
        // they overlap: a rounded shape blurring out at its edges.
        const Vector2 size = armor_size(def.model);
        const float k = kVehicleScale;
        for (int i = 0; i < 9; ++i) {
            const float a = (static_cast<float>(i) / 8.0f - 0.5f) * size.x * 0.9f * k;
            for (const float across : {-0.55f, 0.0f, 0.55f}) {
                const Vector2 p = fr.at(a, across * size.y * k);
                const float end = 1.0f - 0.35f * std::fabs(static_cast<float>(i) / 4.0f - 1.0f);  // the ends rounder
                DrawCircleV(p, 6.5f * end, {24, 46, 60, 34});
                DrawCircleV(p, 4.0f * end, {24, 46, 60, 30});
            }
        }
        const Vector2 f = unit(r.facing);
        const Vector2 g0 = ring;
        const Vector2 g1{ring.x + iso_offset(f).x * 0.8f * k, ring.y + iso_offset(f).y * 0.8f * k};
        DrawLineEx(g0, g1, 2.0f, {24, 46, 60, 110});
        draw_sprite(*turret, ring, unit(r.facing), 0);
        return;
    }
    if (r.sunk) {
        const SpriteSheet* turret = sheet(SpritePart::Turret, armor_variant(def.model, kit(def.model, r.owner), 1, 2), r.owner);
        if (!turret) return;
        const Frame fr = make_frame(map, r.ground, unit(r.hull));
        const Vector2 size = armor_size(def.model);
        mud_halo(fr, {size.x * 0.55f, size.y * 0.7f}, false, r.seed);
        draw_sprite(*turret, ring_at(fr, def.model), unit(r.facing), 0);
        mud_halo(fr, {size.x * 0.55f, size.y * 0.7f}, true, r.seed);
        return;
    }
    const int sink = under == engine::Terrain::Swamp ? 1 : 0;
    const SpriteSheet* hull = sheet(SpritePart::Hull, armor_variant(def.model, 0, 4, sink), r.owner);
    const SpriteSheet* turret = sheet(SpritePart::Turret, armor_variant(def.model, 0, 4, sink), r.owner);
    const SpriteSheet* rust_hull = sheet(SpritePart::Hull, armor_variant(def.model, 0, 5, sink), r.owner);
    const SpriteSheet* rust_turret = sheet(SpritePart::Turret, armor_variant(def.model, 0, 5, sink), r.owner);
    if (!hull || !turret) return;
    const Frame fr = make_frame(map, r.ground, unit(r.hull));
    const float fade = std::clamp((kTankWreckLifetime - r.age) / 3.0f, 0.0f, 1.0f);
    // Burnt black at first, then rusting over.
    const float rust = std::clamp((r.age - 20.0f) / 90.0f, 0.0f, 0.9f);
    const float light = g_light;
    g_light = light * (0.4f + 0.6f * fade);
    if (!sink) draw_scorch(fr.o, r.blown ? 1 : 0, r.age < 40.0f, fade, r.seed);  // the ground burnt round it
    {
        // Blown off it: road wheels lying flat on the ground beside it, like
        // cogs: the rim toothed, the dished disc with its holes, the hub.
        const Dims look = dims_of(def.model);
        const bool wheeled = has_vehicle_look(def.model) && vehicle_look_of(def.model).wheeled;
        const float k = kVehicleScale;
        const uint32_t hw = tile_hash(static_cast<int>(r.seed & 0xFFFF), 53);
        const Color steel = mix({74, 76, 64, 255}, {112, 68, 44, 255}, rust);
        const Color outline{18, 18, 16, 255};
        const float side = (hw & 1u) ? 1.0f : -1.0f;
        const int n = sink ? 0 : 2 + static_cast<int>((hw >> 1) % 2);
        for (int i = 0; i < n; ++i) {
            const uint32_t hi = tile_hash(static_cast<int>(hw >> 4) + i * 31, i * 7 + 3);
            const float along = (hash_unit(hi) - 0.5f) * look.length * k;
            const float out = (look.width + 0.2f + 0.3f * hash_unit(hi >> 8)) * k * (i == 2 ? -side : side);
            const Vector2 p = fr.at(along, out);
            const float rx = look.wheel_r * k * 1.45f;
            const float ry = rx * (0.45f + 0.2f * hash_unit(hi >> 12));
            const float turn = hash_unit(hi >> 20) * 6.2831853f;
            DrawEllipse(static_cast<int>(p.x + 1.5f), static_cast<int>(p.y + 1.2f), rx + 1.5f, ry + 1.0f, lit({16, 14, 12, 90}));  // its shadow
            if (wheeled) {  // a tyre burnt off its rim: the rusty rim, its hub
                DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), rx * 0.8f + 1.0f, ry * 0.8f + 0.8f, lit(outline));
                DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), rx * 0.8f, ry * 0.8f, lit(shade(steel, 0.85f)));
                DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), rx * 0.5f, ry * 0.5f, lit(shade(steel, 1.3f)));
                DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), rx * 0.18f, ry * 0.18f, lit(outline));
                continue;
            }
            // The toothed rim: a ring of teeth, outlined.
            constexpr int kTeeth = 12;
            auto ring = [&](float scale, float tooth, Color color) {
                Vector2 pts[kTeeth * 2];
                for (int j = 0; j < kTeeth * 2; ++j) {
                    const float a = turn + static_cast<float>(j) * 3.14159265f / kTeeth;
                    const float rr = (j % 2 == 0 ? 1.0f : 1.0f - tooth) * scale;
                    pts[j] = {p.x + std::cos(a) * rx * rr, p.y + std::sin(a) * ry * rr};
                }
                for (int j = 0; j < kTeeth * 2; ++j) fill_triangle(p, pts[j], pts[(j + 1) % (kTeeth * 2)], color);
            };
            ring(1.12f, 0.2f, outline);
            ring(1.0f, 0.2f, shade(steel, 0.8f));
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), rx * 0.72f, ry * 0.72f, lit(shade(steel, 1.25f)));  // the disc
            for (int j = 0; j < 5; ++j) {  // its holes
                const float a = turn + static_cast<float>(j) * 6.2831853f / 5.0f;
                DrawEllipse(static_cast<int>(p.x + std::cos(a) * rx * 0.45f), static_cast<int>(p.y + std::sin(a) * ry * 0.45f),
                            std::max(1.0f, rx * 0.12f), std::max(0.6f, ry * 0.12f), lit(outline));
            }
            DrawEllipse(static_cast<int>(p.x - 0.4f), static_cast<int>(p.y - 0.4f), rx * 0.24f, ry * 0.24f, lit(shade(steel, 1.45f)));  // the hub
            DrawPixelV({p.x - 0.4f, p.y - 0.4f}, lit(outline));
        }
    }
    auto both = [&](const SpriteSheet* burnt, const SpriteSheet* rusted, auto draw) {
        draw(*burnt, lit(WHITE));
        if (rusted && rust > 0.0f) draw(*rusted, ColorAlpha(lit(WHITE), rust));
    };
    if (sink) mud_halo(fr, armor_size(def.model), false, r.seed);
    both(hull, rust_hull, [&](const SpriteSheet& sh, Color tint) {
        float a = std::atan2(fr.f.y, fr.f.x);
        if (a < 0.0f) a += 6.2831853f;
        const int d = static_cast<int>(std::lround(a / 6.2831853f * static_cast<float>(sh.dirs))) % sh.dirs;
        DrawTexturePro(sh.atlas, {static_cast<float>(d * sh.w), 0.0f, static_cast<float>(sh.w), static_cast<float>(sh.h)},
                       {std::round(fr.o.x - sh.origin.x), std::round(fr.o.y - sh.origin.y), static_cast<float>(sh.w), static_cast<float>(sh.h)},
                       {0.0f, 0.0f}, 0.0f, tint);
    });
    const Vector2 ring = ring_at(fr, def.model);
    const bool tossed = r.blown;
    // Where the turret lies and how: blown off, a tile or so from the hull,
    // turned, tilted and now and then upside down, its own way each time.
    const uint32_t h1 = tile_hash(static_cast<int>(r.seed & 0xFFFF), 17);
    const uint32_t h2 = tile_hash(static_cast<int>(r.seed >> 16), 29);
    Vector2 at = ring;
    Vector2 dir = unit(r.facing);
    float tilt = 0.0f;
    bool flip = false;
    if (tossed) {
        const float deck = dims_of(def.model).turret_z * kVehicleScale;
        DrawEllipse(static_cast<int>(ring.x), static_cast<int>(ring.y - deck), 7.0f, 3.5f, lit({16, 14, 12, 255}));  // where it sat
        const float a = hash_unit(h1) * 6.2831853f;
        const float dist = 0.95f + 0.3f * hash_unit(h1 >> 16);
        const Vector2 lie = on_terrain(map, {r.ground.x + std::cos(a) * dist, r.ground.y + std::sin(a) * dist});
        at = {lie.x, lie.y + deck};
        const float b = hash_unit(h2) * 6.2831853f;
        dir = {std::cos(b), std::sin(b)};
        tilt = (hash_unit(h2 >> 16) - 0.5f) * 60.0f;
        flip = (h2 >> 5) % 3 == 0;
        // Its first moment: up in an arc, tumbling over, then down where it lies.
        constexpr float kFlight = 1.1f;
        if (r.age < kFlight) {
            const float t = r.age / kFlight;
            const Vector2 ground = lerp(ring, lie, t);
            DrawEllipse(static_cast<int>(ground.x), static_cast<int>(ground.y), 7.0f, 3.0f, lit({16, 14, 12, 110}));  // its shadow
            const float up = 90.0f * 4.0f * t * (1.0f - t);
            at = {ring.x + (at.x - ring.x) * t, ring.y + (at.y - ring.y) * t - up};
            tilt += 360.0f * t;
            flip = flip && t > 0.5f;
        }
    }
    both(turret, rust_turret, [&](const SpriteSheet& sh, Color tint) {
        float a = std::atan2(dir.y, dir.x);
        if (a < 0.0f) a += 6.2831853f;
        const int d = static_cast<int>(std::lround(a / 6.2831853f * static_cast<float>(sh.dirs))) % sh.dirs;
        const float w = static_cast<float>(sh.w);
        const Rectangle src{static_cast<float>(d * sh.w), 0.0f, flip ? -w : w, flip ? -static_cast<float>(sh.h) : static_cast<float>(sh.h)};
        DrawTexturePro(sh.atlas, src, {std::round(at.x), std::round(at.y), w, static_cast<float>(sh.h)}, sh.origin, tilt, tint);
    });
    if (sink) mud_halo(fr, armor_size(def.model), true, r.seed);
    g_light = light;
}

// A truck knocked out: burnt black, the tyres gone to the rims, the glass
// out, the tarpaulin burnt off its hoops; rusting over the weeks; a rim or
// two lying beside it. Drowned with a bridge: the top of it showing, rings
// on the water round it.
void WorldRenderer::draw_truck_wreck(const engine::TileMap& map, const Remains& r, TruckModel model, bool drowned) const {
    const TruckLook& look = truck_look_of(model);
    Vector2 f = r.facing;
    const float fl = std::hypot(f.x, f.y);
    f = fl > 0.0f ? Vector2{f.x / fl, f.y / fl} : Vector2{1.0f, 0.0f};
    const Frame fr = make_frame(map, r.ground, f);
    const float k = kVehicleScale;
    if (drowned) {
        const SpriteSheet* body = sheet(SpritePart::TruckBody, truck_variant(model, 1, 2, 0), r.owner);
        if (!body) return;
        const float t = static_cast<float>(GetTime());
        for (int i = 0; i < 2; ++i) {
            const float grow = std::fmod(t * 0.4f + static_cast<float>(i) * 0.5f, 1.0f);
            DrawEllipseLines(static_cast<int>(fr.o.x), static_cast<int>(fr.o.y), 12.0f + 10.0f * grow, 5.0f + 4.0f * grow,
                             ColorAlpha({190, 214, 226, 255}, 0.55f * (1.0f - grow)));
        }
        for (int i = 0; i < 7; ++i) {  // its shape under the water
            const float a = (static_cast<float>(i) / 6.0f - 0.5f) * look.length * 0.9f * k;
            DrawCircleV(fr.at(a, 0.0f), 6.0f, {24, 46, 60, 34});
        }
        draw_sprite(*body, fr.o, fr.f, 0);
        return;
    }
    const int left = wreck_load(static_cast<Cargo>(r.cargo));  // what's left of its load on it
    const SpriteSheet* burnt = sheet(SpritePart::TruckBody, truck_variant(model, 4, 0, left), r.owner);
    const SpriteSheet* rusted = sheet(SpritePart::TruckBody, truck_variant(model, 5, 0, left), r.owner);
    if (!burnt) burnt = sheet(SpritePart::TruckBody, truck_variant(model, 4, 0, 0), r.owner);
    if (!rusted) rusted = sheet(SpritePart::TruckBody, truck_variant(model, 5, 0, 0), r.owner);
    if (!burnt) return;
    const float fade = std::clamp((kTankWreckLifetime - r.age) / 3.0f, 0.0f, 1.0f);
    const float rust = std::clamp((r.age - 20.0f) / 90.0f, 0.0f, 0.9f);
    const float light = g_light;
    g_light = light * (0.4f + 0.6f * fade);
    draw_scorch(fr.o, r.cargo == static_cast<uint8_t>(Cargo::Fuel) ? 2
                      : r.cargo == static_cast<uint8_t>(Cargo::Drums) || r.cargo == static_cast<uint8_t>(Cargo::Crates) || r.cargo == static_cast<uint8_t>(Cargo::Shells) ? 1 : 0,
                r.age < 40.0f, fade, r.seed);  // the ground burnt round it: wide where the fuel ran out burning, where the rounds went up
    {  // a burnt rim or two lying beside it
        const uint32_t hw = tile_hash(static_cast<int>(r.seed & 0xFFFF), 59);
        const Color steel = mix({74, 76, 64, 255}, {112, 68, 44, 255}, rust);
        const float side = (hw & 1u) ? 1.0f : -1.0f;
        for (int i = 0; i < 1 + static_cast<int>((hw >> 1) % 2); ++i) {
            const uint32_t hi = tile_hash(static_cast<int>(hw >> 4) + i * 31, i * 7 + 5);
            const Vector2 p = fr.at((hash_unit(hi) - 0.5f) * look.length * k, (look.width + 0.2f + 0.25f * hash_unit(hi >> 8)) * k * side);
            const float rx = look.wheel_r * k * 1.1f;
            const float ry = rx * (0.45f + 0.2f * hash_unit(hi >> 12));
            DrawEllipse(static_cast<int>(p.x + 1.5f), static_cast<int>(p.y + 1.2f), rx + 1.5f, ry + 1.0f, lit({16, 14, 12, 90}));
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), rx * 0.8f + 1.0f, ry * 0.8f + 0.8f, lit({18, 18, 16, 255}));
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), rx * 0.8f, ry * 0.8f, lit(shade(steel, 0.85f)));
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), rx * 0.5f, ry * 0.5f, lit(shade(steel, 1.3f)));
            DrawEllipse(static_cast<int>(p.x), static_cast<int>(p.y), rx * 0.18f, ry * 0.18f, lit({18, 18, 16, 255}));
        }
    }
    {
        const auto cargo = static_cast<Cargo>(r.cargo);
        const uint32_t hc = tile_hash(static_cast<int>(r.seed & 0xFFFF), 71);
        if (cargo == Cargo::Crates || cargo == Cargo::Shells) {  // thrown about: the boxes, the shells
            for (int i = 0; i < 12; ++i) {
                const float a = hash_unit(tile_hash(static_cast<int>(hc >> 3) + i * 13, i * 7)) * 6.2831853f;
                const float d = 0.5f + 0.9f * hash_unit(tile_hash(static_cast<int>(hc >> 9) + i * 5, i * 11));
                const float pa = std::cos(a) * d;
                const float pc = std::sin(a) * d;
                const bool shell = cargo == Cargo::Shells ? i % 3 != 0 : i % 4 == 0;
                const bool black = i % 2 == 0;
                if (shell) {
                    block(fr, pa - 0.03f, pa + 0.03f, pc - 0.01f, pc + 0.01f, 0.0f, 1.3f, black ? Color{50, 46, 40, 255} : Color{112, 112, 80, 255});
                } else {
                    const Color box = cargo == Cargo::Shells ? Color{170, 130, 84, 255} : Color{88, 100, 64, 255};
                    block(fr, pa - 0.035f, pa + 0.035f, pc - 0.025f, pc + 0.025f, 0.0f, 2.0f, black ? shade(box, 0.35f) : box);
                }
            }
        }
        if (is_logs(cargo)) {  // a couple rolled off, charred
            for (int i = 0; i < 2; ++i) {
                const float side = (i == 0 ? 1.0f : -1.0f) * (look.width * k + 0.12f + 0.08f * hash_unit(hc >> (4 + i)));
                block(fr, -look.length * k * 0.4f, look.length * k * 0.1f, side - 0.035f, side + 0.035f, 0.0f, 2.2f, {50, 40, 32, 255}, 0.0f, 0.0f, 0.015f);
            }
        }
        if (cargo == Cargo::Sacks) {  // burst sacks, the flour white on the ground
            const Vector2 p = fr.at(-look.length * k * 0.4f, look.width * k + 0.15f);
            for (int i = 0; i < 40; ++i) {  // the flour spilt, in flecks
                const float a = rand01(hc, 300 + i) * 6.2831853f;
                const float d = std::sqrt(rand01(hc, 340 + i));
                DrawRectangleRec({std::round(p.x + std::cos(a) * d * 9.0f), std::round(p.y + std::sin(a) * d * 4.5f), 1.0f + static_cast<float>(i % 2), 1.0f},
                                 lit(i % 3 == 0 ? Color{176, 170, 156, 255} : Color{222, 218, 206, 255}));
            }
            block(fr, -look.length * k * 0.45f, -look.length * k * 0.35f, look.width * k + 0.06f, look.width * k + 0.14f, 0.0f, 1.6f, {120, 108, 84, 255}, 0.01f, 0.01f, 0.015f);
        }
    }
    draw_sprite(*burnt, fr.o, fr.f, 0);
    if (rusted && rust > 0.0f) {
        float a = std::atan2(fr.f.y, fr.f.x);
        if (a < 0.0f) a += 6.2831853f;
        const int d = static_cast<int>(std::lround(a / 6.2831853f * static_cast<float>(rusted->dirs))) % rusted->dirs;
        DrawTexturePro(rusted->atlas, {static_cast<float>(d * rusted->w), 0.0f, static_cast<float>(rusted->w), static_cast<float>(rusted->h)},
                       {std::round(fr.o.x - rusted->origin.x), std::round(fr.o.y - rusted->origin.y), static_cast<float>(rusted->w),
                        static_cast<float>(rusted->h)},
                       {0.0f, 0.0f}, 0.0f, ColorAlpha(lit(WHITE), rust));
    }
    if (truck_turns_top(model)) {
        if (const SpriteSheet* top = sheet(SpritePart::TruckTop, truck_variant(model, 4, 0, 0), r.owner)) {
            const Vector2 ring = truck_top_ring_of(model);
            draw_sprite(*top, fr.at(ring.x * k, ring.y * k), fr.f, 0);
        }
    }
    g_light = light;
}

// Two ruts where a tracked vehicle went: dark earth pressed into the grass
// or the field, the tread marks across them, fading. Drawn tile by tile as
// the ground is (each rut cut to the tile), under whatever grows there: the
// crops flattened over them, the trees standing over them.
void WorldRenderer::draw_track_marks(const engine::World& world, Rectangle view) const {
    const engine::TileMap& map = world.map();
    marks_at_.clear();
    const Rectangle near{view.x - 60.0f, view.y - 60.0f, view.width + 120.0f, view.height + 120.0f};
    for (const TrackMark& m : track_marks_) {
        if (!CheckCollisionPointRec(on_terrain(map, m.a), near)) continue;
        const float reach = m.half + 0.12f;
        for (int ty = static_cast<int>(std::floor(std::min(m.a.y, m.b.y) - reach)); ty <= static_cast<int>(std::floor(std::max(m.a.y, m.b.y) + reach)); ++ty) {
            for (int tx = static_cast<int>(std::floor(std::min(m.a.x, m.b.x) - reach)); tx <= static_cast<int>(std::floor(std::max(m.a.x, m.b.x) + reach)); ++tx) {
                if (map.contains_tile(tx, ty)) marks_at_[ty * map.width() + tx].push_back(&m);
            }
        }
    }
}

void WorldRenderer::draw_marks_on_tile(const engine::TileMap& map, int tx, int ty) const {
    const auto it = marks_at_.find(ty * map.width() + tx);
    if (it == marks_at_.end()) return;
    const auto x0 = static_cast<float>(tx);
    const auto y0 = static_cast<float>(ty);
    for (const TrackMark* mp : it->second) {
        const TrackMark& m = *mp;
        const float fade = 1.0f - m.age / kTrackSeconds;
        const Vector2 d{m.b.x - m.a.x, m.b.y - m.a.y};
        const float len = std::hypot(d.x, d.y);
        if (len <= 0.0f) continue;
        const Vector2 dir{d.x / len, d.y / len};
        const Vector2 side{-dir.y, dir.x};
        const float rut = m.tyres ? 0.05f : 0.11f;  // half a rut's width
        for (const float sgn : {-1.0f, 1.0f}) {
            // The rut's middle line, cut to the tile.
            const Vector2 a{m.a.x + side.x * m.half * sgn, m.a.y + side.y * m.half * sgn};
            float t0 = 0.0f;
            float t1 = 1.0f;
            bool inside = true;
            for (const auto& [p, q] : {std::pair{-d.x, a.x - x0}, std::pair{d.x, x0 + 1.0f - a.x}, std::pair{-d.y, a.y - y0}, std::pair{d.y, y0 + 1.0f - a.y}}) {
                if (p == 0.0f) {
                    if (q < 0.0f) inside = false;
                    continue;
                }
                const float r = q / p;
                if (p < 0.0f) {
                    t0 = std::max(t0, r);
                } else {
                    t1 = std::min(t1, r);
                }
            }
            if (!inside || t1 <= t0) continue;
            const Vector2 pa{a.x + d.x * t0, a.y + d.y * t0};
            const Vector2 pb{a.x + d.x * t1, a.y + d.y * t1};
            auto pt = [&](Vector2 p, float across) { return on_terrain(map, {p.x + side.x * across, p.y + side.y * across}); };
            const Vector2 a0 = pt(pa, -rut);
            const Vector2 a1 = pt(pa, rut);
            const Vector2 b1 = pt(pb, rut);
            const Vector2 b0 = pt(pb, -rut);
            fill_quad(a0, a1, b1, b0, ColorAlpha({58, 46, 32, 255}, (m.tyres ? 0.55f : 0.7f) * fade));
            // The earth squeezed up along its edges; the tread marks across it (a track's).
            DrawLineV(pt(pa, rut * 1.25f), pt(pb, rut * 1.25f), ColorAlpha(lit({112, 94, 66, 255}), 0.35f * fade));
            if (!m.tyres) {
                const float step = 0.09f;
                for (float t = std::ceil((t0 * len) / step) * step; t < t1 * len; t += step) {
                    const Vector2 c{a.x + dir.x * t, a.y + dir.y * t};
                    DrawLineV(pt(c, -rut), pt(c, rut), ColorAlpha(lit({36, 28, 20, 255}), 0.45f * fade));
                }
            }
        }
    }
}

void WorldRenderer::draw_blasts(const engine::TileMap& map) const {
    // The little ones first: where several go off together, the biggest ball of fire over the rest.
    std::vector<const Blast*> order;
    for (const Blast& b : blasts_) order.push_back(&b);
    std::stable_sort(order.begin(), order.end(), [](const Blast* a, const Blast* b) { return a->radius < b->radius; });
    for (const Blast* bp : order) {
        const Blast& b = *bp;
        if (b.age < 0.0f) continue;  // not yet
        if (b.radius >= kMushroomBlast) {  // an aircraft crashing: the mushroom
            if (b.age >= kMushroomSeconds) continue;
            const Vector2 p = on_terrain(map, b.ground);
            const FxSheet& s = fx_sheet(Fx::Mushroom, static_cast<int>(tile_hash(static_cast<int>(b.ground.x * 17.0f), static_cast<int>(b.ground.y * 23.0f)) % 3u));
            draw_fx(s, static_cast<int>(b.age / kMushroomSeconds * static_cast<float>(s.frames)), {p.x, p.y - b.z});
            continue;
        }
        const int size = b.radius < 0.45f ? 0 : b.radius < 1.0f ? 1 : b.radius < 1.8f ? 2 : 3;
        if (b.age >= kBlastSeconds[size]) continue;
        const Vector2 p = on_terrain(map, b.ground);
        const uint32_t v = tile_hash(static_cast<int>(b.ground.x * 17.0f), static_cast<int>(b.ground.y * 23.0f + b.z));
        const FxSheet& s = fx_sheet(static_cast<Fx>(static_cast<int>(Fx::Blast0) + size), static_cast<int>(v % 3u));
        draw_fx(s, static_cast<int>(b.age / kBlastSeconds[size] * static_cast<float>(s.frames)), {p.x, p.y - b.z});
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
    if (const int32_t seats = engine::unit_type(u.type).troop_capacity; seats > 0 && (!u.passengers.empty() || !u.riders.empty())) {
        // The squad aboard: a pip a man, over the seats; those on the armor above, greener.
        const Vector2 feet = on_terrain(map, unit_ground_pos(u, alpha));
        const float x = feet.x - static_cast<float>(seats) * 2.0f;
        const float y = feet.y - 32.0f;
        DrawRectangleRec({x - 1, y - 1, static_cast<float>(seats) * 4.0f + 1, 5}, {0, 0, 0, 170});
        for (int32_t i = 0; i < static_cast<int32_t>(u.passengers.size()); ++i) {
            DrawRectangleRec({x + static_cast<float>(i) * 4.0f, y, 3, 3}, {225, 215, 160, 255});
        }
        if (!u.riders.empty()) {
            const float rx = feet.x - static_cast<float>(engine::kRidersOnArmor) * 2.0f;
            DrawRectangleRec({rx - 1, y - 7, static_cast<float>(engine::kRidersOnArmor) * 4.0f + 1, 5}, {0, 0, 0, 170});
            for (int32_t i = 0; i < static_cast<int32_t>(u.riders.size()); ++i) {
                DrawRectangleRec({rx + static_cast<float>(i) * 4.0f, y - 6, 3, 3}, {170, 214, 120, 255});
            }
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
