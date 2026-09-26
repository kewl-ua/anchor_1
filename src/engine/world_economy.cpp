// The economy half of World: resources on the map, rear troops' work,
// headquarters production and personnel reinforcements.

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
        if (u && u->owner == cmd.player && unit_type(u->type).worker) group.push_back(u);
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
            map_.set_terrain(t.x, t.y, Terrain::Building);
            map_.set_resource(t, 0);
            structure_tiles_[static_cast<size_t>(t.y * map_.width() + t.x)] = s.id;
            s.tiles.push_back(t);
            sum += tile_center(t);
        }
    }
    if (s.tiles.empty()) return 0;
    const auto n = static_cast<int32_t>(s.tiles.size());
    s.center = {sum.x / n, sum.y / n};
    structures_.push_back(std::move(s));  // ids only grow: still sorted
    on_map_changed();
    return structures_.back().id;
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

void World::apply_train(const Command& cmd) {
    Structure* s = find_structure_mut(cmd.target_unit);
    if (!s || s->owner != cmd.player || cmd.unit_type >= kUnitTypeCount) return;
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
        const Structure* hq = nearest_headquarters(u.owner, u.pos);
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

// Every building works on the front of its queue; the unit walks out of the door.
void World::update_production() {
    for (Structure& s : structures_) {
        if (s.queue.empty()) continue;
        const UnitTypeId type = s.queue.front();
        if (++s.progress < unit_type(type).train_time) continue;
        s.progress = 0;
        s.queue.erase(s.queue.begin());
        spawn_unit(s.owner, type, door_of(s, move_class(unit_type(type))));
    }
}

// New men arrive on a schedule at every player that still has a headquarters.
void World::reinforce() {
    if (tick_ < next_reinforcement_) return;
    next_reinforcement_ += kReinforcementInterval;
    std::array<bool, kMaxPlayers> has_hq{};
    for (const Structure& s : structures_) {
        if (s.type == StructureType::Headquarters && s.owner < kMaxPlayers) has_hq[s.owner] = true;
    }
    for (size_t p = 0; p < kMaxPlayers; ++p) {
        if (has_hq[p]) stock_[p][static_cast<size_t>(Resource::Personnel)] += kReinforcementSize;
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
