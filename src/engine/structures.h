#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "engine/command.h"
#include "engine/fixed.h"
#include "engine/terrain.h"
#include "engine/unit_types.h"

namespace engine {

enum class StructureType : uint8_t {
    House,         // infantry can hold it; collapses with everyone inside
    Bridge,        // blown up, it's river again
    Headquarters,  // a player's base: takes in materials, hires rear troops, retrains them
    Count,
};

struct StructureDef {
    const char* name;
    int32_t max_hp;
    // Subtracted from incoming damage. Bullets never hurt structures.
    std::array<int32_t, kDamageTypeCount> armor;
    int32_t capacity;  // garrison size; 0 = can't be entered
};

const StructureDef& structure_type(StructureType type);

inline constexpr PlayerId kNoOwner = 255;

// A building or bridge made of map tiles (House / Bridge terrain). The tiles
// stay in the TileMap for movement and lines of fire; this is its state.
struct Structure {
    EntityId id = 0;
    StructureType type = StructureType::House;
    PlayerId owner = kNoOwner;  // whoever holds the garrison
    int32_t hp = 0;
    std::vector<TilePos> tiles;
    FixedVec2 center{};
    std::vector<EntityId> garrison;  // in the order they entered (in a HQ: men being retrained)

    // Units being trained, front first; already paid for.
    std::vector<UnitTypeId> queue;
    Tick progress = 0;  // ticks spent on queue.front()
};

// Squared distance from a point to the nearest tile of a structure (0 inside it).
inline uint64_t distance_sq_to(const Structure& s, FixedVec2 p) {
    uint64_t best = UINT64_MAX;
    for (const TilePos& t : s.tiles) best = std::min(best, distance_sq_to_tile(t, p));
    return best;
}

// What each building can train.
inline bool can_train(StructureType building, UnitTypeId unit) {
    return building == StructureType::Headquarters && unit == UnitTypeId::Worker;
}

}  // namespace engine
