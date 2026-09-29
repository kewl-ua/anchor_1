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

}  // namespace

Tick World::work_needed(const Unit& u, Tick base) const {
    const bool shovels = u.type == UnitTypeId::Rifleman && has_upgrade(u.owner, UpgradeId::EntrenchingTools);
    return shovels ? base * kShovelWorkPercent / 100 : base;
}

// A building researches one upgrade at a time, paid up front.
void World::apply_research(const Command& cmd) {
    Structure* s = find_structure_mut(cmd.target_unit);
    if (!s || !s->built || s->owner != cmd.player || cmd.upgrade >= kUpgradeCount) return;
    const auto id = static_cast<UpgradeId>(cmd.upgrade);
    const UpgradeDef& def = upgrade_def(id);
    if (def.building != role_of(*s) || s->research != UpgradeId::Count || has_upgrade(cmd.player, id)) return;
    if (def.needs != UpgradeId::Count && !has_upgrade(cmd.player, def.needs)) return;  // the one before it first
    // Nobody researches the same thing twice at once.
    for (const Structure& other : structures_) {
        if (other.owner == cmd.player && other.research == id) return;
    }
    Stock& stock = stock_[cmd.player % kMaxPlayers];
    if (!can_afford(stock, def.cost)) return;
    pay(stock, def.cost);
    s->research = id;
    s->research_progress = 0;
}

void World::update_research() {
    for (Structure& s : structures_) {
        if (s.research == UpgradeId::Count || !s.built || s.owner >= kMaxPlayers) continue;
        if (++s.research_progress < upgrade_def(s.research).time) continue;
        upgrades_[s.owner] |= 1u << static_cast<uint32_t>(s.research);
        s.research = UpgradeId::Count;
        s.research_progress = 0;
    }
}

void World::update_smoke() {
    std::erase_if(smokes_, [&](const Smoke& s) { return tick_ >= s.clears; });
}

// A line of sight into a screen or through it is blind; through thin smoke,
// once it has run through more than kThinSmokeSight of it.
bool World::smoke_hides(FixedVec2 from, FixedVec2 to, const std::vector<const Smoke*>& near) const {
    // In 1/256 of a tile, so the products below stay well inside 64 bits.
    const int64_t ax = from.x.raw >> 8;
    const int64_t ay = from.y.raw >> 8;
    const int64_t dx = (to.x.raw >> 8) - ax;
    const int64_t dy = (to.y.raw >> 8) - ay;
    const auto len = static_cast<int64_t>(isqrt(static_cast<uint64_t>(dx * dx + dy * dy)));
    int64_t thin = 0;
    for (const Smoke* s : near) {
        const int64_t fx = (s->center.x.raw >> 8) - ax;
        const int64_t fy = (s->center.y.raw >> 8) - ay;
        const int64_t r = s->radius.raw >> 8;
        // How far along the line it passes nearest the smoke's middle, how near.
        const int64_t along = len > 0 ? (fx * dx + fy * dy) / len : 0;
        const int64_t off_sq = std::max<int64_t>(0, fx * fx + fy * fy - along * along);
        if (off_sq >= r * r) continue;
        const auto half = static_cast<int64_t>(isqrt(static_cast<uint64_t>(r * r - off_sq)));  // half the chord
        const int64_t inside = std::min(len, along + half) - std::max<int64_t>(0, along - half);
        if (inside <= 0) continue;
        if (s->kind == SmokeKind::Screen) return true;
        thin += inside;
        if (thin > (kThinSmokeSight.raw >> 8)) return true;
    }
    return false;
}

// A burst of a tile and more raises dust and smoke for a few seconds (not off the water).
void World::raise_dust(FixedVec2 at, const WeaponDef& weapon) {
    if (weapon.splash_radius < kDustSplash || map_.terrain_at(at) == Terrain::Water) return;
    const Tick lasts = kDustTicks + static_cast<Tick>((weapon.splash_radius * kDustTicksPerTile).to_int());
    smokes_.push_back({at, weapon.splash_radius * kDustPercent / 100, tick_ + lasts, SmokeKind::Dust, tick_});
}

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
    return terrain == Terrain::Grass || terrain == Terrain::Urban || terrain == Terrain::Ruins ||
           terrain == Terrain::Plowed || terrain == Terrain::Crops || terrain == Terrain::DirtRoad ||
           terrain == Terrain::Crater || terrain == Terrain::Wheat || terrain == Terrain::Orchard ||
           terrain == Terrain::Garden;
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
    s->fitting = TrenchFit::Dugout;
    s->upgrade_work = 0;
}

void World::apply_unload(const Command& cmd) {
    // IFVs told to dismount: they stop, and the squad gets out at the back.
    for (EntityId carrier : cmd.units) {
        Unit* v = find_unit_mut(carrier);
        if (!v || v->owner != cmd.player || (v->passengers.empty() && v->riders.empty())) continue;
        v->order = Order::Idle;
        v->order_path.reset();
        std::vector<EntityId> aboard = v->riders;
        aboard.insert(aboard.end(), v->passengers.begin(), v->passengers.end());
        for (EntityId id : aboard) {
            if (Unit* u = find_unit_mut(id)) {
                leave_structure(*u);
                u->order = Order::Idle;
            }
        }
    }
    const Structure* s = find_structure(cmd.target_unit);
    if (!s || s->owner != cmd.player || (!is_shelter(role_of(*s)) && s->type != StructureType::Hospital)) return;
    // Out of a dugout, back to their places in the trench ("to positions!").
    const bool dugout = s->type == StructureType::Dugout;
    const std::vector<EntityId> inside = s->garrison;
    for (EntityId id : inside) {
        if (Unit* u = find_unit_mut(id)) {
            leave_structure(*u);
            u->order = Order::Idle;
            if (dugout && u->post.x >= 0 && trench_like(u->post)) man_place(*u, u->post, true);
        }
    }
}

// Works being fitted out: the men standing in it and the men sent to it dig (up to four).
void World::update_upgrades() {
    for (Structure& s : structures_) {
        if (!s.upgrading) continue;
        const TilePos tile = s.tiles.front();
        std::vector<Unit*> diggers;
        for (Unit& u : units_) {
            if (u.owner != s.owner || u.inside || u.riding || unit_type(u.type).vehicle || u.hp <= 0) continue;
            const bool standing = tile_of(u.pos) == tile && !u.moving && u.order != Order::Fortify;
            const bool sent = u.order == Order::Fortify && u.order_target == s.id &&
                              (tile_center(tile) - u.pos).length_sq_raw() <= square_raw(kDigReach);
            if ((standing || sent) && static_cast<int32_t>(diggers.size()) < kDugoutDiggers) diggers.push_back(&u);
        }
        s.upgrade_work += static_cast<Tick>(diggers.size());
        if (s.upgrade_work >= fit_def(s.fitting).work) finish_fitting(s, diggers);
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
        if (const UpgradeId needs = ability_def(id).needs; needs != UpgradeId::Count && !has_upgrade(u->owner, needs)) {
            continue;
        }
        if (id == AbilityId::Smoke) {
            // A screen right ahead, at once.
            FixedVec2 facing = u->facing;
            if (facing.x.raw == 0 && facing.y.raw == 0) facing = {Fixed::from_int(1), Fixed{}};
            const FixedVec2 ahead = u->pos + facing * (kSmokeAhead / facing.length());
            smokes_.push_back({clamp_to_map(ahead, Fixed{}), kSmokeRadius, tick_ + kSmokeTicks, SmokeKind::Screen, tick_});
            u->ability_ready[static_cast<size_t>(slot)] = tick_ + ability_def(id).cooldown;
            continue;
        }

        if (id == AbilityId::RadioSilence) {
            u->silent = !u->silent;  // whatever it was doing, it goes on doing
            continue;
        }
        if (id == AbilityId::CallSupply) {
            // Over the radio: with it off and no relay near, nobody hears.
            if (!in_touch(*u)) continue;
            call_supply(*u);
            u->ability_ready[static_cast<size_t>(slot)] = tick_ + ability_def(id).cooldown;
            continue;
        }
        if (id == AbilityId::SwitchAmmo) {
            if (unit_type(u->type).alt_weapon.damage > 0) {
                // The round in the breech has to come out: a full reload.
                u->round_type ^= 1;
                u->cooldown = std::max(u->cooldown, reload_ticks(*u, weapon_of(*u)));
            }
            continue;
        }
        leave_structure(*u);
        u->order = Order::Ability;
        u->order_ability = id;
        // A foxhole is dug where the man stands; a refill or a deploy needs no point at all.
        const bool here = id == AbilityId::DigFoxhole || id == AbilityId::Refill || id == AbilityId::Deploy ||
                          id == AbilityId::DigGunPit || id == AbilityId::Camouflage;
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
        if (id == AbilityId::RapidFire) {
            u->shots_left = kBurstGrenades;
            u->perfect_burst = static_cast<int32_t>(rng_.next_below(100)) < kPerfectBurstPercent;
        }
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
            if (++dig_work_[index] >= work_needed(u, kTrenchWork)) {
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
            if (++u.work < work_needed(u, kFoxholeWork)) return;
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
            if (++u.work < work_needed(u, kParapetWork)) return;
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

        case AbilityId::DigGunPit: {
            const TilePos t = map_.clamp_tile(tile_of(u.order_point));
            if (!diggable(t)) return finish_ability(u);
            if ((tile_center(t) - u.pos).length_sq_raw() > square_raw(kDigReach)) {
                navigate(u, tile_center(t), u.order_path, t, false);
                return;
            }
            if (++u.work < kGunPitWork) return;
            const EntityId id = place_fieldwork(StructureType::GunPit, u.owner, t, def.vehicle ? u.hull : u.facing);
            if (Structure* pit = find_structure_mut(id)) {
                pit->pit = u.type == UnitTypeId::Mortar ? PitKind::Mortar : def.family == Family::Spg ? PitKind::Vehicle : PitKind::Gun;
            }
            return finish_ability(u);
        }

        case AbilityId::Camouflage:
            if (++u.work < kCamouflageWork) return;
            u.camouflaged = true;
            return finish_ability(u);

        case AbilityId::RapidFire: {
            // Five grenades in a row across the line of fire, the middle
            // one on the point; one burst in twenty lands them dead in line.
            const WeaponDef& weapon = ability.weapon;
            if (to_point.length_sq_raw() > square_raw(weapon.range + def.radius)) {
                navigate(u, u.order_point, u.order_path, u.order_goal, false);
                return;
            }
            if (at_point || out_of_rounds(u)) return finish_ability(u);
            u.facing = to_point;
            if (u.work++ % kSweepInterval != 0) return;
            const FixedVec2 dir = to_point * (Fixed::from_int(1) / to_point.length());
            const FixedVec2 across{-dir.y, dir.x};
            const int32_t k = kBurstGrenades - u.shots_left - kBurstGrenades / 2;  // -2 .. 2
            FixedVec2 aim = u.order_point + across * (kBurstSpacing * k);
            if (!u.perfect_burst) {
                aim.x += Fixed::from_raw(rng_.next_range(-kBurstJitter.raw, kBurstJitter.raw));
                aim.y += Fixed::from_raw(rng_.next_range(-kBurstJitter.raw, kBurstJitter.raw));
            }
            if (def.rounds_capacity > 0) u.rounds = std::max(0, u.rounds - 1);
            lob(u, aim, weapon, false);
            if (--u.shots_left <= 0) finish_ability(u);
            return;
        }

        case AbilityId::Salvo: {
            // Everything in the launcher, one rocket after another, over an area.
            const WeaponDef& weapon = weapon_of(u);
            const uint64_t dist_sq = to_point.length_sq_raw();
            if (dist_sq > square_raw(weapon.range + def.radius)) {
                navigate(u, u.order_point, u.order_path, u.order_goal, false);
                return;
            }
            if (dist_sq < square_raw(weapon.min_range) || out_of_rounds(u)) return finish_ability(u);
            if (!at_point) u.facing = to_point;
            if (!deploy_step(u)) return;
            if (u.work++ % kSalvoInterval != 0) return;
            FixedVec2 aim = u.order_point;
            const Fixed spread = scatter_at(kSalvoSpread, to_point.length(), weapon.range);
            aim.x += Fixed::from_raw(rng_.next_range(-spread.raw, spread.raw));
            aim.y += Fixed::from_raw(rng_.next_range(-spread.raw, spread.raw));
            WeaponDef rocket = weapon;
            rocket.accuracy = 100;
            u.rounds = std::max(0, u.rounds - 1);
            lob(u, aim, rocket, false, u.shell);
            if (out_of_rounds(u)) finish_ability(u);
            return;
        }

        case AbilityId::IndirectFire: {
            // A fire mission from cover, like artillery, until told otherwise.
            const int32_t wear = std::max(1, def.max_hp * kBarrelWearPercent / 100);
            const WeaponDef& weapon = ability.weapon;
            if (to_point.length_sq_raw() > square_raw(weapon.range + def.radius)) {
                navigate(u, u.order_point, u.order_path, u.order_goal, false);
                return;
            }
            if (to_point.length_sq_raw() < square_raw(weapon.min_range)) return;
            if (!at_point) u.facing = to_point;
            if (u.cooldown > 0 || out_of_rounds(u)) return;
            fire_indirect(u, u.order_point, weapon, wear);
            return;
        }

        case AbilityId::Atgm: {
            // At the enemy vehicle we see nearest the aim point: the missile
            // flies after it. None there: at the point.
            if (u.missiles <= 0) return finish_ability(u);
            const Unit* target = nullptr;
            uint64_t best = square_raw(kAtgmTargetReach) + 1;
            for (const Unit& o : units_) {
                if (o.owner == u.owner || !unit_type(o.type).vehicle || o.inside || o.airborne || !sees(u.owner, o)) continue;
                const uint64_t d = (o.pos - u.order_point).length_sq_raw();
                if (d < best) {
                    target = &o;
                    best = d;
                }
            }
            const FixedVec2 aim = target ? target->pos : u.order_point;
            const FixedVec2 to_aim = aim - u.pos;
            if (to_aim.length_sq_raw() > square_raw(ability.weapon.range + def.radius)) {
                navigate(u, aim, u.order_path, map_.clamp_tile(tile_of(aim)), false);
                return;
            }
            if (to_aim.x.raw != 0 || to_aim.y.raw != 0) u.facing = to_aim;
            if (u.cooldown > 0) return;
            if (!try_fire(u, aim, target, ability.weapon, false)) {
                navigate(u, aim, u.order_path, map_.clamp_tile(tile_of(aim)), false);  // a hill in the way
            } else if (u.last_shot_tick == tick_) {
                --u.missiles;
                finish_ability(u);
            }
            return;
        }

        case AbilityId::LayApMine:
        case AbilityId::LayAtMine:
            return lay_mine(u, u.order_ability == AbilityId::LayAtMine);
        case AbilityId::ClearMines:
            return clear_mines(u);
        case AbilityId::LayWire:
            return put_up_obstacles(u, StructureType::Wire);
        case AbilityId::PlaceHedgehogs:
            return put_up_obstacles(u, StructureType::Hedgehogs);
        case AbilityId::BuildPillbox:
            return start_pillbox(u);
        case AbilityId::Demolish:
            return plant_charge(u);

        case AbilityId::MountAntenna: {
            // Up a cell tower that's nobody's or ours with a direction finder's aerial.
            const Structure* tower = structure_at(map_.clamp_tile(tile_of(u.order_point)));
            if (!tower || tower->type != StructureType::CellTower || tower->antenna == u.owner ||
                (tower->owner != kNoOwner && tower->owner != u.owner)) {
                return finish_ability(u);
            }
            if (distance_sq_to(*tower, u.pos) > square_raw(Fixed::from_int(1))) {
                navigate(u, tower->center, u.order_path, map_.clamp_tile(tile_of(tower->center)), false);
                return;
            }
            if (++u.work < kAntennaWork) return;
            find_structure_mut(tower->id)->antenna = u.owner;
            return finish_ability(u);
        }

        case AbilityId::BuildPost: {
            // Where he stands, watching the way he was shown: up a tree in a
            // wood, a hide in the crops, the reeds, the rubble; out in the open,
            // a stump. Done, he takes it up.
            if (at_point) return finish_ability(u);
            const TilePos t = map_.clamp_tile(tile_of(u.pos));
            const Structure* there = structure_at(t);
            if (there && (there->type != StructureType::ObservationPost || there->owner != u.owner)) return finish_ability(u);
            u.facing = to_point;
            if (!there && ++u.work < kPostWork) return;
            const Terrain ground = map_.terrain(t);
            const PostKind kind = ground == Terrain::Forest || ground == Terrain::Trail ? PostKind::Tree
                                  : ground == Terrain::Crops || ground == Terrain::Orchard || ground == Terrain::Swamp ||
                                            ground == Terrain::Ruins || ground == Terrain::Crater
                                      ? PostKind::Hide
                                      : PostKind::Stump;
            const EntityId id = there ? there->id : place_fieldwork(StructureType::ObservationPost, u.owner, t, to_point);
            if (Structure* s = find_structure_mut(id)) {
                if (!there) s->post = kind;
                s->facing = to_point;
            }
            const FixedVec2 watch = u.order_point;
            finish_ability(u);
            u.order = Order::Observe;
            u.order_point = watch;
            return;
        }

        case AbilityId::SwitchAmmo:
        case AbilityId::RadioSilence:
        case AbilityId::CallSupply:
        case AbilityId::Count:
            break;
    }
    finish_ability(u);
}

// Off to the depot for the kind of cargo this vehicle carries, and load up
// from the stock until full (or the stock runs dry).
void World::refill(Unit& u) {
    if (!load_up(u)) finish_ability(u);
}

bool World::load_up(Unit& u) {
    const UnitTypeDef& def = unit_type(u.type);
    if (def.supplies == Resource::Count) return false;
    const StructureType depot_type = def.supplies == Resource::Fuel ? StructureType::FuelDepot : StructureType::AmmoDepot;
    const Structure* depot = nearest_owned(u.owner, depot_type, u.pos);
    int32_t& stock = stock_[u.owner % kMaxPlayers][static_cast<size_t>(def.supplies)];
    if (!depot || u.carrying >= def.cargo_capacity || stock <= 0) return false;
    if (distance_sq_to(*depot, u.pos) > square_raw(Fixed::from_int(1))) {
        navigate(u, depot->center, u.order_path, map_.clamp_tile(tile_of(depot->center)), false);
        return true;
    }
    if (++u.work < kRefillInterval) return true;
    u.work = 0;
    --stock;
    ++u.carrying;
    u.carrying_type = def.supplies;
    return true;
}

namespace {

// How much of a full tank or rack (per mille) `v` is short of, in the
// cargo a service vehicle brings (see World::shortage).
bool uses(const Unit& v, Resource cargo) {
    const UnitTypeDef& vd = unit_type(v.type);
    if (vd.aircraft) return false;
    return cargo == Resource::Fuel ? vd.fuel_capacity.raw > 0 : cargo == Resource::Ammo && vd.rounds_capacity > 0;
}

}  // namespace

// Tankers and ammunition trucks attached to one unit of ours: they follow
// it and keep it topped up, and fetch more from the depot when empty.
void World::apply_supply(const Command& cmd) {
    const Unit* v = find_unit(cmd.target_unit);
    // Or a combat position of ours, for an ammunition truck to keep stocked.
    const Structure* post = v ? nullptr : find_structure(cmd.target_unit);
    for (EntityId id : cmd.units) {
        Unit* u = find_unit_mut(id);
        if (!u || u->owner != cmd.player) continue;
        const Resource cargo = unit_type(u->type).supplies;
        const bool stocks = post && cargo == Resource::Ammo && can_stock(*post, cmd.player);
        if (!stocks && (cargo == Resource::Count || !v || v->owner != cmd.player || v->id == u->id || !uses(*v, cargo))) {
            continue;
        }
        u->order = Order::Supply;
        u->serves = cmd.target_unit;  // the unit, or the position
        u->on_call = false;
        u->order_path.reset();
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->engaged = 0;
        u->work = 0;
    }
}

// A call over the radio: for whatever it's short of, the nearest of our
// tankers or ammunition trucks that is free (standing by, with something
// aboard) comes over, unless one is already on its way or with it.
void World::call_supply(const Unit& caller) {
    for (const Resource cargo : {Resource::Fuel, Resource::Ammo}) {
        if (shortage(caller, cargo) <= 0) continue;
        const bool looked_after = std::any_of(units_.begin(), units_.end(), [&](const Unit& o) {
            return o.order == Order::Supply && o.serves == caller.id && unit_type(o.type).supplies == cargo;
        });
        if (looked_after) continue;
        Unit* best = nullptr;
        uint64_t best_sq = 0;
        for (Unit& o : units_) {
            if (o.owner != caller.owner || o.order != Order::Idle || o.inside || o.carrying <= 0) continue;
            if (unit_type(o.type).supplies != cargo) continue;
            const uint64_t d = (o.pos - caller.pos).length_sq_raw();
            if (!best || d < best_sq) {
                best = &o;
                best_sq = d;
            }
        }
        if (!best) continue;
        best->order = Order::Supply;
        best->serves = caller.id;
        best->on_call = true;
        best->order_path.reset();
        best->chase_path.reset();
        best->work = 0;
    }
}

// On the job for one unit: top it up; answering a call, that's it, it's free
// again where it stands. Attached, it stays close by (seeing to anyone else
// nearby who needs it), and when it runs dry it goes for more, fills up and
// comes back. With the unit gone, it's free.
void World::update_supply(Unit& u) {
    const Resource cargo = unit_type(u.type).supplies;
    if (Structure* post = find_structure_mut(u.serves)) return stock_position(u, *post);
    const Unit* v = find_unit(u.serves);
    auto release = [&] {
        u.order = Order::Idle;
        u.serves = 0;
        u.on_call = false;
        u.order_path.reset();
        u.chase_path.reset();
        u.work = 0;
    };
    if (!v || cargo == Resource::Count) return release();
    const StructureType depot_type = cargo == Resource::Fuel ? StructureType::FuelDepot : StructureType::AmmoDepot;
    const Structure* depot = nearest_owned(u.owner, depot_type, u.pos);
    const bool at_depot = depot && distance_sq_to(*depot, u.pos) <= square_raw(Fixed::from_int(1));
    if (u.carrying <= 0 || (!u.on_call && at_depot && u.carrying < unit_type(u.type).cargo_capacity)) {
        if (u.on_call) return release();
        if (load_up(u)) return;  // off for more, or filling up
    }
    if (u.carrying > 0 && shortage(*v, cargo) > 0) return hand_over(u, *v);
    if (u.on_call) return release();
    if ((v->pos - u.pos).length_sq_raw() > square_raw(kEscortDistance)) {
        navigate(u, v->pos, u.chase_path, map_.clamp_tile(tile_of(v->pos)), false);
        return;
    }
    serve(u);
}

bool World::can_stock(const Structure& s, PlayerId player) const {
    return structure_type(s.type).cache_capacity > 0 && s.built && (s.owner == kNoOwner || s.owner == player) &&
           (s.cache == 0 || s.cache_owner == player);
}

// Stocking a position: brings its load over, fetches more when empty, and
// once the position is full waits close by, seeing to whoever needs it,
// ready to top the stock up again. Lost the position, it's free.
void World::stock_position(Unit& u, Structure& post) {
    const int32_t room = structure_type(post.type).cache_capacity - post.cache;
    if (!can_stock(post, u.owner) || unit_type(u.type).supplies != Resource::Ammo) {
        u.order = Order::Idle;
        u.serves = 0;
        u.order_path.reset();
        u.chase_path.reset();
        u.work = 0;
        return;
    }
    const Structure* depot = nearest_owned(u.owner, StructureType::AmmoDepot, u.pos);
    const bool at_depot = depot && distance_sq_to(*depot, u.pos) <= square_raw(Fixed::from_int(1));
    if (room > 0 && (u.carrying <= 0 || (at_depot && u.carrying < unit_type(u.type).cargo_capacity))) {
        if (load_up(u)) return;
    }
    const uint64_t dist_sq = distance_sq_to(post, u.pos);
    if (room > 0 && u.carrying > 0) {
        if (dist_sq > square_raw(kCacheUnload)) {
            navigate(u, post.center, u.chase_path, map_.clamp_tile(tile_of(post.center)), false);
            return;
        }
        if (++u.work < kRearmInterval) return;
        u.work = 0;
        ++post.cache;
        post.cache_owner = u.owner;
        --u.carrying;
        return;
    }
    if (dist_sq > square_raw(kEscortDistance)) {
        navigate(u, post.center, u.chase_path, map_.clamp_tile(tile_of(post.center)), false);
        return;
    }
    serve(u);
}

// Everyone of ours short of rounds at a stocked position of ours takes a
// unit of ammunition from it, the nearest position first.
void World::draw_from_caches() {
    for (Unit& u : units_) {
        const UnitTypeDef& def = unit_type(u.type);
        const int32_t full = rack(u);
        if (full == 0 || u.rounds >= full || u.airborne) continue;
        Structure* best = nullptr;
        uint64_t best_sq = 0;
        for (Structure& s : structures_) {
            if (s.cache <= 0 || s.cache_owner != u.owner) continue;
            const uint64_t d = u.inside == s.id ? 0 : distance_sq_to(s, u.pos);
            if (d > square_raw(kCacheReach) || (best && d >= best_sq)) continue;
            best = &s;
            best_sq = d;
        }
        if (!best) continue;
        --best->cache;
        u.rounds = std::min(full, u.rounds + def.rounds_per_supply);
    }
    // The pumps of a gas station we hold.
    for (Unit& u : units_) {
        const UnitTypeDef& def = unit_type(u.type);
        if (def.fuel_capacity.raw == 0 || u.fuel >= def.fuel_capacity || u.airborne || def.aircraft) continue;
        for (Structure& s : structures_) {
            int32_t& tanks = s.cargo[static_cast<size_t>(Resource::Fuel)];
            if (s.type != StructureType::GasStation || s.owner != u.owner || tanks <= 0) continue;
            if (distance_sq_to(s, u.pos) > square_raw(kPumpReach)) continue;
            u.fuel = min(def.fuel_capacity, u.fuel + Fixed::from_int(kTilesPerFuel));
            --tanks;
            break;
        }
    }
}

void World::hand_over(Unit& u, const Unit& target) {
    const bool fuel = unit_type(u.type).supplies == Resource::Fuel;
    const Fixed reach = unit_type(u.type).radius + unit_type(target.type).radius + Fixed::from_ratio(1, 2);
    if ((target.pos - u.pos).length_sq_raw() > square_raw(reach)) {
        navigate(u, target.pos, u.chase_path, map_.clamp_tile(tile_of(target.pos)), false);
        return;
    }
    if (++u.work < (fuel ? kRefuelInterval : kRearmInterval)) return;
    u.work = 0;
    Unit* v = find_unit_mut(target.id);
    const UnitTypeDef& vd = unit_type(v->type);
    if (fuel) {
        v->fuel = min(vd.fuel_capacity, v->fuel + Fixed::from_int(kTilesPerFuel));
    } else if (v->rounds < rack(*v)) {
        v->rounds = std::min(rack(*v), v->rounds + vd.rounds_per_supply);
    } else {
        v->missiles = std::min(vd.missile_capacity, v->missiles + 1);  // a missile a unit
    }
    --u.carrying;
}

int64_t World::shortage(const Unit& v, Resource cargo) const {
    const UnitTypeDef& vd = unit_type(v.type);
    if (vd.aircraft) return 0;  // rearmed at the airfield
    if (cargo == Resource::Fuel && vd.fuel_capacity.raw > 0) {
        return static_cast<int64_t>(vd.fuel_capacity.raw - v.fuel.raw) * 1000 / vd.fuel_capacity.raw;
    }
    if (cargo != Resource::Ammo) return 0;
    int64_t most = 0;
    if (const int32_t full = rack(v); full > 0) most = static_cast<int64_t>(full - v.rounds) * 1000 / full;
    if (vd.missile_capacity > 0) {
        most = std::max(most, static_cast<int64_t>(vd.missile_capacity - v.missiles) * 1000 / vd.missile_capacity);
    }
    return most;
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
        const int64_t missing = shortage(v, def.supplies);
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
    hand_over(u, *neediest);
}

void World::lob(Unit& shooter, FixedVec2 aim, const WeaponDef& weapon, bool enters, Shell shell) {
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
    p.shell = shell;
    shooter.last_shot_tick = tick_;
    shooter.last_shot_at = aim;
    projectiles_.push_back(p);
}

}  // namespace engine
