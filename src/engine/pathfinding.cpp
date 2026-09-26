#include "engine/pathfinding.h"

#include <functional>
#include <queue>
#include <utility>

namespace engine {

namespace {

constexpr uint32_t kStraightCost = 10;
constexpr uint32_t kDiagonalCost = 14;

// Neighbours in a fixed order: the order breaks ties, so it must never change.
struct Step {
    int32_t dx;
    int32_t dy;
    uint32_t cost;
};
constexpr Step kSteps[] = {
    {1, 0, kStraightCost}, {-1, 0, kStraightCost}, {0, 1, kStraightCost}, {0, -1, kStraightCost},
    {1, 1, kDiagonalCost}, {1, -1, kDiagonalCost}, {-1, 1, kDiagonalCost}, {-1, -1, kDiagonalCost},
};

// Moving from `from` by `s` is allowed if the target is passable and, for
// diagonals, both side tiles are too (no squeezing between two trees).
bool can_step(const TileMap& map, TilePos from, const Step& s, MoveClass cls) {
    if (!map.passable({from.x + s.dx, from.y + s.dy}, cls)) return false;
    if (s.dx != 0 && s.dy != 0) {
        return map.passable({from.x + s.dx, from.y}, cls) && map.passable({from.x, from.y + s.dy}, cls);
    }
    return true;
}

}  // namespace

FlowField::FlowField(const TileMap& map, TilePos goal, MoveClass cls)
    : map_(&map),
      width_(map.width()),
      height_(map.height()),
      requested_(goal),
      goal_(goal),
      cls_(cls),
      revision_(map.blocking_revision()),
      cost_(static_cast<size_t>(map.width() * map.height()), kUnreachable) {
    const std::optional<TilePos> start = nearest_passable(map, map.clamp_tile(goal), cls);
    if (!start) return;  // nowhere to go: every tile stays unreachable
    goal_ = *start;

    // Dijkstra from the goal. The step is symmetric, so cost(t) is the price
    // of walking from t to the goal. Entering a slow tile costs more.
    using Entry = std::pair<uint32_t, uint32_t>;  // (cost, tile index): a strict total order
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    cost_[index(goal_)] = 0;
    open.push({0, static_cast<uint32_t>(index(goal_))});
    while (!open.empty()) {
        const auto [cost, idx] = open.top();
        open.pop();
        if (cost != cost_[idx]) continue;  // stale entry
        const TilePos t{static_cast<int32_t>(idx) % width_, static_cast<int32_t>(idx) / width_};
        for (const Step& s : kSteps) {
            if (!can_step(map, t, s, cls)) continue;
            const TilePos n{t.x + s.dx, t.y + s.dy};
            // Walking n -> t: the price is set by the slower of the two tiles.
            const auto pct = static_cast<uint32_t>(std::min(map.speed_percent(n, cls), map.speed_percent(t, cls)));
            const uint32_t next_cost = cost + s.cost * 100 / pct;
            if (next_cost < cost_[index(n)]) {
                cost_[index(n)] = next_cost;
                open.push({next_cost, static_cast<uint32_t>(index(n))});
            }
        }
    }
}

uint32_t FlowField::cost(TilePos t) const {
    if (t.x < 0 || t.y < 0 || t.x >= width_ || t.y >= height_) return kUnreachable;
    return cost_[index(t)];
}

std::optional<TilePos> FlowField::next(TilePos t) const {
    const uint32_t here = cost(t);
    if (here == kUnreachable || here == 0) return std::nullopt;
    std::optional<TilePos> best;
    uint32_t best_cost = here;
    for (const Step& s : kSteps) {
        if (!can_step(*map_, t, s, cls_)) continue;
        const TilePos n{t.x + s.dx, t.y + s.dy};
        if (cost(n) < best_cost) {
            best = n;
            best_cost = cost(n);
        }
    }
    return best;
}

std::optional<TilePos> nearest_passable(const TileMap& map, TilePos t, MoveClass cls, int32_t max_radius) {
    if (map.passable(t, cls)) return t;
    for (int32_t r = 1; r <= max_radius; ++r) {
        // Best tile on this ring; the fixed scan order breaks distance ties.
        std::optional<TilePos> best;
        int32_t best_d = 0;
        for (int32_t dy = -r; dy <= r; ++dy) {
            for (int32_t dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                const TilePos c{t.x + dx, t.y + dy};
                const int32_t d = dx * dx + dy * dy;
                if (map.passable(c, cls) && (!best || d < best_d)) {
                    best = c;
                    best_d = d;
                }
            }
        }
        if (best) return best;
    }
    return std::nullopt;
}

bool straight_walkable(const TileMap& map, FixedVec2 a, FixedVec2 b, MoveClass cls) {
    // Sample every quarter tile along the segment.
    const Fixed length = (b - a).length();
    const int32_t samples = std::max(1, (length * 4).to_int() + 1);
    for (int32_t i = 1; i <= samples; ++i) {
        const FixedVec2 p = a + (b - a) * Fixed::from_ratio(i, samples);
        if (!map.passable(tile_of(p), cls)) return false;
    }
    return true;
}

}  // namespace engine
