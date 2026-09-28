#include "engine/world.h"

#include <algorithm>
#include <utility>

#include "engine/heights.h"

namespace engine {

namespace {

constexpr Fixed kFormationGap = Fixed::from_ratio(3, 10);
// How far off-center a direct-hit projectile may land and still hit.
constexpr Fixed kHitTolerance = Fixed::from_ratio(1, 10);
// Weight multiplier for moving units: parked units make way for moving ones.
constexpr int32_t kMovingWeight = 2;
// Closer than this to the destination with nothing in the way, a unit just
// walks straight instead of following the route.
constexpr Fixed kDirectRange = Fixed::from_int(8);
// How many route tiles ahead a unit looks for a straight shortcut.
constexpr int kLookAhead = 4;
// Old routes pile up in the cache; sweep them once it grows past this.
constexpr size_t kFieldCacheSweep = 256;

// --- Line of fire ---
// A shot flies in a straight line from the muzzle to the aim point; the first
// hill, house, tree or body in its way stops it (heights in heights.h). From
// high enough ground it passes over them.
// The first stretch in front of the muzzle is ignored (the shooter's own cover).
constexpr Fixed kMuzzleClearance = Fixed::from_ratio(1, 2);
// Chance per quarter tile of forest that the shot hits a trunk or branch:
// ~22% per tile for bullets, ~28% for shells and rockets.
constexpr int32_t kFoliagePercentBullet = 6;
constexpr int32_t kFoliagePercentShell = 8;
constexpr Tick kImpactHistory = 3 * kTicksPerSecond;
// How close to a house's walls a soldier must be to get in.
constexpr Fixed kEnterDistance = Fixed::from_int(1);

const UnitTypeDef& def_of(const Unit& u) { return unit_type(u.type); }
MoveClass class_of(const Unit& u) { return move_class(def_of(u)); }

Fixed top_height(const Unit& u) { return def_of(u).vehicle ? kVehicleTop : kInfantryTop; }
Fixed muzzle_height(const Unit& u) { return def_of(u).vehicle ? kVehicleMuzzle : kInfantryMuzzle; }
Fixed center_height(const Unit& u) { return def_of(u).vehicle ? kVehicleCenter : kInfantryCenter; }

// A shot from `from` into a vehicle's side or rear: more than 60 degrees off
// the way its hull points.
bool from_flank(const Unit& victim, FixedVec2 from) {
    const FixedVec2 to_shot = from - victim.pos;
    const Fixed dot = victim.hull.x * to_shot.x + victim.hull.y * to_shot.y;
    return dot * 2 < victim.hull.length() * to_shot.length();
}

// Units of `owner` referenced by the command, deduplicated, in id order.
// Commands arrive from the network, so never trust ids or ownership.
template <typename FindFn>
std::vector<Unit*> collect_owned(const Command& cmd, FindFn find) {
    std::vector<Unit*> group;
    group.reserve(cmd.units.size());
    for (EntityId id : cmd.units) {
        Unit* u = find(id);
        if (u && u->owner == cmd.player) group.push_back(u);
    }
    std::sort(group.begin(), group.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
    group.erase(std::unique(group.begin(), group.end()), group.end());
    return group;
}

}  // namespace

World::World(uint64_t seed, TileMap map) : map_(std::move(map)), rng_(seed) {
    build_structures();
    init_resources();
    // Hills don't move: the ground's corner heights are worked out once.
    corner_heights_.resize(static_cast<size_t>((map_.width() + 1) * (map_.height() + 1)));
    for (int32_t cy = 0; cy <= map_.height(); ++cy) {
        for (int32_t cx = 0; cx <= map_.width(); ++cx) {
            corner_heights_[static_cast<size_t>(cy * (map_.width() + 1) + cx)] = map_.corner_height(cx, cy);
        }
    }
}

EntityId World::spawn_unit(PlayerId owner, UnitTypeId type, FixedVec2 pos) {
    Unit u;
    u.id = next_id_++;
    u.owner = owner;
    u.type = type;
    u.hp = unit_type(type).max_hp;
    u.pos = clamp_to_map(pos, unit_type(type).radius);
    if (!can_stand(u, u.pos)) {
        if (auto free = nearest_passable(map_, tile_of(u.pos), class_of(u))) u.pos = tile_center(*free);
    }
    u.prev_pos = u.pos;
    // It stands facing the middle of the map, where the fighting is.
    const FixedVec2 middle{Fixed::from_int(map_.width()) / 2, Fixed::from_int(map_.height()) / 2};
    if (middle != u.pos) u.hull = middle - u.pos;
    // Fresh from the barracks: tanks full, racks full, cargo aboard.
    const UnitTypeDef& def = unit_type(type);
    u.fuel = def.fuel_capacity;
    u.rounds = rack(u);
    u.missiles = def.missile_capacity;
    if (def.supplies != Resource::Count) {
        u.carrying = def.cargo_capacity;
        u.carrying_type = def.supplies;
    }
    units_.push_back(u);  // ids only grow, so the vector stays sorted
    return u.id;
}

Tick World::reload_ticks(const Unit& u, const WeaponDef& weapon) const {
    if (is_armor(def_of(u)) && has_upgrade(u.owner, UpgradeId::FastReload)) {
        return weapon.reload * kFastReloadPercent / 100;
    }
    return weapon.reload;
}

// The artillery loads a kind of shell for its next shots: HE, or one
// researched; the one in the breech comes out first (a reload).
void World::apply_load_shell(const Command& cmd) {
    if (cmd.ability >= kShellCount) return;
    const auto shell = static_cast<Shell>(cmd.ability);
    if (shell != Shell::He && !has_upgrade(cmd.player, shell_upgrade(shell))) return;
    for (Unit* u : collect_owned(cmd, [this](EntityId id) { return find_unit_mut(id); })) {
        const WeaponDef& gun = def_of(*u).weapon;
        if (!gun.indirect || u->shell == shell) continue;
        u->shell = shell;
        u->cooldown = std::max(u->cooldown, gun.reload);
    }
}

int32_t World::rack(const Unit& u) const {
    const UnitTypeDef& def = def_of(u);
    if (can_ride(def) && has_upgrade(u.owner, UpgradeId::LoadVests)) return def.rounds_capacity * kVestRoundsPercent / 100;
    return def.rounds_capacity;
}

const Unit* World::find_unit(EntityId id) const {
    auto it = std::lower_bound(units_.begin(), units_.end(), id,
                               [](const Unit& u, EntityId value) { return u.id < value; });
    return (it != units_.end() && it->id == id) ? &*it : nullptr;
}

Unit* World::find_unit_mut(EntityId id) {
    return const_cast<Unit*>(static_cast<const World*>(this)->find_unit(id));
}

const Structure* World::find_structure(EntityId id) const {
    auto it = std::lower_bound(structures_.begin(), structures_.end(), id,
                               [](const Structure& s, EntityId value) { return s.id < value; });
    return (it != structures_.end() && it->id == id) ? &*it : nullptr;
}

Structure* World::find_structure_mut(EntityId id) {
    return const_cast<Structure*>(static_cast<const World*>(this)->find_structure(id));
}

EntityId World::structure_id_at(TilePos t) const {
    if (!map_.contains(t)) return 0;
    return structure_tiles_[static_cast<size_t>(t.y * map_.width() + t.x)];
}

const Structure* World::structure_at(TilePos t) const {
    const EntityId id = structure_id_at(t);
    return id ? find_structure(id) : nullptr;
}

// --- Structures --------------------------------------------------------------

// Every connected patch of House tiles becomes one house, every patch of
// Bridge tiles one bridge. Scanned row by row, so ids are the same everywhere.
void World::build_structures() {
    const int32_t w = map_.width();
    const int32_t h = map_.height();
    structure_tiles_.assign(static_cast<size_t>(w * h), 0);

    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            const Terrain terrain = map_.terrain(x, y);
            const bool made = terrain == Terrain::House || terrain == Terrain::Bridge || terrain == Terrain::Apartment ||
                              terrain == Terrain::Tower || terrain == Terrain::GasStation || terrain == Terrain::Elevator;
            if (!made || structure_id_at({x, y}) != 0) continue;

            Structure s;
            s.id = next_id_++;
            s.type = terrain == Terrain::House        ? StructureType::House
                     : terrain == Terrain::Bridge     ? StructureType::Bridge
                     : terrain == Terrain::Apartment  ? StructureType::Apartment
                     : terrain == Terrain::Tower      ? StructureType::CellTower
                     : terrain == Terrain::GasStation ? StructureType::GasStation
                                                      : StructureType::Elevator;
            s.hp = structure_type(s.type).max_hp;
            if (s.type == StructureType::GasStation) s.cargo[static_cast<size_t>(Resource::Fuel)] = kGasStationFuel;
            if (s.type == StructureType::Elevator) s.cargo[static_cast<size_t>(Resource::Food)] = kElevatorFood;

            // Flood fill the patch (4-connected, fixed neighbour order).
            std::vector<TilePos> open{{x, y}};
            structure_tiles_[static_cast<size_t>(y * w + x)] = s.id;
            while (!open.empty()) {
                const TilePos t = open.back();
                open.pop_back();
                s.tiles.push_back(t);
                const TilePos neighbours[] = {{t.x + 1, t.y}, {t.x - 1, t.y}, {t.x, t.y + 1}, {t.x, t.y - 1}};
                for (const TilePos& n : neighbours) {
                    if (!map_.contains(n) || map_.terrain(n) != terrain || structure_id_at(n) != 0) continue;
                    structure_tiles_[static_cast<size_t>(n.y * w + n.x)] = s.id;
                    open.push_back(n);
                }
            }
            std::sort(s.tiles.begin(), s.tiles.end(),
                      [](const TilePos& a, const TilePos& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });

            FixedVec2 sum{};
            for (const TilePos& t : s.tiles) sum += tile_center(t);
            const auto n = static_cast<int32_t>(s.tiles.size());
            s.center = {sum.x / n, sum.y / n};
            structures_.push_back(std::move(s));
        }
    }
}

void World::apply_garrison(const Command& cmd) {
    if (const Unit* carrier = find_unit(cmd.target_unit)) return apply_board(cmd, *carrier);
    const Structure* s = find_structure(cmd.target_unit);
    // A house, a dugout...; or our own field hospital, to be healed.
    const bool ward = s && s->type == StructureType::Hospital && s->built && s->owner == cmd.player;
    if (!s || (!is_shelter(role_of(*s)) && !ward)) return;
    const TilePos goal = map_.clamp_tile(tile_of(s->center));
    for (Unit* u : collect_owned(cmd, [this](EntityId id) { return find_unit_mut(id); })) {
        if (def_of(*u).vehicle || u->inside == s->id) continue;  // only infantry goes in
        leave_structure(*u);
        u->order = Order::Garrison;
        u->order_target = s->id;
        u->order_goal = goal;
        u->order_path = field_to(goal, MoveClass::Foot);
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->engaged = 0;
    }
}

// Foot soldiers mount up: they walk to our IFV, following it if it drives
// off, and get in while there's room.
void World::apply_board(const Command& cmd, const Unit& carrier) {
    if (carrier.owner != cmd.player || def_of(carrier).troop_capacity == 0) return;
    const TilePos goal = map_.clamp_tile(tile_of(carrier.pos));
    for (Unit* u : collect_owned(cmd, [this](EntityId id) { return find_unit_mut(id); })) {
        if (!can_ride(def_of(*u)) || u->inside == carrier.id) continue;
        leave_structure(*u);
        u->order = Order::Garrison;
        u->order_target = carrier.id;
        u->order_goal = goal;
        u->order_path = field_to(goal, MoveClass::Foot);
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->engaged = 0;
    }
}

void World::seek_carrier(Unit& u, Unit& carrier) {
    auto done = [&u] {
        u.order = Order::Idle;
        u.order_path.reset();
    };
    if ((carrier.pos - u.pos).length_sq_raw() <= square_raw(kBoardDistance + def_of(carrier).radius)) {
        board(u, carrier);  // full: he waits beside it
        return done();
    }
    const TilePos goal = map_.clamp_tile(tile_of(carrier.pos));
    if (goal != u.order_goal) {  // it drove on: after it
        u.order_goal = goal;
        u.order_path = field_to(goal, MoveClass::Foot);
    }
    if (navigate(u, carrier.pos, u.order_path, u.order_goal, false) == Step::Blocked) done();
}

bool World::board(Unit& u, Unit& carrier) {
    if (static_cast<int32_t>(carrier.passengers.size()) >= def_of(carrier).troop_capacity) return false;
    carrier.passengers.push_back(u.id);
    u.inside = carrier.id;
    u.pos = carrier.pos;
    u.prev_pos = carrier.prev_pos;
    u.moving = false;
    u.engaged = 0;
    u.order_path.reset();
    u.chase_path.reset();
    return true;
}

void World::seek_garrison(Unit& u) {
    if (Unit* carrier = find_unit_mut(u.order_target)) return seek_carrier(u, *carrier);
    Structure* s = find_structure_mut(u.order_target);
    auto done = [&u] {
        u.order = Order::Idle;
        u.order_path.reset();
    };
    if (!s) return done();
    if (distance_sq_to(*s, u.pos) <= square_raw(kEnterDistance)) {
        enter(u, *s);
        return done();
    }
    // The house itself is impassable, so the route ends next to it.
    if (navigate(u, s->center, u.order_path, u.order_goal, false) == Step::Blocked &&
        distance_sq_to(*s, u.pos) > square_raw(kEnterDistance)) {
        done();
    }
}

// Returns false if the house is full or held by the enemy (storming it is
// for later).
bool World::enter(Unit& u, Structure& s) {
    if (static_cast<int32_t>(s.garrison.size()) >= structure_type(s.type).capacity) return false;
    if (s.owner != kNoOwner && s.owner != u.owner) return false;
    s.garrison.push_back(u.id);
    s.owner = u.owner;
    u.inside = s.id;
    u.pos = s.center;
    u.prev_pos = s.center;
    u.moving = false;
    u.engaged = 0;
    u.order_path.reset();
    u.chase_path.reset();
    return true;
}

void World::leave_structure(Unit& u) {
    if (!u.inside) return;
    FixedVec2 from = u.pos;
    if (Structure* s = find_structure_mut(u.inside)) {
        std::erase(s->garrison, u.id);
        if (s->garrison.empty() && is_shelter(role_of(*s))) s->owner = kNoOwner;
        from = s->center;
    } else if (Unit* carrier = find_unit_mut(u.inside)) {
        // Out at the back doors, three abreast.
        std::erase(carrier->passengers, u.id);
        const auto n = static_cast<int32_t>(carrier->passengers.size());
        const Fixed length = carrier->facing.length();
        if (length.raw > 0) {
            const FixedVec2 ahead = carrier->facing * (Fixed::from_int(1) / length);
            const FixedVec2 side{-ahead.y, ahead.x};
            const Fixed back = def_of(*carrier).radius + Fixed::from_ratio(1, 2) + Fixed::from_ratio(1, 2) * (n / 3);
            from = carrier->pos - ahead * back + side * (Fixed::from_ratio(2, 5) * (n % 3 - 1));
        } else {
            from = carrier->pos;
        }
        u.inside = 0;
        const TilePos t = map_.clamp_tile(tile_of(from));
        if (map_.passable(t, class_of(u))) {
            u.pos = clamp_to_map(from, def_of(u).radius);
        } else if (auto door = nearest_passable(map_, t, class_of(u))) {
            u.pos = tile_center(*door);
        }
        u.prev_pos = u.pos;
        return;
    }
    u.inside = 0;
    if (auto door = nearest_passable(map_, tile_of(from), class_of(u))) u.pos = tile_center(*door);
    u.prev_pos = u.pos;
}

// Garrisoned soldiers can't move; they fire from the windows at whatever
// comes into sight and range.
void World::update_garrisoned(Unit& u) {
    if (find_unit(u.inside)) return;  // riding in an IFV
    if (u.order == Order::Retrain) return update_retrain(u);  // at drill in the headquarters
    const Structure* home = find_structure(u.inside);
    if (home && home->type == StructureType::Hospital) {
        // In a hospital bed: healing, not fighting; out when well.
        const int32_t full = def_of(u).max_hp;
        if (tick_ % kHealTicks == 0) u.hp = std::min(full, u.hp + 1);
        if (u.hp >= full) {
            leave_structure(u);
            u.order = Order::Idle;
        }
        return;
    }
    if (home && home->type == StructureType::Dugout) return;  // sheltering
    const Unit* target = home && home->type == StructureType::Pillbox ? find_enemy_in_slit(u, *home) : find_enemy_in_sight(u);
    if (!target) return;
    if (target->airborne) return engage(u, *target);  // a missile out of the window
    const UnitTypeDef& def = def_of(u);
    const Fixed reach = weapon_of(u).range + def.radius + def_of(*target).radius;
    const FixedVec2 to_target = target->pos - u.pos;
    if (to_target.length_sq_raw() > square_raw(reach)) return;
    if (to_target.x.raw != 0 || to_target.y.raw != 0) u.facing = to_target;
    if (u.cooldown == 0) try_fire(u, target->pos, target, weapon_of(u));
}

void World::hurt_structure(const Structure& s, const WeaponDef& weapon) {
    if (weapon.damage_type == DamageType::Bullet) return;  // rifles don't knock down walls
    const int32_t damage = weapon.structure_damage > 0 ? weapon.structure_damage : weapon.damage;
    const int32_t amount = damage - structure_type(s.type).armor[static_cast<size_t>(weapon.damage_type)];
    if (amount > 0) pending_damage_.push_back({s.id, amount});
    if (weapon.aerial_bomb) {  // a bomb from the air: a section of it down
        if (Structure* hit = find_structure_mut(s.id); hit && hit->bombed < 3) ++hit->bombed;
    }
}

// What a building's rubble looks like (see RuinKind).
RuinKind ruin_of(const Structure& s) {
    switch (s.type) {
        case StructureType::Apartment: return RuinKind::Apartment;
        case StructureType::Elevator: return RuinKind::Elevator;
        case StructureType::GasStation: return RuinKind::GasStation;
        case StructureType::CellTower: return RuinKind::Tower;
        case StructureType::ArmorBarracks:
        case StructureType::Warehouse:
        case StructureType::Workshop: return RuinKind::Hangar;
        case StructureType::House:
            if (s.look == HouseLook::Factory) return RuinKind::Factory;
            if (s.look == HouseLook::Cowshed || s.look == HouseLook::Coop) return RuinKind::Farm;
            return s.tiles.size() >= kSpaciousTiles ? RuinKind::Barn : RuinKind::House;
        default: return RuinKind::Base;
    }
}

// A house comes down on everyone inside; a bridge drops whoever is on it
// into the river.
void World::collapse(const Structure& s) {
    if (role_of(s) == StructureType::FuelDepot) burn_fuel_depot(s);
    if (s.type == StructureType::GasStation && s.cargo[static_cast<size_t>(Resource::Fuel)] > 0) burn_fuel_depot(s, false);
    if (s.type == StructureType::Airfield) {
        // The aircraft parked on the runway go with it.
        for (Unit& u : units_) {
            const TilePos t = tile_of(u.pos);
            if (def_of(u).aircraft && !u.airborne && std::find(s.tiles.begin(), s.tiles.end(), t) != s.tiles.end()) {
                u.hp = 0;
            }
        }
    } else if (s.type != StructureType::Bridge) {
        for (EntityId id : s.garrison) {
            if (Unit* u = find_unit_mut(id)) u->hp = 0;
        }
    } else {
        for (Unit& u : units_) {
            if (u.inside) continue;
            const TilePos t = tile_of(u.pos);
            if (std::find(s.tiles.begin(), s.tiles.end(), t) != s.tiles.end()) u.hp = 0;
        }
    }
    Terrain rubble = s.type == StructureType::Bridge ? Terrain::Water : Terrain::Ruins;
    if (is_fieldwork(s.type) || is_obstacle(s.type) || s.type == StructureType::Dugout ||
        s.type == StructureType::Pillbox || s.type == StructureType::Airfield) {
        rubble = Terrain::Grass;  // filled in, cut, torn down, cratered
    }
    for (const TilePos& t : s.tiles) {
        if (s.type == StructureType::Parapet) {  // just a mound on the ground
            structure_tiles_[static_cast<size_t>(t.y * map_.width() + t.x)] = 0;
            continue;
        }
        if (rubble == Terrain::Ruins) map_.set_ruin(t.x, t.y, ruin_of(s));
        map_.set_terrain(t.x, t.y, rubble);
        structure_tiles_[static_cast<size_t>(t.y * map_.width() + t.x)] = 0;
    }
}

// Terrain changed: every cached route may now lead through a river.
void World::on_map_changed() { field_cache_.clear(); }

// --- Commands ----------------------------------------------------------------

// Units keeping radio silence out of a relay's reach get their part of the
// order by courier, a while later; the rest at once.
void World::apply(const Command& cmd) {
    if (!cmd.units.empty()) {
        Command now = cmd;
        Command late = cmd;
        now.units.clear();
        late.units.clear();
        for (EntityId id : cmd.units) {
            const Unit* u = find_unit(id);
            (u && u->owner == cmd.player && !in_touch(*u) ? late : now).units.push_back(id);
        }
        if (!late.units.empty()) {
            couriers_.push_back({tick_ + kCourierTicks, std::move(late)});
            if (!now.units.empty()) deliver(now);
            return;
        }
    }
    deliver(cmd);
}

void World::deliver(const Command& cmd) {
    switch (cmd.type) {
        case CommandType::Move: apply_group_move(cmd, Order::Move); break;
        case CommandType::AttackMove: apply_group_move(cmd, Order::AttackMove); break;
        case CommandType::Attack: apply_attack(cmd); break;
        case CommandType::AttackGround: apply_attack_ground(cmd); break;
        case CommandType::Garrison: apply_garrison(cmd); break;
        case CommandType::Gather: apply_gather(cmd); break;
        case CommandType::Train: apply_train(cmd); break;
        case CommandType::Retrain: apply_retrain(cmd); break;
        case CommandType::Build: apply_build(cmd); break;
        case CommandType::Haul: apply_haul(cmd); break;
        case CommandType::Observe: apply_observe(cmd); break;
        case CommandType::Ability: apply_ability(cmd); break;
        case CommandType::Upgrade: apply_upgrade(cmd); break;
        case CommandType::Unload: apply_unload(cmd); break;
        case CommandType::Research: apply_research(cmd); break;
        case CommandType::Supply: apply_supply(cmd); break;
        case CommandType::Collect: apply_collect(cmd); break;
        case CommandType::Rally: apply_rally(cmd); break;
        case CommandType::LoadShell: apply_load_shell(cmd); break;
        case CommandType::Stop: apply_stop(cmd); break;
    }
}

void World::apply_group_move(const Command& cmd, Order order) {
    std::vector<Unit*> group = collect_owned(cmd, [this](EntityId id) { return find_unit_mut(id); });
    std::erase_if(group, [](const Unit* u) { return def_of(*u).aircraft; });  // aircraft fly missions only
    if (group.empty()) return;
    for (Unit* u : group) leave_structure(*u);  // any order to go somewhere starts at the door

    const FixedVec2 target = clamp_to_map(cmd.target, Fixed{});
    const TilePos goal = map_.clamp_tile(tile_of(target));

    // The group marches at the pace of its slowest unit, like a formation in AoE II.
    Fixed slowest = def_of(*group.front()).speed;
    for (const Unit* u : group) slowest = min(slowest, def_of(*u).speed);

    // Spread the group over a grid of slots around the target, so units don't
    // fight over a single point. Slots are assigned in the group's current
    // layout (rows top to bottom, each row left to right) to limit crossings.
    const auto n = static_cast<int32_t>(group.size());
    int32_t cols = 1;
    while (cols * cols < n) ++cols;
    const int32_t rows = (n + cols - 1) / cols;

    Fixed spacing{};
    for (const Unit* u : group) spacing = max(spacing, def_of(*u).radius * 2);
    spacing += kFormationGap;

    std::sort(group.begin(), group.end(), [](const Unit* a, const Unit* b) {
        if (a->pos.y != b->pos.y) return a->pos.y < b->pos.y;
        if (a->pos.x != b->pos.x) return a->pos.x < b->pos.x;
        return a->id < b->id;
    });

    for (int32_t row = 0; row < rows; ++row) {
        const auto first = group.begin() + row * cols;
        const auto last = group.begin() + std::min(n, (row + 1) * cols);
        std::sort(first, last, [](const Unit* a, const Unit* b) {
            if (a->pos.x != b->pos.x) return a->pos.x < b->pos.x;
            return a->id < b->id;
        });

        const auto in_row = static_cast<int32_t>(last - first);
        for (int32_t col = 0; col < in_row; ++col) {
            Unit* u = *(first + col);
            const FixedVec2 offset{
                spacing * (2 * col - (in_row - 1)) / 2,
                spacing * (2 * row - (rows - 1)) / 2,
            };
            FixedVec2 slot = clamp_to_map(target + offset, def_of(*u).radius);
            // A slot in a forest or river is no good for a tank: take the nearest dry land.
            if (!can_stand(*u, slot)) {
                if (auto free = nearest_passable(map_, tile_of(slot), class_of(*u))) slot = tile_center(*free);
            }

            u->order = order;
            u->order_point = slot;
            u->order_goal = goal;
            u->order_path = field_to(goal, class_of(*u));
            u->chase_path.reset();
            u->speed_cap = n > 1 ? slowest : Fixed{};
            u->order_target = 0;
            u->engaged = 0;
        }
    }
}

void World::apply_attack(const Command& cmd) {
    const Unit* target = find_unit(cmd.target_unit);
    // Nobody can be ordered to hunt what the player doesn't see.
    if (!target || target->owner == cmd.player || !sees(cmd.player, *target)) return;
    for (Unit* u : collect_owned(cmd, [this](EntityId id) { return find_unit_mut(id); })) {
        if (!is_armed(def_of(*u))) continue;  // a truck has nothing to attack with
        if (def_of(*u).aircraft) {
            if (!target->airborne) give_mission(*u, target->pos, target->id);
            continue;
        }
        // Only air defence reaches an aircraft in the air, and a missile nothing else.
        if (target->airborne ? !weapon_of(*u).anti_air : weapon_of(*u).air_only) continue;
        leave_structure(*u);
        u->order = Order::Attack;
        u->order_target = target->id;
        u->engaged = target->id;
        u->order_path.reset();
        u->speed_cap = Fixed{};
    }
}

void World::apply_attack_ground(const Command& cmd) {
    const FixedVec2 target = clamp_to_map(cmd.target, Fixed{});
    const TilePos goal = map_.clamp_tile(tile_of(target));
    for (Unit* u : collect_owned(cmd, [this](EntityId id) { return find_unit_mut(id); })) {
        if (!is_armed(def_of(*u)) || weapon_of(*u).air_only) continue;
        if (def_of(*u).aircraft) {
            give_mission(*u, target, 0);
            continue;
        }
        leave_structure(*u);
        u->order = Order::AttackGround;
        u->order_point = target;  // everyone fires at the same spot
        u->order_goal = goal;
        u->order_path = field_to(goal, class_of(*u));
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->order_target = 0;
        u->engaged = 0;
    }
}

void World::apply_stop(const Command& cmd) {
    for (Unit* u : collect_owned(cmd, [this](EntityId id) { return find_unit_mut(id); })) {
        if (u->airborne) continue;  // a mission in the air is flown to the end
        u->order = Order::Idle;
        u->order_target = 0;
        u->engaged = 0;
        u->order_path.reset();
        u->chase_path.reset();
        u->speed_cap = Fixed{};
    }
}

// --- Tick --------------------------------------------------------------------

void World::step() {
    update_couriers();
    update_trains();
    update_rations();
    update_production();
    update_upgrades();
    update_research();
    update_smoke();
    for (Unit& u : units_) {
        u.prev_pos = u.pos;
        u.moving = false;
    }

    for (Unit& u : units_) update_unit(u);
    for (Unit& u : units_) u.still = u.moving ? 0 : u.still + 1;
    update_bogs();
    update_repairs();
    // Projectiles move after units, so a unit that stepped aside this tick dodges.
    move_projectiles();
    update_fires();
    apply_damage_and_remove_dead();

    separate_units();
    for (Unit& u : units_) u.pos = clamp_to_map(u.pos, def_of(u).radius);
    // The men aboard ride along.
    for (const Unit& v : units_) {
        for (EntityId id : v.passengers) {
            if (Unit* p = find_unit_mut(id)) {
                p->pos = v.pos;
                p->prev_pos = v.prev_pos;
            }
        }
    }
    update_mines();
    update_charges();
    if (tick_ % kRearmInterval == 0) draw_from_caches();

    while (!recent_impacts_.empty() && recent_impacts_.front().tick + kImpactHistory < tick_) {
        recent_impacts_.pop_front();
    }
    ++tick_;
}

void World::update_unit(Unit& u) {
    if (u.cooldown > 0) --u.cooldown;
    if (def_of(u).aircraft) return update_aircraft(u);
    if (u.inside) return update_garrisoned(u);

    auto finish_order = [&u] {
        u.order = Order::Idle;
        u.order_path.reset();
        u.speed_cap = Fixed{};
    };

    switch (u.order) {
        case Order::Idle:
            if (def_of(u).supplies != Resource::Count) serve(u);
            if (!is_armed(def_of(u))) break;
            if (weapon_of(u).indirect) break;  // guns fire when told to, not at whatever shows up
            if (const Unit* target = find_enemy_in_sight(u)) engage(u, *target);
            break;

        case Order::Move:
            if (navigate(u, u.order_point, u.order_path, u.order_goal, true) != Step::Moved) finish_order();
            if (def_of(u).tank && is_armed(def_of(u))) fire_on_the_move(u);
            break;

        case Order::Attack:
            if (const Unit* target = find_unit(u.order_target); target && sees(u.owner, *target)) {
                engage(u, *target);
            } else if (target) {
                // Lost from view: go where it was last seen, ready to fight.
                u.order = Order::AttackMove;
                u.order_point = target->pos;
                u.order_goal = map_.clamp_tile(tile_of(target->pos));
                u.order_path.reset();
                u.order_target = 0;
                u.engaged = 0;
            } else {
                finish_order();  // target is dead
                u.engaged = 0;
            }
            break;

        case Order::AttackMove:
            if (const Unit* target = is_armed(def_of(u)) ? find_enemy_in_sight(u) : nullptr) {
                engage(u, *target);
            } else if (navigate(u, u.order_point, u.order_path, u.order_goal, true) != Step::Moved) {
                finish_order();
            }
            break;

        case Order::AttackGround:
            engage_ground(u);
            break;

        case Order::Garrison:
            seek_garrison(u);
            break;

        case Order::Gather:
            update_gathering(u);
            break;

        case Order::Retrain:
            update_retrain(u);
            break;

        case Order::Build:
            update_building(u);
            break;

        case Order::Haul:
            update_hauling(u);
            break;

        case Order::Observe:
            break;  // an observation post stays put and quiet

        case Order::Ability:
            update_ability(u);
            break;

        case Order::Supply:
            update_supply(u);
            break;

        case Order::Collect:
            update_collect(u);
            break;
    }
}

// Keeps shooting the current enemy while it stays in sight, otherwise picks
// the nearest one (ties go to the lower id, so every peer picks the same).
// Only enemies the player sees count: nobody shoots into the fog.
const Unit* World::find_enemy_in_sight(Unit& u) {
    // Air defence watches the sky first; a missile crew nothing else.
    if (weapon_of(u).anti_air) {
        if (const Unit* plane = find_air_target(u)) {
            u.engaged = plane->id;
            return plane;
        }
        if (weapon_of(u).air_only) {
            u.engaged = 0;
            return nullptr;
        }
    }
    const uint64_t sight_sq = square_raw(def_of(u).sight);

    if (const Unit* current = find_unit(u.engaged); current && !current->airborne && sees(u.owner, *current) &&
                                                    (current->pos - u.pos).length_sq_raw() <= sight_sq) {
        return current;
    }

    const Unit* best = nullptr;
    uint64_t best_sq = 0;
    for (const Unit& other : units_) {
        if (other.owner == u.owner || other.airborne || !sees(u.owner, other)) continue;
        const uint64_t d = (other.pos - u.pos).length_sq_raw();
        if (d <= sight_sq && (!best || d < best_sq)) {
            best = &other;
            best_sq = d;
        }
    }
    u.engaged = best ? best->id : 0;
    return best;
}

void World::engage(Unit& u, const Unit& target) {
    u.engaged = target.id;
    if (target.airborne) {
        // Air defence fires at an aircraft in reach; nobody chases one.
        const FixedVec2 to_target = target.pos - u.pos;
        const Fixed reach = weapon_of(u).range + def_of(u).radius + def_of(target).radius;
        if (!weapon_of(u).anti_air || to_target.length_sq_raw() > square_raw(reach)) return;
        if (to_target.x.raw != 0 || to_target.y.raw != 0) u.facing = to_target;
        if (u.cooldown == 0 && !out_of_rounds(u)) fire_at_air(u, target);
        return;
    }
    if (weapon_of(u).air_only) {  // down on the ground it's none of a missile crew's business
        u.engaged = 0;
        if (u.order == Order::Attack) u.order = Order::Idle;
        return;
    }
    if (weapon_of(u).indirect) return engage_indirect(u, target.pos, u.chase_path, tile_of(target.pos), weapon_of(u));
    const UnitTypeDef& def = def_of(u);
    const Fixed reach = weapon_of(u).range + def.radius + def_of(target).radius;
    const FixedVec2 to_target = target.pos - u.pos;

    // Out of range, or in range but a hill or house hides the target: move in.
    // Chase at full speed, around obstacles if needed. If the target is
    // somewhere we can't go (a tank vs. infantry in a forest), wait at the edge.
    if (to_target.length_sq_raw() > square_raw(reach)) {
        navigate(u, target.pos, u.chase_path, tile_of(target.pos), false);
        return;
    }
    if (to_target.x.raw != 0 || to_target.y.raw != 0) u.facing = to_target;
    if (u.cooldown == 0 && !try_fire(u, target.pos, &target, weapon_of(u))) {
        navigate(u, target.pos, u.chase_path, tile_of(target.pos), false);
    }
}

// A tank on the move keeps its gun on an enemy in reach, its turret
// turning whichever way the hull goes; once its gunner has held the target
// a moment (the lock) it fires without stopping.
void World::fire_on_the_move(Unit& u) {
    const WeaponDef& weapon = weapon_of(u);
    auto in_reach = [&](const Unit& t) {
        return !t.airborne && sees(u.owner, t) &&
               (t.pos - u.pos).length_sq_raw() <= square_raw(weapon.range + def_of(u).radius + def_of(t).radius);
    };
    const Unit* target = find_unit(u.lock);
    if (!target || !in_reach(*target)) {
        target = find_enemy_in_sight(u);
        if (target && !in_reach(*target)) target = nullptr;
        u.lock = target ? target->id : 0;
        u.lock_ticks = 0;
    }
    if (!target) {
        u.engaged = 0;
        return;
    }
    ++u.lock_ticks;
    u.engaged = target->id;
    const FixedVec2 to_target = target->pos - u.pos;
    if (to_target.x.raw != 0 || to_target.y.raw != 0) u.facing = to_target;  // the turret on it
    const auto lock = static_cast<int32_t>(has_upgrade(u.owner, UpgradeId::FireControl) ? kFireControlLockTicks : kLockTicks);
    if (u.lock_ticks >= lock && u.cooldown == 0) try_fire(u, target->pos, target, weapon);
}

void World::engage_ground(Unit& u) {
    if (weapon_of(u).indirect) return engage_indirect(u, u.order_point, u.order_path, u.order_goal, weapon_of(u));
    const UnitTypeDef& def = def_of(u);
    const Fixed reach = weapon_of(u).range + def.radius;
    const FixedVec2 to_point = u.order_point - u.pos;

    if (to_point.length_sq_raw() > square_raw(reach)) {
        navigate(u, u.order_point, u.order_path, u.order_goal, false);
        return;
    }
    if (to_point.x.raw != 0 || to_point.y.raw != 0) u.facing = to_point;
    if (u.cooldown == 0 && !try_fire(u, u.order_point, nullptr, weapon_of(u))) {
        navigate(u, u.order_point, u.order_path, u.order_goal, false);
    }
}

// --- Movement ----------------------------------------------------------------

std::shared_ptr<const FlowField> World::field_to(TilePos goal, MoveClass cls) {
    const uint64_t key = (static_cast<uint64_t>(cls) << 48) | (static_cast<uint64_t>(static_cast<uint32_t>(goal.y)) << 24) |
                         static_cast<uint32_t>(goal.x);
    if (auto cached = field_cache_[key].lock(); cached && !cached->stale(map_)) return cached;

    auto field = std::make_shared<const FlowField>(map_, goal, cls);
    field_cache_[key] = field;
    if (field_cache_.size() > kFieldCacheSweep) {
        std::erase_if(field_cache_, [](const auto& entry) { return entry.second.expired(); });
    }
    return field;
}

World::Step World::navigate(Unit& u, FixedVec2 point, std::shared_ptr<const FlowField>& path, TilePos field_goal,
                            bool formation) {
    if (def_of(u).fuel_capacity.raw > 0 && u.fuel.raw <= 0) return Step::Blocked;  // dry: going nowhere
    const MoveClass cls = class_of(u);

    // Close and nothing in the way: walk straight there.
    if ((point - u.pos).length_sq_raw() <= square_raw(kDirectRange) && straight_walkable(map_, u.pos, point, cls)) {
        return step_towards(u, point, formation);
    }

    if (!path || path->requested_goal() != field_goal || path->move_class() != cls || path->stale(map_)) {
        path = field_to(field_goal, cls);
    }
    const TilePos here = tile_of(u.pos);
    if (!path->reachable(here)) return Step::Blocked;
    if (here == path->goal()) {
        // At the end of the route but the point itself can't be reached in a straight line.
        return path->goal() == map_.clamp_tile(tile_of(point)) ? step_towards(u, point, formation) : Step::Blocked;
    }

    // Look a few tiles ahead along the route and aim at the farthest one in
    // plain sight: straight lines instead of grid zig-zags.
    TilePos t = here;
    FixedVec2 waypoint = tile_center(*path->next(here));
    for (int i = 0; i < kLookAhead; ++i) {
        const std::optional<TilePos> n = path->next(t);
        if (!n) break;
        t = *n;
        const FixedVec2 c = tile_center(t);
        if (!straight_walkable(map_, u.pos, c, cls)) break;
        waypoint = c;
    }

    const Step step = step_towards(u, waypoint, formation);
    if (step == Step::Blocked) {
        // Caught on a corner: back to the middle of our own tile, which is always free.
        step_towards(u, tile_center(here), formation);
    }
    return Step::Moved;
}

World::Step World::step_towards(Unit& u, FixedVec2 point, bool formation) {
    const FixedVec2 to_point = point - u.pos;
    const Fixed dist = to_point.length();
    if (dist.raw == 0) return Step::Arrived;

    const UnitTypeDef& def = def_of(u);
    const bool thirsty = def.fuel_capacity.raw > 0;
    if (thirsty && u.fuel.raw <= 0) return Step::Blocked;  // out of fuel: stuck where it stands
    if (u.deployed && !pack_step(u)) return Step::Moved;   // a gun packs up before it goes anywhere
    Fixed speed = def.speed;
    if (formation && u.speed_cap.raw > 0) speed = min(speed, u.speed_cap);
    if (hungry(u.owner)) speed = speed * kHungrySpeedPercent / 100;
    if (is_armor(def) && has_upgrade(u.owner, UpgradeId::TankEngine)) {
        speed = speed * kEnginePercent / 100;
    }
    // Terrain slows down (forest for infantry, villages for vehicles...);
    // on soft ground a light tank loses less of its speed, a heavy one more.
    const TilePos under = map_.clamp_tile(tile_of(u.pos));
    int32_t terrain_pct = map_.speed_percent(under, move_class(def));
    if (def.soft_ground_percent != 100 && terrain_pct > 0 && terrain_pct < 100 && is_soft_ground(map_.terrain(under))) {
        terrain_pct = std::max(kMinSoftGroundPercent, 100 - (100 - terrain_pct) * def.soft_ground_percent / 100);
    }
    if (terrain_pct > 0) speed = speed * terrain_pct / 100;
    if (u.mired > 0) speed = speed * (100 - kBogSlowPercent * u.mired / kBogLimit) / 100;  // sinking in the bog

    u.facing = to_point;
    const FixedVec2 next = dist <= speed ? point : u.pos + to_point * (speed / dist);
    const FixedVec2 before = u.pos;
    if (!move_to(u, next)) return Step::Blocked;
    u.moving = true;
    u.hull = to_point;
    if (thirsty) u.fuel = max(Fixed{}, u.fuel - (u.pos - before).length());
    // Tracks roll barbed wire flat.
    if (move_class(def) == MoveClass::Vehicle) {
        if (const Structure* s = structure_at(map_.clamp_tile(tile_of(u.pos))); s && s->type == StructureType::Wire) {
            pending_damage_.push_back({s->id, s->hp});
        }
    }
    u.deploy_work = 0;     // setting up starts over wherever it stops
    u.camouflaged = false;  // and the nets stay behind
    return u.pos == point ? Step::Arrived : Step::Moved;
}

bool World::can_stand(const Unit& u, FixedVec2 p) const { return map_.passable(tile_of(p), class_of(u)); }

// Moves to `next` if the terrain allows it, otherwise slides along the obstacle.
bool World::move_to(Unit& u, FixedVec2 next) {
    // A unit somehow standing where it can't be (e.g. pushed) may always walk out.
    if (can_stand(u, next) || !can_stand(u, u.pos)) {
        u.pos = next;
        return true;
    }
    if (next.x != u.pos.x && can_stand(u, {next.x, u.pos.y})) {
        u.pos.x = next.x;
        return true;
    }
    if (next.y != u.pos.y && can_stand(u, {u.pos.x, next.y})) {
        u.pos.y = next.y;
        return true;
    }
    return false;
}

// --- Line of fire -------------------------------------------------------------

Fixed World::window_height(EntityId structure) const {
    const Structure* s = find_structure(structure);
    if (!s) return kWindowHeight;
    if (s->type == StructureType::Apartment) return kApartmentWindow;
    if (s->type == StructureType::CellTower) return kTowerEye;
    if (s->type == StructureType::Elevator) return kElevatorWindow;
    return kWindowHeight;
}

World::FireLine World::fire_line(const Unit& shooter, FixedVec2 aim, Fixed aim_height) const {
    const Fixed muzzle = shooter.inside ? window_height(shooter.inside) : muzzle_height(shooter);
    return {shooter.pos, map_.surface_height(shooter.pos) + muzzle, aim, aim_height};
}

// Walks the line in quarter-tile steps from `start` (a fraction of the line)
// and reports the first hill or house it runs into, or, with roll_foliage,
// the first tree that catches it. `stop` is where along the line that happened.
World::Obstruction World::trace_terrain(const FireLine& line, Fixed start, bool roll_foliage,
                                        int32_t foliage_percent, EntityId own_structure, Fixed& stop) {
    const Fixed length = (line.to - line.from).length();
    const int32_t samples = std::max(1, (length * 4).to_int() + 1);
    for (int32_t i = 1; i < samples; ++i) {  // the end point is the target itself
        const Fixed t = Fixed::from_ratio(i, samples);
        if (t < start) continue;
        const FixedVec2 p = line.point(t);
        const Fixed h = line.height(t);
        const TilePos tile = tile_of(p);
        if (!map_.contains(tile)) continue;
        const Fixed ground = map_.surface_height(p);
        const Terrain terrain = map_.terrain(tile);

        stop = t;
        if (h < ground) return Obstruction::Terrain;
        if (terrain == Terrain::House && h < ground + kHouseHeight && structure_id_at(tile) != own_structure) {
            return Obstruction::Terrain;
        }
        if (terrain == Terrain::Apartment && h < ground + kApartmentHeight && structure_id_at(tile) != own_structure) {
            return Obstruction::Terrain;
        }
        if (terrain == Terrain::Elevator && h < ground + kElevatorHeight && structure_id_at(tile) != own_structure) {
            return Obstruction::Terrain;
        }
        if (terrain == Terrain::GasStation && h < ground + kHouseHeight && structure_id_at(tile) != own_structure) {
            return Obstruction::Terrain;
        }
        if (terrain == Terrain::Building && h < ground + kBuildingHeight && structure_id_at(tile) != own_structure) {
            return Obstruction::Terrain;
        }
        if (terrain == Terrain::Rock && h < ground + kRockHeight) return Obstruction::Terrain;
        if (terrain == Terrain::Pillbox && h < ground + kRockHeight && structure_id_at(tile) != own_structure) {
            return Obstruction::Terrain;
        }
        if (terrain == Terrain::Forest && h < ground + kTreeHeight && roll_foliage &&
            static_cast<int32_t>(rng_.next_below(100)) < foliage_percent) {
            return Obstruction::Foliage;
        }
    }
    stop = Fixed::from_int(1);
    return Obstruction::None;
}

// The first unit (other than `ignore`) whose body the line passes through
// between fractions t0 and t1, if the line is low enough to hit it there.
const Unit* World::first_unit_on(const FireLine& line, Fixed t0, Fixed t1, EntityId ignore, Fixed& hit) const {
    const FixedVec2 d = line.to - line.from;
    const Fixed len_sq = d.x * d.x + d.y * d.y;
    if (len_sq.raw == 0 || t1 < t0) return nullptr;

    // Only units near this stretch of the line can be in the way.
    const FixedVec2 a = line.point(t0);
    const FixedVec2 b = line.point(t1);
    const Fixed margin = Fixed::from_int(1);
    const Fixed min_x = min(a.x, b.x) - margin;
    const Fixed max_x = max(a.x, b.x) + margin;
    const Fixed min_y = min(a.y, b.y) - margin;
    const Fixed max_y = max(a.y, b.y) + margin;

    const Unit* best = nullptr;
    Fixed best_t{};
    for (const Unit& u : units_) {
        if (u.id == ignore || u.inside || u.airborne) continue;  // the garrison is behind walls, aircraft up high
        if (u.pos.x < min_x || u.pos.x > max_x || u.pos.y < min_y || u.pos.y > max_y) continue;
        const FixedVec2 w = u.pos - line.from;
        const Fixed t = clamp((w.x * d.x + w.y * d.y) / len_sq, t0, t1);
        if ((u.pos - line.point(t)).length_sq_raw() > square_raw(def_of(u).radius)) continue;
        if (line.height(t) > map_.surface_height(u.pos) + top_height(u)) continue;  // flies over
        if (!best || t < best_t) {
            best = &u;
            best_t = t;
        }
    }
    hit = best_t;
    return best;
}

// Crews don't fire through their own men when they can see them. Troops in a
// forest can't be seen, so they may still get hit.
bool World::own_troops_in_line(const Unit& shooter, const FireLine& line, Fixed start) const {
    const FixedVec2 d = line.to - line.from;
    const Fixed len_sq = d.x * d.x + d.y * d.y;
    if (len_sq.raw == 0) return false;
    for (const Unit& u : units_) {
        if (u.owner != shooter.owner || u.id == shooter.id || u.inside || u.airborne) continue;
        if (map_.terrain_at(u.pos) == Terrain::Forest) continue;
        const FixedVec2 w = u.pos - line.from;
        const Fixed t = (w.x * d.x + w.y * d.y) / len_sq;
        if (t < start || t > Fixed::from_int(1)) continue;
        if ((u.pos - line.point(t)).length_sq_raw() > square_raw(def_of(u).radius)) continue;
        if (line.height(t) > map_.surface_height(u.pos) + top_height(u)) continue;
        return true;
    }
    return false;
}

bool World::out_of_rounds(const Unit& u) const { return def_of(u).rounds_capacity > 0 && u.rounds <= 0; }

bool World::try_fire(Unit& shooter, FixedVec2 aim, const Unit* target, const WeaponDef& weapon, bool spends) {
    // Nothing left in the racks: the crew holds its ground and waits for a truck.
    if (spends && out_of_rounds(shooter)) return true;
    // A grenade launcher lobs over whatever is in between.
    if (weapon.lobbed) {
        if (weapon.reload > 0) shooter.cooldown = reload_ticks(shooter, weapon);
        if (spends && def_of(shooter).rounds_capacity > 0) shooter.rounds = std::max(0, shooter.rounds - 1);
        lob(shooter, aim, weapon, false);
        return true;
    }
    // Someone in a house is shot at through the house: aim at the windows.
    Fixed aim_height = map_.surface_height(aim) + kGroundAim;
    if (target) aim_height = map_.surface_height(aim) + (target->inside ? window_height(target->inside) : center_height(*target));
    const FireLine line = fire_line(shooter, aim, aim_height);
    const Fixed length = (line.to - line.from).length();
    const Fixed start = length.raw > 0 ? min(Fixed::from_int(1), kMuzzleClearance / length) : Fixed{};

    Fixed stop;
    if (trace_terrain(line, start, false, 0, shooter.inside, stop) == Obstruction::Terrain) {
        // Running into the very house we're shooting at is the point.
        const EntityId aimed = structure_id_at(tile_of(aim));
        if (aimed == 0 || structure_id_at(tile_of(line.point(stop))) != aimed) return false;
    }
    if (own_troops_in_line(shooter, line, start)) return true;  // hold fire, the line itself is fine
    fire(shooter, aim, aim_height, weapon, spends, target ? target->id : 0);
    return true;
}

// --- Combat ------------------------------------------------------------------

void World::fire(Unit& shooter, FixedVec2 aim, Fixed aim_height, const WeaponDef& weapon, bool spends,
                 EntityId guide) {
    // A skill's own gun (a coaxial machine gun) doesn't reload the main one.
    if (weapon.reload > 0) shooter.cooldown = reload_ticks(shooter, weapon);
    if (spends && def_of(shooter).rounds_capacity > 0) shooter.rounds = std::max(0, shooter.rounds - 1);

    // Where he fires from: a foxhole spoils the aim; small arms fired from a
    // trench before settling at a position barely count; assault troops
    // are deadly up close.
    const Structure* works = structure_at(map_.clamp_tile(tile_of(shooter.pos)));
    int32_t accuracy = weapon.accuracy;
    if (hungry(shooter.owner)) accuracy = accuracy * kHungryAccuracyPercent / 100;
    if (def_of(shooter).tank && shooter.moving) {  // on the move
        accuracy = accuracy * (has_upgrade(shooter.owner, UpgradeId::FireControl) ? kFireControlOnTheMovePercent : kOnTheMovePercent) / 100;
    }
    // A gun's aim by the range, as in life: all but sure point-blank, its
    // accuracy at its effective range, falling off past it. A miss goes
    // wider the farther the target (the dispersion is an angle): as wide as
    // the miss spread at the effective range, in proportion nearer and farther.
    Fixed spread = weapon.miss_spread;
    if (const Fixed near = weapon.effective_range; near.raw > 0 && weapon.range > near) {
        const Fixed distance = min((aim - shooter.pos).length(), weapon.range);
        spread = max(weapon.miss_spread / 4, weapon.miss_spread * (distance / near));
        if (distance <= near) {
            const int32_t sure = accuracy + (100 - accuracy) * kPointBlankPercent / 100;
            accuracy = sure - ((distance * (sure - accuracy)) / near).to_int();
        } else {
            const int32_t far = has_upgrade(shooter.owner, UpgradeId::FireControl) ? kFireControlFarPercent
                                                                                     : kFarAccuracyPercent;
            accuracy = accuracy * (100 - (((distance - near) * (100 - far)) / (weapon.range - near)).to_int()) / 100;
        }
    }
    Shot shot{shooter.pos, map_.elevation_at(shooter.pos)};
    // From the upper floors or up a mast: as from higher ground.
    if (const Structure* home = find_structure(shooter.inside);
        home && (home->type == StructureType::Apartment || home->type == StructureType::CellTower ||
                 home->type == StructureType::Elevator)) {
        shot.elevation = static_cast<uint8_t>(shot.elevation + kUpperFloorLevels);
    }
    if (works && !shooter.inside) {
        if (works->type == StructureType::Foxhole) accuracy = accuracy * kFoxholeAccuracyPercent / 100;
        if (works->type == StructureType::Trench && shooter.still < kSettleTicks &&
            weapon.damage_type == DamageType::Bullet) {
            shot.damage_percent = kTrenchWalkingFirePercent;
        }
    }
    if (shooter.type == UnitTypeId::Assault && (aim - shooter.pos).length_sq_raw() <= square_raw(kCloseQuarters)) {
        shot.damage_percent = shot.damage_percent * kAssaultCloseQuartersPercent / 100;
    }

    // A miss lands somewhere near the aim point, on the ground.
    const bool missed = static_cast<int32_t>(rng_.next_below(100)) >= accuracy;
    if (missed) {
        aim.x += Fixed::from_raw(rng_.next_range(-spread.raw, spread.raw));
        aim.y += Fixed::from_raw(rng_.next_range(-spread.raw, spread.raw));
        aim = clamp_to_map(aim, Fixed{});
        aim_height = map_.surface_height(aim) + kGroundAim;
    }

    FireLine line = fire_line(shooter, aim, aim_height);
    const Fixed length = (line.to - line.from).length();
    const Fixed start = length.raw > 0 ? min(Fixed::from_int(1), kMuzzleClearance / length) : Fixed{};
    const bool instant = weapon.projectile_speed.raw == 0;

    // Trees, houses and hills in the way stop the shot early.
    Fixed end = Fixed::from_int(1);
    trace_terrain(line, start, true, instant ? kFoliagePercentBullet : kFoliagePercentShell, shooter.inside, end);

    const uint8_t elevation = shot.elevation;
    shooter.last_shot_tick = tick_;

    if (instant) {
        // Bullets hit whoever is first in the line: the target, someone else, or nobody.
        Fixed hit_t;
        if (const Unit* victim = first_unit_on(line, start, end, shooter.id, hit_t)) {
            hurt(*victim, weapon, shot);
            end = hit_t;
        }
        shooter.last_shot_at = line.point(end);
        return;
    }

    Projectile p;
    p.id = next_projectile_id_++;
    p.owner = shooter.owner;
    p.shooter = shooter.id;
    p.shooter_type = shooter.type;
    p.shooter_elevation = elevation;
    p.origin = shooter.pos;
    p.pos = shooter.pos;
    p.prev_pos = shooter.pos;
    p.target = line.point(end);
    p.origin_height = line.from_height;
    p.target_height = line.height(end);
    p.weapon = weapon;
    // Sabot rounds: the same round, a harder punch.
    if (def_of(shooter).tank && &weapon == &def_of(shooter).alt_weapon && has_upgrade(shooter.owner, UpgradeId::SabotRounds)) {
        p.weapon.damage = weapon.damage * kSabotPercent / 100;
    }
    if (weapon.guided && !missed) p.homing = guide;
    shooter.last_shot_at = p.target;
    projectiles_.push_back(p);
}

void World::move_projectiles() {
    for (Projectile& p : projectiles_) {
        if (p.at_air) {
            move_missile(p);
            continue;
        }
        // A guided missile flies after its target while the launcher guides it.
        if (const Unit* homed = p.homing && find_unit(p.shooter) ? find_unit(p.homing) : nullptr) {
            p.target = homed->pos;
            p.target_height = map_.surface_height(homed->pos) + center_height(*homed);
        }
        p.prev_pos = p.pos;
        const Fixed speed = p.weapon.projectile_speed;
        const FixedVec2 to_target = p.target - p.pos;
        const bool arrived = to_target.length() <= speed;
        p.pos = arrived ? p.target : p.pos + to_target * (speed / to_target.length());

        // Whoever is in the path this tick (moved into it, or was standing
        // there all along) takes the hit.
        const FireLine line{p.origin, p.origin_height, p.target, p.target_height};
        const Fixed total = (p.target - p.origin).length();
        if (total.raw > 0 && !p.lobbed) {
            const Fixed start = min(Fixed::from_int(1), kMuzzleClearance / total);
            const Fixed t0 = max(start, (p.prev_pos - p.origin).length() / total);
            const Fixed t1 = arrived ? Fixed::from_int(1) : (p.pos - p.origin).length() / total;
            Fixed hit_t;
            if (const Unit* victim = first_unit_on(line, t0, t1, p.shooter, hit_t)) {
                explode(p, line.point(hit_t), victim);
                p.id = 0;
                continue;
            }
        }
        if (arrived) {
            explode(p, p.target, nullptr);
            p.id = 0;
        }
    }
    std::erase_if(projectiles_, [](const Projectile& p) { return p.id == 0; });
}

// Explosions don't care whose units they hit (except the gun that fired). A
// garrison is safe behind its walls until the house falls.
void World::splash(const Projectile& p, FixedVec2 at, const WeaponDef& weapon, const Unit* direct_hit) {
    const Shot blast{at, p.shooter_elevation, true, p.lobbed};
    for (const Unit& u : units_) {
        if (u.id == p.shooter || u.inside || u.airborne) continue;
        const uint64_t d = (u.pos - at).length_sq_raw();
        if (d <= square_raw(weapon.splash_radius + def_of(u).radius)) {
            Shot shot = blast;
            shot.on_it = &u == direct_hit || d <= square_raw(def_of(u).radius);  // the shell struck it
            hurt(u, weapon, shot);
        }
    }
    // Every house or bridge the blast reaches takes it once.
    std::vector<EntityId> hit;
    const int32_t reach = weapon.splash_radius.to_int() + 1;
    const TilePos c = tile_of(at);
    for (int32_t y = c.y - reach; y <= c.y + reach; ++y) {
        for (int32_t x = c.x - reach; x <= c.x + reach; ++x) {
            const EntityId id = structure_id_at({x, y});
            if (id == 0 || std::find(hit.begin(), hit.end(), id) != hit.end()) continue;
            if (distance_sq_to_tile({x, y}, at) > square_raw(weapon.splash_radius)) continue;
            hit.push_back(id);
            hurt_structure(*find_structure(id), weapon);
        }
    }
}

// The artillery's other shells. Cluster: it opens over the target and the
// bomblets land around it. Incendiary: a weaker burst, and the ground burns.
// Phosphorus: a small burst, a smoke screen, and it burns under it.
void World::burst_shell(const Projectile& p, FixedVec2 at) {
    WeaponDef weapon = p.weapon;
    auto blow = [&](FixedVec2 spot) {
        recent_impacts_.push_back({tick_, spot, p.shooter_type, weapon.splash_radius});
        maybe_crater(p, spot, weapon);
        raise_dust(spot, weapon);
        shred_trees(spot, weapon);
        splash(p, spot, weapon);
    };
    switch (p.shell) {
        case Shell::Cluster:
            weapon.damage = weapon.damage * kBombletPercent / 100;
            weapon.splash_radius = kBombletSplash;
            for (int i = 0; i < kClusterBomblets; ++i) {
                FixedVec2 spot = at;
                spot.x += Fixed::from_raw(rng_.next_range(-kClusterScatter.raw, kClusterScatter.raw));
                spot.y += Fixed::from_raw(rng_.next_range(-kClusterScatter.raw, kClusterScatter.raw));
                blow(clamp_to_map(spot, Fixed{}));
            }
            return;
        case Shell::Incendiary:
            weapon.damage = weapon.damage * kIncendiaryBurstPercent / 100;
            blow(at);
            fires_.push_back({at, kFireRadius, tick_ + kFireTicks, p.owner});
            smokes_.push_back({clamp_to_map(at + kPlumeDrift, Fixed{}), kFireRadius + Fixed::from_ratio(1, 2), tick_ + kFireTicks,
                               SmokeKind::Plume, tick_});
            return;
        case Shell::Phosphorus:
            weapon.damage = weapon.damage * kPhosphorusBurstPercent / 100;
            blow(at);
            smokes_.push_back({at, kPhosphorusSmokeRadius, tick_ + kPhosphorusSmokeTicks, SmokeKind::Screen, tick_});
            fires_.push_back({at, kPhosphorusFireRadius, tick_ + kPhosphorusFireTicks, p.owner});
            return;
        default:
            return;
    }
}

// What burns: houses, blocks, buildings; not a bridge, not field works.
static bool burns(StructureType type) {
    return type != StructureType::Bridge && !is_fieldwork(type) && !is_obstacle(type) &&
           type != StructureType::Dugout && type != StructureType::Pillbox && type != StructureType::Airfield;
}

// Fires burn out. While they burn, every second whoever is in one (a
// garrison in a house it reaches too) takes the flames, and every building
// it reaches loses some.
void World::update_fires() {
    std::erase_if(fires_, [&](const Fire& f) { return tick_ >= f.until; });
    if (fires_.empty() || tick_ % kFireInterval != 0) return;
    static constexpr WeaponDef kFlames{.name = "Fire", .damage = kFireBurn, .damage_type = DamageType::Explosive,
                                       .range = Fixed{}, .reload = 0, .projectile_speed = Fixed{},
                                       .splash_radius = Fixed{}, .accuracy = 100, .miss_spread = Fixed{}};
    for (const Fire& f : fires_) {
        const Shot flames{f.center, map_.elevation_at(f.center), true, true};
        for (const Unit& u : units_) {
            if (u.airborne || u.hp <= 0) continue;
            const Structure* home = find_structure(u.inside);
            if (u.inside && !home) continue;  // aboard an IFV
            const uint64_t d = home ? distance_sq_to(*home, f.center) : (u.pos - f.center).length_sq_raw();
            if (d <= square_raw(f.radius + def_of(u).radius)) hurt(u, kFlames, flames);
        }
        for (const Structure& s : structures_) {
            if (burns(s.type) && distance_sq_to(s, f.center) <= square_raw(f.radius)) {
                pending_damage_.push_back({s.id, kFireStructureBurn});
            }
        }
    }
}

void World::explode(const Projectile& p, FixedVec2 at, const Unit* direct_hit) {
    if (p.shell != Shell::He) return burst_shell(p, at);
    const WeaponDef& weapon = p.weapon;
    recent_impacts_.push_back({tick_, at, p.shooter_type, weapon.splash_radius});
    maybe_crater(p, at, weapon);
    raise_dust(at, weapon);
    shred_trees(at, weapon);

    // Thrown in through a window or down a dugout's entrance: the men
    // inside take it, walls or not.
    if (p.enters) {
        if (const Structure* s = structure_at(map_.clamp_tile(tile_of(at))); s && !s->garrison.empty()) {
            const std::vector<EntityId> inside = s->garrison;
            for (size_t i = 0; i < inside.size() && i < static_cast<size_t>(kGrenadeVictims); ++i) {
                if (const Unit* v = find_unit(inside[i])) hurt(*v, weapon, {at, p.shooter_elevation, true, true});
            }
            return;
        }
    }

    if (weapon.splash_radius.raw > 0) return splash(p, at, weapon, direct_hit);

    if (!direct_hit) {
        // A rocket into a wall hits the house; into a pillbox, through the slit, the gunner too.
        if (const Structure* s = structure_at(tile_of(at))) {
            if (s->type == StructureType::Pillbox && weapon.damage_type == DamageType::AntiTank && !s->garrison.empty()) {
                if (const Unit* gunner = find_unit(s->garrison.front())) hurt(*gunner, weapon, {at, 0, true, true});
            }
            return hurt_structure(*s, weapon);
        }
        // Landed: hits whoever stands right at the spot, friend or foe.
        uint64_t best_sq = 0;
        for (const Unit& u : units_) {
            if (u.id == p.shooter || u.inside || u.airborne) continue;
            const uint64_t d = (u.pos - at).length_sq_raw();
            if (d <= square_raw(def_of(u).radius + kHitTolerance) && (!direct_hit || d < best_sq)) {
                direct_hit = &u;
                best_sq = d;
            }
        }
    }
    if (direct_hit) hurt(*direct_hit, weapon, {p.origin, p.shooter_elevation, false, p.lobbed});
}

void World::shred_trees(FixedVec2 at, const WeaponDef& weapon) {
    if (weapon.damage_type != DamageType::Explosive || weapon.splash_radius.raw <= 0) return;
    const int32_t hits = weapon.splash_radius >= kHeavyBurst ? 2 : 1;
    const Fixed reach = weapon.splash_radius + Fixed::from_ratio(1, 2);
    const TilePos c = tile_of(at);
    const int32_t r = reach.to_int() + 1;
    for (int32_t y = c.y - r; y <= c.y + r; ++y) {
        for (int32_t x = c.x - r; x <= c.x + r; ++x) {
            if (!map_.contains_tile(x, y)) continue;
            const Terrain t = map_.terrain(x, y);
            if (t != Terrain::Forest && t != Terrain::Orchard && t != Terrain::Urban) continue;
            if ((tile_center({x, y}) - at).length_sq_raw() > square_raw(reach)) continue;
            map_.add_shred(x, y, hits + (x == c.x && y == c.y ? 1 : 0));  // worst where it burst
        }
    }
}

void World::update_bogs() {
    for (Unit& u : units_) {
        const UnitTypeDef& def = def_of(u);
        if (!sinks_in_bog(def) || u.inside) continue;  // an amphibious one swims, wheels don't get in
        if (map_.terrain_at(u.pos) != Terrain::Swamp) {
            u.mired = 0;  // out on firm ground: free
            continue;
        }
        u.mired = std::min(kBogLimit, u.mired + def.soft_ground_percent * (u.moving ? 1 : 2));
        if (u.mired >= kBogLimit) u.hp = 0;  // gone under: see apply_damage_and_remove_dead
    }
}

void World::maybe_crater(const Projectile& p, FixedVec2 at, const WeaponDef& weapon) {
    if (weapon.damage_type != DamageType::Explosive || weapon.splash_radius < kMediumBurst) return;
    const TilePos t = map_.clamp_tile(tile_of(at));
    const Terrain ground = map_.terrain(t);
    const bool open = ground == Terrain::Grass || ground == Terrain::Plowed || ground == Terrain::Crops ||
                      ground == Terrain::DirtRoad || ground == Terrain::Road || ground == Terrain::Wheat ||
                      ground == Terrain::Garden || ground == Terrain::Crater;
    if (!open || structure_id_at(t) != 0) return;
    const int32_t chance = weapon.splash_radius >= kHeavyBurst ? kHeavyCraterPercent : kMediumCraterPercent;
    if (static_cast<int32_t>(rng_.next_below(100)) >= chance) return;
    // Its size by what made it; a bigger one swallows a smaller one.
    CraterKind kind = CraterKind::Small;
    const UnitTypeDef& shooter = unit_type(p.shooter_type);
    if (shooter.aircraft || (shooter.weapon.indirect && !is_tube_artillery(shooter))) {
        kind = CraterKind::Rocket;  // rockets, bombs
    } else if (is_tube_artillery(shooter)) {
        kind = weapon.splash_radius >= kHeavyShellBurst ? CraterKind::Heavy : weapon.splash_radius >= kHeavyBurst ? CraterKind::Shell : CraterKind::Small;
    } else if (shooter.tank) {
        kind = weapon.splash_radius >= kHeavyBurst ? CraterKind::Shell : CraterKind::Small;
    }
    if (ground == Terrain::Crater && crater_cover(map_.crater_kind(t.x, t.y)) >= crater_cover(kind)) return;
    map_.set_terrain(t.x, t.y, Terrain::Crater);  // passable for all: no route goes stale
    map_.set_crater(t.x, t.y, kind, octant(p.origin - at));
}

int32_t World::cover_percent(const Unit& victim, const Shot& shot) const {
    if (shot.plunging || victim.inside) return 0;
    if (shot.elevation > map_.elevation_at(victim.pos)) return 0;  // fired down into it
    const TilePos tile = map_.clamp_tile(tile_of(victim.pos));
    const Structure* works = structure_at(tile);
    if (!works || !is_fieldwork(works->type)) {
        // A shell crater hides a man lying in it, not a vehicle.
        if (map_.terrain(tile) != Terrain::Crater || def_of(victim).vehicle || victim.airborne) return 0;
        if (shot.blast ? tile_of(shot.from) == tile : (shot.from - victim.pos).length_sq_raw() <= square_raw(kCloseQuarters)) {
            return 0;
        }
        return crater_cover(map_.crater_kind(tile.x, tile.y));
    }
    if (shot.blast) {
        if (tile_of(shot.from) == tile) return 0;  // burst right in it
    } else if ((shot.from - victim.pos).length_sq_raw() <= square_raw(kCloseQuarters)) {
        return 0;  // a trench fight
    }
    int32_t cover = 0;
    if (works->type == StructureType::Foxhole) cover = kFoxholeCover;
    if (works->type == StructureType::GunPit) cover = kGunPitCover;
    if (works->type == StructureType::Trench) cover = victim.still >= kSettleTicks ? kTrenchCover : kTrenchWalkingCover;
    if (works->parapet) {
        const FixedVec2 to_shot = shot.from - victim.pos;
        const int64_t front = static_cast<int64_t>(to_shot.x.raw) * works->facing.x.raw +
                              static_cast<int64_t>(to_shot.y.raw) * works->facing.y.raw;
        if (front > 0) cover = 100 - (100 - cover) * (100 - kParapetCover) / 100;
    }
    return cover;
}

void World::hurt(const Unit& victim, const WeaponDef& weapon, const Shot& shot) {
    // The walls of a trench or foxhole take it instead.
    if (const int32_t cover = cover_percent(victim, shot);
        cover > 0 && static_cast<int32_t>(rng_.next_below(100)) < cover) {
        return;
    }
    const auto type = static_cast<size_t>(weapon.damage_type);
    int32_t amount = std::max(1, weapon.damage - def_of(victim).armor[type]);
    amount = amount * shot.damage_percent / 100;
    if (weapon.damage_type == DamageType::AntiTank && def_of(victim).vehicle && !shot.blast &&
        from_flank(victim, shot.from)) {
        amount = amount * kFlankHitPercent / 100;  // into the side or the rear
    }
    // A tank's (an IFV's) armor in front: what it lets through of an anti-tank hit there.
    const bool vd_tank = def_of(victim).tank;
    if (is_armor(def_of(victim)) && weapon.damage_type == DamageType::AntiTank && !shot.blast && !from_flank(victim, shot.from)) {
        amount = amount * def_of(victim).front_percent / 100;
    }
    // What our upgrades take off it.
    const UnitTypeDef& vd = def_of(victim);
    if (vd_tank && weapon.damage_type == DamageType::AntiTank && !shot.blast) {
        const int level = std::min(era_level(victim.owner), def_of(victim).era_max);
        if (level > 0) {
            const EraLevel& era = kEraLevels[level - 1];
            amount = amount * (weapon.kinetic ? era.kinetic_percent : era.shaped_percent) / 100;
        }
    }
    if (!vd.vehicle && !vd.aircraft && weapon.damage_type != DamageType::AntiTank &&
        has_upgrade(victim.owner, UpgradeId::BodyArmor)) {
        amount = amount * kBodyArmorPercent / 100;
    }
    if (vd.aircraft && has_upgrade(victim.owner, UpgradeId::CockpitArmor)) amount = amount * kCockpitArmorPercent / 100;
    if (is_armor(vd) && weapon.damage_type != DamageType::AntiTank && has_upgrade(victim.owner, UpgradeId::AddOnArmor)) {
        amount = amount * kAddOnArmorPercent / 100;
    }

    const uint8_t victim_elevation = map_.elevation_at(victim.pos);
    if (!victim.airborne && shot.elevation > victim_elevation) amount = amount * kHighGroundPercent / 100;
    if (!victim.airborne && shot.elevation < victim_elevation) amount = amount * kLowGroundPercent / 100;

    pending_damage_.push_back({victim.id, std::max(1, amount), !shot.blast || shot.on_it});
}

void World::apply_damage_and_remove_dead() {
    for (const PendingDamage& d : pending_damage_) {
        if (Unit* victim = find_unit_mut(d.victim)) {
            if (victim->hp > 0 && victim->hp - d.amount <= 0) victim->killed_directly = d.direct;  // the blow that did it
            victim->hp -= d.amount;
        } else if (Structure* s = find_structure_mut(d.victim)) {
            s->hp -= d.amount;
        }
    }
    pending_damage_.clear();

    // Collapses first: they kill whoever is inside or on top.
    bool map_changed = false;
    for (const Structure& s : structures_) {
        if (s.hp > 0) continue;
        collapse(s);
        map_changed = true;
    }
    if (map_changed) {
        std::erase_if(structures_, [](const Structure& s) { return s.hp <= 0; });
        on_map_changed();
    }

    // An IFV knocked out: the squad bails out, knocked about; the worse off
    // don't make it.
    for (const Unit& v : units_) {
        if (v.hp > 0 || v.passengers.empty()) continue;
        const std::vector<EntityId> aboard = v.passengers;
        for (EntityId id : aboard) {
            Unit* p = find_unit_mut(id);
            if (!p) continue;
            leave_structure(*p);
            p->order = Order::Idle;
            p->hp -= unit_type(p->type).max_hp * kBailOutHurtPercent / 100;
        }
    }

    // A tanker or an ammunition truck hit with its load aboard goes up.
    for (const Unit& u : units_) {
        if (u.hp > 0 || u.carrying <= 0 || def_of(u).supplies == Resource::Count) continue;
        static constexpr WeaponDef kTankerFire{.name = "Burning fuel", .damage = 60,
                                               .damage_type = DamageType::Explosive, .range = Fixed{}, .reload = 0,
                                               .projectile_speed = Fixed{}, .splash_radius = Fixed::from_ratio(3, 2),
                                               .accuracy = 100, .miss_spread = Fixed{}};
        static constexpr WeaponDef kAmmoCookOff{.name = "Cooking-off rounds", .damage = 45,
                                                .damage_type = DamageType::Explosive, .range = Fixed{}, .reload = 0,
                                                .projectile_speed = Fixed{}, .splash_radius = Fixed::from_int(1),
                                                .accuracy = 100, .miss_spread = Fixed{}};
        burst_into_flames(u.pos, u.owner, def_of(u).supplies == Resource::Fuel ? kTankerFire : kAmmoCookOff);
    }
    // A tank killed by a direct hit with its racks more than a quarter full
    // goes up: the rounds cook off round it, the turret's thrown off.
    auto blows_up = [&](const Unit& u) {
        return def_of(u).tank && u.killed_directly && u.rounds * 100 > def_of(u).rounds_capacity * kBlowUpRoundsPercent;
    };
    for (const Unit& u : units_) {
        if (u.hp > 0 || !blows_up(u)) continue;
        static constexpr WeaponDef kRoundsGoingUp{.name = "A tank's rounds going up", .damage = 45,
                                                  .damage_type = DamageType::Explosive, .range = Fixed{}, .reload = 0,
                                                  .projectile_speed = Fixed{}, .splash_radius = Fixed::from_int(1),
                                                  .accuracy = 100, .miss_spread = Fixed{}};
        burst_into_flames(u.pos, u.owner, kRoundsGoingUp, u.id);
    }
    // A tank (an IFV, a gun, an aircraft) knocked out: its crew may get out
    // (the men come back to the pool), unless its rounds went up.
    for (const Unit& u : units_) {
        if (u.hp > 0 || def_of(u).family == Family::None || u.owner >= kMaxPlayers || blows_up(u)) continue;
        // Sunk in a bog, slowly: the crew gets out. Knocked out: as its armour lets them.
        if (u.mired >= kBogLimit || static_cast<int32_t>(rng_.next_below(100)) < def_of(u).crew_survives_percent) {
            stock_[u.owner][static_cast<size_t>(Resource::Personnel)] += def_of(u).cost[static_cast<size_t>(Resource::Personnel)];
        }
    }
    // A vehicle knocked out burns: its smoke hides what's behind it a while
    // (not one gone under the water or into a bog).
    for (const Unit& u : units_) {
        if (u.hp > 0 || !def_of(u).vehicle || u.inside) continue;
        if (map_.terrain_at(u.pos) == Terrain::Water || u.mired >= kBogLimit) continue;
        smokes_.push_back({clamp_to_map(u.pos + kPlumeDrift, Fixed{}), kPlumeRadius, tick_ + kPlumeTicks, SmokeKind::Plume, tick_});
    }
    std::erase_if(units_, [](const Unit& u) { return u.hp <= 0; });
    for (Structure& s : structures_) {
        std::erase_if(s.garrison, [this](EntityId id) { return find_unit(id) == nullptr; });
        if (s.garrison.empty() && is_shelter(role_of(s))) s.owner = kNoOwner;
    }
}

// Pushes overlapping units apart, lighter and parked units giving way more,
// but never into terrain they can't stand on.
// O(n^2): fine for a few hundred units, replace with a spatial grid once
// armies get bigger.
void World::separate_units() {
    for (size_t i = 0; i < units_.size(); ++i) {
        for (size_t j = i + 1; j < units_.size(); ++j) {
            Unit& a = units_[i];
            Unit& b = units_[j];
            if (a.inside || b.inside || def_of(a).aircraft || def_of(b).aircraft) continue;

            const FixedVec2 delta = b.pos - a.pos;
            const Fixed min_dist = def_of(a).radius + def_of(b).radius;
            if (delta.length_sq_raw() >= square_raw(min_dist)) continue;

            const Fixed dist = delta.length();
            const FixedVec2 dir = dist.raw == 0 ? FixedVec2{Fixed::from_int(1), Fixed{}}
                                                : FixedVec2{delta.x / dist, delta.y / dist};
            const Fixed overlap = min_dist - dist;

            const int32_t weight_a = def_of(a).mass * (a.moving ? kMovingWeight : 1);
            const int32_t weight_b = def_of(b).mass * (b.moving ? kMovingWeight : 1);
            const Fixed push_a = overlap * Fixed::from_ratio(weight_b, weight_a + weight_b);

            const FixedVec2 new_a = a.pos - dir * push_a;
            const FixedVec2 new_b = b.pos + dir * (overlap - push_a);
            if (can_stand(a, new_a)) a.pos = new_a;
            if (can_stand(b, new_b)) b.pos = new_b;
        }
    }
}

FixedVec2 World::clamp_to_map(FixedVec2 p, Fixed margin) const {
    const FixedVec2 size = map_.size();
    return {clamp(p.x, margin, size.x - margin), clamp(p.y, margin, size.y - margin)};
}

uint64_t World::checksum() const {
    // FNV-1a over every field that affects the game.
    uint64_t hash = 14695981039346656037ULL;
    auto mix = [&hash](uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= (value >> (i * 8)) & 0xFF;
            hash *= 1099511628211ULL;
        }
    };
    auto mix_fixed = [&mix](Fixed f) { mix(static_cast<uint32_t>(f.raw)); };
    auto mix_vec = [&mix_fixed](FixedVec2 v) {
        mix_fixed(v.x);
        mix_fixed(v.y);
    };

    mix(tick_);
    mix(rng_.state());
    mix(next_id_);
    mix(next_projectile_id_);
    mix(static_cast<uint32_t>(map_.width()));
    mix(static_cast<uint32_t>(map_.height()));
    for (const Unit& u : units_) {
        mix(u.id);
        mix(u.owner);
        mix(static_cast<uint8_t>(u.type));
        mix(static_cast<uint32_t>(u.hp));
        mix_vec(u.pos);
        mix_vec(u.facing);
        mix_vec(u.hull);
        mix(u.moving ? 1 : 0);
        mix(static_cast<uint8_t>(u.order));
        mix_vec(u.order_point);
        mix(static_cast<uint32_t>(u.order_goal.x));
        mix(static_cast<uint32_t>(u.order_goal.y));
        mix_fixed(u.speed_cap);
        mix(u.order_target);
        mix(u.engaged);
        mix(u.cooldown);
        mix(u.last_shot_tick);
        mix_vec(u.last_shot_at);
        mix(u.inside);
        mix(static_cast<uint32_t>(u.passengers.size()));
        for (EntityId id : u.passengers) mix(id);
        mix(static_cast<uint32_t>(u.gather_tile.x));
        mix(static_cast<uint32_t>(u.gather_tile.y));
        mix(static_cast<uint32_t>(u.carrying));
        mix(static_cast<uint8_t>(u.carrying_type));
        mix(u.work);
        mix(u.seen_by);
        mix(u.round_type);
        for (Tick t : u.ability_ready) mix(t);
        mix(static_cast<uint8_t>(u.order_ability));
        mix_vec(u.order_point2);
        mix(static_cast<uint32_t>(u.shots_left));
        mix(u.still);
        mix_fixed(u.fuel);
        mix(static_cast<uint32_t>(u.rounds));
        mix(static_cast<uint32_t>(u.missiles));
        mix(static_cast<uint8_t>(u.shell));
        mix(u.deployed ? 1 : 0);
        mix(static_cast<uint32_t>(u.mired));
        mix(static_cast<uint32_t>(u.laid) | static_cast<uint32_t>(u.laying_to) << 8);
        mix(static_cast<uint32_t>(u.lay_work));
        mix(u.lock);
        mix(static_cast<uint32_t>(u.lock_ticks));
        mix(u.deploy_work);
        mix_vec(u.ranging_point);
        mix(u.ranging_shots);
        mix(u.camouflaged ? 1 : 0);
        mix(u.perfect_burst ? 1 : 0);
        mix(u.silent ? 1 : 0);
        mix(u.airborne ? 1 : 0);
        mix(static_cast<uint8_t>(u.haul_cargo));
        mix(u.haul_depot);
        mix(u.serves);
        mix(u.on_call ? 1 : 0);
        mix(u.delivering ? 1 : 0);
    }
    for (const Courier& c : couriers_) {
        mix(c.arrives);
        mix(static_cast<uint8_t>(c.cmd.type));
        mix(c.cmd.player);
        for (EntityId id : c.cmd.units) mix(id);
        mix_vec(c.cmd.target);
        mix(c.cmd.target_unit);
        mix(c.cmd.unit_type);
        mix(c.cmd.structure_type);
        mix(c.cmd.ability);
        mix_vec(c.cmd.target_end);
        mix(c.cmd.upgrade);
        mix(c.cmd.cargo);
    }
    for (const Mine& m : mines_) {
        mix(m.id);
        mix(m.owner);
        mix(static_cast<uint32_t>(m.tile.x));
        mix(static_cast<uint32_t>(m.tile.y));
        mix(m.anti_tank ? 1 : 0);
        mix(m.found_by);
    }
    mix(next_mine_id_);
    for (uint32_t bits : upgrades_) mix(bits);
    for (bool h : hungry_) mix(h ? 1 : 0);
    for (const Smoke& s : smokes_) {
        mix_vec(s.center);
        mix_fixed(s.radius);
        mix(s.clears);
        mix(static_cast<uint32_t>(s.kind));
        mix(s.made);
    }
    for (const Fire& f : fires_) {
        mix_vec(f.center);
        mix_fixed(f.radius);
        mix(f.until);
        mix(f.owner);
    }
    for (const Structure& s : structures_) {
        mix(static_cast<uint8_t>(s.research));
        mix(s.research_progress);
    }
    for (const Charge& c : charges_) {
        mix(c.owner);
        mix(c.target);
        mix_vec(c.pos);
        mix(c.goes_off);
    }
    for (const Structure& s : structures_) {
        mix(s.id);
        mix(static_cast<uint8_t>(s.type));
        mix(s.owner);
        mix(static_cast<uint32_t>(s.hp));
        mix(s.tiles.size());
        for (EntityId id : s.garrison) mix(id);
        for (UnitTypeId t : s.queue) mix(static_cast<uint8_t>(t));
        mix(s.progress);
        mix(s.built ? 1 : 0);
        mix(s.build_progress);
        mix(s.bombed);
        for (int32_t amount : s.cargo) mix(static_cast<uint32_t>(amount));
        mix(s.next_train);
        mix(s.parapet ? 1 : 0);
        mix_vec(s.facing);
        mix(s.upgrading ? 1 : 0);
        mix(s.upgrade_work);
        mix(static_cast<uint8_t>(s.converted));
        mix(static_cast<uint32_t>(s.cache));
        mix(s.cache_owner);
        mix_vec(s.rally);
        mix(s.rally_set ? 1 : 0);
    }
    for (const auto& [tile, work] : dig_work_) {
        mix(static_cast<uint32_t>(tile));
        mix(work);
    }
    for (const Stock& st : stock_) {
        for (int32_t amount : st) mix(static_cast<uint32_t>(amount));
    }
    mix(map_.revision());
    for (const Projectile& p : projectiles_) {
        mix(p.id);
        mix(p.owner);
        mix(p.shooter);
        mix(static_cast<uint8_t>(p.shooter_type));
        mix(p.shooter_elevation);
        mix_vec(p.origin);
        mix_vec(p.pos);
        mix_vec(p.target);
        mix_fixed(p.origin_height);
        mix_fixed(p.target_height);
        mix(static_cast<uint32_t>(p.weapon.damage));
        mix(static_cast<uint8_t>(p.weapon.damage_type));
        mix_fixed(p.weapon.projectile_speed);
        mix_fixed(p.weapon.splash_radius);
        mix(p.lobbed ? 1 : 0);
        mix(p.enters ? 1 : 0);
        mix(p.at_air ? 1 : 0);
        mix(p.homing);
    }
    return hash;
}

}  // namespace engine
