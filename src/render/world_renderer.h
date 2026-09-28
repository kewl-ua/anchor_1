#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <raylib.h>

#include "engine/world.h"
#include "render/camera.h"
#include "render/iso.h"

namespace render {

// A railway car on the track: where it is and which way it points.
struct TrainCar {
    Vector2 ground;
    Vector2 facing;
    int kind;  // 0 is the locomotive, then the wagons
};

// A building the player is about to place: drawn green where it fits, red where not.
struct BuildGhost {
    engine::StructureType type;
    engine::TilePos origin;
    bool valid;
};

// Draws the game world in isometric view. Reads the engine state, never
// changes it. Owns purely visual state: order markers, explosions, wrecks.
//
// Units are placeholder shapes for now; sprites will replace draw_soldier()
// and draw_vehicle() without touching anything else.
class WorldRenderer {
public:
    // Marker where the player ordered a move (green) or an attack-move (red).
    void add_order_ping(Vector2 ground, bool attack);

    // Whose eyes the world is drawn through. `reveal` lifts the fog of war
    // (development and replays; it changes nothing in the game).
    void set_viewer(engine::PlayerId viewer, bool reveal);

    // Spawns effects for what happened since the last frame (impacts, deaths)
    // and ages them. Call once per frame after the simulation advanced.
    void update(const engine::World& world, float dt);

    // alpha in [0, 1]: progress from the last tick towards the next one.
    // `selection` must be sorted.
    // `trench` is a trench being laid out: its tiles, drawn green where they can be dug.
    void draw(const engine::World& world, const RtsCamera& camera, float alpha,
              std::span<const engine::EntityId> selection, engine::EntityId selected_structure = 0,
              const BuildGhost* ghost = nullptr, std::span<const engine::TilePos> trench = {}) const;

private:
    struct Ping {
        Vector2 ground;
        float age;
        bool attack;
    };
    struct Blast {
        Vector2 ground;
        float age;
        float radius;  // tiles
    };
    struct Remains {
        Vector2 ground;
        float age;
        bool vehicle;
        engine::UnitTypeId type = engine::UnitTypeId::Rifleman;
        engine::PlayerId owner = 0;
        Vector2 hull{1.0f, 0.0f};    // a tank's, when it was knocked out
        Vector2 facing{1.0f, 0.0f};
        uint32_t seed = 0;
    };
    // Smoke, flames, clods of earth, spray: flying about for a moment. Only
    // for the eye: nothing in the game depends on them.
    struct Particle {
        enum class Kind : uint8_t { Smoke, Clod, Flame, Spray };
        Kind kind;
        Vector2 ground;  // tiles
        float z;         // pixels above the ground
        Vector2 vel;     // tiles a second
        float vz;        // pixels a second, up
        float life;      // seconds
        float size;      // pixels
        float grow;      // pixels a second
        Color color;
        float age = 0.0f;
    };
    std::vector<Particle> particles_;
    std::unordered_map<engine::EntityId, engine::Tick> shots_seen_;  // each unit's last shot we've made smoke for
    uint32_t fx_rng_ = 0x2545F491u;
    float fx_random();  // 0..1
    void spawn_burst(const engine::World& world, Vector2 at, float splash);
    void spawn_muzzle(const engine::World& world, const engine::Unit& u);
    void spawn_fire(Vector2 at, float height, int wear, float dt);
    void draw_particles(const engine::TileMap& map) const;
    void draw_wreck(const engine::TileMap& map, const Remains& r) const;
    struct StructureSeen {
        Vector2 center;
        engine::StructureType type;
    };

    void draw_terrain(const engine::World& world, Rectangle view) const;
    // The world is drawn through a shader that gives surfaces their grain
    // (loaded on the first frame: it needs the window).
    mutable Shader grain_{};
    mutable int grain_zoom_loc_ = -1;
    // Pixel-art sprites baked from the vehicles' drawing: every direction
    // (and frame) of a part of a kind of vehicle, for each side's colours.
    struct SpriteSheet {
        Texture2D atlas{};
        int w = 0;
        int h = 0;
        int dirs = 0;
        int frames = 0;
        Vector2 origin{};  // where the ground point under the part is, in a frame
    };
    enum class SpritePart : int { TankHull, TankTurret };
    // (part, its variant: a tank's reactive armor), owner.
    mutable std::map<std::pair<std::pair<int, int>, int>, SpriteSheet> sheets_;
    mutable RenderTexture2D bake_target_{};
    mutable std::array<int, 4> world_era_{};  // each side's reactive armor, as last baked
    void bake_sprites(const engine::World& world) const;
    const SpriteSheet* sheet(SpritePart part, int variant, engine::PlayerId owner) const;
    void draw_sprite(const SpriteSheet& sheet, Vector2 at, Vector2 dir, int frame) const;
    // Fog state of a tile for the viewer.
    static constexpr int kUnexplored = 0;
    static constexpr int kRemembered = 1;  // explored, not in view now
    static constexpr int kInView = 2;
    int fog(const engine::World& world, int tx, int ty) const;
    // The light on the ground at a tile corner, from the slope around it,
    // and at any ground point, blended from its tile's corners: smooth over
    // hills and gullies.
    float corner_light(int cx, int cy) const;
    float light_at(Vector2 ground) const;
    // Whether a tile lies hidden behind the ground in front of it (behind a
    // spoil tip, say): what stands or lies on it isn't drawn.
    bool behind_relief(int tx, int ty) const;
    // A tile's ground, drawn like AoE II's: grass in patches of green and
    // dry, ragged edges where kinds of ground meet, sand and foam along the
    // water, bare trodden earth, tufts, bushes, flowers, stones.
    // `overlay`: false, the ground itself; true, what goes over it once the
    // tile's own markings (furrows, ruts, slabs) are down.
    void paint_ground(const engine::World& world, int tx, int ty, engine::Terrain terrain, bool overlay) const;
    // What ground shows at a point, and how much water is about it: every
    // tile we know weighs on the points around its centre, fading out a tile
    // away, and the heaviest ground shows, each weight wobbling a little so
    // the lines between kinds of ground run ragged, not along tile edges.
    struct GroundAt {
        engine::Terrain terrain;
        float water;  // 0..1
    };
    GroundAt ground_at(const engine::World& world, Vector2 p, engine::Terrain fallback) const;
    bool in_view(const engine::World& world, Vector2 ground) const;
    bool shows(const engine::World& world, const engine::Unit& u) const;
    void remember(const engine::World& world);
    void draw_remains(const engine::TileMap& map) const;
    void draw_pings(const engine::TileMap& map) const;
    void draw_orders(const engine::World& world, const engine::Unit& u, float alpha) const;
    void draw_unit(const engine::TileMap& map, const engine::Unit& u, float alpha) const;
    void draw_aircraft(const engine::TileMap& map, const engine::Unit& u, float alpha) const;
    void draw_soldier(const engine::Unit& u, Vector2 feet, Vector2 facing) const;
    void draw_vehicle(const engine::TileMap& map, const engine::Unit& u, Vector2 ground, Vector2 facing) const;
    void draw_projectile(const engine::Projectile& p, float alpha) const;
    void draw_shots(const engine::World& world, float alpha) const;
    void draw_blasts(const engine::TileMap& map) const;
    void draw_health_bar(const engine::TileMap& map, const engine::Unit& u, float alpha) const;
    void draw_supply_warning(const engine::TileMap& map, const engine::Unit& u, float alpha) const;
    void draw_radio_marks(const engine::World& world, const engine::Unit& u, float alpha) const;
    void draw_load_bar(const engine::TileMap& map, const engine::Unit& u, float alpha) const;
    void draw_structure_overlays(const engine::World& world, Rectangle view) const;
    // Trains running on schedule to each station, as railway cars to draw.
    void collect_trains(const engine::World& world, float alpha, std::vector<TrainCar>& cars) const;
    static std::vector<Vector2> rail_route(const engine::World& world, const engine::Structure& station);

    // Smoothed height of every tile corner, (width + 1) x (height + 1).
    // Terrain doesn't change yet, so it's computed once per map.
    float corner(int cx, int cy) const { return corner_heights_[static_cast<size_t>(cy * (cache_width_ + 1) + cx)]; }
    std::vector<float> corner_heights_;
    // The tops of the spoil tips (ground points), for the gullies down their sides.
    std::vector<Vector2> spoil_peaks_;
    void draw_spoil_gullies(const engine::World& world, Rectangle view) const;
    // When each crater appeared (GetTime(); those there from the start count
    // as long weathered), and its kind as last seen, to notice new ones.
    std::vector<float> crater_born_;
    std::vector<uint8_t> crater_seen_;
    uint32_t crater_revision_ = 0;
    void draw_crater(const engine::World& world, int tx, int ty) const;
    // Yards of a village (not of the town or the works): earth and grass,
    // fences, woodpiles, wells, fruit trees. One per tile, found once per map.
    std::vector<uint8_t> village_;
    bool village(int tx, int ty) const {
        return tx >= 0 && ty >= 0 && tx < cache_width_ && ty < cache_height_ &&
               village_.size() == static_cast<size_t>(cache_width_ * cache_height_) &&
               village_[static_cast<size_t>(ty * cache_width_ + tx)] != 0;
    }
    // Each bridge's deck, from its tiles (see iso::Deck).
    std::vector<std::pair<engine::EntityId, iso::Deck>> bridge_decks_;
    void draw_bridges(const engine::World& world) const;
    int cache_width_ = 0;
    int cache_height_ = 0;

    std::vector<Ping> pings_;
    std::vector<Blast> blasts_;
    std::vector<Remains> remains_;
    // Impacts from ticks before this one have already become blasts.
    engine::Tick impacts_seen_until_ = 0;
    // What was alive / standing last frame, to notice deaths and collapses.
    std::unordered_map<engine::EntityId, Remains> units_seen_;
    std::unordered_map<engine::EntityId, StructureSeen> structures_seen_;

    engine::PlayerId viewer_ = 0;
    bool reveal_ = false;
    // The ground as the viewer last saw it, and others' buildings likewise.
    std::vector<engine::Terrain> seen_terrain_;
    std::map<engine::EntityId, engine::Structure> remembered_;
    uint32_t remembered_revision_ = 0;
    // Each station's track, from its wall to the end of the line.
    std::vector<std::pair<engine::EntityId, std::vector<Vector2>>> rail_routes_;
    uint32_t routes_revision_ = 0;
};

// Interpolated ground position of a unit.
Vector2 unit_ground_pos(const engine::Unit& unit, float alpha);
// Screen position of the middle of a unit's body, for clicking and box selection.
Vector2 unit_screen_pos(const RtsCamera& camera, const engine::TileMap& map, const engine::Unit& unit,
                        float alpha);
// How close to unit_screen_pos() a click must be, in screen pixels.
float unit_pick_radius(const RtsCamera& camera, const engine::Unit& unit);
// What is drawn under a screen point: a building's walls and roof and a
// tree's crown stand above the tile they're on, and a click on them counts.
const engine::Structure* structure_on_screen(const RtsCamera& camera, const engine::World& world, Vector2 screen);
std::optional<engine::TilePos> resource_on_screen(const RtsCamera& camera, const engine::World& world, Vector2 screen);

}  // namespace render
