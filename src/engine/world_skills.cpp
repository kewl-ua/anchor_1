// The skills half of World: what the command-grid buttons do.

#include <algorithm>
#include <cstdlib>
#include <optional>

#include "engine/heights.h"
#include "engine/world.h"

namespace engine {

namespace {

// A machine-gun sweep: this many rounds, one every few ticks, scattered
// across the front up to this far either side of the line at full range.
constexpr int32_t kSweepShots = 15;
constexpr Tick kSweepInterval = 3;
constexpr Fixed kSweepSpread = Fixed::from_int(3);
// How close a digger has to be to the middle of the tile he digs.
constexpr Fixed kDigReach = Fixed::from_ratio(3, 4);

}  // namespace

std::vector<TilePos> trench_line(TilePos a, TilePos b) {
    std::vector<TilePos> line{a};
    TilePos t = a;
    while (t != b && static_cast<int32_t>(line.size()) < kMaxTrenchLength) {
        if (std::abs(b.x - t.x) >= std::abs(b.y - t.y)) {
            t.x += b.x > t.x ? 1 : -1;
        } else {
            t.y += b.y > t.y ? 1 : -1;
        }
        line.push_back(t);
    }
    return line;
}

bool World::diggable(TilePos t) const {
    if (!map_.contains(t) || structure_id_at(t) != 0) return false;
    const Terrain terrain = map_.terrain(t);
    return terrain == Terrain::Grass || terrain == Terrain::Urban || terrain == Terrain::Ruins;
}

EntityId World::place_fieldwork(StructureType type, PlayerId owner, TilePos t, FixedVec2 facing) {
    const EntityId id = place_structure(type, owner, t, 1, 1);
    if (Structure* s = find_structure_mut(id)) {
        s->parapet = type == StructureType::Parapet;
        s->facing = facing;
    }
    return id;
}

// The men standing in a foxhole dig it out into a dugout; paid up front.
void World::apply_upgrade(const Command& cmd) {
    Structure* s = find_structure_mut(cmd.target_unit);
    if (!s || s->owner != cmd.player || s->type != StructureType::Foxhole || s->upgrading) return;
    Stock& stock = stock_[cmd.player % kMaxPlayers];
    if (!can_afford(stock, kDugoutCost)) return;
    pay(stock, kDugoutCost);
    s->upgrading = true;
    s->upgrade_work = 0;
}

void World::apply_unload(const Command& cmd) {
    const Structure* s = find_structure(cmd.target_unit);
    if (!s || s->owner != cmd.player || !is_shelter(s->type)) return;
    const std::vector<EntityId> inside = s->garrison;
    for (EntityId id : inside) {
        if (Unit* u = find_unit_mut(id)) {
            leave_structure(*u);
            u->order = Order::Idle;
        }
    }
}

void World::update_upgrades() {
    for (Structure& s : structures_) {
        if (!s.upgrading) continue;
        const TilePos tile = s.tiles.front();
        std::vector<Unit*> diggers;
        for (Unit& u : units_) {
            if (u.owner != s.owner || u.inside || unit_type(u.type).vehicle || tile_of(u.pos) != tile) continue;
            if (static_cast<int32_t>(diggers.size()) < kDugoutDiggers) diggers.push_back(&u);
        }
        s.upgrade_work += static_cast<Tick>(diggers.size());
        if (s.upgrade_work < kDugoutWork) continue;

        // Done: roofed over, and the men who dug it are inside.
        s.upgrading = false;
        s.type = StructureType::Dugout;
        s.hp = structure_type(StructureType::Dugout).max_hp;
        s.parapet = false;
        s.owner = kNoOwner;
        map_.set_terrain(tile.x, tile.y, Terrain::Dugout);
        on_map_changed();
        for (Unit* u : diggers) {
            u->order = Order::Idle;
            enter(*u, s);
        }
    }
}

void World::apply_ability(const Command& cmd) {
    if (cmd.ability >= kAbilityCount) return;
    const auto id = static_cast<AbilityId>(cmd.ability);
    for (EntityId unit_id : cmd.units) {
        Unit* u = find_unit_mut(unit_id);
        if (!u || u->owner != cmd.player) continue;
        const int slot = ability_slot(unit_type(u->type), id);
        if (slot < 0 || u->ability_ready[static_cast<size_t>(slot)] > tick_) continue;

        if (id == AbilityId::SwitchAmmo) {
            if (unit_type(u->type).alt_weapon.damage > 0) {
                // The round in the breech has to come out: a full reload.
                u->round_type ^= 1;
                u->cooldown = std::max(u->cooldown, weapon_of(*u).reload);
            }
            continue;
        }
        leave_structure(*u);
        u->order = Order::Ability;
        u->order_ability = id;
        // A foxhole is dug where the man stands; a refill or a deploy needs no point at all.
        const bool here = id == AbilityId::DigFoxhole || id == AbilityId::Refill || id == AbilityId::Deploy;
        u->order_point = here ? u->pos : clamp_to_map(cmd.target, Fixed{});
        u->order_point2 = clamp_to_map(cmd.target_end, Fixed{});
        u->order_goal = map_.clamp_tile(tile_of(u->order_point));
        u->order_path.reset();
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->order_target = 0;
        u->engaged = 0;
        u->shots_left = id == AbilityId::MgSweep ? kSweepShots : 0;
        if (id == AbilityId::Deploy) u->shots_left = u->deployed ? 0 : 1;  // which way: set up or pack up
        u->work = 0;
    }
}

// Done: the skill cools down, the unit stands by.
void World::finish_ability(Unit& u) {
    const int slot = ability_slot(unit_type(u.type), u.order_ability);
    if (slot >= 0) u.ability_ready[static_cast<size_t>(slot)] = tick_ + ability_def(u.order_ability).cooldown;
    u.order = Order::Idle;
    u.order_path.reset();
    u.shots_left = 0;
    u.work = 0;
}

void World::update_ability(Unit& u) {
    const UnitTypeDef& def = unit_type(u.type);
    const AbilityDef& ability = ability_def(u.order_ability);
    const FixedVec2 to_point = u.order_point - u.pos;
    const bool at_point = to_point.x.raw == 0 && to_point.y.raw == 0;

    switch (u.order_ability) {
        case AbilityId::AreaShot: {
            // Like firing at a spot, but one wide-bursting shell and done.
            if (to_point.length_sq_raw() > square_raw(ability.weapon.range + def.radius)) {
                navigate(u, u.order_point, u.order_path, u.order_goal, false);
                return;
            }
            if (!at_point) u.facing = to_point;
            if (u.cooldown > 0) return;  // still loading
            if (!try_fire(u, u.order_point, nullptr, ability.weapon)) {
                navigate(u, u.order_point, u.order_path, u.order_goal, false);  // a hill in the way
            } else if (u.last_shot_tick == tick_) {
                finish_ability(u);
            }  // otherwise holding fire: our men are in the line
            return;
        }

        case AbilityId::MgSweep: {
            // Along the front, not at anyone: rounds scatter across it and hit
            // whoever is in the way, seen or not, ours included.
            if (at_point) return finish_ability(u);
            u.facing = to_point;
            if (++u.work % kSweepInterval != 0) return;
            const FixedVec2 dir = to_point * (Fixed::from_int(1) / to_point.length());
            const FixedVec2 across{-dir.y, dir.x};
            const Fixed offset = Fixed::from_raw(rng_.next_range(-kSweepSpread.raw, kSweepSpread.raw));
            const FixedVec2 aim = clamp_to_map(u.pos + dir * ability.range + across * offset, Fixed{});
            fire(u, aim, map_.surface_height(aim) + kInfantryCenter, ability.weapon, false);  // the coaxial's own belt
            if (--u.shots_left <= 0) finish_ability(u);
            return;
        }

        case AbilityId::LobGrenade:
        case AbilityId::ThrowGrenade: {
            if (to_point.length_sq_raw() > square_raw(ability.range + def.radius)) {
                navigate(u, u.order_point, u.order_path, u.order_goal, false);
                return;
            }
            if (!at_point) u.facing = to_point;
            lob(u, u.order_point, ability.weapon, u.order_ability == AbilityId::ThrowGrenade);
            return finish_ability(u);
        }

        case AbilityId::DigTrench: {
            // The nearest tile of the line still to dig; everyone on the
            // order digs there together.
            std::optional<TilePos> best;
            uint64_t best_sq = 0;
            for (const TilePos& t : trench_line(tile_of(u.order_point), tile_of(u.order_point2))) {
                if (!diggable(t)) continue;
                const uint64_t d = (tile_center(t) - u.pos).length_sq_raw();
                if (!best || d < best_sq) {
                    best = t;
                    best_sq = d;
                }
            }
            if (!best) return finish_ability(u);
            if (best_sq > square_raw(kDigReach)) {
                navigate(u, tile_center(*best), u.order_path, *best, false);
                return;
            }
            const int32_t index = best->y * map_.width() + best->x;
            if (++dig_work_[index] >= kTrenchWork) {
                dig_work_.erase(index);
                place_fieldwork(StructureType::Trench, u.owner, *best, {});
            }
            return;
        }

        case AbilityId::DigFoxhole: {
            const TilePos t = map_.clamp_tile(tile_of(u.order_point));
            if (!diggable(t)) return finish_ability(u);
            if ((tile_center(t) - u.pos).length_sq_raw() > square_raw(kDigReach)) {
                navigate(u, tile_center(t), u.order_path, t, false);  // pushed off the spot
                return;
            }
            if (++u.work < kFoxholeWork) return;
            place_fieldwork(StructureType::Foxhole, u.owner, t, {});
            return finish_ability(u);
        }

        case AbilityId::BuildParapet: {
            // On his own tile: on top of a trench or foxhole, or a mound on open ground.
            if (at_point) return finish_ability(u);
            const TilePos t = map_.clamp_tile(tile_of(u.pos));
            Structure* works = find_structure_mut(structure_id_at(t));
            const bool on_works = works && (works->type == StructureType::Trench || works->type == StructureType::Foxhole) &&
                                  !works->parapet;
            if (!on_works && !diggable(t)) return finish_ability(u);
            u.facing = to_point;
            if (++u.work < kParapetWork) return;
            if (on_works) {
                works->parapet = true;
                works->facing = to_point;
            } else {
                place_fieldwork(StructureType::Parapet, u.owner, t, to_point);
            }
            return finish_ability(u);
        }

        case AbilityId::Refill:
            return refill(u);

        case AbilityId::Deploy:
            if (u.shots_left == 1 ? deploy_step(u) : pack_step(u)) finish_ability(u);
            return;

        case AbilityId::SwitchAmmo:
        case AbilityId::Count:
            break;
    }
    finish_ability(u);
}

// Off to the depot for the kind of cargo this vehicle carries, and load up
// from the stock until full (or the stock runs dry).
void World::refill(Unit& u) {
    const UnitTypeDef& def = unit_type(u.type);
    const StructureType depot_type = def.supplies == Resource::Fuel ? StructureType::FuelDepot : StructureType::AmmoDepot;
    const Structure* depot = nearest_owned(u.owner, depot_type, u.pos);
    int32_t& stock = stock_[u.owner % kMaxPlayers][static_cast<size_t>(def.supplies)];
    if (!depot || def.supplies == Resource::Count || u.carrying >= def.cargo_capacity || stock <= 0) {
        return finish_ability(u);
    }
    if (distance_sq_to(*depot, u.pos) > square_raw(Fixed::from_int(1))) {
        navigate(u, depot->center, u.order_path, map_.clamp_tile(tile_of(depot->center)), false);
        return;
    }
    if (++u.work < kRefillInterval) return;
    u.work = 0;
    --stock;
    ++u.carrying;
    u.carrying_type = def.supplies;
}

// A tanker or an ammunition truck standing by looks after the neediest of
// our vehicles close to it: drives up and hands over, a unit at a time.
void World::serve(Unit& u) {
    const UnitTypeDef& def = unit_type(u.type);
    if (u.carrying <= 0) return;
    const bool fuel = def.supplies == Resource::Fuel;
    const Unit* neediest = nullptr;
    int64_t most_missing = 0;  // per mille of a full tank or rack
    for (const Unit& v : units_) {
        if (v.owner != u.owner || v.inside || v.id == u.id) continue;
        const UnitTypeDef& vd = unit_type(v.type);
        int64_t missing = 0;
        if (fuel && vd.fuel_capacity.raw > 0) {
            missing = static_cast<int64_t>(vd.fuel_capacity.raw - v.fuel.raw) * 1000 / vd.fuel_capacity.raw;
        } else if (!fuel && vd.rounds_capacity > 0) {
            missing = static_cast<int64_t>(vd.rounds_capacity - v.rounds) * 1000 / vd.rounds_capacity;
        }
        if (missing <= 0 || (v.pos - u.pos).length_sq_raw() > square_raw(kServiceRadius)) continue;
        if (!neediest || missing > most_missing) {
            neediest = &v;
            most_missing = missing;
        }
    }
    if (!neediest) {
        u.work = 0;
        return;
    }
    const Fixed reach = def.radius + unit_type(neediest->type).radius + Fixed::from_ratio(1, 2);
    if ((neediest->pos - u.pos).length_sq_raw() > square_raw(reach)) {
        navigate(u, neediest->pos, u.chase_path, map_.clamp_tile(tile_of(neediest->pos)), false);
        return;
    }
    if (++u.work < (fuel ? kRefuelInterval : kRearmInterval)) return;
    u.work = 0;
    Unit* v = find_unit_mut(neediest->id);
    const UnitTypeDef& vd = unit_type(v->type);
    if (fuel) {
        v->fuel = min(vd.fuel_capacity, v->fuel + Fixed::from_int(kTilesPerFuel));
    } else {
        v->rounds = std::min(vd.rounds_capacity, v->rounds + vd.rounds_per_supply);
    }
    --u.carrying;
}

void World::lob(Unit& shooter, FixedVec2 aim, const WeaponDef& weapon, bool enters) {
    if (rng_.next_below(100) >= weapon.accuracy) {
        aim.x += Fixed::from_raw(rng_.next_range(-weapon.miss_spread.raw, weapon.miss_spread.raw));
        aim.y += Fixed::from_raw(rng_.next_range(-weapon.miss_spread.raw, weapon.miss_spread.raw));
    }
    aim = clamp_to_map(aim, Fixed{});
    Projectile p;
    p.id = next_projectile_id_++;
    p.owner = shooter.owner;
    p.shooter = shooter.id;
    p.shooter_type = shooter.type;
    p.shooter_elevation = map_.elevation_at(shooter.pos);
    p.origin = shooter.pos;
    p.pos = shooter.pos;
    p.prev_pos = shooter.pos;
    p.target = aim;
    p.origin_height = map_.surface_height(shooter.pos) + kVehicleMuzzle;
    p.target_height = map_.surface_height(aim);
    p.weapon = weapon;
    p.lobbed = true;
    p.enters = enters;
    shooter.last_shot_tick = tick_;
    shooter.last_shot_at = aim;
    projectiles_.push_back(p);
}

}  // namespace engine
