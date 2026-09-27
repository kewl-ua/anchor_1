// The artillery half of World: indirect fire, ranging, setting up and
// packing up guns, and guns giving themselves away.

#include <algorithm>

#include "engine/world.h"

namespace engine {

bool World::deploy_step(Unit& u) {
    const Tick time = unit_type(u.type).deploy_time;
    if (u.deployed || time == 0) return true;
    if (++u.deploy_work >= time) {
        u.deployed = true;
        u.deploy_work = 0;
    }
    return u.deployed;
}

bool World::pack_step(Unit& u) {
    if (!u.deployed) return true;
    if (++u.deploy_work >= unit_type(u.type).deploy_time) {
        u.deployed = false;
        u.deploy_work = 0;
    }
    return !u.deployed;
}

// A fire mission at a point: get within range (packing up to move), set up,
// and fire whenever loaded. Nothing closer than the minimum range.
void World::engage_indirect(Unit& u, FixedVec2 aim, std::shared_ptr<const FlowField>& path, TilePos goal) {
    const WeaponDef& weapon = weapon_of(u);
    const FixedVec2 to_aim = aim - u.pos;
    const uint64_t dist_sq = to_aim.length_sq_raw();
    if (dist_sq > square_raw(weapon.range + unit_type(u.type).radius)) {
        navigate(u, aim, path, goal, false);
        return;
    }
    if (dist_sq < square_raw(weapon.min_range)) return;  // too close to lob at
    if (to_aim.x.raw != 0 || to_aim.y.raw != 0) u.facing = to_aim;
    if (!deploy_step(u) || u.cooldown > 0 || out_of_rounds(u)) return;
    fire_indirect(u, aim);
}

// Bracketing: each shot at the same target lands closer to it. Scouts
// looking at the target correct the fire, a step ahead.
void World::fire_indirect(Unit& u, FixedVec2 aim) {
    const WeaponDef& weapon = weapon_of(u);
    const UnitTypeDef& def = unit_type(u.type);
    if (weapon.reload > 0) u.cooldown = weapon.reload;
    if (def.rounds_capacity > 0) u.rounds = std::max(0, u.rounds - 1);

    const bool same = u.ranging_shots > 0 && (aim - u.ranging_point).length_sq_raw() <= square_raw(kSameTarget);
    u.ranging_shots = same ? static_cast<uint8_t>(std::min<int>(3, u.ranging_shots + 1)) : 1;
    u.ranging_point = aim;
    int step = u.ranging_shots;
    if (spotted_by_scouts(u.owner, aim)) step = std::min(3, step + 1);
    const auto i = static_cast<size_t>(step - 1);

    const bool on_target = static_cast<int32_t>(rng_.next_below(100)) < kRangingChance[i];
    const Fixed spread = on_target ? kOnTargetSpread : kRangingSpread[i];
    FixedVec2 landing = aim;
    landing.x += Fixed::from_raw(rng_.next_range(-spread.raw, spread.raw));
    landing.y += Fixed::from_raw(rng_.next_range(-spread.raw, spread.raw));
    WeaponDef shell = weapon;
    shell.accuracy = 100;  // where it lands is decided above
    lob(u, landing, shell, false);
}

bool World::spotted_by_scouts(PlayerId player, FixedVec2 point) const {
    const TilePos tile = map_.clamp_tile(tile_of(point));
    if (!visible(player, tile)) return false;
    const int32_t index = tile.y * map_.width() + tile.x;
    for (const Sector& s : sectors_) {
        if (s.owner == player && std::binary_search(s.tiles.begin(), s.tiles.end(), index)) return true;
    }
    for (const Unit& u : units_) {
        const UnitTypeDef& def = unit_type(u.type);
        if (u.owner != player || u.inside || def.sector_range.raw == 0) continue;
        if ((u.pos - point).length_sq_raw() <= square_raw(def.sight)) return true;
    }
    return false;
}

bool World::betrayed(PlayerId player, const Unit& gun) const {
    if (!weapon_of(gun).indirect || gun.last_shot_tick == kNeverFired) return false;
    const Tick since = tick_ - gun.last_shot_tick;
    // Fired from a village: the locals tell.
    const Terrain ground = map_.terrain_at(gun.pos);
    if (since < kReportedTicks && (ground == Terrain::Urban || ground == Terrain::Ruins)) return true;
    if (since >= kGunRevealTicks) return false;
    // The flash, over any tree line, by an observation post looking this way.
    for (const Sector& s : sectors_) {
        if (s.owner != player) continue;
        const FixedVec2 v = gun.pos - s.from;
        const Fixed reach = Fixed::from_int(s.radius * kFlashSectorPercent / 100);
        if (v.length_sq_raw() > square_raw(reach)) continue;
        const int64_t dx = s.dir.x.raw >> 8;
        const int64_t dy = s.dir.y.raw >> 8;
        const int64_t vx = v.x.raw >> 8;
        const int64_t vy = v.y.raw >> 8;
        const int64_t dot = dx * vx + dy * vy;
        // Within 45 degrees of where it looks: cos^2 >= 1/2.
        if (dot > 0 && 2 * (dot / 256) * (dot / 256) >= ((dx * dx + dy * dy) / 256) * ((vx * vx + vy * vy) / 256)) {
            return true;
        }
    }
    return false;
}

}  // namespace engine
