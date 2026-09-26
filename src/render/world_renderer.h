#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <raylib.h>

#include "engine/world.h"
#include "render/camera.h"

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
    void draw(const engine::World& world, const RtsCamera& camera, float alpha,
              std::span<const engine::EntityId> selection, engine::EntityId selected_structure = 0,
              const BuildGhost* ghost = nullptr) const;

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
    };
    struct StructureSeen {
        Vector2 center;
        engine::StructureType type;
    };

    void draw_terrain(const engine::World& world, Rectangle view) const;
    // Fog state of a tile for the viewer.
    static constexpr int kUnexplored = 0;
    static constexpr int kRemembered = 1;  // explored, not in view now
    static constexpr int kInView = 2;
    int fog(const engine::World& world, int tx, int ty) const;
    bool in_view(const engine::World& world, Vector2 ground) const;
    bool shows(const engine::World& world, const engine::Unit& u) const;
    void remember(const engine::World& world);
    void draw_remains(const engine::TileMap& map) const;
    void draw_pings(const engine::TileMap& map) const;
    void draw_orders(const engine::World& world, const engine::Unit& u, float alpha) const;
    void draw_unit(const engine::TileMap& map, const engine::Unit& u, float alpha) const;
    void draw_soldier(const engine::Unit& u, Vector2 feet, Vector2 facing) const;
    void draw_vehicle(const engine::TileMap& map, const engine::Unit& u, Vector2 ground, Vector2 facing) const;
    void draw_projectile(const engine::Projectile& p, float alpha) const;
    void draw_shots(const engine::World& world, float alpha) const;
    void draw_blasts(const engine::TileMap& map) const;
    void draw_health_bar(const engine::TileMap& map, const engine::Unit& u, float alpha) const;
    void draw_structure_overlays(const engine::World& world, Rectangle view) const;
    // Trains running on schedule to each station, as railway cars to draw.
    void collect_trains(const engine::World& world, float alpha, std::vector<TrainCar>& cars) const;
    static std::vector<Vector2> rail_route(const engine::World& world, const engine::Structure& station);

    // Smoothed height of every tile corner, (width + 1) x (height + 1).
    // Terrain doesn't change yet, so it's computed once per map.
    float corner(int cx, int cy) const { return corner_heights_[static_cast<size_t>(cy * (cache_width_ + 1) + cx)]; }
    std::vector<float> corner_heights_;
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

}  // namespace render
