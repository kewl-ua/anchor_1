#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "engine/command.h"
#include "engine/economy.h"
#include "engine/fixed.h"
#include "engine/terrain.h"
#include "engine/unit_types.h"

namespace engine {

enum class StructureType : uint8_t {
    House,             // infantry can hold it; collapses with everyone inside
    Bridge,            // blown up, it's river again
    Headquarters,      // a player's base: takes in materials, hires rear troops, retrains them
    InfantryBarracks,  // riflemen, machine gunners, grenadiers (assault troops later)
    ArmorBarracks,     // armor crews: tanks and IFVs
    Warehouse,         // materials from rear troops, food from trucks
    Station,           // the railhead: trains unload here, trucks load
    AmmoDepot,         // trucks unload ammunition here
    FuelDepot,         // trucks unload fuel here; flimsy, and it burns
    ReconBarracks,     // hires scouts
    Trench,            // field works, one per tile, dug by riflemen
    Foxhole,
    Parapet,           // on open ground; on a trench or foxhole it is part of that
    Dugout,            // a foxhole upgraded into a shelter
    ArtilleryBarracks, // hires mortars, grenade launchers, howitzers, rocket launchers
    GunPit,            // a gun's dug-in position
    EngineerBarracks,  // hires sappers
    Wire,              // obstacles, one per tile, put up by sappers
    Hedgehogs,
    Pillbox,           // a firing point with a slit facing `facing`
    Count,
};
inline constexpr size_t kStructureTypeCount = static_cast<size_t>(StructureType::Count);
inline constexpr size_t kMaxRoster = 5;  // the grid's top row

struct StructureDef {
    const char* name;
    int32_t max_hp;
    // Subtracted from incoming damage. Bullets never hurt structures.
    std::array<int32_t, kDamageTypeCount> armor;
    int32_t capacity = 0;  // garrison size; 0 = can't be entered

    // Buildings rear troops can put up.
    bool buildable = false;
    int32_t width = 0;  // footprint in tiles
    int32_t height = 0;
    Stock cost{};
    Tick build_time = 0;  // for one rear trooper; more of them build faster

    // Who can be hired here, in button order (Q, W, E, ...).
    std::array<UnitTypeId, kMaxRoster> roster{};
    uint8_t roster_size = 0;
};

const StructureDef& structure_type(StructureType type);

// What rear troops can build, in button order (1, 2, 3, ...).
inline constexpr StructureType kBuildable[] = {
    StructureType::InfantryBarracks,
    StructureType::ArmorBarracks,
    StructureType::ReconBarracks,
    StructureType::ArtilleryBarracks,
    StructureType::EngineerBarracks,
    StructureType::Warehouse,
    StructureType::AmmoDepot,
    StructureType::FuelDepot,
};

inline constexpr PlayerId kNoOwner = 255;

// A building or bridge made of map tiles (House / Bridge / Building terrain).
// The tiles stay in the TileMap for movement and lines of fire; this is its state.
struct Structure {
    EntityId id = 0;
    StructureType type = StructureType::House;
    PlayerId owner = kNoOwner;  // whoever holds the garrison
    int32_t hp = 0;
    std::vector<TilePos> tiles;
    FixedVec2 center{};
    std::vector<EntityId> garrison;  // in the order they entered (in a HQ: men being retrained)

    // Under construction until build_progress reaches the type's build_time.
    bool built = true;
    Tick build_progress = 0;

    // Units being trained, front first; already paid for.
    std::vector<UnitTypeId> queue;
    Tick progress = 0;  // ticks spent on queue.front()

    // A station: supplies the trains brought, waiting for trucks, and when
    // the next train comes.
    Stock cargo{};
    Tick next_train = 0;

    // Field works: a parapet on the side facing `facing` (for a Parapet,
    // it is the whole thing), and a foxhole being turned into a dugout.
    bool parapet = false;
    FixedVec2 facing{};
    bool upgrading = false;
    Tick upgrade_work = 0;
};

// Which depot takes in a resource that trucks bring (food, ammo, fuel).
inline std::optional<StructureType> depot_for(Resource r) {
    switch (r) {
        case Resource::Food: return StructureType::Warehouse;
        case Resource::Ammo: return StructureType::AmmoDepot;
        case Resource::Fuel: return StructureType::FuelDepot;
        default: return std::nullopt;
    }
}

// Squared distance from a point to the nearest tile of a structure (0 inside it).
// Field works: passable ground that gives cover.
inline bool is_fieldwork(StructureType t) {
    return t == StructureType::Trench || t == StructureType::Foxhole || t == StructureType::Parapet ||
           t == StructureType::GunPit;
}
// Infantry can go inside: a house, a dugout, a pillbox. Whoever is inside holds it.
inline bool is_shelter(StructureType t) {
    return t == StructureType::House || t == StructureType::Dugout || t == StructureType::Pillbox;
}
// Obstacles: in the way, but no cover.
inline bool is_obstacle(StructureType t) { return t == StructureType::Wire || t == StructureType::Hedgehogs; }

inline uint64_t distance_sq_to(const Structure& s, FixedVec2 p) {
    uint64_t best = UINT64_MAX;
    for (const TilePos& t : s.tiles) best = std::min(best, distance_sq_to_tile(t, p));
    return best;
}

// What each building can train.
inline bool can_train(StructureType building, UnitTypeId unit) {
    const StructureDef& def = structure_type(building);
    return std::find(def.roster.begin(), def.roster.begin() + def.roster_size, unit) !=
           def.roster.begin() + def.roster_size;
}

}  // namespace engine
