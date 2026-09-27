#pragma once

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <raylib.h>

#include "app/player_controller.h"
#include "engine/simulation.h"
#include "hud/hud.h"
#include "net/lockstep.h"
#include "net/transport.h"
#include "render/camera.h"
#include "render/world_renderer.h"

namespace app {

// One running match: the simulation, the lockstep driving it and the local
// player's view of it. Offline and online games are the same class; an online
// game just passes a transport.
class Game {
public:
    // `transport` may be null (offline); otherwise it must outlive the Game.
    Game(uint64_t seed, int32_t map_size, engine::PlayerId local_player, int player_count,
         net::Transport* transport);
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    void update(float dt);
    // `net` carries connection info from the session; the game adds the lockstep status.
    void draw(hud::NetStatus net) const;

    const engine::World& world() const { return sim_.world(); }
    engine::PlayerId local_player() const { return lockstep_.local_player(); }

    // Jumps the camera to the middle of the selected units (Space key).
    void center_camera_on_selection();
    void center_camera_on(Vector2 ground);
    void set_camera_zoom(float zoom) { camera_.set_zoom(zoom); }

    // Development helpers for --smoke-test.
    void submit(engine::Command cmd) { lockstep_.submit(std::move(cmd)); }
    void select_structure(engine::EntityId id) { controller_.select_structure(id); }
    void select_units(std::vector<engine::EntityId> ids) { controller_.select_units(std::move(ids)); }
    void set_tick_limit(engine::Tick limit) { tick_limit_ = limit; }
    void set_time_scale(float scale) { time_scale_ = scale; }
    void select_army_and_attack_move(Vector2 ground);
    // Lifts the fog of war on screen (the game itself still plays by it).
    void set_reveal(bool reveal);
    // Offline smoke scenes only: a peer changing the world directly would desync.
    engine::World& world_for_setup() { return sim_.world_for_setup(); }

private:
    engine::Simulation sim_;
    net::Lockstep lockstep_;  // refers to sim_, so must be declared after it
    render::RtsCamera camera_;
    render::WorldRenderer renderer_;
    hud::Hud hud_;
    PlayerController controller_;

    float accumulator_ = 0.0f;
    float alpha_ = 0.0f;
    float stall_time_ = 0.0f;
    float time_scale_ = 1.0f;
    bool reveal_ = false;
    bool minimap_drag_ = false;  // left button went down on the minimap
    engine::Tick tick_limit_ = std::numeric_limits<engine::Tick>::max();
};

}  // namespace app
