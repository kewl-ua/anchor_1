// The engineering half of World: mines, obstacles, pillboxes and
// demolition charges.

#include <algorithm>
#include <optional>

#include "engine/world.h"

namespace engine {

namespace {

// How close a sapper has to be to the middle of the tile he works on.
constexpr Fixed kWorkReach = Fixed::from_ratio(3, 4);

constexpr WeaponDef kApMine{.name = "Anti-personnel mine", .damage = 50, .damage_type = DamageType::Explosive,
                            .range = Fixed{}, .reload = 0, .projectile_speed = Fixed{},
                            .splash_radius = Fixed::from_ratio(3, 5), .accuracy = 100, .miss_spread = Fixed{}};
constexpr WeaponDef kAtMine{.name = "Anti-tank mine", .damage = 160, .damage_type = DamageType::AntiTank,
                            .range = Fixed{}, .reload = 0, .projectile_speed = Fixed{}, .splash_radius = Fixed{},
                            .accuracy = 100, .miss_spread = Fixed{}};
constexpr WeaponDef kDirectionalMine{.name = "Directional mine", .damage = 60, .damage_type = DamageType::Explosive,
                                     .range = kDirectionalReach, .reload = 0, .projectile_speed = Fixed{},
                                     .splash_radius = Fixed{}, .accuracy = 100, .miss_spread = Fixed{}};
constexpr WeaponDef kCharge{.name = "Demolition charge", .damage = 900, .damage_type = DamageType::AntiTank,
                            .range = Fixed{}, .reload = 0, .projectile_speed = Fixed{},
                            .splash_radius = Fixed::from_ratio(3, 2), .accuracy = 100, .miss_spread = Fixed{}};
constexpr WeaponDef kChargeBlast{.name = "Demolition charge", .damage = 80, .damage_type = DamageType::Explosive,
                                 .range = Fixed{}, .reload = 0, .projectile_speed = Fixed{},
                                 .splash_radius = Fixed::from_ratio(3, 2), .accuracy = 100, .miss_spread = Fixed{}};
// A line charge going off: the men along it, and the wire and hedgehogs it tears away.
constexpr WeaponDef kLineChargeBlast{.name = "Line charge", .damage = 90, .damage_type = DamageType::Explosive,
                                     .range = Fixed{}, .reload = 0, .projectile_speed = Fixed{},
                                     .splash_radius = Fixed::from_int(1), .accuracy = 100, .miss_spread = Fixed{}};
constexpr WeaponDef kLineChargeRip{.name = "Line charge", .damage = 800, .damage_type = DamageType::AntiTank,
                                   .range = Fixed{}, .reload = 0, .projectile_speed = Fixed{},
                                   .splash_radius = Fixed::from_int(1), .accuracy = 100, .miss_spread = Fixed{}};

// Whether a point is in a directional mine's sector, no farther out than `reach`.
bool in_sector(const Mine& m, FixedVec2 p, Fixed reach) {
    const FixedVec2 d = p - tile_center(m.tile);
    const uint64_t far = d.length_sq_raw();
    if (far > square_raw(reach)) return false;
    const Fixed along = d.x * m.facing.x + d.y * m.facing.y;
    if (along.raw <= 0) return false;
    const auto a = static_cast<uint64_t>(along.raw);
    return a * a * 5 >= far * static_cast<uint64_t>(kDirectionalSectorFifths);
}

}  // namespace

// A pressure mine: walk to the tile, work on it, pay from the stock, and
// it's in. A directional one goes where he stands, facing the way he was
// shown.
void World::lay_mine(Unit& u, MineKind kind) {
    const bool directional = kind == MineKind::Directional;
    const FixedVec2 facing = u.order_point - u.pos;
    const TilePos t = map_.clamp_tile(tile_of(directional ? u.pos : u.order_point));
    const bool taken = std::any_of(mines_.begin(), mines_.end(), [&](const Mine& m) { return m.tile == t; });
    // Not in concrete: a mine needs earth to go into (one on its legs stands on anything).
    if (taken || !map_.passable(t, MoveClass::Foot) || (!directional && map_.terrain(t) == Terrain::Road) ||
        (directional && facing.x.raw == 0 && facing.y.raw == 0)) {
        return finish_ability(u);
    }
    if (!directional && (tile_center(t) - u.pos).length_sq_raw() > square_raw(kWorkReach)) {
        navigate(u, tile_center(t), u.order_path, t, false);
        return;
    }
    if (directional) u.facing = facing;
    if (++u.work < kMineWork) return;
    Stock& stock = stock_[u.owner % kMaxPlayers];
    const Stock& cost = kind == MineKind::AntiTank ? kAtMineCost : directional ? kDirectionalMineCost : kApMineCost;
    if (can_afford(stock, cost)) {
        pay(stock, cost);
        mines_.push_back({next_mine_id_++, u.owner, t, kind, 0, directional ? facing * (Fixed::from_int(1) / facing.length()) : FixedVec2{}});
    }
    finish_ability(u);
}

// A directional mine goes off: its fragments across its sector, into every
// man there (a trench's walls stop them; armor shrugs them off).
void World::set_off_directional(const Mine& m) {
    const FixedVec2 at = tile_center(m.tile);
    const int32_t percent = has_upgrade(m.owner, UpgradeId::HeavyCharges) ? kHeavyChargePercent : 100;
    recent_impacts_.push_back({tick_, at, UnitTypeId::Sapper, Fixed::from_ratio(1, 3)});
    recent_impacts_.back().toward = m.facing;
    for (const Unit& u : units_) {
        if (u.inside || u.airborne || u.hp <= 0 || unit_type(u.type).vehicle || !in_sector(m, u.pos, kDirectionalReach)) continue;
        hurt(u, kDirectionalMine, {at, 0, true, false, percent});
    }
}

// Lift the enemy mines we have found around the point, nearest first.
void World::clear_mines(Unit& u) {
    const Mine* nearest = nullptr;
    uint64_t best = 0;
    for (const Mine& m : mines_) {
        if (m.owner == u.owner || !knows(u.owner, m)) continue;
        if ((tile_center(m.tile) - u.order_point).length_sq_raw() > square_raw(kClearRadius)) continue;
        const uint64_t d = (tile_center(m.tile) - u.pos).length_sq_raw();
        if (!nearest || d < best) {
            nearest = &m;
            best = d;
        }
    }
    if (!nearest) return finish_ability(u);
    if (best > square_raw(kWorkReach)) {
        u.work = 0;
        navigate(u, tile_center(nearest->tile), u.order_path, nearest->tile, false);
        return;
    }
    if (++u.work < kClearWork) return;
    u.work = 0;
    const uint32_t lifted = nearest->id;
    std::erase_if(mines_, [&](const Mine& m) { return m.id == lifted; });
}

// Wire or hedgehogs along the line, a tile at a time, paid tile by tile.
void World::put_up_obstacles(Unit& u, StructureType type) {
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
    if (best_sq > square_raw(kWorkReach)) {
        navigate(u, tile_center(*best), u.order_path, *best, false);
        return;
    }
    const bool wire = type == StructureType::Wire;
    const int32_t index = best->y * map_.width() + best->x;
    if (++dig_work_[index] < (wire ? kWireWork : kHedgehogWork)) return;
    dig_work_.erase(index);
    Stock& stock = stock_[u.owner % kMaxPlayers];
    const Stock& cost = wire ? kWireCost : kHedgehogCost;
    if (!can_afford(stock, cost)) return finish_ability(u);
    pay(stock, cost);
    place_fieldwork(type, u.owner, *best, {});
}

// A pillbox on the sapper's own tile, its slit towards the point: laid out
// and paid for now, then built like any building (other sappers can help).
void World::start_pillbox(Unit& u) {
    const TilePos t = map_.clamp_tile(tile_of(u.pos));
    const FixedVec2 facing = u.order_point - u.pos;
    const StructureDef& def = structure_type(StructureType::Pillbox);
    Stock& stock = stock_[u.owner % kMaxPlayers];
    if ((facing.x.raw == 0 && facing.y.raw == 0) || !diggable(t) || !can_afford(stock, def.cost)) {
        return finish_ability(u);
    }
    pay(stock, def.cost);
    const EntityId site = place_structure(StructureType::Pillbox, u.owner, t, 1, 1);
    Structure* s = find_structure_mut(site);
    s->built = false;
    s->hp = 1;
    s->facing = facing;
    u.order = Order::Build;
    u.order_target = site;
    u.order_goal = t;
    u.order_path.reset();
    u.work = 0;
}

// Up to the structure, a charge planted, a fuse lit, and away.
void World::plant_charge(Unit& u) {
    const Structure* target = structure_at(map_.clamp_tile(tile_of(u.order_point)));
    if (!target) return finish_ability(u);
    if (distance_sq_to(*target, u.pos) > square_raw(Fixed::from_int(1))) {
        navigate(u, target->center, u.order_path, map_.clamp_tile(tile_of(target->center)), false);
        return;
    }
    if (++u.work < kPlantWork) return;
    charges_.push_back({u.owner, target->id, u.pos, tick_ + kFuseTicks});
    // Run for it, away from the charge.
    FixedVec2 away = u.pos - target->center;
    if (away.x.raw == 0 && away.y.raw == 0) away = {Fixed::from_int(1), Fixed{}};
    const FixedVec2 safe = clamp_to_map(u.pos + away * (kSapperRetreat / away.length()), Fixed{});
    u.order = Order::Move;
    u.order_point = safe;
    u.order_goal = map_.clamp_tile(tile_of(safe));
    u.order_path.reset();
    u.work = 0;
}

// The first enemy of the right kind onto a mine's tile sets it off. The
// side that laid it knows where it is and keeps clear.
void World::update_mines() {
    std::vector<uint32_t> gone;
    for (const Mine& m : mines_) {
        if (m.kind == MineKind::Directional) {  // a man of the enemy's at the tripwire
            const bool tripped = std::any_of(units_.begin(), units_.end(), [&](const Unit& u) {
                return u.owner != m.owner && !u.inside && !u.airborne && u.hp > 0 && !unit_type(u.type).vehicle &&
                       in_sector(m, u.pos, kTripwireReach);
            });
            if (tripped) {
                set_off_directional(m);
                gone.push_back(m.id);
            }
            continue;
        }
        for (const Unit& u : units_) {
            if (u.owner == m.owner || u.inside || u.airborne || u.hp <= 0) continue;
            if (unit_type(u.type).vehicle != m.anti_tank() || tile_of(u.pos) != m.tile) continue;
            const FixedVec2 at = tile_center(m.tile);
            const WeaponDef& blast = m.anti_tank() ? kAtMine : kApMine;
            const int32_t percent = has_upgrade(m.owner, UpgradeId::HeavyCharges) ? kHeavyChargePercent : 100;
            recent_impacts_.push_back({tick_, at, u.type, m.anti_tank() ? Fixed::from_int(1) : blast.splash_radius});
            hurt(u, blast, {at, 0, true, true, percent});
            if (!m.anti_tank()) {  // the fragments fly
                for (const Unit& other : units_) {
                    if (other.id == u.id || other.inside || other.airborne || unit_type(other.type).vehicle) continue;
                    if ((other.pos - u.pos).length_sq_raw() <= square_raw(blast.splash_radius)) {
                        hurt(other, blast, {at, 0, true, true, percent});
                    }
                }
            }
            gone.push_back(m.id);
            break;
        }
    }
    std::erase_if(mines_, [&](const Mine& m) { return std::find(gone.begin(), gone.end(), m.id) != gone.end(); });
}

// Sappers find the enemy's mines near them; once found, a mine stays known.
void World::find_mines() {
    for (Mine& m : mines_) {
        for (const Unit& u : units_) {
            if (u.owner == m.owner || u.owner >= kMaxPlayers || u.inside || (!unit_type(u.type).engineer && !unit_type(u.type).breacher)) continue;
            if ((tile_center(m.tile) - u.pos).length_sq_raw() <= square_raw(kMineDetection)) {
                m.found_by = static_cast<uint8_t>(m.found_by | (1u << u.owner));
            }
        }
    }
}

void World::update_charges() {
    std::vector<Charge> ticking;
    for (const Charge& c : charges_) {
        if (tick_ < c.goes_off) {
            ticking.push_back(c);
            continue;
        }
        recent_impacts_.push_back({tick_, c.pos, UnitTypeId::Sapper, kChargeBlast.splash_radius});
        const int32_t percent = has_upgrade(c.owner, UpgradeId::HeavyCharges) ? kHeavyChargePercent : 100;
        WeaponDef charge = kCharge;
        charge.damage = charge.damage * percent / 100;
        if (const Structure* s = find_structure(c.target)) hurt_structure(*s, charge);
        for (const Unit& u : units_) {
            if (u.inside || u.airborne) continue;
            if ((u.pos - c.pos).length_sq_raw() <= square_raw(kChargeBlast.splash_radius)) {
                hurt(u, kChargeBlast, {c.pos, 0, true, true, percent});
            }
        }
    }
    charges_ = std::move(ticking);
}

// Line charges going off: along the hose the mines are set off, wire and
// hedgehogs torn away, men by it hurt; its bursts all along it.
void World::update_line_charges() {
    std::vector<LineCharge> lying;
    for (const LineCharge& c : line_charges_) {
        if (tick_ < c.goes_off) {
            lying.push_back(c);
            continue;
        }
        const FixedVec2 d = c.to - c.from;
        const Fixed l2 = d.x * d.x + d.y * d.y;
        auto nearest = [&](FixedVec2 p) {  // the point of the hose nearest p
            if (l2.raw <= 0) return c.from;
            const Fixed t = clamp(((p.x - c.from.x) * d.x + (p.y - c.from.y) * d.y) / l2, Fixed{}, Fixed::from_int(1));
            return c.from + d * t;
        };
        const uint64_t lane = square_raw(kLineChargeLane);
        std::erase_if(mines_, [&](const Mine& m) {
            const FixedVec2 at = tile_center(m.tile);
            if ((at - nearest(at)).length_sq_raw() > lane) return false;
            recent_impacts_.push_back({tick_, at, UnitTypeId::Sapper, Fixed::from_ratio(1, 2)});
            return true;
        });
        std::vector<EntityId> torn;
        for (const Structure& s : structures_) {
            if (!is_obstacle(s.type) || s.tiles.empty()) continue;
            const FixedVec2 at = tile_center(s.tiles.front());
            if ((at - nearest(at)).length_sq_raw() <= lane) torn.push_back(s.id);
        }
        for (EntityId id : torn) {
            if (const Structure* s = find_structure(id)) hurt_structure(*s, kLineChargeRip);
        }
        for (const Unit& u : units_) {
            if (u.inside || u.airborne || u.hp <= 0) continue;
            const FixedVec2 q = nearest(u.pos);
            if ((u.pos - q).length_sq_raw() <= lane) hurt(u, kLineChargeBlast, {q, 0, true, true, 100});
        }
        const int32_t steps = std::max(1, (Fixed::from_int(2) * d.length()).to_int());
        for (int32_t k = 0; k <= steps; ++k) {
            recent_impacts_.push_back({tick_, c.from + d * Fixed::from_ratio(k, steps), UnitTypeId::Ur77, Fixed::from_int(1)});
        }
    }
    line_charges_ = std::move(lying);
}

// Through the slit only: the nearest enemy in sight within the sector it faces.
const Unit* World::find_enemy_in_slit(Unit& u, const Structure& pillbox) {
    const uint64_t sight_sq = square_raw(unit_type(u.type).sight);
    // cos^2 of half the sector: 1/4 for 120 degrees.
    static_assert(kPillboxSectorDegrees == 120);
    const int64_t fx = pillbox.facing.x.raw >> 8;
    const int64_t fy = pillbox.facing.y.raw >> 8;
    const Unit* best = nullptr;
    uint64_t best_sq = 0;
    for (const Unit& other : units_) {
        if (other.owner == u.owner || other.airborne || !sees(u.owner, other)) continue;
        const FixedVec2 v = other.pos - pillbox.center;
        const uint64_t d = v.length_sq_raw();
        if (d > sight_sq) continue;
        const int64_t vx = v.x.raw >> 8;
        const int64_t vy = v.y.raw >> 8;
        const int64_t dot = fx * vx + fy * vy;
        if (dot <= 0 || 4 * (dot / 256) * (dot / 256) < ((fx * fx + fy * fy) / 256) * ((vx * vx + vy * vy) / 256)) {
            continue;
        }
        if (!best || d < best_sq) {
            best = &other;
            best_sq = d;
        }
    }
    u.engaged = best ? best->id : 0;
    return best;
}

}  // namespace engine
