// The economy half of World: resources on the map, rear troops' work,
// construction, production, and the supply chain: trains, trucks and depots.

#include <algorithm>

#include "engine/world.h"

namespace engine {

namespace {

// How close to a forest or rock tile a rear trooper must stand to work it.
constexpr Fixed kWorkReach = Fixed::from_ratio(3, 5);
// How close to the headquarters' walls to hand over materials or go in.
constexpr Fixed kDoorReach = Fixed::from_int(1);
// Retraining costs the rifle and the ammunition; the man is already there.
constexpr Stock kRetrainPrice = {0, 0, 0, 20, 0};

bool is_resource_terrain(Terrain t) { return t == Terrain::Forest || t == Terrain::Rock; }

// Units of `owner` referenced by the command, deduplicated, in id order.
template <typename FindFn>
std::vector<Unit*> owned_workers(const Command& cmd, FindFn find) {
    std::vector<Unit*> group;
    for (EntityId id : cmd.units) {
        Unit* u = find(id);
        if (u && u->owner == cmd.player && (unit_type(u->type).worker || unit_type(u->type).engineer)) {
            group.push_back(u);
        }
    }
    std::sort(group.begin(), group.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
    group.erase(std::unique(group.begin(), group.end()), group.end());
    return group;
}

}  // namespace

void World::init_resources() {
    for (int32_t y = 0; y < map_.height(); ++y) {
        for (int32_t x = 0; x < map_.width(); ++x) {
            const Terrain t = map_.terrain(x, y);
            if (t == Terrain::Forest) map_.set_resource({x, y}, kForestMaterials);
            if (t == Terrain::Rock) map_.set_resource({x, y}, kRockMaterials);
        }
    }
}

EntityId World::place_structure(StructureType type, PlayerId owner, TilePos origin, int32_t w, int32_t h) {
    Structure s;
    s.id = next_id_++;
    s.type = type;
    s.owner = owner;
    s.hp = structure_type(type).max_hp;
    FixedVec2 sum{};
    for (int32_t dy = 0; dy < h; ++dy) {
        for (int32_t dx = 0; dx < w; ++dx) {
            const TilePos t{origin.x + dx, origin.y + dy};
            if (!map_.contains(t)) continue;
            switch (type) {
                case StructureType::Trench: map_.set_terrain(t.x, t.y, Terrain::Trench); break;
                case StructureType::Foxhole: map_.set_terrain(t.x, t.y, Terrain::Foxhole); break;
                case StructureType::Dugout: map_.set_terrain(t.x, t.y, Terrain::Dugout); break;
                case StructureType::GunPit: map_.set_terrain(t.x, t.y, Terrain::GunPit); break;
                case StructureType::Wire: map_.set_terrain(t.x, t.y, Terrain::Wire); break;
                case StructureType::Hedgehogs: map_.set_terrain(t.x, t.y, Terrain::Hedgehogs); break;
                case StructureType::Pillbox: map_.set_terrain(t.x, t.y, Terrain::Pillbox); break;
                case StructureType::Parapet: break;  // a mound on the ground it stands on
                case StructureType::Airfield: map_.set_terrain(t.x, t.y, Terrain::Airstrip); break;
                default: map_.set_terrain(t.x, t.y, Terrain::Building); break;
            }
            map_.set_resource(t, 0);
            structure_tiles_[static_cast<size_t>(t.y * map_.width() + t.x)] = s.id;
            s.tiles.push_back(t);
            sum += tile_center(t);
        }
    }
    if (s.tiles.empty()) return 0;
    const auto n = static_cast<int32_t>(s.tiles.size());
    s.center = {sum.x / n, sum.y / n};
    if (type == StructureType::Station) s.next_train = tick_ + kTrainInterval;
    structures_.push_back(std::move(s));  // ids only grow: still sorted
    on_map_changed();
    return structures_.back().id;
}

bool World::can_convert(const Structure& s, PlayerId player) const {
    return s.type == StructureType::House && s.converted == StructureType::Count && s.built &&
           s.tiles.size() >= kSpaciousTiles && (s.owner == kNoOwner || s.owner == player);
}

bool World::can_place(StructureType type, TilePos origin) const {
    const StructureDef& def = structure_type(type);
    if (!def.buildable) return false;
    for (int32_t dy = 0; dy < def.height; ++dy) {
        for (int32_t dx = 0; dx < def.width; ++dx) {
            const TilePos t{origin.x + dx, origin.y + dy};
            if (!map_.contains(t) || structure_id_at(t) != 0) return false;
            const Terrain terrain = map_.terrain(t);
            // Open ground: fields, village yards, ruins. Not in a forest, not on a road through it.
            if (terrain != Terrain::Grass && terrain != Terrain::Urban && terrain != Terrain::Ruins) return false;
        }
    }
    return true;
}

// --- Orders ------------------------------------------------------------------

void World::apply_gather(const Command& cmd) {
    const TilePos clicked = map_.clamp_tile(tile_of(cmd.target));
    const bool on_resource = is_resource_terrain(map_.terrain(clicked)) && map_.resource(clicked) > 0;
    const std::optional<TilePos> tile = on_resource ? std::optional<TilePos>(clicked) : nearest_resource(clicked, 3);
    if (!tile) return;
    for (Unit* u : owned_workers(cmd, [this](EntityId id) { return find_unit_mut(id); })) {
        leave_structure(*u);
        u->order = Order::Gather;
        u->gather_tile = *tile;
        u->order_goal = *tile;
        u->order_path = field_to(*tile, MoveClass::Foot);
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->engaged = 0;
        u->work = 0;
    }
}

void World::apply_build(const Command& cmd) {
    std::vector<Unit*> builders = owned_workers(cmd, [this](EntityId id) { return find_unit_mut(id); });
    if (cmd.target_unit == 0) std::erase_if(builders, [](const Unit* u) { return !unit_type(u->type).worker; });
    if (builders.empty()) return;

    EntityId site = cmd.target_unit;
    if (site != 0) {
        Structure* s = find_structure_mut(site);
        if (!s) return;
        const auto depot = static_cast<StructureType>(cmd.structure_type);
        if (s->owner == cmd.player && !s->built) {
            // Help finish one of our own building sites.
        } else if (cmd.structure_type < kStructureTypeCount && depot_cargo(depot) && can_convert(*s, cmd.player)) {
            // Turn a spacious village building into a depot: whoever of ours
            // is inside comes out, and the work starts.
            Stock& stock = stock_[cmd.player % kMaxPlayers];
            if (!can_afford(stock, kConversionCost)) return;
            pay(stock, kConversionCost);
            const std::vector<EntityId> inside = s->garrison;
            for (EntityId id : inside) {
                if (Unit* u = find_unit_mut(id)) leave_structure(*u);
            }
            s = find_structure_mut(site);
            s->converted = depot;
            s->owner = cmd.player;
            s->built = false;
            s->build_progress = 0;
        } else {
            return;
        }
    } else {
        // Lay a new foundation: paid up front, like in AoE II.
        if (cmd.structure_type >= kStructureTypeCount) return;
        const auto type = static_cast<StructureType>(cmd.structure_type);
        const StructureDef& def = structure_type(type);
        const TilePos origin = tile_of(cmd.target);
        Stock& stock = stock_[cmd.player % kMaxPlayers];
        if (!can_place(type, origin) || !can_afford(stock, def.cost)) return;
        pay(stock, def.cost);
        site = place_structure(type, cmd.player, origin, def.width, def.height);
        Structure* s = find_structure_mut(site);
        s->built = false;
        s->hp = 1;
    }

    const Structure* s = find_structure(site);
    const TilePos goal = map_.clamp_tile(tile_of(s->center));
    for (Unit* u : builders) {
        leave_structure(*u);
        u->order = Order::Build;
        u->order_target = site;
        u->order_goal = goal;
        u->order_path = field_to(goal, MoveClass::Foot);
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->engaged = 0;
        u->work = 0;
    }
}

void World::apply_train(const Command& cmd) {
    Structure* s = find_structure_mut(cmd.target_unit);
    if (!s || !s->built || s->owner != cmd.player || cmd.unit_type >= kUnitTypeCount) return;
    const auto type = static_cast<UnitTypeId>(cmd.unit_type);
    if (!can_train(s->type, type) || s->queue.size() >= kMaxQueue) return;
    Stock& stock = stock_[cmd.player % kMaxPlayers];
    if (!can_afford(stock, unit_type(type).cost)) return;
    pay(stock, unit_type(type).cost);
    s->queue.push_back(type);
}

void World::apply_retrain(const Command& cmd) {
    for (Unit* u : owned_workers(cmd, [this](EntityId id) { return find_unit_mut(id); })) {
        const Structure* hq = nearest_headquarters(u->owner, u->pos);
        if (!hq || u->inside == hq->id) continue;
        leave_structure(*u);
        u->order = Order::Retrain;
        u->order_target = hq->id;
        u->order_goal = map_.clamp_tile(tile_of(hq->center));
        u->order_path = field_to(u->order_goal, MoveClass::Foot);
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->engaged = 0;
        u->work = 0;
    }
}

// --- Work --------------------------------------------------------------------

// Cut / quarry until the hands are full, carry it to the headquarters, go
// back. When the tile is used up, move on to the nearest one like it.
void World::update_gathering(Unit& u) {
    auto finish = [&u] {
        u.order = Order::Idle;
        u.order_path.reset();
        u.chase_path.reset();
    };

    const bool tile_left = is_resource_terrain(map_.terrain(u.gather_tile)) && map_.resource(u.gather_tile) > 0;
    if (!tile_left) {
        if (const std::optional<TilePos> next = nearest_resource(u.gather_tile, 8)) {
            u.gather_tile = *next;
            u.order_goal = *next;
            u.work = 0;
        } else if (u.carrying == 0) {
            return finish();
        }
    }
    const bool nothing_left = !is_resource_terrain(map_.terrain(u.gather_tile)) || map_.resource(u.gather_tile) <= 0;

    if (u.carrying >= kCarryCapacity || (nothing_left && u.carrying > 0)) {
        const Structure* hq = nearest_drop_off(u.owner, u.pos);
        if (!hq) return finish();
        if (distance_sq_to(*hq, u.pos) <= square_raw(kDoorReach)) {
            stock_[u.owner % kMaxPlayers][static_cast<size_t>(Resource::Materials)] += u.carrying;
            u.carrying = 0;
            if (nothing_left) finish();
            return;
        }
        navigate(u, hq->center, u.chase_path, map_.clamp_tile(tile_of(hq->center)), false);
        return;
    }

    if (distance_sq_to_tile(u.gather_tile, u.pos) <= square_raw(kWorkReach)) {
        u.facing = tile_center(u.gather_tile) - u.pos;
        const Tick needed = map_.terrain(u.gather_tile) == Terrain::Rock ? kQuarryTicks : kChopTicks;
        if (++u.work < needed) return;
        u.work = 0;
        const int32_t left = map_.resource(u.gather_tile) - 1;
        map_.set_resource(u.gather_tile, left);
        ++u.carrying;
        // A cut-down forest is an open field: cover gone, tanks can pass.
        if (left <= 0) map_.set_terrain(u.gather_tile.x, u.gather_tile.y, Terrain::Grass);
        return;
    }
    if (navigate(u, tile_center(u.gather_tile), u.order_path, u.order_goal, false) == Step::Blocked &&
        distance_sq_to_tile(u.gather_tile, u.pos) > square_raw(kWorkReach + kWorkReach)) {
        finish();  // can't get there
    }
}

// Walk to the headquarters, hand in the carbine, drill, come out a rifleman.
void World::update_retrain(Unit& u) {
    Structure* hq = find_structure_mut(u.order_target);
    auto finish = [&u] {
        u.order = Order::Idle;
        u.order_path.reset();
        u.work = 0;
    };

    if (u.inside) {
        if (++u.work < kRetrainTicks) return;
        leave_structure(u);
        u.type = UnitTypeId::Rifleman;
        u.hp = unit_type(u.type).max_hp;
        u.carrying = 0;
        u.cooldown = 0;
        return finish();
    }

    if (!hq) return finish();
    if (distance_sq_to(*hq, u.pos) <= square_raw(kDoorReach)) {
        Stock& stock = stock_[u.owner % kMaxPlayers];
        if (!can_afford(stock, kRetrainPrice)) return finish();
        pay(stock, kRetrainPrice);
        hq->garrison.push_back(u.id);
        u.inside = hq->id;
        u.pos = hq->center;
        u.prev_pos = hq->center;
        u.work = 0;
        u.order_path.reset();
        return;
    }
    if (navigate(u, hq->center, u.order_path, u.order_goal, false) == Step::Blocked &&
        distance_sq_to(*hq, u.pos) > square_raw(kDoorReach)) {
        finish();
    }
}

// Rear troops at a building site add their work until it stands. Its health
// grows with the work, so a half-built barracks is also half as tough.
void World::update_building(Unit& u) {
    Structure* s = find_structure_mut(u.order_target);
    if (!s || s->built) {
        u.order = Order::Idle;
        u.order_path.reset();
        return;
    }
    if (distance_sq_to(*s, u.pos) <= square_raw(kDoorReach)) {
        const StructureDef& def = structure_type(s->type);
        u.facing = s->center - u.pos;
        if (s->converted != StructureType::Count) {
            // A building being turned into a depot stands already: just the work.
            if (++s->build_progress >= kConversionWork) s->built = true;
            return;
        }
        const int32_t before = def.max_hp * static_cast<int32_t>(s->build_progress) / static_cast<int32_t>(def.build_time);
        ++s->build_progress;
        const int32_t after = def.max_hp * static_cast<int32_t>(s->build_progress) / static_cast<int32_t>(def.build_time);
        s->hp = std::min(def.max_hp, s->hp + after - before);
        if (s->build_progress >= def.build_time) {
            s->built = true;
            // A finished pillbox is held by whoever sits in it, like a house.
            if (is_shelter(s->type) && s->garrison.empty()) s->owner = kNoOwner;
        }
        return;
    }
    if (navigate(u, s->center, u.order_path, u.order_goal, false) == Step::Blocked &&
        distance_sq_to(*s, u.pos) > square_raw(kDoorReach)) {
        u.order = Order::Idle;
        u.order_path.reset();
    }
}

// Every building works on the front of its queue; the unit walks out of the door.
void World::update_production() {
    for (Structure& s : structures_) {
        if (s.queue.empty() || !s.built) continue;
        const UnitTypeId type = s.queue.front();
        if (++s.progress < unit_type(type).train_time) continue;
        s.progress = 0;
        s.queue.erase(s.queue.begin());
        const UnitTypeDef& def = unit_type(type);
        // Aircraft are rolled out onto the runway; the rest walk out of the door.
        const FixedVec2 out = def.aircraft ? parking_spot(s, 0) : door_of(s, move_class(def));
        const EntityId id = spawn_unit(s.owner, type, out);
        if (type == UnitTypeId::Truck) find_unit_mut(id)->order = Order::Haul;  // straight onto the supply run
        // A tanker or an ammunition truck comes with what was paid for aboard; for more, the depot.
        if (def.supplies != Resource::Count) find_unit_mut(id)->carrying = def.cost[static_cast<size_t>(def.supplies)];
    }
}

// Trains come to every station on schedule: the men join the personnel pool,
// the freight waits at the station for trucks. No station, no trains.
void World::update_trains() {
    for (Structure& s : structures_) {
        if (s.type != StructureType::Station || s.owner >= kMaxPlayers || tick_ < s.next_train) continue;
        s.next_train += has_upgrade(s.owner, UpgradeId::TrainSchedule) ? kTrainIntervalUpgraded : kTrainInterval;
        const int32_t percent = has_upgrade(s.owner, UpgradeId::TrainCapacity) ? kTrainCapacityPercent : 100;
        Stock& stock = stock_[s.owner];
        for (size_t r = 0; r < kResourceCount; ++r) {
            const int32_t load = kTrainCargo[r] * percent / 100;
            if (static_cast<Resource>(r) == Resource::Personnel) {
                stock[r] += load;
            } else {
                s.cargo[r] += load;
            }
        }
    }
}

// Back on the supply run. Sent to one of our depots, a truck is assigned to
// it: that depot's freight, to that depot. Or it's given a kind of freight
// (to the nearest depot for it), or left to haul whatever piles up.
int32_t World::mouths(PlayerId player) const {
    int32_t men = 0;
    for (const Unit& u : units_) {
        if (u.owner == player) men += unit_type(u.type).cost[static_cast<size_t>(Resource::Personnel)];
    }
    return men;
}

// Ration time: everyone's ration from the stock, or all of it and hunger.
void World::update_rations() {
    if (tick_ == 0 || tick_ % kRationInterval != 0) return;
    for (size_t p = 0; p < kMaxPlayers; ++p) {
        const int32_t need = mouths(static_cast<PlayerId>(p)) * kRationPerMan;
        int32_t& food = stock_[p][static_cast<size_t>(Resource::Food)];
        hungry_[p] = food < need;
        food = std::max(0, food - need);
    }
}

void World::apply_haul(const Command& cmd) {
    const Structure* depot = find_structure(cmd.target_unit);
    const std::optional<Resource> depot_takes =
        depot && depot->owner == cmd.player ? depot_cargo(role_of(*depot)) : std::nullopt;
    std::optional<Resource> cargo;
    if (cmd.cargo >= haul_code(Resource::Personnel) && cmd.cargo <= haul_code(Resource::Fuel)) {
        cargo = static_cast<Resource>(cmd.cargo - 1);
        if (!depot_for(*cargo)) cargo.reset();  // no depot ever takes it from trucks
    }
    for (EntityId id : cmd.units) {
        Unit* u = find_unit_mut(id);
        if (!u || u->owner != cmd.player || u->type != UnitTypeId::Truck) continue;
        if (depot_takes) {
            u->haul_cargo = *depot_takes;
            u->haul_depot = depot->id;
        } else if (cargo) {
            u->haul_cargo = *cargo;
            u->haul_depot = 0;
        } else if (cmd.cargo == kHaulAuto) {
            u->haul_cargo = Resource::Count;
            u->haul_depot = 0;
        }
        u->order = Order::Haul;
        u->order_path.reset();
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->engaged = 0;
        u->work = 0;
    }
}

const Structure* World::haul_destination(const Unit& truck, Resource cargo) const {
    if (const Structure* own = find_structure(truck.haul_depot);
        own && own->owner == truck.owner && own->built && depot_cargo(role_of(*own)) == cargo) {
        return own;
    }
    const std::optional<StructureType> type = depot_for(cargo);
    return type ? nearest_owned(truck.owner, *type, truck.pos) : nullptr;
}

// The supply run: load at the station the freight the truck is assigned
// (or, left to itself, whatever a depot of ours takes, most plentiful
// first), drive it to its depot, unload, repeat. With nothing to carry or
// nowhere to put it, the truck waits.
void World::update_hauling(Unit& u) {
    if (u.carrying == 0) {
        const Structure* station = station_of(u.owner);
        if (!station) return;
        std::optional<Resource> pick;
        for (Resource r : {Resource::Ammo, Resource::Fuel, Resource::Food}) {
            const auto i = static_cast<size_t>(r);
            if (u.haul_cargo != Resource::Count && r != u.haul_cargo) continue;
            if (station->cargo[i] <= 0 || !haul_destination(u, r)) continue;
            if (!pick || station->cargo[i] > station->cargo[static_cast<size_t>(*pick)]) pick = r;
        }
        if (!pick) return;
        if (distance_sq_to(*station, u.pos) > square_raw(kDoorReach)) {
            navigate(u, station->center, u.order_path, map_.clamp_tile(tile_of(station->center)), false);
            u.work = 0;
            return;
        }
        if (++u.work < kTruckLoadTicks) return;
        u.work = 0;
        Structure* s = find_structure_mut(station->id);
        const auto i = static_cast<size_t>(*pick);
        const int32_t load = std::min(kTruckCapacity, s->cargo[i]);
        s->cargo[i] -= load;
        u.carrying = load;
        u.carrying_type = *pick;
        return;
    }

    const Structure* depot = haul_destination(u, u.carrying_type);
    if (!depot) return;
    if (distance_sq_to(*depot, u.pos) > square_raw(kDoorReach)) {
        navigate(u, depot->center, u.chase_path, map_.clamp_tile(tile_of(depot->center)), false);
        u.work = 0;
        return;
    }
    // The driver unloads alone at a crawl; rear troops standing by make it quick.
    int32_t helpers = 0;
    const uint64_t reach_sq = square_raw(Fixed::from_int(2));
    for (const Unit& other : units_) {
        if (helpers >= kMaxUnloadHelpers) break;
        if (other.owner != u.owner || other.inside || !unit_type(other.type).worker) continue;
        if (distance_sq_to(*depot, other.pos) <= reach_sq) ++helpers;
    }
    u.work += static_cast<Tick>(kUnloadDriverWork + kUnloadHelperWork * helpers);
    Stock& stock = stock_[u.owner % kMaxPlayers];
    while (u.work >= static_cast<Tick>(kUnloadWorkPerUnit) && u.carrying > 0) {
        u.work -= kUnloadWorkPerUnit;
        --u.carrying;
        ++stock[static_cast<size_t>(u.carrying_type)];
    }
    if (u.carrying == 0) u.work = 0;
}

// A fuel depot going up: a fireball that hurts everything around, and a good
// part of the owner's fuel gone with it.
void World::burn_fuel_depot(const Structure& depot) {
    static constexpr WeaponDef kFireball{.name = "Fuel fire", .damage = 120, .damage_type = DamageType::Explosive,
                                         .range = Fixed{}, .reload = 0, .projectile_speed = Fixed{},
                                         .splash_radius = Fixed::from_int(2), .accuracy = 100, .miss_spread = Fixed{}};
    for (const Unit& u : units_) {
        if (u.inside) continue;
        if (distance_sq_to(depot, u.pos) <= square_raw(kFireball.splash_radius)) {
            hurt(u, kFireball, {depot.center, 0, true, true});
        }
    }
    for (const Structure& s : structures_) {
        if (s.id != depot.id && s.hp > 0) {
            uint64_t closest = UINT64_MAX;
            for (const TilePos& t : s.tiles) closest = std::min(closest, distance_sq_to_tile(t, depot.center));
            if (closest <= square_raw(kFireball.splash_radius)) hurt_structure(s, kFireball);
        }
    }
    if (depot.owner < kMaxPlayers) {
        int32_t& fuel = stock_[depot.owner][static_cast<size_t>(Resource::Fuel)];
        fuel -= fuel * kFuelDepotLossPercent / 100;
    }
}

// --- Lookups -----------------------------------------------------------------

const Structure* World::nearest_headquarters(PlayerId owner, FixedVec2 from) const {
    const Structure* best = nullptr;
    uint64_t best_sq = 0;
    for (const Structure& s : structures_) {
        if (s.type != StructureType::Headquarters || s.owner != owner) continue;
        const uint64_t d = (s.center - from).length_sq_raw();
        if (!best || d < best_sq) {
            best = &s;
            best_sq = d;
        }
    }
    return best;
}

void World::burst_into_flames(FixedVec2 at, PlayerId owner, const WeaponDef& fire) {
    recent_impacts_.push_back({tick_, at, UnitTypeId::FuelTanker, fire.splash_radius});
    for (const Unit& u : units_) {
        if (u.inside || u.airborne || u.hp <= 0) continue;
        if ((u.pos - at).length_sq_raw() <= square_raw(fire.splash_radius + unit_type(u.type).radius)) {
            hurt(u, fire, {at, 0, true, true});
        }
    }
    for (const Structure& s : structures_) {
        uint64_t closest = UINT64_MAX;
        for (const TilePos& t : s.tiles) closest = std::min(closest, distance_sq_to_tile(t, at));
        if (s.hp > 0 && closest <= square_raw(fire.splash_radius)) hurt_structure(s, fire);
    }
    (void)owner;
}

const Structure* World::station_of(PlayerId player) const {
    for (const Structure& s : structures_) {
        if (s.type == StructureType::Station && s.owner == player) return &s;
    }
    return nullptr;
}

const Structure* World::nearest_owned(PlayerId owner, StructureType type, FixedVec2 from) const {
    const Structure* best = nullptr;
    uint64_t best_sq = 0;
    for (const Structure& s : structures_) {
        if (role_of(s) != type || s.owner != owner || !s.built) continue;
        const uint64_t d = distance_sq_to(s, from);
        if (!best || d < best_sq) {
            best = &s;
            best_sq = d;
        }
    }
    return best;
}

const Structure* World::nearest_drop_off(PlayerId owner, FixedVec2 from) const {
    const Structure* best = nullptr;
    uint64_t best_sq = 0;
    for (const Structure& s : structures_) {
        if (s.owner != owner || !s.built) continue;
        if (s.type != StructureType::Headquarters && s.type != StructureType::Warehouse) continue;
        const uint64_t d = distance_sq_to(s, from);
        if (!best || d < best_sq) {
            best = &s;
            best_sq = d;
        }
    }
    return best;
}

std::optional<TilePos> World::nearest_resource(TilePos around, int32_t radius) const {
    std::optional<TilePos> best;
    int32_t best_d = 0;
    for (int32_t dy = -radius; dy <= radius; ++dy) {
        for (int32_t dx = -radius; dx <= radius; ++dx) {
            const TilePos t{around.x + dx, around.y + dy};
            if (!map_.contains(t) || !is_resource_terrain(map_.terrain(t)) || map_.resource(t) <= 0) continue;
            const int32_t d = dx * dx + dy * dy;
            if (!best || d < best_d) {
                best = t;
                best_d = d;
            }
        }
    }
    return best;
}

FixedVec2 World::door_of(const Structure& s, MoveClass cls) const {
    const std::optional<TilePos> door = nearest_passable(map_, tile_of(s.center), cls);
    return door ? tile_center(*door) : s.center;
}

}  // namespace engine
