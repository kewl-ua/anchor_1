#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <raylib.h>

#include "engine/world.h"
#include "render/camera.h"

namespace render {

// Draws the game world in isometric view. Reads the engine state, never
// changes it. Owns purely visual state: order markers, explosions, wrecks.
//
// Units are placeholder shapes for now; sprites will replace draw_soldier()
// and draw_vehicle() without touching anything else.
class WorldRenderer {
public:
    // Marker where the player ordered a move (green) or an attack-move (red).
    void add_order_ping(Vector2 ground, bool attack);

    // Spawns effects for what happened since the last frame (impacts, deaths)
    // and ages them. Call once per frame after the simulation advanced.
    void update(const engine::World& world, float dt);

    // alpha in [0, 1]: progress from the last tick towards the next one.
    // `selection` must be sorted.
    void draw(const engine::World& world, const RtsCamera& camera, float alpha,
              std::span<const engine::EntityId> selection, engine::EntityId selected_structure = 0) const;

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

    void draw_terrain(const engine::TileMap& map, Rectangle view) const;
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
    std::unordered_map<engine::EntityId, Vector2> structures_seen_;
};

// Interpolated ground position of a unit.
Vector2 unit_ground_pos(const engine::Unit& unit, float alpha);
// Screen position of the middle of a unit's body, for clicking and box selection.
Vector2 unit_screen_pos(const RtsCamera& camera, const engine::TileMap& map, const engine::Unit& unit,
                        float alpha);
// How close to unit_screen_pos() a click must be, in screen pixels.
float unit_pick_radius(const RtsCamera& camera, const engine::Unit& unit);

}  // namespace render
