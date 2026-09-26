#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "engine/fixed.h"
#include "engine/terrain.h"

namespace engine {

// Travel cost from every tile to one goal tile for one movement class
// (Dijkstra outwards from the goal). All units heading to the same tile share
// one field and simply walk "downhill", so a whole army costs one search.
//
// Costs are integers and ties are broken by tile index, so every peer builds
// exactly the same field.
class FlowField {
public:
    static constexpr uint32_t kUnreachable = UINT32_MAX;

    FlowField(const TileMap& map, TilePos goal, MoveClass cls);

    TilePos requested_goal() const { return requested_; }
    // Where the field actually leads: the requested tile, or the nearest tile
    // this class can stand on if the requested one is impassable.
    TilePos goal() const { return goal_; }
    MoveClass move_class() const { return cls_; }
    // True once some tile became impassable since this field was built.
    bool stale(const TileMap& map) const { return map.blocking_revision() != revision_; }

    uint32_t cost(TilePos t) const;
    bool reachable(TilePos t) const { return cost(t) != kUnreachable; }
    // The neighbour to step to from `t`; nullopt at the goal or when unreachable.
    std::optional<TilePos> next(TilePos t) const;

private:
    size_t index(TilePos t) const { return static_cast<size_t>(t.y * width_ + t.x); }

    const TileMap* map_;
    int32_t width_;
    int32_t height_;
    TilePos requested_;
    TilePos goal_;
    MoveClass cls_;
    uint32_t revision_;
    std::vector<uint32_t> cost_;
};

// The tile closest to `t` that `cls` can stand on, searched outwards ring by ring.
std::optional<TilePos> nearest_passable(const TileMap& map, TilePos t, MoveClass cls, int32_t max_radius = 24);

// True if a unit of `cls` can walk the straight segment from `a` to `b`.
bool straight_walkable(const TileMap& map, FixedVec2 a, FixedVec2 b, MoveClass cls);

}  // namespace engine
