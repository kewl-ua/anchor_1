// Trenches put together, as walls and towers in AoE II: a trench tile fitted
// out with a firing cell, a machine-gun nest, an anti-tank post, a mortar's
// position, a breastwork, a dugout; the trench manned, each man to a place
// along it; into the dugouts under fire and back to the places after; along
// the trench from one tile of it to another, under cover.
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <vector>

#include "engine/world.h"

namespace engine {

namespace {

const UnitTypeDef& def_of(const Unit& u) { return unit_type(u.type); }
MoveClass class_of(const Unit& u) { return move_class(def_of(u)); }

// The player's own foot soldiers of those the command names, in id order
// (commands come over the network: never trust ids or owners).
std::vector<Unit*> men_of(const Command& cmd, const std::function<Unit*(EntityId)>& find) {
    std::vector<Unit*> men;
    for (EntityId id : cmd.units) {
        Unit* u = find(id);
        if (u && u->owner == cmd.player && !def_of(*u).vehicle && !def_of(*u).aircraft) men.push_back(u);
    }
    std::sort(men.begin(), men.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
    men.erase(std::unique(men.begin(), men.end()), men.end());
    return men;
}

}  // namespace

bool World::trench_like(TilePos t) const {
    if (!map_.contains(t)) return false;
    const Terrain terrain = map_.terrain(t);
    return terrain == Terrain::Trench || terrain == Terrain::Foxhole || terrain == Terrain::GunPit || terrain == Terrain::Dugout;
}

bool World::can_fit(PlayerId player, TilePos t, TrenchFit fit) const {
    if (!map_.contains(t) || fit == TrenchFit::None || fit >= TrenchFit::Count) return false;
    const Structure* s = structure_at(t);
    if (!s || s->owner != player || s->upgrading || !s->built) return false;
    switch (fit) {
        case TrenchFit::Parapet: return (s->type == StructureType::Trench || s->type == StructureType::Foxhole) && !s->parapet;
        case TrenchFit::Dugout: return s->type == StructureType::Trench || s->type == StructureType::Foxhole;
        default: return s->type == StructureType::Trench && s->fit == TrenchFit::None;
    }
}

// Fit out a trench tile: paid up front; the men go there and dig it, and
// stay at it when it's done (the man it's for in his place).
void World::apply_fortify(const Command& cmd) {
    const TilePos tile = map_.clamp_tile(tile_of(cmd.target));
    if (cmd.structure_type >= kTrenchFitCount) return;
    const auto fit = static_cast<TrenchFit>(cmd.structure_type);
    if (!can_fit(cmd.player, tile, fit)) return;
    // (Nobody sent: the men standing in it dig, as a foxhole is made a dugout.)
    std::vector<Unit*> men = men_of(cmd, [this](EntityId id) { return find_unit_mut(id); });
    Stock& stock = stock_[cmd.player % kMaxPlayers];
    if (!can_afford(stock, fit_def(fit).cost)) return;
    pay(stock, fit_def(fit).cost);
    Structure* s = find_structure_mut(structure_id_at(tile));
    s->upgrading = true;
    s->fitting = fit;
    s->upgrade_work = 0;
    FixedVec2 facing = cmd.target_end - tile_center(tile);
    if (facing.x.raw == 0 && facing.y.raw == 0) facing = s->facing;
    s->fit_facing = facing;
    for (Unit* u : men) {
        leave_structure(*u);
        u->order = Order::Fortify;
        u->order_target = s->id;
        u->order_goal = tile;
        u->order_point = tile_center(tile);
        u->order_path = field_to(tile, class_of(*u));
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->engaged = 0;
        route_by_trench(*u, tile);
    }
}

void World::update_fortify(Unit& u) {
    Structure* s = find_structure_mut(u.order_target);
    if (!s || !s->upgrading) {  // done (or gone): at it, facing its front
        u.order = Order::Idle;
        u.order_path.reset();
        face_front(u);
        return;
    }
    const FixedVec2 spot = tile_center(s->tiles.front());
    if ((spot - u.pos).length_sq_raw() <= square_raw(kDigReach)) {  // digging (see update_upgrades)
        if (s->fit_facing.x.raw != 0 || s->fit_facing.y.raw != 0) u.facing = s->fit_facing;
        return;
    }
    if (navigate(u, spot, u.order_path, u.order_goal, false) == Step::Blocked) {
        u.order = Order::Idle;
        u.order_path.reset();
    }
}

void World::finish_fitting(Structure& s, const std::vector<Unit*>& diggers) {
    const TrenchFit fit = s.fitting;
    const TilePos tile = s.tiles.front();
    s.upgrading = false;
    s.fitting = TrenchFit::None;
    s.upgrade_work = 0;
    if (fit == TrenchFit::Dugout) {
        // Roofed over, and the men who dug it are inside.
        s.type = StructureType::Dugout;
        s.hp = structure_type(StructureType::Dugout).max_hp;
        s.parapet = false;
        s.fit = TrenchFit::None;
        s.owner = kNoOwner;
        map_.set_terrain(tile.x, tile.y, Terrain::Dugout);
        on_map_changed();
        for (Unit* u : diggers) {
            u->order = Order::Idle;
            enter(*u, s);
        }
        return;
    }
    if (s.fit_facing.x.raw != 0 || s.fit_facing.y.raw != 0) s.facing = s.fit_facing;
    if (fit == TrenchFit::Parapet) {
        s.parapet = true;
    } else {
        s.fit = fit;
    }
    for (Unit* u : diggers) {
        if (u->order == Order::Fortify) u->order = Order::Idle;
        u->facing = s.facing;
        u->post = tile;
    }
}

namespace {

// Who goes to which fitting when a trench is manned.
TrenchFit wants(UnitTypeId type) {
    switch (type) {
        case UnitTypeId::MachineGunner: return TrenchFit::MgNest;
        case UnitTypeId::Grenadier: return TrenchFit::AtPost;
        case UnitTypeId::Mortar:
        case UnitTypeId::Ags: return TrenchFit::MortarPost;
        default: return TrenchFit::Cell;
    }
}
int rank(UnitTypeId type) { return wants(type) == TrenchFit::Cell ? 1 : 0; }  // the specialists choose first

constexpr int32_t kSteps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

}  // namespace

// Where on a trench tile a man takes his place: the first at the position
// built into it (a step forward; a mortar's pit a step back), anyone else in
// the trench itself.
void World::man_place(Unit& u, TilePos t, bool first) {
    FixedVec2 spot = tile_center(t);
    const Structure* s = structure_at(t);
    if (first && s && s->type == StructureType::Trench && s->fit != TrenchFit::None) {
        const Fixed l = s->facing.length();
        const Fixed step = s->fit == TrenchFit::MortarPost ? -Fixed::from_ratio(1, 5) : Fixed::from_ratio(3, 20);
        if (l.raw > 0) spot += s->facing * (step / l);
    }
    leave_structure(u);
    u.order = Order::Move;
    u.order_point = spot;
    u.order_goal = t;
    u.order_path = field_to(t, class_of(u));
    u.chase_path.reset();
    u.speed_cap = Fixed{};
    u.order_target = 0;
    u.engaged = 0;
    u.post = t;
    route_by_trench(u, t);
}

// Man the trench: each to a place along it, the nearest to the tile clicked
// first: machine gunners to the nests, grenadiers to the anti-tank posts,
// mortar and AGS crews to the mortars' positions, the rest to the firing
// cells, then anywhere in the trench, one man a place, then two.
void World::apply_man_works(const Command& cmd) {
    const TilePos start = map_.clamp_tile(tile_of(cmd.target));
    if (!trench_like(start) || map_.terrain(start) == Terrain::Dugout) return;
    std::vector<Unit*> men = men_of(cmd, [this](EntityId id) { return find_unit_mut(id); });
    if (men.empty()) return;
    // Its tiles, along it from the one clicked.
    std::vector<TilePos> tiles{start};
    for (size_t head = 0; head < tiles.size() && static_cast<int32_t>(tiles.size()) < kManningReach; ++head) {
        for (const auto& d : kSteps) {
            const TilePos n{tiles[head].x + d[0], tiles[head].y + d[1]};
            if (!trench_like(n) || map_.terrain(n) == Terrain::Dugout) continue;
            if (std::find(tiles.begin(), tiles.end(), n) != tiles.end()) continue;
            tiles.push_back(n);
            if (static_cast<int32_t>(tiles.size()) >= kManningReach) break;
        }
    }
    auto index_of = [&](TilePos t) -> int {
        const auto it = std::find(tiles.begin(), tiles.end(), t);
        return it == tiles.end() ? -1 : static_cast<int>(it - tiles.begin());
    };
    // Who's in it already: at his place in it, on his way there, in a dugout
    // with his place in it; or just standing in it.
    std::vector<int32_t> taken(tiles.size(), 0);
    for (const Unit& u : units_) {
        if (u.owner != cmd.player || def_of(u).vehicle || u.hp <= 0) continue;
        if (std::find(cmd.units.begin(), cmd.units.end(), u.id) != cmd.units.end()) continue;
        const TilePos here = tile_of(u.pos);
        const bool placed = u.post.x >= 0 && (u.inside || here == u.post || (u.order == Order::Move && u.order_goal == u.post));
        const int i = placed ? index_of(u.post) : u.inside ? -1 : index_of(here);
        if (i >= 0) ++taken[static_cast<size_t>(i)];
    }
    auto fit_at = [&](size_t i) {
        const Structure* s = structure_at(tiles[i]);
        return s && s->type == StructureType::Trench ? s->fit : TrenchFit::None;
    };
    std::stable_sort(men.begin(), men.end(), [](const Unit* a, const Unit* b) {
        return rank(a->type) != rank(b->type) ? rank(a->type) < rank(b->type) : a->id < b->id;
    });
    for (Unit* u : men) {
        const TrenchFit want = wants(u->type);
        int best = -1;
        for (int pass = 0; pass < 4 && best < 0; ++pass) {
            for (size_t i = 0; i < tiles.size() && best < 0; ++i) {
                const TrenchFit f = fit_at(i);
                const int32_t n = taken[i];
                const bool ok = pass == 0   ? f == want && n == 0
                                : pass == 1 ? n == 0 && (f == TrenchFit::None || f == TrenchFit::Cell)  // (not a nest or a post meant for another)
                                : pass == 2 ? n == 0
                                            : n < 2;
                if (ok) best = static_cast<int>(i);
            }
        }
        if (best < 0) continue;
        const bool first = taken[static_cast<size_t>(best)]++ == 0;
        man_place(*u, tiles[static_cast<size_t>(best)], first);
    }
}

// Take cover, as AoE's town bell: each into the nearest dugout along his
// trench with room in it, remembering his place; out of the trenches, the
// nearest dugout near him.
void World::apply_take_cover(const Command& cmd) {
    std::vector<std::pair<EntityId, int32_t>> going;  // dugouts, and how many are on their way in
    auto room = [&](const Structure& s) {
        int32_t n = static_cast<int32_t>(s.garrison.size());
        for (const auto& [id, k] : going) {
            if (id == s.id) n += k;
        }
        return structure_type(s.type).capacity - n;
    };
    auto usable = [&](const Structure* s) {
        return s && s->type == StructureType::Dugout && (s->owner == kNoOwner || s->owner == cmd.player) && room(*s) > 0;
    };
    for (Unit* u : men_of(cmd, [this](EntityId id) { return find_unit_mut(id); })) {
        if (u->inside || u->riding) continue;
        const TilePos here = map_.clamp_tile(tile_of(u->pos));
        const Structure* best = nullptr;
        if (trench_like(here)) {  // along the trench
            std::vector<TilePos> seen{here};
            for (size_t head = 0; head < seen.size() && !best && static_cast<int32_t>(seen.size()) < kTrenchRouteNodes; ++head) {
                for (const auto& d : kSteps) {
                    const TilePos n{seen[head].x + d[0], seen[head].y + d[1]};
                    if (!trench_like(n) || std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
                    if (map_.terrain(n) == Terrain::Dugout) {
                        if (usable(structure_at(n))) {
                            best = structure_at(n);
                            break;
                        }
                        continue;  // (full, or the enemy's: no way through it)
                    }
                    seen.push_back(n);
                }
            }
        }
        if (!best) {  // the nearest one about
            uint64_t best_sq = square_raw(kCoverReach);
            for (const Structure& s : structures_) {
                if (!usable(&s)) continue;
                const uint64_t d = distance_sq_to(s, u->pos);
                if (d <= best_sq) {
                    best = &s;
                    best_sq = d;
                }
            }
        }
        if (!best) continue;
        bool counted = false;
        for (auto& [id, k] : going) {
            if (id == best->id) {
                ++k;
                counted = true;
            }
        }
        if (!counted) going.push_back({best->id, 1});
        if (trench_like(here) && map_.terrain(here) != Terrain::Dugout) u->post = here;
        const TilePos goal = map_.clamp_tile(tile_of(best->center));
        leave_structure(*u);
        u->order = Order::Garrison;
        u->order_target = best->id;
        u->order_goal = goal;
        u->order_path = field_to(goal, MoveClass::Foot);
        u->chase_path.reset();
        u->speed_cap = Fixed{};
        u->engaged = 0;
        route_by_trench(*u, goal);
    }
}

// The way along a trench from one tile of it to another (a dugout's: up to
// its door), the tiles after `from` in turn; empty if they aren't joined.
std::vector<TilePos> World::trench_route(TilePos from, TilePos to) const {
    const bool shelter = map_.contains(to) && map_.terrain(to) == Terrain::Dugout;
    auto key = [this](TilePos t) { return t.y * map_.width() + t.x; };
    std::map<int32_t, int32_t> parent;  // tile -> the tile it was reached from
    std::vector<TilePos> open{from};
    parent[key(from)] = -1;
    bool found = false;
    for (size_t head = 0; head < open.size() && !found && static_cast<int32_t>(open.size()) < kTrenchRouteNodes; ++head) {
        for (const auto& d : kSteps) {
            const TilePos n{open[head].x + d[0], open[head].y + d[1]};
            if (!trench_like(n) || parent.count(key(n))) continue;
            if (map_.terrain(n) == Terrain::Dugout && n != to) continue;  // (no way through another dugout)
            parent[key(n)] = key(open[head]);
            if (n == to) {
                found = true;
                break;
            }
            open.push_back(n);
        }
    }
    if (!found) return {};
    std::vector<TilePos> route;
    for (int32_t k = key(to); k != key(from); k = parent[k]) route.push_back({k % map_.width(), k / map_.width()});
    std::reverse(route.begin(), route.end());
    if (shelter && !route.empty()) route.pop_back();  // up to its door: he goes in from there
    return route;
}

void World::route_by_trench(Unit& u, TilePos goal) const {
    u.via.clear();
    const TilePos here = map_.clamp_tile(tile_of(u.pos));
    if (def_of(u).vehicle || here == goal || !trench_like(here) || !trench_like(goal)) return;
    std::vector<TilePos> route = trench_route(here, goal);
    const int32_t across = std::abs(goal.x - here.x) + std::abs(goal.y - here.y);
    if (route.empty() || static_cast<int32_t>(route.size()) > across * kTrenchDetour + 4) return;  // (a long way round: across the open)
    u.via = std::move(route);
    u.via_goal = goal;
}

void World::face_front(Unit& u) const {
    if (def_of(u).vehicle) return;
    const Structure* s = structure_at(map_.clamp_tile(tile_of(u.pos)));
    if (!s || (s->type != StructureType::Trench && s->type != StructureType::Foxhole)) return;
    if (!s->parapet && s->fit == TrenchFit::None) return;
    if (s->facing.x.raw != 0 || s->facing.y.raw != 0) u.facing = s->facing;
}

}  // namespace engine
