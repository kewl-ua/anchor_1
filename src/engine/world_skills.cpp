// The skills half of World: what the command-grid buttons do.

#include <algorithm>

#include "engine/heights.h"
#include "engine/world.h"

namespace engine {

namespace {

// A machine-gun sweep: this many rounds, one every few ticks, scattered
// across the front up to this far either side of the line at full range.
constexpr int32_t kSweepShots = 15;
constexpr Tick kSweepInterval = 3;
constexpr Fixed kSweepSpread = Fixed::from_int(3);

}  // namespace

void World::apply_ability(const Command& cmd) {
    if (cmd.ability >= kAbilityCount) return;
    const auto id = static_cast<AbilityId>(cmd.ability);
    const AbilityDef& ability = ability_def(id);
    for (EntityId unit_id : cmd.units) {
        Unit* u = find_unit_mut(unit_id);
        if (!u || u->owner != cmd.player) continue;
        const int slot = ability_slot(unit_type(u->type), id);
        if (slot < 0 || u->ability_ready[static_cast<size_t>(slot)] > tick_) continue;

        if (ability.target == AbilityTarget::Instant) {
            if (id == AbilityId::SwitchAmmo && unit_type(u->type).alt_weapon.damage > 0) {
                // The round in the breech has to come out: a full reload.
                u->ammo ^= 1;
                u->cooldown = std::max(u->cooldown, weapon_of(*u).reload);
            }
            continue;
        }
        leave_structure(*u);
        u->order = Order::Ability;
        u->order_ability = id;
        u->order_point = clamp_to_map(cmd.target, Fixed{});
        u->order_point2 = clamp_to_map(cmd.target_end, Fixed{});
        u->order_goal = map_.clamp_tile(tile_of(u->order_point));
        u->order_path.reset();
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->order_target = 0;
        u->engaged = 0;
        u->shots_left = id == AbilityId::MgSweep ? kSweepShots : 0;
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
            fire(u, aim, map_.surface_height(aim) + kInfantryCenter, ability.weapon);
            if (--u.shots_left <= 0) finish_ability(u);
            return;
        }

        case AbilityId::LobGrenade: {
            if (to_point.length_sq_raw() > square_raw(ability.range + def.radius)) {
                navigate(u, u.order_point, u.order_path, u.order_goal, false);
                return;
            }
            if (!at_point) u.facing = to_point;
            lob(u, u.order_point, ability.weapon);
            return finish_ability(u);
        }

        case AbilityId::SwitchAmmo:
        case AbilityId::Count:
            break;
    }
    finish_ability(u);
}

void World::lob(Unit& shooter, FixedVec2 aim, const WeaponDef& weapon) {
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
    shooter.last_shot_tick = tick_;
    shooter.last_shot_at = aim;
    projectiles_.push_back(p);
}

}  // namespace engine
