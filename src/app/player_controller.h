#pragma once

#include <span>
#include <vector>

#include <raylib.h>

#include "engine/world.h"
#include "hud/hud.h"
#include "net/lockstep.h"
#include "render/camera.h"
#include "render/world_renderer.h"

namespace app {

// Turns the local player's mouse and keyboard into selection changes and
// engine commands. Selection is client-side only: it never enters the
// simulation, just the unit ids inside the commands do.
class PlayerController {
public:
    explicit PlayerController(engine::PlayerId player) : player_(player) {}

    // Commands go to `lockstep`, which decides on which tick they run.
    // alpha: render interpolation factor, so clicks hit units where they are drawn.
    void update(const engine::World& world, net::Lockstep& lockstep, const render::RtsCamera& camera,
                const hud::Hud& hud, render::WorldRenderer& renderer, float alpha);

    void select_army(const engine::World& world);
    void order_move(net::Lockstep& lockstep, render::WorldRenderer& renderer, Vector2 ground);
    void order_attack_move(net::Lockstep& lockstep, render::WorldRenderer& renderer, Vector2 ground);
    void order_attack(net::Lockstep& lockstep, engine::EntityId target);
    void order_attack_ground(net::Lockstep& lockstep, render::WorldRenderer& renderer, Vector2 ground);
    void order_garrison(net::Lockstep& lockstep, engine::EntityId structure);
    void order_gather(net::Lockstep& lockstep, const engine::World& world, render::WorldRenderer& renderer,
                      Vector2 ground);
    void order_retrain(net::Lockstep& lockstep);
    void order_train(net::Lockstep& lockstep, const engine::World& world, engine::UnitTypeId type);
    void order_stop(net::Lockstep& lockstep);

    std::span<const engine::EntityId> selection() const { return selection_; }
    // A selected building of ours (the headquarters), 0 if none.
    engine::EntityId selected_structure() const { return selected_structure_; }
    bool dragging() const;
    Rectangle drag_rect() const;  // screen space
    // A hotkey was pressed and the next left click picks the order's target
    // point ("A": attack-move, "G": fire at ground). Empty when not targeting.
    const char* targeting_label() const;
    bool targeting() const { return targeting_ != Targeting::None; }

private:
    void prune_selection(const engine::World& world);
    // The unit drawn under the cursor, own or enemy.
    const engine::Unit* unit_at(const engine::World& world, const render::RtsCamera& camera, Vector2 mouse,
                                float alpha, bool own) const;
    void click_select(const engine::World& world, const render::RtsCamera& camera, Vector2 mouse,
                      bool additive, float alpha);
    void box_select(const engine::World& world, const render::RtsCamera& camera, Rectangle box,
                    bool additive, float alpha);
    void order_to_point(net::Lockstep& lockstep, render::WorldRenderer& renderer, Vector2 ground,
                        engine::CommandType type);
    bool has_workers(const engine::World& world) const;

    enum class Targeting { None, AttackMove, AttackGround };

    engine::PlayerId player_;
    std::vector<engine::EntityId> selection_;  // sorted, unique
    engine::EntityId selected_structure_ = 0;
    bool pressing_ = false;
    Vector2 press_pos_{};
    Targeting targeting_ = Targeting::None;
};

}  // namespace app
