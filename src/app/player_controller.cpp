#include "app/player_controller.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include "render/convert.h"
#include "render/iso.h"

namespace app {

namespace {

constexpr float kDragThreshold = 6.0f;  // screen pixels

Rectangle rect_from_points(Vector2 a, Vector2 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(a.x - b.x), std::fabs(a.y - b.y)};
}

Vector2 ground_under(const engine::World& world, const render::RtsCamera& camera, Vector2 screen) {
    return render::iso::pick_ground(world.map(), camera.screen_to_world(screen));
}

}  // namespace

void PlayerController::update(const engine::World& world, net::Lockstep& lockstep,
                              const render::RtsCamera& camera, const hud::Hud& hud,
                              render::WorldRenderer& renderer, float alpha) {
    prune_selection(world);

    const Vector2 mouse = GetMousePosition();
    const bool over_hud = hud.captures_point(mouse);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (IsKeyPressed(KEY_A) && !selection_.empty()) targeting_ = Targeting::AttackMove;
    if (IsKeyPressed(KEY_G) && !selection_.empty()) targeting_ = Targeting::AttackGround;
    if (IsKeyPressed(KEY_ESCAPE) || selection_.empty()) targeting_ = Targeting::None;
    if (IsKeyPressed(KEY_ESCAPE) || !has_workers(world)) placing_.reset();

    // Command panel hotkeys: 1-5 build (rear troops); Q, W, E, T hire (a building).
    constexpr KeyboardKey kBuildKeys[] = {KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_FIVE};
    constexpr KeyboardKey kHireKeys[] = {KEY_Q, KEY_W, KEY_E, KEY_T};
    static_assert(std::size(kBuildKeys) >= std::size(engine::kBuildable));
    for (size_t i = 0; i < std::size(kBuildKeys); ++i) {
        if (IsKeyPressed(kBuildKeys[i]) && selected_structure_ == 0) press_button(lockstep, world, i);
    }
    for (size_t i = 0; i < std::size(kHireKeys); ++i) {
        if (IsKeyPressed(kHireKeys[i]) && selected_structure_ != 0) press_button(lockstep, world, i);
    }
    update_placement(world, camera, mouse);

    // Orders can target the minimap too, like in AoE II.
    const std::optional<Vector2> minimap_ground = hud.minimap_to_ground(mouse);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const std::optional<Vector2> target =
            over_hud ? minimap_ground : std::optional<Vector2>(ground_under(world, camera, mouse));
        if (const std::optional<size_t> button = hud.button_at(mouse)) {
            press_button(lockstep, world, *button);
        } else if (placement_ && !over_hud) {
            if (placement_->valid) {
                order_build(lockstep, *placement_);
                if (!shift) placing_.reset();  // shift: lay several foundations
            }
        } else if (targeting() && target) {
            if (targeting_ == Targeting::AttackMove) order_attack_move(lockstep, renderer, *target);
            if (targeting_ == Targeting::AttackGround) order_attack_ground(lockstep, renderer, *target);
            if (!shift) targeting_ = Targeting::None;  // shift keeps it armed for more clicks
        } else if (!over_hud) {
            pressing_ = true;
            press_pos_ = mouse;
        }
    }
    if (pressing_ && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (dragging()) {
            box_select(world, camera, drag_rect(), shift, alpha);
        } else {
            click_select(world, camera, mouse, shift, alpha);
        }
        pressing_ = false;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        if (placing_ || targeting()) {
            placing_.reset();  // right click cancels, like in most RTS games
            targeting_ = Targeting::None;
        } else if (over_hud) {
            if (minimap_ground) order_move(lockstep, renderer, *minimap_ground);
        } else if (const engine::Unit* enemy = unit_at(world, camera, mouse, alpha, false); enemy && !enemy->inside) {
            order_attack(lockstep, enemy->id);
        } else {
            const Vector2 ground = ground_under(world, camera, mouse);
            const engine::TilePos tile{static_cast<int32_t>(std::floor(ground.x)),
                                       static_cast<int32_t>(std::floor(ground.y))};
            const engine::Structure* structure = world.structure_at(tile);
            const engine::Terrain terrain = world.map().contains(tile) ? world.map().terrain(tile) : engine::Terrain::Grass;
            const bool resource = (terrain == engine::Terrain::Forest || terrain == engine::Terrain::Rock) &&
                                  world.map().resource(tile) > 0;
            if (resource && has_workers(world)) {
                // Rear troops go to work; anyone else selected just goes there.
                order_gather(lockstep, world, renderer, ground);
            } else if (structure && structure->owner == player_ && !structure->built && has_workers(world)) {
                order_help_build(lockstep, structure->id);
            } else if (structure && structure->owner == player_ && is_supply_point(structure->type) &&
                       has_trucks(world)) {
                // Trucks go back on the supply run; anyone else selected just goes there.
                order_haul(lockstep, world, renderer, ground);
            } else if (structure && structure->type == engine::StructureType::House) {
                // Our infantry moves in; a house the enemy holds gets shelled.
                if (structure->owner == engine::kNoOwner || structure->owner == player_) {
                    order_garrison(lockstep, structure->id);
                } else {
                    order_attack_ground(lockstep, renderer, render::to_vector2(structure->center));
                }
            } else {
                order_move(lockstep, renderer, ground);
            }
        }
    }
    if (IsKeyPressed(KEY_S)) order_stop(lockstep);
    if (IsKeyPressed(KEY_F2)) select_army(world);
    if (IsKeyPressed(KEY_R) && has_workers(world)) order_retrain(lockstep);
}

// The same buttons the HUD shows: a building's hiring roster, or the rear
// troops' building list.
void PlayerController::press_button(net::Lockstep& lockstep, const engine::World& world, size_t index) {
    if (const engine::Structure* s = world.find_structure(selected_structure_)) {
        const engine::StructureDef& def = engine::structure_type(s->type);
        if (s->built && index < def.roster_size) order_train(lockstep, world, def.roster[index]);
        return;
    }
    if (has_workers(world) && index < std::size(engine::kBuildable)) {
        placing_ = engine::kBuildable[index];
        targeting_ = Targeting::None;
    }
}

// The foundation follows the cursor, centred on it.
void PlayerController::update_placement(const engine::World& world, const render::RtsCamera& camera,
                                        Vector2 mouse) {
    if (!placing_) {
        placement_.reset();
        return;
    }
    const engine::StructureDef& def = engine::structure_type(*placing_);
    const Vector2 ground = ground_under(world, camera, mouse);
    const engine::TilePos origin{
        static_cast<int32_t>(std::floor(ground.x - static_cast<float>(def.width) * 0.5f + 0.5f)),
        static_cast<int32_t>(std::floor(ground.y - static_cast<float>(def.height) * 0.5f + 0.5f))};
    const bool affordable = engine::can_afford(world.stock(player_), def.cost);
    placement_ = Placement{*placing_, origin, affordable && world.can_place(*placing_, origin)};
}

void PlayerController::select_army(const engine::World& world) {
    selection_.clear();
    selected_structure_ = 0;
    for (const engine::Unit& u : world.units()) {
        if (u.owner == player_ && engine::is_armed(engine::unit_type(u.type)) && !engine::unit_type(u.type).worker) {
            selection_.push_back(u.id);
        }
    }
}

bool PlayerController::has_trucks(const engine::World& world) const {
    return std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && u->type == engine::UnitTypeId::Truck;
    });
}

bool PlayerController::is_supply_point(engine::StructureType type) {
    return type == engine::StructureType::Station || type == engine::StructureType::Warehouse ||
           type == engine::StructureType::AmmoDepot || type == engine::StructureType::FuelDepot;
}

void PlayerController::order_haul(net::Lockstep& lockstep, const engine::World& world,
                                  render::WorldRenderer& renderer, Vector2 ground) {
    engine::Command haul;
    haul.type = engine::CommandType::Haul;
    engine::Command move;
    move.type = engine::CommandType::Move;
    move.target = render::to_fixed_vec2(ground);
    for (engine::EntityId id : selection_) {
        const engine::Unit* u = world.find_unit(id);
        if (!u) continue;
        (u->type == engine::UnitTypeId::Truck ? haul : move).units.push_back(id);
    }
    if (!haul.units.empty()) lockstep.submit(std::move(haul));
    if (!move.units.empty()) lockstep.submit(std::move(move));
    renderer.add_order_ping(ground, false);
}

bool PlayerController::has_workers(const engine::World& world) const {
    return std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && engine::unit_type(u->type).worker;
    });
}

void PlayerController::order_gather(net::Lockstep& lockstep, const engine::World& world,
                                    render::WorldRenderer& renderer, Vector2 ground) {
    engine::Command gather;
    gather.type = engine::CommandType::Gather;
    gather.target = render::to_fixed_vec2(ground);
    engine::Command move;
    move.type = engine::CommandType::Move;
    move.target = gather.target;
    for (engine::EntityId id : selection_) {
        const engine::Unit* u = world.find_unit(id);
        if (!u) continue;
        (engine::unit_type(u->type).worker ? gather : move).units.push_back(id);
    }
    if (!gather.units.empty()) lockstep.submit(std::move(gather));
    if (!move.units.empty()) lockstep.submit(std::move(move));
    renderer.add_order_ping(ground, false);
}

void PlayerController::order_retrain(net::Lockstep& lockstep) {
    engine::Command cmd;
    cmd.type = engine::CommandType::Retrain;
    cmd.units = selection_;
    lockstep.submit(std::move(cmd));
}

void PlayerController::order_train(net::Lockstep& lockstep, const engine::World& world, engine::UnitTypeId type) {
    const engine::Structure* s = world.find_structure(selected_structure_);
    if (!s || s->owner != player_ || !engine::can_train(s->type, type)) return;
    engine::Command cmd;
    cmd.type = engine::CommandType::Train;
    cmd.target_unit = s->id;
    cmd.unit_type = static_cast<uint8_t>(type);
    lockstep.submit(std::move(cmd));
}

void PlayerController::order_move(net::Lockstep& lockstep, render::WorldRenderer& renderer, Vector2 ground) {
    order_to_point(lockstep, renderer, ground, engine::CommandType::Move);
}

void PlayerController::order_attack_move(net::Lockstep& lockstep, render::WorldRenderer& renderer,
                                         Vector2 ground) {
    order_to_point(lockstep, renderer, ground, engine::CommandType::AttackMove);
}

void PlayerController::order_attack_ground(net::Lockstep& lockstep, render::WorldRenderer& renderer,
                                           Vector2 ground) {
    order_to_point(lockstep, renderer, ground, engine::CommandType::AttackGround);
}

void PlayerController::order_to_point(net::Lockstep& lockstep, render::WorldRenderer& renderer, Vector2 ground,
                                      engine::CommandType type) {
    if (selection_.empty()) return;
    engine::Command cmd;
    cmd.type = type;
    cmd.units = selection_;
    cmd.target = render::to_fixed_vec2(ground);
    lockstep.submit(std::move(cmd));
    renderer.add_order_ping(ground, type != engine::CommandType::Move);
}

const char* PlayerController::targeting_label() const {
    switch (targeting_) {
        case Targeting::AttackMove: return "Attack-move";
        case Targeting::AttackGround: return "Fire at ground";
        case Targeting::None: break;
    }
    return "";
}

void PlayerController::order_attack(net::Lockstep& lockstep, engine::EntityId target) {
    if (selection_.empty()) return;
    engine::Command cmd;
    cmd.type = engine::CommandType::Attack;
    cmd.units = selection_;
    cmd.target_unit = target;
    lockstep.submit(std::move(cmd));
}

void PlayerController::order_garrison(net::Lockstep& lockstep, engine::EntityId structure) {
    if (selection_.empty()) return;
    engine::Command cmd;
    cmd.type = engine::CommandType::Garrison;
    cmd.units = selection_;
    cmd.target_unit = structure;
    lockstep.submit(std::move(cmd));
}

void PlayerController::order_build(net::Lockstep& lockstep, const Placement& placement) {
    engine::Command cmd;
    cmd.type = engine::CommandType::Build;
    cmd.units = selection_;
    cmd.target = engine::tile_center(placement.origin);
    cmd.structure_type = static_cast<uint8_t>(placement.type);
    lockstep.submit(std::move(cmd));
}

void PlayerController::order_help_build(net::Lockstep& lockstep, engine::EntityId site) {
    engine::Command cmd;
    cmd.type = engine::CommandType::Build;
    cmd.units = selection_;
    cmd.target_unit = site;
    lockstep.submit(std::move(cmd));
}

void PlayerController::order_stop(net::Lockstep& lockstep) {
    if (selection_.empty()) return;
    engine::Command cmd;
    cmd.type = engine::CommandType::Stop;
    cmd.units = selection_;
    lockstep.submit(std::move(cmd));
}

bool PlayerController::dragging() const {
    if (!pressing_) return false;
    const Vector2 m = GetMousePosition();
    return std::fabs(m.x - press_pos_.x) > kDragThreshold || std::fabs(m.y - press_pos_.y) > kDragThreshold;
}

Rectangle PlayerController::drag_rect() const { return rect_from_points(press_pos_, GetMousePosition()); }

void PlayerController::prune_selection(const engine::World& world) {
    std::erase_if(selection_, [&](engine::EntityId id) { return world.find_unit(id) == nullptr; });
    if (!world.find_structure(selected_structure_)) selected_structure_ = 0;
}

const engine::Unit* PlayerController::unit_at(const engine::World& world, const render::RtsCamera& camera,
                                              Vector2 mouse, float alpha, bool own) const {
    const engine::Unit* best = nullptr;
    float best_dist = 0.0f;
    for (const engine::Unit& u : world.units()) {
        if ((u.owner == player_) != own) continue;
        const Vector2 p = render::unit_screen_pos(camera, world.map(), u, alpha);
        const float dist = std::hypot(p.x - mouse.x, p.y - mouse.y);
        if (dist <= render::unit_pick_radius(camera, u) && (!best || dist < best_dist)) {
            best = &u;
            best_dist = dist;
        }
    }
    return best;
}

void PlayerController::click_select(const engine::World& world, const render::RtsCamera& camera,
                                    Vector2 mouse, bool additive, float alpha) {
    const engine::Unit* hit = unit_at(world, camera, mouse, alpha, true);
    if (hit && hit->inside) hit = nullptr;  // clicking a building picks the building
    if (!hit) {
        if (additive) return;
        selection_.clear();
        selected_structure_ = 0;
        // One of our own buildings under the cursor?
        const Vector2 ground = ground_under(world, camera, mouse);
        const engine::Structure* s = world.structure_at(
            {static_cast<int32_t>(std::floor(ground.x)), static_cast<int32_t>(std::floor(ground.y))});
        if (s && s->owner == player_ && s->type != engine::StructureType::House) selected_structure_ = s->id;
        return;
    }
    selected_structure_ = 0;

    const auto it = std::lower_bound(selection_.begin(), selection_.end(), hit->id);
    const bool already = it != selection_.end() && *it == hit->id;
    if (additive) {
        // Shift+click toggles, like in most RTS games.
        if (already) {
            selection_.erase(it);
        } else {
            selection_.insert(it, hit->id);
        }
    } else {
        selection_.assign(1, hit->id);
    }
}

void PlayerController::box_select(const engine::World& world, const render::RtsCamera& camera,
                                  Rectangle box, bool additive, float alpha) {
    if (!additive) selection_.clear();
    selected_structure_ = 0;
    for (const engine::Unit& u : world.units()) {
        if (u.owner != player_) continue;
        if (const engine::Structure* s = world.find_structure(u.inside); s && s->type != engine::StructureType::House) {
            continue;  // at drill in the headquarters, not up for orders
        }
        if (CheckCollisionPointRec(render::unit_screen_pos(camera, world.map(), u, alpha), box)) {
            selection_.push_back(u.id);
        }
    }
    std::sort(selection_.begin(), selection_.end());
    selection_.erase(std::unique(selection_.begin(), selection_.end()), selection_.end());
}

}  // namespace app
