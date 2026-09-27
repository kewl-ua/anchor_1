#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <raylib.h>

#include "engine/world.h"
#include "hud/hud.h"
#include "net/lockstep.h"
#include "render/camera.h"
#include "render/world_renderer.h"

namespace app {

// A building being placed: where it would go and whether it fits there.
struct Placement {
    engine::StructureType type;
    engine::TilePos origin;
    bool valid;
};

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
    // The next rear trooper or supply truck with nothing to do, one after
    // another (AoE's idle villager key). False if there is none.
    bool select_next_idle(const engine::World& world);
    void order_move(net::Lockstep& lockstep, render::WorldRenderer& renderer, Vector2 ground);
    void order_attack_move(net::Lockstep& lockstep, render::WorldRenderer& renderer, Vector2 ground);
    void order_attack(net::Lockstep& lockstep, engine::EntityId target);
    void order_attack_ground(net::Lockstep& lockstep, render::WorldRenderer& renderer, Vector2 ground);
    void order_garrison(net::Lockstep& lockstep, engine::EntityId structure);
    void order_gather(net::Lockstep& lockstep, const engine::World& world, render::WorldRenderer& renderer,
                      Vector2 ground);
    void order_retrain(net::Lockstep& lockstep);
    // Supply trucks in the selection resume their run (sent to a depot of
    // ours, they're assigned to it); the rest move to the point.
    void order_haul(net::Lockstep& lockstep, const engine::World& world, render::WorldRenderer& renderer,
                    Vector2 ground, engine::EntityId structure);
    void order_train(net::Lockstep& lockstep, const engine::World& world, engine::UnitTypeId type);
    void order_build(net::Lockstep& lockstep, const Placement& placement);
    void order_help_build(net::Lockstep& lockstep, engine::EntityId site);
    void order_stop(net::Lockstep& lockstep);
    // Rear troops turn the spacious village building at the point into a depot.
    void order_convert(net::Lockstep& lockstep, const engine::World& world, render::WorldRenderer& renderer,
                       Vector2 mouse, const render::RtsCamera& camera);

    // The building following the cursor, if the player is placing one.
    const std::optional<Placement>& placement() const { return placement_; }

    std::span<const engine::EntityId> selection() const { return selection_; }
    // A selected building of ours (the headquarters), 0 if none.
    engine::EntityId selected_structure() const { return selected_structure_; }
    void select_structure(engine::EntityId id) {
        selection_.clear();
        selected_structure_ = id;
    }
    void select_units(std::vector<engine::EntityId> ids) {
        std::sort(ids.begin(), ids.end());
        selection_ = std::move(ids);
        selected_structure_ = 0;
    }
    bool dragging() const;
    Rectangle drag_rect() const;  // screen space
    // A hotkey was pressed and the next left click picks the order's target
    // point ("A": attack-move, "G": fire at ground). Empty when not targeting.
    const char* targeting_label() const;
    // What a right click would do on whatever is under the cursor, for a
    // hint by it ("RMB: cut timber"); empty if nothing special.
    const char* cursor_hint() const { return hint_; }
    bool targeting() const { return targeting_ != Targeting::None; }
    // The command grid for the current selection, for the HUD to draw.
    std::span<const hud::CommandButton> command_buttons() const { return buttons_; }
    // A trench being dragged out: its tiles.
    std::span<const engine::TilePos> trench_preview() const { return trench_preview_; }

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
    bool has_trucks(const engine::World& world) const;
    engine::EntityId last_idle_ = 0;
    bool has_engineers(const engine::World& world) const;
    bool has_service_vehicles(const engine::World& world) const;
    bool has_ammo_trucks(const engine::World& world) const;
    // Tankers and ammunition trucks in the selection are attached to `unit`;
    // the rest go where it is.
    void order_supply(net::Lockstep& lockstep, const engine::World& world, render::WorldRenderer& renderer,
                      const engine::Unit& unit);
    // Where a supply truck loads or unloads.
    static bool is_supply_point(engine::StructureType type);
    // Where a tanker or an ammunition truck loads up, and whether any selected one does there.
    static std::optional<engine::StructureType> depot_for_refill(engine::UnitTypeId type);
    bool refills_at(const engine::World& world, engine::StructureType depot) const;
    void update_placement(const engine::World& world, const render::RtsCamera& camera, Vector2 mouse);

    // The command grid. Each cell holds an action; its hotkey is the cell's.
    enum class Action : uint8_t {
        None, AttackMove, Stop, FireAt, Observe, Haul, Retrain, BuildMenu, Back, Build, Hire, Ability, Upgrade, Unload,
        Research, ConvertMenu, Convert, Gather
    };
    struct Cell {
        Action action = Action::None;
        uint8_t param = 0;  // the unit, building or skill
    };
    void rebuild_grid(const engine::World& world);
    void press_cell(net::Lockstep& lockstep, const engine::World& world, size_t slot);
    // The unit type the grid is for: the most numerous one selected.
    std::optional<engine::UnitTypeId> leading_type(const engine::World& world) const;
    void order_ability(net::Lockstep& lockstep, const engine::World& world, render::WorldRenderer& renderer,
                       engine::AbilityId ability, Vector2 target, Vector2 end);

    enum class Targeting { None, AttackMove, AttackGround, Observe, Ability, Convert, Gather };

    engine::PlayerId player_;
    std::vector<engine::EntityId> selection_;  // sorted, unique
    engine::EntityId selected_structure_ = 0;
    bool pressing_ = false;
    Vector2 press_pos_{};
    Targeting targeting_ = Targeting::None;
    engine::AbilityId aiming_ = engine::AbilityId::AreaShot;  // Targeting::Ability
    std::optional<engine::StructureType> placing_;
    std::optional<Placement> placement_;
    bool build_menu_ = false;  // rear troops: the grid shows what they can build
    bool convert_menu_ = false;  // rear troops: what depot to make of a village building
    const char* hint_ = "";      // see cursor_hint()
    void update_hint(const engine::World& world, const render::RtsCamera& camera, Vector2 mouse, float alpha);
    engine::StructureType converting_ = engine::StructureType::Warehouse;  // Targeting::Convert
    std::optional<Vector2> line_start_;  // a line skill being dragged: where it started
    std::vector<engine::TilePos> trench_preview_;
    std::array<hud::CommandButton, hud::kGridSlots> buttons_{};
    std::array<Cell, hud::kGridSlots> cells_{};
};

}  // namespace app
