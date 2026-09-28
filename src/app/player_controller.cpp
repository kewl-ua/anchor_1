#include "app/player_controller.h"

#include <algorithm>
#include <span>
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
        convert_menu_ = false;
        section_ = 0;
    }
    if (selection_.empty()) targeting_ = Targeting::None;
    if (!has_workers(world)) {
        placing_.reset();
        build_menu_ = false;
        convert_menu_ = false;
        if (targeting_ == Targeting::Convert || targeting_ == Targeting::Gather) targeting_ = Targeting::None;
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

    // A line skill (a trench): press where it starts, release where it ends.
    const bool aiming_line = targeting_ == Targeting::Ability &&
                             engine::ability_def(aiming_).target == engine::AbilityTarget::Line;
    if (!aiming_line) line_start_.reset();
    trench_preview_.clear();
    if (aiming_line && !over_hud) {
        const Vector2 here = ground_under(world, camera, mouse);
        const Vector2 from = line_start_ ? *line_start_ : here;
        auto tile = [](Vector2 g) {
            return engine::TilePos{static_cast<int32_t>(std::floor(g.x)), static_cast<int32_t>(std::floor(g.y))};
        };
        trench_preview_ = engine::trench_line(tile(from), tile(here));
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !hud.button_at(mouse)) line_start_ = here;
        if (line_start_ && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            order_ability(lockstep, world, renderer, aiming_, *line_start_, here);
            line_start_.reset();
            if (!shift) targeting_ = Targeting::None;
        }
    }

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
        } else if (aiming_line) {
            // handled above: a line is dragged
        } else if (targeting() && target) {
            if (targeting_ == Targeting::AttackMove) order_attack_move(lockstep, renderer, *target);
            if (targeting_ == Targeting::AttackGround) order_attack_ground(lockstep, renderer, *target);
            if (targeting_ == Targeting::Observe) order_to_point(lockstep, renderer, *target, engine::CommandType::Observe);
            if (targeting_ == Targeting::Ability) order_ability(lockstep, world, renderer, aiming_, *target, *target);
            if (targeting_ == Targeting::Convert) {
                order_convert(lockstep, world, renderer, mouse, camera);
                convert_menu_ = false;
            }
            if (targeting_ == Targeting::Gather) {
                if (const auto tile = render::resource_on_screen(camera, world, mouse)) {
                    order_gather(lockstep, world, renderer, render::to_vector2(engine::tile_center(*tile)));
                }
            }
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
        } else if (const engine::Structure* building = world.find_structure(selected_structure_);
                   selection_.empty() && building && building->owner == player_ &&
                   engine::structure_type(building->type).roster_size > 0 && (!over_hud || minimap_ground)) {
            // The building's rally point, like in AoE II: the units it hires go there.
            const Vector2 ground = over_hud ? *minimap_ground : ground_under(world, camera, mouse);
            lockstep.submit({.type = engine::CommandType::Rally, .target = render::to_fixed_vec2(ground),
                             .target_unit = building->id});
            renderer.add_order_ping(ground, false);
        } else if (over_hud) {
            if (minimap_ground) order_move(lockstep, renderer, *minimap_ground);
        } else if (const engine::Unit* enemy = unit_at(world, camera, mouse, alpha, false); enemy && !enemy->inside) {
            order_attack(lockstep, enemy->id);
        } else if (const engine::Unit* carrier = has_riders(world) ? unit_at(world, camera, mouse, alpha, true) : nullptr;
                   carrier && engine::unit_type(carrier->type).troop_capacity > 0) {
            // Foot soldiers right-clicked onto our IFV: they mount up.
            order_board(lockstep, world, renderer, *carrier);
        } else if (const engine::Unit* own = has_service_vehicles(world) ? unit_at(world, camera, mouse, alpha, true) : nullptr;
                   own && !own->inside && !std::binary_search(selection_.begin(), selection_.end(), own->id)) {
            // A tanker or an ammunition truck right-clicked onto one of ours: attached to it.
            order_supply(lockstep, world, renderer, *own);
        } else {
            Vector2 ground = ground_under(world, camera, mouse);
            // A click on a roof or a tree's crown means the building or the tree.
            const engine::Structure* structure = render::structure_on_screen(camera, world, mouse);
            const std::optional<engine::TilePos> resource = render::resource_on_screen(camera, world, mouse);
            if (structure) ground = render::to_vector2(structure->center);
            if (structure && has_ammo_trucks(world) && world.can_stock(*structure, player_)) {
                // Ammunition trucks keep the position stocked; the rest go there.
                engine::Command stock{.type = engine::CommandType::Supply, .target_unit = structure->id};
                engine::Command move{.type = engine::CommandType::Move, .target = structure->center};
                for (engine::EntityId id : selection_) {
                    const engine::Unit* u = world.find_unit(id);
                    if (!u) continue;
                    (engine::unit_type(u->type).supplies == engine::Resource::Ammo ? stock : move).units.push_back(id);
                }
                if (!stock.units.empty()) lockstep.submit(std::move(stock));
                if (!move.units.empty()) lockstep.submit(std::move(move));
                renderer.add_order_ping(render::to_vector2(structure->center), false);
            } else if (resource && !structure && (has_workers(world) || has_trucks(world))) {
                // Rear troops go to work; anyone else selected just goes there.
                order_gather(lockstep, world, renderer, render::to_vector2(engine::tile_center(*resource)));
            } else if (structure && structure->owner == player_ && !structure->built &&
                       (has_workers(world) || has_engineers(world))) {
                order_help_build(lockstep, structure->id);
            } else if (structure && structure->owner == player_ && refills_at(world, engine::role_of(*structure))) {
                // Tankers to the fuel depot, ammunition trucks to the ammunition depot: load up.
                engine::Command refill{.type = engine::CommandType::Ability,
                                       .ability = static_cast<uint8_t>(engine::AbilityId::Refill)};
                engine::Command haul{.type = engine::CommandType::Haul, .target_unit = structure->id};
                engine::Command move{.type = engine::CommandType::Move, .target = render::to_fixed_vec2(ground)};
                for (engine::EntityId id : selection_) {
                    const engine::Unit* u = world.find_unit(id);
                    if (!u) continue;
                    if (depot_for_refill(u->type) == engine::role_of(*structure)) {
                        refill.units.push_back(id);
                    } else {
                        (u->type == engine::UnitTypeId::Truck ? haul : move).units.push_back(id);
                    }
                }
                if (!refill.units.empty()) lockstep.submit(std::move(refill));
                if (!haul.units.empty()) lockstep.submit(std::move(haul));
                if (!move.units.empty()) lockstep.submit(std::move(move));
                renderer.add_order_ping(ground, false);
            } else if (structure && structure->owner == player_ && is_supply_point(engine::role_of(*structure)) &&
                       (has_trucks(world) ||
                        (structure->type == engine::StructureType::Station && has_service_vehicles(world)))) {
                // Trucks go back on the supply run (to this depot); anyone else selected just goes there.
                order_haul(lockstep, world, renderer, ground, structure->id);
            } else if (structure && structure->type == engine::StructureType::Hospital && structure->owner == player_ &&
                       structure->built && has_foot_soldiers(world)) {
                order_garrison(lockstep, structure->id);  // the wounded to their beds
            } else if (structure && engine::is_shelter(engine::role_of(*structure))) {
                // Our infantry moves in; a house or dugout the enemy holds gets shelled.
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
    hint_ = "";
    if (!over_hud && !placing_) update_hint(world, camera, mouse, alpha);
}

// Like AoE's cursors: what a right click (or, aiming, a left click) would
// do to whatever is under the cursor, given who is selected.
void PlayerController::update_hint(const engine::World& world, const render::RtsCamera& camera, Vector2 mouse,
                                   float alpha) {
    const engine::Structure* s = render::structure_on_screen(camera, world, mouse);
    if (targeting_ == Targeting::Convert) {
        if (!s) return;
        hint_ = world.can_convert(*s, player_)
                    ? TextFormat("Click: make it a %s", engine::structure_type(converting_).name)
                    : "Not a spacious village building we can take over";
        return;
    }
    const std::optional<engine::TilePos> resource = render::resource_on_screen(camera, world, mouse);
    if (targeting_ == Targeting::Gather) {
        if (resource) {
            hint_ = TextFormat("Click: %s (%d left)",
                               world.map().terrain(*resource) == engine::Terrain::Rock ? "quarry stone" : "cut timber",
                               world.map().resource(*resource));
        }
        return;
    }
    if (targeting() || selection_.empty()) return;
    if (const engine::Unit* enemy = unit_at(world, camera, mouse, alpha, false); enemy && !enemy->inside) {
        hint_ = "RMB: attack";
        return;
    }
    const engine::Unit* own = unit_at(world, camera, mouse, alpha, true);
    if (own && engine::unit_type(own->type).troop_capacity > 0 && has_riders(world)) {
        hint_ = TextFormat("RMB: mount up (%d / %d aboard)", static_cast<int>(own->passengers.size()),
                           engine::unit_type(own->type).troop_capacity);
        return;
    }
    if (own && !own->inside && has_service_vehicles(world) &&
        !std::binary_search(selection_.begin(), selection_.end(), own->id)) {
        hint_ = TextFormat("RMB: attach to this %s: follow it, keep it supplied", engine::unit_type(own->type).name);
        return;
    }
    if (s && s->owner == player_ && !s->built && (has_workers(world) || has_engineers(world))) {
        hint_ = "RMB: help build";
        return;
    }
    if (s && has_ammo_trucks(world) && world.can_stock(*s, player_)) {
        hint_ = TextFormat("RMB: keep this %s stocked with ammunition (%d / %d)", engine::structure_type(s->type).name,
                           s->cache, engine::structure_type(s->type).cache_capacity);
        return;
    }
    const engine::StructureType role = s ? engine::role_of(*s) : engine::StructureType::Count;
    if (s && s->owner == player_ && refills_at(world, role)) {
        hint_ = "RMB: load up here from the stock";
        return;
    }
    if (s && s->type == engine::StructureType::Station && s->owner == player_ && !has_trucks(world) &&
        has_service_vehicles(world)) {
        hint_ = "RMB: haul what they carry from the station to the depots (the rail run)";
        return;
    }
    if (s && s->owner == player_ && is_supply_point(role) && has_trucks(world)) {
        const std::optional<engine::Resource> cargo = engine::depot_cargo(role);
        hint_ = cargo ? TextFormat("RMB: haul %s from the station to this %s", engine::resource_name(*cargo),
                                   engine::structure_type(role).name)
                      : "RMB: back on the supply run";
        return;
    }
    if (s && s->type == engine::StructureType::Hospital && s->owner == player_ && s->built && has_foot_soldiers(world)) {
        hint_ = TextFormat("RMB: into the hospital beds (%d / %d): healed, out when well",
                           static_cast<int>(s->garrison.size()), engine::structure_type(s->type).capacity);
        return;
    }
    if (s && s->type == engine::StructureType::Workshop && s->owner == player_ && s->built && has_vehicles(world)) {
        hint_ = "RMB: to the workshop: parked by it, vehicles get repaired (a material a second each)";
        return;
    }
    if (s && engine::is_shelter(role)) {
        hint_ = s->owner == engine::kNoOwner || s->owner == player_ ? "RMB: go in" : "RMB: shell it";
        return;
    }
    if (resource && !s && has_trucks(world) && !has_workers(world)) {
        hint_ = "RMB: park by it; rear troops hand it their loads, it takes them in 40 at a time";
        return;
    }
    if (resource && !s && has_workers(world)) {
        hint_ = TextFormat("RMB: %s (%d left), carry it in",
                           world.map().terrain(*resource) == engine::Terrain::Rock ? "quarry stone" : "cut timber",
                           world.map().resource(*resource));
    }
}

namespace {

// Short names that fit a grid cell; the tooltip has the full one.
const char* unit_label(engine::UnitTypeId type) {
    switch (type) {
        case engine::UnitTypeId::Rifleman: return "Rifleman";
        case engine::UnitTypeId::MachineGunner: return "MG crew";
        case engine::UnitTypeId::Grenadier: return "RPG";
        case engine::UnitTypeId::Tank: return "T-72B3";
        case engine::UnitTypeId::T64BV: return "T-64BV";
        case engine::UnitTypeId::T64BM: return "Bulat";
        case engine::UnitTypeId::Leopard1A5: return "Leo 1A5";
        case engine::UnitTypeId::Leopard2A6: return "Leo 2A6";
        case engine::UnitTypeId::M1A1: return "Abrams";
        case engine::UnitTypeId::Type10: return "Type 10";
        case engine::UnitTypeId::K2: return "K2";
        case engine::UnitTypeId::Merkava4: return "Merkava";
        case engine::UnitTypeId::T62M: return "T-62M";
        case engine::UnitTypeId::T80BVM: return "T-80BVM";
        case engine::UnitTypeId::T90M: return "T-90M";
        case engine::UnitTypeId::Type99A: return "Type 99A";
        case engine::UnitTypeId::Karrar: return "Karrar";
        case engine::UnitTypeId::Ifv: return "IFV";
        case engine::UnitTypeId::Worker: return "Rear troop";
        case engine::UnitTypeId::Truck: return "Truck";
        case engine::UnitTypeId::Scout: return "Scout";
        case engine::UnitTypeId::Assault: return "Assault";
        case engine::UnitTypeId::FuelTanker: return "Tanker";
        case engine::UnitTypeId::AmmoTruck: return "Ammo truck";
        case engine::UnitTypeId::Mortar: return "Mortar";
        case engine::UnitTypeId::Howitzer: return "Howitzer";
        case engine::UnitTypeId::Ags: return "AGS";
        case engine::UnitTypeId::Mlrs: return "MLRS";
        case engine::UnitTypeId::Sapper: return "Sapper";
        case engine::UnitTypeId::Spg: return "SPG";
        case engine::UnitTypeId::Signaler: return "Signaller";
        case engine::UnitTypeId::FieldHq: return "Cmd vehicle";
        case engine::UnitTypeId::DfStation: return "DF station";
        case engine::UnitTypeId::Su25: return "Su-25";
        case engine::UnitTypeId::Manpads: return "MANPADS";
        case engine::UnitTypeId::Shilka: return "Shilka";
        case engine::UnitTypeId::AirRadar: return "AD radar";
        case engine::UnitTypeId::Count: break;
    }
    return "?";
}

const char* building_label(engine::StructureType type) {
    switch (type) {
        case engine::StructureType::InfantryBarracks: return "Infantry";
        case engine::StructureType::ArmorBarracks: return "Armor";
        case engine::StructureType::ReconBarracks: return "Recon";
        case engine::StructureType::ArtilleryBarracks: return "Artillery";
        case engine::StructureType::EngineerBarracks: return "Engineers";
        case engine::StructureType::SignalsBarracks: return "Signals";
        case engine::StructureType::AirDefenseBarracks: return "Air defence";
        case engine::StructureType::Airfield: return "Airfield";
        case engine::StructureType::Warehouse: return "Warehouse";
        case engine::StructureType::AmmoDepot: return "Ammo";
        case engine::StructureType::FuelDepot: return "Fuel";
        case engine::StructureType::Quarters: return "Quarters";
        case engine::StructureType::Workshop: return "Workshop";
        case engine::StructureType::Hospital: return "Hospital";
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
        if (s->type == engine::StructureType::Foxhole) {
            put(0, Action::Upgrade, 0, "Dugout", "Dig it out into a dugout: the men standing in it do the work",
                engine::kDugoutCost)
                .enabled = !s->upgrading;
        }
        if ((engine::is_shelter(engine::role_of(*s)) || s->type == engine::StructureType::Hospital) &&
            !s->garrison.empty()) {
            put(0, Action::Unload, 0, "Leave", "Everyone out");
        }
        // What it hires along the top row, what it researches along the bottom
        // row, then the middle one. What doesn't fit is nested in sections:
        // the tanks of the axis (the armor barracks) behind a "Tanks" button,
        // on a page of their own with a way back.
        const std::span<const engine::UnitTypeId> roster = engine::roster_of(s->type, engine::axis_of(player_));
        const auto tanks = std::count_if(roster.begin(), roster.end(), [](engine::UnitTypeId t) { return engine::unit_type(t).tank; });
        const bool tank_section = tanks > 1;
        auto hire = [&](size_t slot, engine::UnitTypeId type) {
            const engine::UnitTypeDef& unit = engine::unit_type(type);
            put(slot, Action::Hire, static_cast<uint8_t>(type), unit_label(type), unit.name, unit.cost).enabled =
                s->queue.size() < engine::kMaxQueue;
        };
        if (tank_section && section_ == s->id) {
            size_t slot = 0;
            for (const engine::UnitTypeId type : roster) {
                if (engine::unit_type(type).tank && slot < 14) hire(slot++, type);
            }
            put(14, Action::Back, 0, "Back", "Back to the barracks");
            return;
        }
        static constexpr size_t kResearchSlots[] = {10, 11, 12, 13, 14, 5, 6, 7, 8, 9};
        size_t research = 0;
        for (size_t i = 0; i < engine::kUpgradeCount && research < std::size(kResearchSlots); ++i) {
            const auto id = static_cast<engine::UpgradeId>(i);
            const engine::UpgradeDef& up = engine::upgrade_def(id);
            if (up.building != engine::role_of(*s)) continue;
            const bool done = world.has_upgrade(player_, id);
            // Of a line of upgrades (Kontakt-1, -5, Relikt), the next one to research, or the last one done.
            if (up.needs != engine::UpgradeId::Count && !world.has_upgrade(player_, up.needs)) continue;
            bool superseded = false;
            for (size_t j = 0; j < engine::kUpgradeCount; ++j) {
                superseded = superseded || (done && engine::upgrade_def(static_cast<engine::UpgradeId>(j)).needs == id);
            }
            if (superseded) continue;
            hud::CommandButton& b = put(kResearchSlots[research++], Action::Research, static_cast<uint8_t>(i), up.label,
                                        TextFormat("%s: %s%s", up.name, up.description, done ? " (done)" : ""),
                                        done ? engine::Stock{} : up.cost);
            b.enabled = !done && s->research == engine::UpgradeId::Count;
            b.active = done || s->research == id;
            if (s->research == id) {
                b.cooldown = 1.0f - static_cast<float>(s->research_progress) / static_cast<float>(up.time);
            }
        }
        size_t slot = 0;
        if (tank_section) put(slot++, Action::Section, 0, "Tanks >", "The axis's tanks: pick one to hire");
        for (const engine::UnitTypeId type : roster) {
            if ((!tank_section || !engine::unit_type(type).tank) && slot < 5) hire(slot++, type);
        }
        return;
    }
    const std::optional<engine::UnitTypeId> lead = leading_type(world);
    if (!lead) return;
    const engine::UnitTypeDef& def = engine::unit_type(*lead);

    if (def.worker && build_menu_) {
        // Barracks along the top and middle rows, depots along the bottom one.
        static constexpr size_t kBuildSlots[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 8, 9};
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

    if (def.worker && convert_menu_) {
        // What to make of a spacious village building: a depot near the front.
        struct Depot {
            engine::StructureType type;
            const char* tooltip;
        };
        static constexpr Depot kDepots[] = {
            {engine::StructureType::Warehouse, "Warehouse in a village building: food from trucks, materials"},
            {engine::StructureType::AmmoDepot, "Ammo depot in a village building: ammunition trucks load up nearer the front"},
            {engine::StructureType::FuelDepot, "Fuel depot in a village building: tankers fill up nearer the front"},
        };
        for (size_t i = 0; i < std::size(kDepots); ++i) {
            put(i, Action::Convert, static_cast<uint8_t>(kDepots[i].type), building_label(kDepots[i].type),
                kDepots[i].tooltip, engine::kConversionCost)
                .active = targeting_ == Targeting::Convert && converting_ == kDepots[i].type;
        }
        put(14, Action::Back, 0, "Back", "Back (Esc)");
        return;
    }

    if (def.worker) {
        put(0, Action::BuildMenu, 0, "Build", "Build: barracks, warehouses, depots");
        put(3, Action::Gather, 0, "Gather",
            "Cut timber or quarry stone: click a forest or a rock (RMB on it does the same). They carry it in themselves")
            .active = targeting_ == Targeting::Gather;
        put(2, Action::ConvertMenu, 0, "Take over",
            "Turn a spacious village building (a barn) into a depot: nearer the front, less driving for the supply");
        engine::Stock retrain{};
        retrain[static_cast<size_t>(engine::Resource::Ammo)] = 20;
        put(1, Action::Retrain, 0, "Retrain", "Retrain as riflemen at the headquarters", retrain);
    }
    if (def.supplies != engine::Resource::Count) {
        // Tankers and ammunition trucks can do the rail run for their own freight too.
        bool all = true;
        for (engine::EntityId id : selection_) {
            const engine::Unit* u = world.find_unit(id);
            if (u && u->type == *lead) all = all && u->order == engine::Order::Haul;
        }
        put(1, Action::Haul, engine::kHaulKeep, "Rail run",
            TextFormat("Rail run: %s from the station to the nearest %s, again and again (RMB on the station does the same)",
                       engine::resource_name(def.supplies), engine::structure_type(*engine::depot_for(def.supplies)).name))
            .active = all;
    }
    if (*lead == engine::UnitTypeId::Truck) {
        // What the trucks haul: whatever piles up at the station, or one kind of freight.
        struct Choice {
            uint8_t code;
            const char* label;
            const char* tooltip;
        };
        static constexpr Choice kChoices[] = {
            {engine::kHaulAuto, "Auto", "Haul whatever piles up most at the station, to the nearest depot for it"},
            {engine::haul_code(engine::Resource::Food), "Food",
             "Haul food: station -> nearest warehouse (RMB on a warehouse: to that one)"},
            {engine::haul_code(engine::Resource::Ammo), "Ammo",
             "Haul ammunition: station -> nearest ammo depot (RMB on one: to that one)"},
            {engine::haul_code(engine::Resource::Fuel), "Fuel",
             "Haul fuel: station -> nearest fuel depot (RMB on one: to that one)"},
        };
        for (size_t i = 0; i < std::size(kChoices); ++i) {
            // Lit when every selected truck hauls it.
            bool all = true;
            for (engine::EntityId id : selection_) {
                const engine::Unit* u = world.find_unit(id);
                if (!u || u->type != engine::UnitTypeId::Truck) continue;
                const uint8_t code = u->haul_cargo == engine::Resource::Count ? engine::kHaulAuto
                                                                              : engine::haul_code(u->haul_cargo);
                all = all && u->order == engine::Order::Haul && code == kChoices[i].code;
            }
            put(i, Action::Haul, kChoices[i].code, kChoices[i].label, kChoices[i].tooltip).active = all;
        }
    }
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
        bool deployed = false;
        bool on_air = false;  // any radio of the selection
        bool heard = false;   // any of them can be heard over the radio (on the air, or by a relay)
        bool missiles = false;  // any of them has a guided missile left
        for (engine::EntityId unit_id : selection_) {
            const engine::Unit* u = world.find_unit(unit_id);
            if (u && engine::unit_type(u->type).emitter && !u->silent) on_air = true;
            if (!u || u->type != *lead) continue;
            heard = heard || world.in_touch(*u);
            missiles = missiles || u->missiles > 0;
            alt_loaded = u->round_type == 1;
            deployed = u->deployed;
            const engine::Tick ready = u->ability_ready[i];
            const float left = ready > world.tick() && ability.cooldown > 0
                                   ? static_cast<float>(ready - world.tick()) / static_cast<float>(ability.cooldown)
                                   : 0.0f;
            cooldown = std::min(cooldown, left);
        }
        const char* label = ability.label;
        const bool locked = ability.needs != engine::UpgradeId::Count && !world.has_upgrade(player_, ability.needs);
        if (id == engine::AbilityId::SwitchAmmo) label = alt_loaded ? "Load HE" : "Load AP";
        if (id == engine::AbilityId::Deploy) label = deployed ? "Pack up" : "Deploy";
        if (id == engine::AbilityId::DigGunPit) label = *lead == engine::UnitTypeId::Mortar ? "Position" : "Capunier";
        if (id == engine::AbilityId::RadioSilence) label = on_air ? "Radio off" : "Radio on";
        // Skills fill the top row, then the bottom one; the middle row is for orders.
        const size_t slot = i < hud::kGridColumns ? i : 2 * hud::kGridColumns + (i - hud::kGridColumns);
        hud::CommandButton& b = put(slot, Action::Ability, static_cast<uint8_t>(id), label, ability.name);
        b.cooldown = cooldown;
        b.enabled = !locked && (id != engine::AbilityId::CallSupply || heard) && (id != engine::AbilityId::Atgm || missiles);
        b.active = (targeting_ == Targeting::Ability && aiming_ == id) || (id == engine::AbilityId::SwitchAmmo && alt_loaded) ||
                   (id == engine::AbilityId::RadioSilence && !on_air);
    }

    if (def.weapon.indirect) {
        // The artillery's shells, on the bottom row: HE as standard, the rest once researched.
        struct Kind {
            const char* label;
            const char* tooltip;
        };
        static constexpr Kind kShells[] = {
            {"HE", "High-explosive fragmentation shells (standard)"},
            {"Cluster", "Cluster: bomblets over a 4x4 area (research in the artillery barracks)"},
            {"Incend.", "Incendiary: the ground burns 15 s, men and buildings in it too (research in the artillery barracks)"},
            {"WP", "White phosphorus: a smoke screen 20 s, and it burns (research in the artillery barracks)"},
        };
        static_assert(std::size(kShells) == engine::kShellCount);
        for (size_t k = 0; k < engine::kShellCount; ++k) {
            const auto shell = static_cast<engine::Shell>(k);
            bool all = true;
            for (engine::EntityId id : selection_) {
                const engine::Unit* u = world.find_unit(id);
                if (u && u->type == *lead) all = all && u->shell == shell;
            }
            hud::CommandButton& b = put(2 * hud::kGridColumns + k, Action::Shell, static_cast<uint8_t>(k),
                                        kShells[k].label, kShells[k].tooltip);
            b.enabled = shell == engine::Shell::He || world.has_upgrade(player_, engine::shell_upgrade(shell));
            b.active = all;
        }
    }

    const bool armed = std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && engine::is_armed(engine::unit_type(u->type));
    });
    if (def.aircraft) {
        // Aircraft fly missions, nothing else: one mission a sortie.
        put(9, Action::FireAt, 0, "Mission",
            "Mission: a rocket run at a spot (right-click an enemy we see to strike him). One mission a sortie")
            .active = targeting_ == Targeting::AttackGround;
        return;
    }
    if (armed) {
        put(5, Action::AttackMove, 0, "Attack", "Attack-move: go there, fighting on the way").active =
            targeting_ == Targeting::AttackMove;
    }
    put(6, Action::Stop, 0, "Stop", "Stop");
    if (def.troop_capacity > 0) {
        int aboard = 0;
        for (engine::EntityId id : selection_) {
            if (const engine::Unit* u = world.find_unit(id)) aboard += static_cast<int>(u->passengers.size());
        }
        put(7, Action::Dismount, 0, "Dismount",
            "Dismount the squad: the IFV stops, the men get out at the back (RMB with infantry on an IFV: mount up)")
            .enabled = aboard > 0;
    }
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
        case Action::ConvertMenu: convert_menu_ = true; break;
        case Action::Section: section_ = selected_structure_; break;
        case Action::Gather: targeting_ = Targeting::Gather; break;
        case Action::Back:
            build_menu_ = false;
            convert_menu_ = false;
            section_ = 0;
            if (targeting_ == Targeting::Convert) targeting_ = Targeting::None;
            break;
        case Action::Convert:
            targeting_ = Targeting::Convert;
            converting_ = static_cast<engine::StructureType>(cell.param);
            break;
        case Action::Build:
            placing_ = static_cast<engine::StructureType>(cell.param);
            targeting_ = Targeting::None;
            break;
        case Action::Hire: order_train(lockstep, world, static_cast<engine::UnitTypeId>(cell.param)); break;
        case Action::Research: {
            engine::Command cmd{.type = engine::CommandType::Research, .target_unit = selected_structure_,
                                .upgrade = cell.param};
            lockstep.submit(std::move(cmd));
            break;
        }
        case Action::Upgrade:
        case Action::Unload: {
            engine::Command cmd{.type = cell.action == Action::Upgrade ? engine::CommandType::Upgrade
                                                                      : engine::CommandType::Unload,
                                .target_unit = selected_structure_};
            lockstep.submit(std::move(cmd));
            break;
        }
        case Action::Shell: {
            engine::Command load{.type = engine::CommandType::LoadShell, .ability = cell.param};
            for (engine::EntityId id : selection_) {
                const engine::Unit* u = world.find_unit(id);
                if (u && engine::unit_type(u->type).weapon.indirect) load.units.push_back(id);
            }
            if (!load.units.empty()) lockstep.submit(std::move(load));
            break;
        }
        case Action::Dismount: {
            engine::Command out{.type = engine::CommandType::Unload};
            for (engine::EntityId id : selection_) {
                const engine::Unit* u = world.find_unit(id);
                if (u && !u->passengers.empty()) out.units.push_back(id);
            }
            if (!out.units.empty()) lockstep.submit(std::move(out));
            break;
        }
        case Action::Haul: {
            engine::Command haul{.type = engine::CommandType::Haul, .cargo = cell.param};
            for (engine::EntityId id : selection_) {
                const engine::Unit* u = world.find_unit(id);
                if (u && (u->type == engine::UnitTypeId::Truck ||
                          engine::unit_type(u->type).supplies != engine::Resource::Count)) {
                    haul.units.push_back(id);
                }
            }
            if (!haul.units.empty()) lockstep.submit(std::move(haul));
            break;
        }
        case Action::Ability: {
            const auto id = static_cast<engine::AbilityId>(cell.param);
            if (engine::ability_def(id).target == engine::AbilityTarget::Instant) {
                engine::Command cmd{.type = engine::CommandType::Ability, .ability = cell.param};
                // The radio switch is a toggle: with any radio of the
                // selection on the air, the ones on it go quiet; otherwise
                // they all come back on.
                bool on_air = false;
                for (engine::EntityId unit_id : selection_) {
                    const engine::Unit* u = world.find_unit(unit_id);
                    if (u && engine::unit_type(u->type).emitter && !u->silent) on_air = true;
                }
                for (engine::EntityId unit_id : selection_) {
                    const engine::Unit* u = world.find_unit(unit_id);
                    if (!u || engine::ability_slot(engine::unit_type(u->type), id) < 0) continue;
                    if (id == engine::AbilityId::RadioSilence && on_air && u->silent) continue;
                    cmd.units.push_back(unit_id);
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

void PlayerController::order_convert(net::Lockstep& lockstep, const engine::World& world,
                                     render::WorldRenderer& renderer, Vector2 mouse, const render::RtsCamera& camera) {
    const engine::Structure* s = render::structure_on_screen(camera, world, mouse);
    if (!s || !world.can_convert(*s, player_)) return;
    engine::Command cmd{.type = engine::CommandType::Build, .target_unit = s->id,
                        .structure_type = static_cast<uint8_t>(converting_)};
    for (engine::EntityId id : selection_) {
        const engine::Unit* u = world.find_unit(id);
        if (u && engine::unit_type(u->type).worker) cmd.units.push_back(id);
    }
    if (cmd.units.empty()) return;
    lockstep.submit(std::move(cmd));
    renderer.add_order_ping(render::to_vector2(s->center), false);
}

bool PlayerController::select_next_idle(const engine::World& world) {
    std::vector<engine::EntityId> idle;
    for (const engine::Unit& u : world.units()) {
        if (u.owner == player_ && engine::idle_hand(u)) idle.push_back(u.id);  // in id order
    }
    if (idle.empty()) return false;
    const auto next = std::upper_bound(idle.begin(), idle.end(), last_idle_);
    last_idle_ = next == idle.end() ? idle.front() : *next;
    select_units({last_idle_});
    targeting_ = Targeting::None;
    return true;
}

bool PlayerController::has_trucks(const engine::World& world) const {
    return std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && u->type == engine::UnitTypeId::Truck;
    });
}

std::optional<engine::StructureType> PlayerController::depot_for_refill(engine::UnitTypeId type) {
    switch (engine::unit_type(type).supplies) {
        case engine::Resource::Fuel: return engine::StructureType::FuelDepot;
        case engine::Resource::Ammo: return engine::StructureType::AmmoDepot;
        default: return std::nullopt;
    }
}

bool PlayerController::refills_at(const engine::World& world, engine::StructureType depot) const {
    return std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && depot_for_refill(u->type) == depot;
    });
}

bool PlayerController::has_ammo_trucks(const engine::World& world) const {
    return std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && engine::unit_type(u->type).supplies == engine::Resource::Ammo;
    });
}

bool PlayerController::has_service_vehicles(const engine::World& world) const {
    return std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && engine::unit_type(u->type).supplies != engine::Resource::Count;
    });
}

void PlayerController::order_supply(net::Lockstep& lockstep, const engine::World& world,
                                    render::WorldRenderer& renderer, const engine::Unit& unit) {
    engine::Command supply{.type = engine::CommandType::Supply, .target_unit = unit.id};
    engine::Command move{.type = engine::CommandType::Move, .target = unit.pos};
    for (engine::EntityId id : selection_) {
        const engine::Unit* u = world.find_unit(id);
        if (!u) continue;
        (engine::unit_type(u->type).supplies != engine::Resource::Count ? supply : move).units.push_back(id);
    }
    if (!supply.units.empty()) lockstep.submit(std::move(supply));
    if (!move.units.empty()) lockstep.submit(std::move(move));
    renderer.add_order_ping(render::to_vector2(unit.pos), false);
}

bool PlayerController::has_engineers(const engine::World& world) const {
    return std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && engine::unit_type(u->type).engineer;
    });
}

bool PlayerController::is_supply_point(engine::StructureType type) {
    return type == engine::StructureType::Station || type == engine::StructureType::Warehouse ||
           type == engine::StructureType::AmmoDepot || type == engine::StructureType::FuelDepot;
}

void PlayerController::order_haul(net::Lockstep& lockstep, const engine::World& world,
                                  render::WorldRenderer& renderer, Vector2 ground, engine::EntityId structure) {
    engine::Command haul;
    haul.type = engine::CommandType::Haul;
    haul.target_unit = structure;  // a depot assigns them to it; the station keeps what they haul
    engine::Command move;
    move.type = engine::CommandType::Move;
    move.target = render::to_fixed_vec2(ground);
    for (engine::EntityId id : selection_) {
        const engine::Unit* u = world.find_unit(id);
        if (!u) continue;
        const bool hauls = u->type == engine::UnitTypeId::Truck || engine::unit_type(u->type).supplies != engine::Resource::Count;
        (hauls ? haul : move).units.push_back(id);
    }
    if (!haul.units.empty()) lockstep.submit(std::move(haul));
    if (!move.units.empty()) lockstep.submit(std::move(move));
    renderer.add_order_ping(ground, false);
}

bool PlayerController::has_foot_soldiers(const engine::World& world) const {
    return std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && !engine::unit_type(u->type).vehicle;
    });
}

bool PlayerController::has_vehicles(const engine::World& world) const {
    return std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && engine::unit_type(u->type).vehicle && !engine::unit_type(u->type).aircraft;
    });
}

bool PlayerController::has_riders(const engine::World& world) const {
    return std::any_of(selection_.begin(), selection_.end(), [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return u && engine::can_ride(engine::unit_type(u->type));
    });
}

void PlayerController::order_board(net::Lockstep& lockstep, const engine::World& world,
                                   render::WorldRenderer& renderer, const engine::Unit& carrier) {
    engine::Command board{.type = engine::CommandType::Garrison, .target_unit = carrier.id};
    for (engine::EntityId id : selection_) {
        const engine::Unit* u = world.find_unit(id);
        if (u && engine::can_ride(engine::unit_type(u->type))) board.units.push_back(id);
    }
    lockstep.submit(std::move(board));
    renderer.add_order_ping(render::to_vector2(carrier.pos), false);
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
    engine::Command collect{.type = engine::CommandType::Collect, .target = gather.target};
    engine::Command move;
    move.type = engine::CommandType::Move;
    move.target = gather.target;
    for (engine::EntityId id : selection_) {
        const engine::Unit* u = world.find_unit(id);
        if (!u) continue;
        if (engine::unit_type(u->type).worker) {
            gather.units.push_back(id);
        } else if (u->type == engine::UnitTypeId::Truck) {
            collect.units.push_back(id);  // parks by the wood and takes their loads in
        } else {
            move.units.push_back(id);
        }
    }
    if (!gather.units.empty()) lockstep.submit(std::move(gather));
    if (!collect.units.empty()) lockstep.submit(std::move(collect));
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
    if (!s || s->owner != player_ || !engine::can_train(s->type, type, engine::axis_of(player_))) return;
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
        case Targeting::Convert: return "Click a spacious village building (a barn) to take it over";
        case Targeting::Gather: return "Click a forest or a rock";
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
    std::erase_if(selection_, [&](engine::EntityId id) {
        const engine::Unit* u = world.find_unit(id);
        return !u || world.find_unit(u->inside) != nullptr;  // the men aboard an IFV go with it
    });
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
        const engine::Structure* s = render::structure_on_screen(camera, world, mouse);
        if (s && s->owner == player_) selected_structure_ = s->id;
        return;
    }
    selected_structure_ = 0;

    // A double click, or Ctrl+click: everyone of this type on screen (Shift: added).
    const double now = GetTime();
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    const bool double_click = hit->id == last_click_ && now - last_click_time_ < 0.35;
    last_click_ = hit->id;
    last_click_time_ = now;
    if (double_click || ctrl) return select_type_on_screen(world, camera, hit->type, additive, alpha);

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

void PlayerController::select_type_on_screen(const engine::World& world, const render::RtsCamera& camera,
                                             engine::UnitTypeId type, bool additive, float alpha) {
    if (!additive) selection_.clear();
    selected_structure_ = 0;
    const Rectangle screen{0, 0, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
    for (const engine::Unit& u : world.units()) {
        if (u.owner != player_ || u.type != type || u.inside) continue;
        if (CheckCollisionPointRec(render::unit_screen_pos(camera, world.map(), u, alpha), screen)) selection_.push_back(u.id);
    }
    std::sort(selection_.begin(), selection_.end());
    selection_.erase(std::unique(selection_.begin(), selection_.end()), selection_.end());
}

bool PlayerController::update_groups(const engine::World& world) {
    static constexpr KeyboardKey kDigits[] = {KEY_ZERO, KEY_ONE, KEY_TWO,   KEY_THREE, KEY_FOUR,
                                              KEY_FIVE, KEY_SIX, KEY_SEVEN, KEY_EIGHT, KEY_NINE};
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    for (int digit = 0; digit < 10; ++digit) {
        if (!IsKeyPressed(kDigits[digit])) continue;
        std::vector<engine::EntityId>& group = groups_[static_cast<size_t>(digit)];
        // The dead drop out of their groups.
        std::erase_if(group, [&](engine::EntityId id) { return world.find_unit(id) == nullptr; });
        if (ctrl) {
            group = selection_;
            return false;
        }
        if (shift) {
            group.insert(group.end(), selection_.begin(), selection_.end());
            std::sort(group.begin(), group.end());
            group.erase(std::unique(group.begin(), group.end()), group.end());
        }
        if (group.empty()) return false;
        const double now = GetTime();
        const bool again = digit == last_group_ && now - last_group_time_ < 0.4;
        last_group_ = digit;
        last_group_time_ = now;
        select_units(group);
        targeting_ = Targeting::None;
        return again;
    }
    return false;
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
