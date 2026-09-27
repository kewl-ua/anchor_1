// The aviation half of World: attack aircraft flying their missions from
// airfields, and the air defence that shoots them down.

#include <algorithm>

#include "engine/heights.h"
#include "engine/world.h"

namespace engine {

namespace {

FixedVec2 unit_vector(FixedVec2 v) {
    const Fixed len = v.length();
    if (len.raw == 0) return {Fixed::from_int(1), Fixed{}};
    return v * (Fixed::from_int(1) / len);
}

Fixed dot(FixedVec2 a, FixedVec2 b) { return a.x * b.x + a.y * b.y; }

}  // namespace

// A mission: a strike at a point, or at an enemy we see (followed while we
// see him). Only an aircraft on the ground takes one (it takes off with
// rockets and fuel enough, see update_aircraft); in the air it flies the one
// it has, and it's one mission a sortie.
void World::give_mission(Unit& plane, FixedVec2 point, EntityId target) {
    if (plane.airborne) return;
    plane.order = target ? Order::Attack : Order::AttackGround;
    plane.order_point = clamp_to_map(point, Fixed{});
    plane.order_target = target;
    plane.engaged = 0;
    plane.shots_left = 0;
    plane.work = 0;
}

const Structure* World::home_airfield(const Unit& u) const {
    const Structure* best = nullptr;
    uint64_t best_sq = 0;
    for (const Structure& s : structures_) {
        if (s.type != StructureType::Airfield || s.owner != u.owner || !s.built) continue;
        const uint64_t d = (s.center - u.pos).length_sq_raw();
        if (!best || d < best_sq) {
            best = &s;
            best_sq = d;
        }
    }
    return best;
}

// The first tile of the runway no other aircraft is parked on.
FixedVec2 World::parking_spot(const Structure& airfield, EntityId self) const {
    for (const TilePos& t : airfield.tiles) {
        const bool taken = std::any_of(units_.begin(), units_.end(), [&](const Unit& o) {
            return o.id != self && unit_type(o.type).aircraft && !o.airborne && tile_of(o.pos) == t;
        });
        if (!taken) return tile_center(t);
    }
    return airfield.center;
}

// A tick of flight: turning towards `goal` at the aircraft's rate (or
// straight at it, or not at all), then on along the heading. Fuel burns.
void World::fly(Unit& u, FixedVec2 goal, Steer how) {
    FixedVec2 heading = unit_vector(u.facing);
    const FixedVec2 to_goal = goal - u.pos;
    if (how != Steer::Straight && (to_goal.x.raw != 0 || to_goal.y.raw != 0)) {
        const FixedVec2 want = unit_vector(to_goal);
        if (how == Steer::Direct || dot(heading, want) >= kAlignedCos) {
            heading = want;
        } else {
            const Fixed cross = heading.x * want.y - heading.y * want.x;
            const FixedVec2 side = cross.raw >= 0 ? FixedVec2{-heading.y, heading.x} : FixedVec2{heading.y, -heading.x};
            heading = unit_vector(heading + side * kAircraftTurn);
        }
    }
    const Fixed speed = unit_type(u.type).speed;
    u.facing = heading;
    u.pos = clamp_to_map(u.pos + heading * speed, Fixed{});
    u.moving = true;
    u.fuel = max(Fixed{}, u.fuel - speed);
}

// One rocket of the run: at the ground a little ahead, scattered.
void World::fire_rocket(Unit& u) {
    const WeaponDef& weapon = unit_type(u.type).weapon;
    FixedVec2 aim = u.pos + unit_vector(u.facing) * kRocketAhead;
    aim.x += Fixed::from_raw(rng_.next_range(-kRocketScatter.raw, kRocketScatter.raw));
    aim.y += Fixed::from_raw(rng_.next_range(-kRocketScatter.raw, kRocketScatter.raw));
    aim = clamp_to_map(aim, Fixed{});
    Projectile p;
    p.id = next_projectile_id_++;
    p.owner = u.owner;
    p.shooter = u.id;
    p.shooter_type = u.type;
    p.shooter_elevation = map_.elevation_at(u.pos);
    p.origin = u.pos;
    p.pos = u.pos;
    p.prev_pos = u.pos;
    p.target = aim;
    p.origin_height = ground_at(u.pos) + kFlightHeight;
    p.target_height = map_.surface_height(aim);
    p.weapon = weapon;
    p.lobbed = true;  // from above: over everything, down on whoever is there, trench or not
    projectiles_.push_back(p);
    u.rounds = std::max(0, u.rounds - 1);
    u.last_shot_tick = tick_;
    u.last_shot_at = aim;
}

// On the airfield: rearmed and refuelled from the stock, a rocket and a
// unit of fuel at a time.
void World::rearm_aircraft(Unit& u) {
    const Structure* s = structure_at(map_.clamp_tile(tile_of(u.pos)));
    if (!s || s->type != StructureType::Airfield || s->owner != u.owner || !s->built) return;
    const UnitTypeDef& def = unit_type(u.type);
    if (u.rounds >= def.rounds_capacity && u.fuel >= def.fuel_capacity) {
        u.work = 0;
        return;
    }
    if (++u.work < kAirRearmInterval) return;
    u.work = 0;
    Stock& stock = stock_[u.owner % kMaxPlayers];
    int32_t& ammo = stock[static_cast<size_t>(Resource::Ammo)];
    int32_t& fuel = stock[static_cast<size_t>(Resource::Fuel)];
    if (u.rounds < def.rounds_capacity && ammo > 0) {
        u.rounds = std::min(def.rounds_capacity, u.rounds + def.rounds_per_supply);
        --ammo;
    }
    if (u.fuel < def.fuel_capacity && fuel > 0) {
        u.fuel = min(def.fuel_capacity, u.fuel + Fixed::from_int(kAircraftTilesPerFuel));
        --fuel;
    }
}

// Takes off on a mission, flies to the target, makes one rocket run lined
// up on it, and flies home to land and rearm. Short of fuel it turns back
// early; with no airfield left to land on it is lost to the fight.
void World::update_aircraft(Unit& u) {
    const bool on_mission = u.order == Order::Attack || u.order == Order::AttackGround;
    if (!u.airborne) {
        if (!on_mission) return rearm_aircraft(u);
        if (u.rounds <= 0 || u.fuel <= kBingoReserve) {
            u.order = Order::Idle;  // not flying like this
            return;
        }
        u.airborne = true;  // takes off
        u.work = 0;
    }
    if (u.fuel.raw <= 0) {
        u.hp = 0;  // came down
        return;
    }
    const Structure* home = home_airfield(u);

    if (on_mission && u.shots_left == 0) {
        if (u.order == Order::Attack) {
            const Unit* target = find_unit(u.order_target);
            if (target && !target->airborne && sees(u.owner, *target)) {
                u.order_point = target->pos;
            } else if (!target) {
                u.order = Order::AttackGround;  // gone: the strike goes where it was
                u.order_target = 0;
            }
        }
        // Bingo fuel: home while there's enough left to get there.
        if (home && u.fuel <= (home->center - u.pos).length() + kBingoReserve) u.order = Order::Idle;
    }

    if (u.shots_left > 0) {
        // The run: straight on, rockets away.
        fly(u, u.pos, Steer::Straight);
        if (u.work++ % kRocketInterval == 0) {
            fire_rocket(u);
            --u.shots_left;
        }
        if (u.shots_left <= 0 || u.rounds <= 0) {
            u.shots_left = 0;
            u.order = Order::Idle;  // the mission is flown
            u.order_target = 0;
        }
        return;
    }

    if (u.order == Order::Attack || u.order == Order::AttackGround) {
        // Right on top of the target it flies on and comes round again.
        const FixedVec2 to_target = u.order_point - u.pos;
        const Fixed dist = to_target.length();
        fly(u, u.order_point, dist >= kTurnClearance ? Steer::Turn : Steer::Straight);
        if (dist <= kRunStart && dot(unit_vector(u.facing), unit_vector(to_target)) >= kRunAlignedCos) {
            u.shots_left = u.rounds;
            u.work = 0;
        }
        return;
    }

    if (!home) {
        u.hp = 0;  // nowhere to land: diverted to the rear, out of the fight
        return;
    }
    const FixedVec2 spot = parking_spot(*home, u.id);
    const FixedVec2 to_spot = spot - u.pos;
    if (to_spot.length_sq_raw() <= square_raw(unit_type(u.type).speed)) {
        u.pos = spot;  // touch down
        u.airborne = false;
        u.facing = {Fixed::from_int(1), Fixed{}};
        u.work = 0;
        return;
    }
    fly(u, spot, to_spot.length_sq_raw() <= square_raw(kTurnClearance) ? Steer::Direct : Steer::Turn);
}

// The nearest enemy aircraft in the air that the player sees within reach
// of this unit's guns or missiles (the one it's on first).
const Unit* World::find_air_target(Unit& u) {
    const Fixed reach = weapon_of(u).range + unit_type(u.type).radius;
    auto in_reach = [&](const Unit& o) {
        return o.airborne && o.owner != u.owner && sees(u.owner, o) &&
               (o.pos - u.pos).length_sq_raw() <= square_raw(reach + unit_type(o.type).radius);
    };
    if (const Unit* current = find_unit(u.engaged); current && in_reach(*current)) return current;
    const Unit* best = nullptr;
    uint64_t best_sq = 0;
    for (const Unit& o : units_) {
        if (!in_reach(o)) continue;
        const uint64_t d = (o.pos - u.pos).length_sq_raw();
        if (!best || d < best_sq) {
            best = &o;
            best_sq = d;
        }
    }
    return best;
}

// Guns hit or miss at once; a missile flies after the aircraft and bursts
// on it, or wide of it. Walls and hills don't come into it.
void World::fire_at_air(Unit& u, const Unit& target) {
    const UnitTypeDef& def = unit_type(u.type);
    const WeaponDef& weapon = weapon_of(u);
    if (weapon.reload > 0) u.cooldown = weapon.reload;
    if (def.rounds_capacity > 0) u.rounds = std::max(0, u.rounds - 1);
    u.last_shot_tick = tick_;
    u.last_shot_at = target.pos;
    int32_t accuracy = weapon.accuracy;
    if (def.emitter && u.silent) accuracy = accuracy * kOpticalSightPercent / 100;  // radar off: by eye
    const bool hit = static_cast<int32_t>(rng_.next_below(100)) < accuracy;
    if (weapon.projectile_speed.raw == 0) {
        if (hit) hurt(target, weapon, {u.pos, 0, false, true});
        return;
    }
    Projectile p;
    p.id = next_projectile_id_++;
    p.owner = u.owner;
    p.shooter = u.id;
    p.shooter_type = u.type;
    p.origin = u.pos;
    p.pos = u.pos;
    p.prev_pos = u.pos;
    p.target = target.pos;
    p.origin_height = ground_at(u.pos) + (def.vehicle ? kVehicleTop : kInfantryTop);
    p.target_height = ground_at(target.pos) + kFlightHeight;
    p.weapon = weapon;
    p.at_air = true;
    p.homing = hit ? target.id : 0;
    projectiles_.push_back(p);
}

// A missile after an aircraft: on it if it's going to hit, to where the
// aircraft was if not. It bursts up there either way.
void World::move_missile(Projectile& p) {
    const Unit* plane = p.homing ? find_unit(p.homing) : nullptr;
    if (plane && plane->airborne) {
        p.target = plane->pos;
        p.target_height = ground_at(plane->pos) + kFlightHeight;
    }
    p.prev_pos = p.pos;
    const Fixed speed = p.weapon.projectile_speed;
    const FixedVec2 to_target = p.target - p.pos;
    if (to_target.length() > speed) {
        p.pos = p.pos + to_target * (speed / to_target.length());
        return;
    }
    p.pos = p.target;
    if (plane && plane->airborne) hurt(*plane, p.weapon, {p.origin, 0, false, true});
    recent_impacts_.push_back({tick_, p.pos, p.shooter_type, Fixed::from_ratio(1, 2), true});
    p.id = 0;
}

// An aircraft in the air is seen by anyone who has it within his sight, over
// trees and houses (it's up in the sky), and far out by an air defence
// radar that is set up and on the air.
bool World::sky_watch(PlayerId player, const Unit& plane) const {
    if (!plane.airborne) return false;
    return std::any_of(units_.begin(), units_.end(), [&](const Unit& o) {
        if (o.owner != player) return false;
        const UnitTypeDef& def = unit_type(o.type);
        const bool radar = def.radar_range.raw > 0 && o.deployed && !o.silent;
        const Fixed reach = radar ? max(def.radar_range, def.sight) : def.sight;
        return (o.pos - plane.pos).length_sq_raw() <= square_raw(reach);
    });
}

}  // namespace engine
