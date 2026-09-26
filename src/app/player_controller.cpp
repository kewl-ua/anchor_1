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

    if (IsKeyPressed(KEY_ESCAPE)) {
        targeting_ = Targeting::None;
        placing_.reset();
        build_menu_ = false;
    }
    if (selection_.empty()) targeting_ = Targeting::None;
    if (!has_workers(world)) {
        placing_.reset();
        build_menu_ = false;
    }

    // The grid's hotkeys: a key presses whatever its cell holds.
    rebuild_grid(world);
    for (size_t slot = 0; slot < hud::kGridSlots; ++slot) {
        if (IsKeyPressed(static_cast<KeyboardKey>(hud::kGridKeys[slot]))) {
            press_cell(lockstep, world, slot);
            rebuild_grid(world);
        }
    }
    update_placement(world, camera, mouse);

    // Orders can target the minimap too, like in AoE II.
    const std::optional<Vector2> minimap_ground = hud.minimap_to_ground(mouse);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const std::optional<Vector2> target =
            over_hud ? minimap_ground : std::optional<Vector2>(ground_under(world, camera, mouse));
        const bool grid_shown = selected_structure_ != 0 || !selection_.empty();
        if (const std::optional<size_t> button = hud.button_at(mouse); button && grid_shown) {
            press_cell(lockstep, world, *button);
        } else if (placement_ && !over_hud) {
            if (placement_->valid) {
                order_build(lockstep, *placement_);
                if (!shift) {  // shift: lay several foundations
                    placing_.reset();
                    build_menu_ = false;
                }
            }
        } else if (targeting() && target) {
            if (targeting_ == Targeting::AttackMove) order_attack_move(lockstep, renderer, *target);
            if (targeting_ == Targeting::AttackGround) order_attack_ground(lockstep, renderer, *target);
            if (targeting_ == Targeting::Observe) order_to_point(lockstep, renderer, *target, engine::CommandType::Observe);
            if (targeting_ == Targeting::Ability) order_ability(lockstep, world, renderer, aiming_, *target, *target);
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
    if (IsKeyPressed(KEY_F2)) select_army(world);
    rebuild_grid(world);  // what the HUD shows this frame
}

namespace {

// Short names that fit a grid cell; the tooltip has the full one.
const char* unit_label(engine::UnitTypeId type) {
    switch (type) {
        case engine::UnitTypeId::Rifleman: return "Rifleman";
        case engine::UnitTypeId::MachineGunner: return "MG crew";
        case engine::UnitTypeId::Grenadier: return "RPG";
        case engine::UnitTypeId::Tank: return "Tank";
        case engine::UnitTypeId::Ifv: return "IFV";
        case engine::UnitTypeId::Worker: return "Rear troop";
        case engine::UnitTypeId::Truck: return "Truck";
        case engine::UnitTypeId::Scout: return "Scout";
        case engine::UnitTypeId::Count: break;
    }
    return "?";
}

const char* building_label(engine::StructureType type) {
    switch (type) {
        case engine::StructureType::InfantryBarracks: return "Infantry";
        case engine::StructureType::ArmorBarracks: return "Armor";
        case engine::StructureType::ReconBarracks: return "Recon";
        case engine::StructureType::Warehouse: return "Warehouse";
        case engine::StructureType::AmmoDepot: return "Ammo";
        case engine::StructureType::FuelDepot: return "Fuel";
        default: return engine::structure_type(type).name;
    }
}

}  // namespace

std::optional<engine::UnitTypeId> PlayerController::leading_type(const engine::World& world) const {
    std::array<int, engine::kUnitTypeCount> count{};
    for (engine::EntityId id : selection_) {
        if (const engine::Unit* u = world.find_unit(id)) ++count[static_cast<size_t>(u->type)];
    }
    const auto best = std::max_element(count.begin(), count.end());  // ties: the first type
    if (*best == 0) return std::nullopt;
    return static_cast<engine::UnitTypeId>(best - count.begin());
}

// Row 1 (Q W E R T): what this kind of unit or building does. Row 2
// (A S D F G): orders every unit knows. Row 3 (Z X C V B): more of row 1.
void PlayerController::rebuild_grid(const engine::World& world) {
    buttons_ = {};
    cells_ = {};
    auto put = [&](size_t slot, Action action, uint8_t param, const char* label, const char* tooltip,
                   engine::Stock cost = {}) -> hud::CommandButton& {
        cells_[slot] = {action, param};
        buttons_[slot] = {.label = label, .tooltip = tooltip, .cost = cost};
        return buttons_[slot];
    };

    if (const engine::Structure* s = world.find_structure(selected_structure_)) {
        if (s->owner != player_ || !s->built) return;
        const engine::StructureDef& def = engine::structure_type(s->type);
        for (uint8_t i = 0; i < def.roster_size; ++i) {
            const engine::UnitTypeDef& unit = engine::unit_type(def.roster[i]);
            put(i, Action::Hire, static_cast<uint8_t>(def.roster[i]), unit_label(def.roster[i]), unit.name, unit.cost)
                .enabled = s->queue.size() < engine::kMaxQueue;
        }
        return;
    }
    const std::optional<engine::UnitTypeId> lead = leading_type(world);
    if (!lead) return;
    const engine::UnitTypeDef& def = engine::unit_type(*lead);

    if (def.worker && build_menu_) {
        static constexpr size_t kBuildSlots[] = {0, 1, 2, 3, 4, 10, 11, 12, 13};
        static_assert(std::size(kBuildSlots) >= std::size(engine::kBuildable));
        for (size_t i = 0; i < std::size(engine::kBuildable); ++i) {
            const engine::StructureDef& b = engine::structure_type(engine::kBuildable[i]);
            put(kBuildSlots[i], Action::Build, static_cast<uint8_t>(engine::kBuildable[i]),
                building_label(engine::kBuildable[i]), b.name, b.cost)
                .active = placing_ == engine::kBuildable[i];
        }
        put(14, Action::Back, 0, "Back", "Back (Esc)");
        return;
    }

    if (def.worker) {
        put(0, Action::BuildMenu, 0, "Build", "Build: barracks, warehouses, depots");
        engine::Stock retrain{};
        retrain[static_cast<size_t>(engine::Resource::Ammo)] = 20;
        put(1, Action::Retrain, 0, "Retrain", "Retrain as riflemen at the headquarters", retrain);
    }
    if (*lead == engine::UnitTypeId::Truck) put(0, Action::Haul, 0, "Supply run", "Back on the supply run");
    if (def.sector_range.raw > 0) {
        put(0, Action::Observe, 0, "Observe", "Observation post: watch a sector, holding fire").active =
            targeting_ == Targeting::Observe;
    }
    for (uint8_t i = 0; i < def.ability_count; ++i) {
        const engine::AbilityId id = def.abilities[i];
        const engine::AbilityDef& ability = engine::ability_def(id);
        // Ready as soon as one of them is; a switch shows what is loaded.
        float cooldown = 1.0f;
        bool alt_loaded = false;
        for (engine::EntityId unit_id : selection_) {
            const engine::Unit* u = world.find_unit(unit_id);
            if (!u || u->type != *lead) continue;
            alt_loaded = u->ammo == 1;
            const engine::Tick ready = u->ability_ready[i];
            const float left = ready > world.tick() && ability.cooldown > 0
                                   ? static_cast<float>(ready - world.tick()) / static_cast<float>(ability.cooldown)
                                   : 0.0f;
            cooldown = std::min(cooldown, left);
        }
        const char* label = ability.label;
        if (id == engine::AbilityId::SwitchAmmo) label = alt_loaded ? "Load HE" : "Load AP";
        hud::CommandButton& b = put(i, Action::Ability, static_cast<uint8_t>(id), label, ability.name);
        b.cooldown = cooldown;
        b.active = (targeting_ == Targeting::Ability && aiming_ == id) || (id == engine::AbilityId::SwitchAmmo && alt_loaded);
    }

    const bool armed = std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && engine::is_armed(engine::unit_type(u->type));
    });
    if (armed) {
        put(5, Action::AttackMove, 0, "Attack", "Attack-move: go there, fighting on the way").active =
            targeting_ == Targeting::AttackMove;
    }
    put(6, Action::Stop, 0, "Stop", "Stop");
    if (armed) {
        put(9, Action::FireAt, 0, "Fire at", "Fire at a spot, seen or not (a tree line, a house)").active =
            targeting_ == Targeting::AttackGround;
    }
}

void PlayerController::press_cell(net::Lockstep& lockstep, const engine::World& world, size_t slot) {
    if (slot >= hud::kGridSlots) return;
    const Cell cell = cells_[slot];
    const hud::CommandButton& b = buttons_[slot];
    if (cell.action == Action::None || !b.enabled || b.cooldown > 0.0f) return;
    if (!engine::can_afford(world.stock(player_), b.cost)) return;
    switch (cell.action) {
        case Action::AttackMove: targeting_ = Targeting::AttackMove; break;
        case Action::FireAt: targeting_ = Targeting::AttackGround; break;
        case Action::Observe: targeting_ = Targeting::Observe; break;
        case Action::Stop: order_stop(lockstep); break;
        case Action::Retrain: order_retrain(lockstep); break;
        case Action::BuildMenu: build_menu_ = true; break;
        case Action::Back: build_menu_ = false; break;
        case Action::Build:
            placing_ = static_cast<engine::StructureType>(cell.param);
            targeting_ = Targeting::None;
            break;
        case Action::Hire: order_train(lockstep, world, static_cast<engine::UnitTypeId>(cell.param)); break;
        case Action::Haul: {
            engine::Command haul{.type = engine::CommandType::Haul};
            for (engine::EntityId id : selection_) {
                const engine::Unit* u = world.find_unit(id);
                if (u && u->type == engine::UnitTypeId::Truck) haul.units.push_back(id);
            }
            if (!haul.units.empty()) lockstep.submit(std::move(haul));
            break;
        }
        case Action::Ability: {
            const auto id = static_cast<engine::AbilityId>(cell.param);
            if (engine::ability_def(id).target == engine::AbilityTarget::Instant) {
                engine::Command cmd{.type = engine::CommandType::Ability, .ability = cell.param};
                for (engine::EntityId unit_id : selection_) {
                    const engine::Unit* u = world.find_unit(unit_id);
                    if (u && engine::ability_slot(engine::unit_type(u->type), id) >= 0) cmd.units.push_back(unit_id);
                }
                if (!cmd.units.empty()) lockstep.submit(std::move(cmd));
            } else {
                targeting_ = Targeting::Ability;
                aiming_ = id;
            }
            break;
        }
        case Action::None: break;
    }
}

void PlayerController::order_ability(net::Lockstep& lockstep, const engine::World& world,
                                     render::WorldRenderer& renderer, engine::AbilityId ability, Vector2 target,
                                     Vector2 end) {
    engine::Command cmd;
    cmd.type = engine::CommandType::Ability;
    cmd.ability = static_cast<uint8_t>(ability);
    cmd.target = render::to_fixed_vec2(target);
    cmd.target_end = render::to_fixed_vec2(end);
    for (engine::EntityId id : selection_) {
        const engine::Unit* u = world.find_unit(id);
        if (u && engine::ability_slot(engine::unit_type(u->type), ability) >= 0) cmd.units.push_back(id);
    }
    if (cmd.units.empty()) return;
    lockstep.submit(std::move(cmd));
    renderer.add_order_ping(target, true);
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
        case Targeting::Observe: return "Observation sector";
        case Targeting::Ability: return engine::ability_def(aiming_).name;
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
        if (!own && !world.sees(player_, u)) continue;  // can't click what we can't see
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
