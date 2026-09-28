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

// One of the salvo let go in the dive (`salvo`: how many in all): at the
// target, rockets walking along it, a string of bombs across it, scattered.
void World::release(Unit& u, int32_t salvo) {
    const WeaponDef& weapon = unit_type(u.type).weapon;
    const int32_t i = salvo - u.shots_left;  // which of them
    const Fixed spacing = weapon.aerial_bomb ? kSalvoSpacing + kSalvoSpacing : kSalvoSpacing;
    FixedVec2 aim = u.order_point + unit_vector(u.facing) * Fixed::from_raw(spacing.raw * (2 * i - (salvo - 1)) / 2);
    aim.x += Fixed::from_raw(rng_.next_range(-kSalvoScatter.raw, kSalvoScatter.raw));
    aim.y += Fixed::from_raw(rng_.next_range(-kSalvoScatter.raw, kSalvoScatter.raw));
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
    p.origin_height = ground_at(u.pos) + u.altitude;
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

// Takes off on a mission, climbs to cruise, flies to the target and dives
// on it lined up (going round when it comes in too close to), lets its
// rockets or bombs go short of it, pulls out and climbs away home, and glides
// down to land and rearm. Short of fuel it turns back early; with no
// airfield left to land on it is lost to the fight.
void World::update_aircraft(Unit& u) {
    const bool on_mission = u.order == Order::Attack || u.order == Order::AttackGround;
    if (!u.airborne) {
        u.altitude = Fixed{};
        if (!on_mission) return rearm_aircraft(u);
        if (u.rounds <= 0 || u.fuel <= kBingoReserve) {
            u.order = Order::Idle;  // not flying like this
            return;
        }
        u.airborne = true;  // takes off
        u.work = 0;
        u.shots_left = 0;
    }
    if (u.fuel.raw <= 0) {
        u.hp = 0;  // came down
        return;
    }
    const Structure* home = home_airfield(u);
    const bool diving = on_mission && u.shots_left > 0;

    if (on_mission && !diving) {
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
        if (home && u.fuel <= (home->center - u.pos).length() + kBingoReserve) {
            u.order = Order::Idle;
            u.shots_left = 0;
        }
    }

    if (u.order == Order::Attack || u.order == Order::AttackGround) {
        const FixedVec2 to_target = u.order_point - u.pos;
        const Fixed dist = to_target.length();
        const bool bombs = unit_type(u.type).weapon.aerial_bomb;
        const Fixed let_go = bombs ? kBombRelease : kRocketRelease;
        if (u.shots_left > 0) {
            // The dive: down towards the target, lower the nearer, to where it lets go.
            fly(u, u.order_point, dist >= kTurnClearance ? Steer::Turn : Steer::Straight);
            const Fixed span = kDiveStart - let_go;
            const Fixed k = max(Fixed{}, min(Fixed::from_int(1), (dist - let_go) / span));
            const Fixed want = kReleaseHeight + (kCruiseHeight - kReleaseHeight) * k;
            u.altitude = max(want, u.altitude - kDiveRate);
            if (dist <= let_go && u.altitude <= kReleaseHeight + kDiveRate) {
                release(u, u.work);  // (u.work: how many in the salvo)
                --u.shots_left;
                if (u.shots_left <= 0 || u.rounds <= 0) {
                    u.shots_left = 0;
                    u.order = Order::Idle;  // the mission is flown: pulling out
                    u.order_target = 0;
                }
            } else if (dist < let_go - Fixed::from_int(2)) {
                u.shots_left = -1;  // too steep, too close: out and round again
            }
            return;
        }
        u.altitude = min(kCruiseHeight, u.altitude + kClimbRate);
        if (u.shots_left < 0) {  // going round: on out, then back
            fly(u, u.pos, Steer::Straight);
            if (dist >= kDiveStart) u.shots_left = 0;
            return;
        }
        fly(u, u.order_point, dist >= kTurnClearance ? Steer::Turn : Steer::Straight);
        const bool lined_up = dot(unit_vector(u.facing), unit_vector(to_target)) >= kRunAlignedCos;
        if (dist < kDiveNearest && !lined_up) {
            u.shots_left = -1;  // too close to come round on it: out and round
        } else if (dist <= kDiveStart && dist >= let_go && lined_up) {
            u.shots_left = u.rounds;  // into the dive
            u.work = u.rounds;
        }
        return;
    }

    if (!home) {
        u.hp = 0;  // nowhere to land: diverted to the rear, out of the fight
        u.diverted = true;
        return;
    }
    const FixedVec2 spot = parking_spot(*home, u.id);
    const FixedVec2 to_spot = spot - u.pos;
    const Fixed away = to_spot.length();
    // Climbing away to cruise; gliding down the last of the way in.
    const Fixed glide = kCruiseHeight * min(Fixed::from_int(1), away / kLandApproach);
    u.altitude = min(glide, min(kCruiseHeight, u.altitude + kClimbRate));
    if (to_spot.length_sq_raw() <= square_raw(unit_type(u.type).speed)) {
        u.pos = spot;  // touch down
        u.airborne = false;
        u.altitude = Fixed{};
        u.facing = {Fixed::from_int(1), Fixed{}};
        u.work = 0;
        return;
    }
    fly(u, spot, to_spot.length_sq_raw() <= square_raw(kTurnClearance) ? Steer::Direct : Steer::Turn);
}

// The nearest enemy aircraft in the air that the player sees within reach
// of this unit's guns or missiles (the one it's on first).
bool World::air_in_reach(const Unit& u, const Unit& plane) const {
    const WeaponDef& weapon = weapon_of(u);
    if (!plane.airborne || plane.altitude > weapon.ceiling) return false;
    const Fixed reach = weapon.range + unit_type(u.type).radius + unit_type(plane.type).radius;
    return (plane.pos - u.pos).length_sq_raw() + square_raw(plane.altitude) <= square_raw(reach);
}

const Unit* World::find_air_target(Unit& u) {
    auto in_reach = [&](const Unit& o) { return o.airborne && o.owner != u.owner && sees(u.owner, o) && air_in_reach(u, o); };
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
    {  // nearer and lower, likelier: off it for the slant range and the height, a bonus close in, low
        const FixedVec2 slant{(target.pos - u.pos).length(), target.altitude};
        const Fixed reach = weapon.range + def.radius + unit_type(target.type).radius;
        const auto range_off = static_cast<int32_t>(static_cast<int64_t>(slant.length().raw) * kAirRangeOffPercent / std::max<int64_t>(1, reach.raw));
        const auto height_off =
            static_cast<int32_t>(static_cast<int64_t>(target.altitude.raw) * kAirHeightOffPercent / std::max<int64_t>(1, weapon.ceiling.raw));
        const int32_t percent = std::clamp(100 + kCloseAirBonus - range_off - height_off, 10, 100 + kCloseAirBonus);
        accuracy = std::min(95, accuracy * percent / 100);
    }
    if (def.emitter && u.silent) accuracy = accuracy * kOpticalSightPercent / 100;  // radar off: by eye
    if (hungry(u.owner)) accuracy = accuracy * kHungryAccuracyPercent / 100;
    if (has_upgrade(u.owner, UpgradeId::RadarTracking)) accuracy = accuracy * kRadarTrackingPercent / 100;
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
    p.target_height = ground_at(target.pos) + target.altitude;
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
        p.target_height = ground_at(plane->pos) + plane->altitude;
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
    recent_impacts_.back().height = max(Fixed{}, p.target_height - ground_at(p.target));
    p.id = 0;
}

// Aircraft brought down, falling: coming down now, a burst of burning fuel
// and what they had aboard where they crash, whatever is there burning in
// it; a building it comes down on takes it as it would a bomb from the air
// (a block of flats has the section there brought down). The wreck's smoke
// rising there. Kept a little after, to be seen.
void World::update_crashes() {
    static constexpr WeaponDef kCrashFire{.name = "An aircraft crashing", .damage = 120, .damage_type = DamageType::Explosive,
                                          .range = Fixed{}, .reload = 0, .projectile_speed = Fixed{},
                                          .splash_radius = Fixed::from_int(2), .accuracy = 100, .miss_spread = Fixed{},
                                          .aerial_bomb = true};
    for (const Crash& c : crashes_) {
        if (c.hits != tick_) continue;
        burst_into_flames(c.to, c.owner, kCrashFire);
        smokes_.push_back({clamp_to_map(c.to + kPlumeDrift, Fixed{}), kPlumeRadius, tick_ + kPlumeTicks, SmokeKind::Plume, tick_});
    }
    std::erase_if(crashes_, [&](const Crash& c) { return c.hits + kCrashHistory < tick_; });
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
