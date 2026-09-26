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
    units_.push_back(u);  // ids only grow, so the vector stays sorted
    return u.id;
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
            if ((terrain != Terrain::House && terrain != Terrain::Bridge) || structure_id_at({x, y}) != 0) continue;

            Structure s;
            s.id = next_id_++;
            s.type = terrain == Terrain::House ? StructureType::House : StructureType::Bridge;
            s.hp = structure_type(s.type).max_hp;

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
    const Structure* s = find_structure(cmd.target_unit);
    if (!s || structure_type(s->type).capacity == 0) return;
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

void World::seek_garrison(Unit& u) {
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
        if (s->garrison.empty() && s->type == StructureType::House) s->owner = kNoOwner;
        from = s->center;
    }
    u.inside = 0;
    if (auto door = nearest_passable(map_, tile_of(from), class_of(u))) u.pos = tile_center(*door);
    u.prev_pos = u.pos;
}

// Garrisoned soldiers can't move; they fire from the windows at whatever
// comes into sight and range.
void World::update_garrisoned(Unit& u) {
    if (u.order == Order::Retrain) return update_retrain(u);  // at drill in the headquarters
    const Unit* target = find_enemy_in_sight(u);
    if (!target) return;
    const UnitTypeDef& def = def_of(u);
    const Fixed reach = def.weapon.range + def.radius + def_of(*target).radius;
    const FixedVec2 to_target = target->pos - u.pos;
    if (to_target.length_sq_raw() > square_raw(reach)) return;
    if (to_target.x.raw != 0 || to_target.y.raw != 0) u.facing = to_target;
    if (u.cooldown == 0) try_fire(u, target->pos, target);
}

void World::hurt_structure(const Structure& s, const WeaponDef& weapon) {
    if (weapon.damage_type == DamageType::Bullet) return;  // rifles don't knock down walls
    const int32_t amount = weapon.damage - structure_type(s.type).armor[static_cast<size_t>(weapon.damage_type)];
    if (amount > 0) pending_damage_.push_back({s.id, amount});
}

// A house comes down on everyone inside; a bridge drops whoever is on it
// into the river.
void World::collapse(const Structure& s) {
    if (s.type == StructureType::FuelDepot) burn_fuel_depot(s);
    if (s.type != StructureType::Bridge) {
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
    const Terrain rubble = s.type == StructureType::Bridge ? Terrain::Water : Terrain::Ruins;
    for (const TilePos& t : s.tiles) {
        map_.set_terrain(t.x, t.y, rubble);
        structure_tiles_[static_cast<size_t>(t.y * map_.width() + t.x)] = 0;
    }
}

// Terrain changed: every cached route may now lead through a river.
void World::on_map_changed() { field_cache_.clear(); }

// --- Commands ----------------------------------------------------------------

void World::apply(const Command& cmd) {
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
        case CommandType::Stop: apply_stop(cmd); break;
    }
}

void World::apply_group_move(const Command& cmd, Order order) {
    std::vector<Unit*> group = collect_owned(cmd, [this](EntityId id) { return find_unit_mut(id); });
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
        if (!is_armed(def_of(*u))) continue;
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
    update_trains();
    update_production();
    for (Unit& u : units_) {
        u.prev_pos = u.pos;
        u.moving = false;
    }

    for (Unit& u : units_) update_unit(u);
    // Projectiles move after units, so a unit that stepped aside this tick dodges.
    move_projectiles();
    apply_damage_and_remove_dead();

    separate_units();
    for (Unit& u : units_) u.pos = clamp_to_map(u.pos, def_of(u).radius);

    while (!recent_impacts_.empty() && recent_impacts_.front().tick + kImpactHistory < tick_) {
        recent_impacts_.pop_front();
    }
    ++tick_;
}

void World::update_unit(Unit& u) {
    if (u.cooldown > 0) --u.cooldown;
    if (u.inside) return update_garrisoned(u);

    auto finish_order = [&u] {
        u.order = Order::Idle;
        u.order_path.reset();
        u.speed_cap = Fixed{};
    };

    switch (u.order) {
        case Order::Idle:
            if (!is_armed(def_of(u))) break;
            if (const Unit* target = find_enemy_in_sight(u)) engage(u, *target);
            break;

        case Order::Move:
            if (navigate(u, u.order_point, u.order_path, u.order_goal, true) != Step::Moved) finish_order();
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
    }
}

// Keeps shooting the current enemy while it stays in sight, otherwise picks
// the nearest one (ties go to the lower id, so every peer picks the same).
// Only enemies the player sees count: nobody shoots into the fog.
const Unit* World::find_enemy_in_sight(Unit& u) {
    const uint64_t sight_sq = square_raw(def_of(u).sight);

    if (const Unit* current = find_unit(u.engaged);
        current && sees(u.owner, *current) && (current->pos - u.pos).length_sq_raw() <= sight_sq) {
        return current;
    }

    const Unit* best = nullptr;
    uint64_t best_sq = 0;
    for (const Unit& other : units_) {
        if (other.owner == u.owner || !sees(u.owner, other)) continue;
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
    const UnitTypeDef& def = def_of(u);
    const Fixed reach = def.weapon.range + def.radius + def_of(target).radius;
    const FixedVec2 to_target = target.pos - u.pos;

    // Out of range, or in range but a hill or house hides the target: move in.
    // Chase at full speed, around obstacles if needed. If the target is
    // somewhere we can't go (a tank vs. infantry in a forest), wait at the edge.
    if (to_target.length_sq_raw() > square_raw(reach)) {
        navigate(u, target.pos, u.chase_path, tile_of(target.pos), false);
        return;
    }
    if (to_target.x.raw != 0 || to_target.y.raw != 0) u.facing = to_target;
    if (u.cooldown == 0 && !try_fire(u, target.pos, &target)) {
        navigate(u, target.pos, u.chase_path, tile_of(target.pos), false);
    }
}

void World::engage_ground(Unit& u) {
    const UnitTypeDef& def = def_of(u);
    const Fixed reach = def.weapon.range + def.radius;
    const FixedVec2 to_point = u.order_point - u.pos;

    if (to_point.length_sq_raw() > square_raw(reach)) {
        navigate(u, u.order_point, u.order_path, u.order_goal, false);
        return;
    }
    if (to_point.x.raw != 0 || to_point.y.raw != 0) u.facing = to_point;
    if (u.cooldown == 0 && !try_fire(u, u.order_point, nullptr)) {
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
    Fixed speed = def.speed;
    if (formation && u.speed_cap.raw > 0) speed = min(speed, u.speed_cap);
    // Terrain slows down (forest for infantry, villages for vehicles...).
    const int32_t terrain_pct = map_.speed_percent(tile_of(u.pos), move_class(def));
    if (terrain_pct > 0) speed = speed * terrain_pct / 100;

    u.facing = to_point;
    const FixedVec2 next = dist <= speed ? point : u.pos + to_point * (speed / dist);
    if (!move_to(u, next)) return Step::Blocked;
    u.moving = true;
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

World::FireLine World::fire_line(const Unit& shooter, FixedVec2 aim, Fixed aim_height) const {
    const Fixed muzzle = shooter.inside ? kWindowHeight : muzzle_height(shooter);
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
        if (terrain == Terrain::Building && h < ground + kBuildingHeight && structure_id_at(tile) != own_structure) {
            return Obstruction::Terrain;
        }
        if (terrain == Terrain::Rock && h < ground + kRockHeight) return Obstruction::Terrain;
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
        if (u.id == ignore || u.inside) continue;  // the garrison is behind walls
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
        if (u.owner != shooter.owner || u.id == shooter.id || u.inside) continue;
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

bool World::try_fire(Unit& shooter, FixedVec2 aim, const Unit* target) {
    // Someone in a house is shot at through the house: aim at the windows.
    Fixed aim_height = map_.surface_height(aim) + kGroundAim;
    if (target) aim_height = map_.surface_height(aim) + (target->inside ? kWindowHeight : center_height(*target));
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
    fire(shooter, aim, aim_height);
    return true;
}

// --- Combat ------------------------------------------------------------------

void World::fire(Unit& shooter, FixedVec2 aim, Fixed aim_height) {
    const WeaponDef& weapon = def_of(shooter).weapon;
    shooter.cooldown = weapon.reload;

    // A miss lands somewhere near the aim point, on the ground.
    if (rng_.next_below(100) >= weapon.accuracy) {
        aim.x += Fixed::from_raw(rng_.next_range(-weapon.miss_spread.raw, weapon.miss_spread.raw));
        aim.y += Fixed::from_raw(rng_.next_range(-weapon.miss_spread.raw, weapon.miss_spread.raw));
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

    const uint8_t elevation = map_.elevation_at(shooter.pos);
    shooter.last_shot_tick = tick_;

    if (instant) {
        // Bullets hit whoever is first in the line: the target, someone else, or nobody.
        Fixed hit_t;
        if (const Unit* victim = first_unit_on(line, start, end, shooter.id, hit_t)) {
            hurt(*victim, weapon, elevation);
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
    shooter.last_shot_at = p.target;
    projectiles_.push_back(p);
}

void World::move_projectiles() {
    for (Projectile& p : projectiles_) {
        p.prev_pos = p.pos;
        const Fixed speed = unit_type(p.shooter_type).weapon.projectile_speed;
        const FixedVec2 to_target = p.target - p.pos;
        const bool arrived = to_target.length() <= speed;
        p.pos = arrived ? p.target : p.pos + to_target * (speed / to_target.length());

        // Whoever is in the path this tick (moved into it, or was standing
        // there all along) takes the hit.
        const FireLine line{p.origin, p.origin_height, p.target, p.target_height};
        const Fixed total = (p.target - p.origin).length();
        if (total.raw > 0) {
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

void World::explode(const Projectile& p, FixedVec2 at, const Unit* direct_hit) {
    recent_impacts_.push_back({tick_, at, p.shooter_type});
    const WeaponDef& weapon = unit_type(p.shooter_type).weapon;

    if (weapon.splash_radius.raw > 0) {
        // Explosions don't care whose units they hit (except the gun that
        // fired). A garrison is safe behind its walls until the house falls.
        for (const Unit& u : units_) {
            if (u.id == p.shooter || u.inside) continue;
            if ((u.pos - at).length_sq_raw() <= square_raw(weapon.splash_radius + def_of(u).radius)) {
                hurt(u, weapon, p.shooter_elevation);
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
        return;
    }

    if (!direct_hit) {
        // A rocket into a wall hits the house.
        if (const Structure* s = structure_at(tile_of(at))) return hurt_structure(*s, weapon);
        // Landed: hits whoever stands right at the spot, friend or foe.
        uint64_t best_sq = 0;
        for (const Unit& u : units_) {
            if (u.id == p.shooter || u.inside) continue;
            const uint64_t d = (u.pos - at).length_sq_raw();
            if (d <= square_raw(def_of(u).radius + kHitTolerance) && (!direct_hit || d < best_sq)) {
                direct_hit = &u;
                best_sq = d;
            }
        }
    }
    if (direct_hit) hurt(*direct_hit, weapon, p.shooter_elevation);
}

void World::hurt(const Unit& victim, const WeaponDef& weapon, uint8_t attacker_elevation) {
    const auto type = static_cast<size_t>(weapon.damage_type);
    int32_t amount = std::max(1, weapon.damage - def_of(victim).armor[type]);

    const uint8_t victim_elevation = map_.elevation_at(victim.pos);
    if (attacker_elevation > victim_elevation) amount = amount * kHighGroundPercent / 100;
    if (attacker_elevation < victim_elevation) amount = amount * kLowGroundPercent / 100;

    pending_damage_.push_back({victim.id, std::max(1, amount)});
}

void World::apply_damage_and_remove_dead() {
    for (const PendingDamage& d : pending_damage_) {
        if (Unit* victim = find_unit_mut(d.victim)) {
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

    std::erase_if(units_, [](const Unit& u) { return u.hp <= 0; });
    for (Structure& s : structures_) {
        std::erase_if(s.garrison, [this](EntityId id) { return find_unit(id) == nullptr; });
        if (s.garrison.empty() && s.type == StructureType::House) s.owner = kNoOwner;
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
            if (a.inside || b.inside) continue;

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
        mix(static_cast<uint32_t>(u.gather_tile.x));
        mix(static_cast<uint32_t>(u.gather_tile.y));
        mix(static_cast<uint32_t>(u.carrying));
        mix(static_cast<uint8_t>(u.carrying_type));
        mix(u.work);
        mix(u.seen_by);
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
        for (int32_t amount : s.cargo) mix(static_cast<uint32_t>(amount));
        mix(s.next_train);
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
    }
    return hash;
}

}  // namespace engine
