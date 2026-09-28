#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "engine/command.h"
#include "engine/economy.h"
#include "engine/fixed.h"
#include "engine/terrain.h"
#include "engine/unit_types.h"

namespace engine {

// What a house looks like: the game treats them all as houses (a long one is
// spacious, so it can be turned into a depot).
enum class HouseLook : uint8_t { House, Cowshed, Coop, Factory };  // (a factory: a works' shop floor)

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
    SignalsBarracks,   // hires signallers, command vehicles, DF stations
    AirDefenseBarracks,  // hires MANPADS crews, Shilkas, air defence radars
    Airfield,          // a runway: builds and rearms attack aircraft
    Apartment,         // a five-storey apartment block: a big garrison high up
    CellTower,         // a cell tower: a spotter up it sees far; it relays radio for whoever holds it
    GasStation,        // fuel in its tanks for whoever holds it: at the pumps, or hauled off
    Elevator,          // a grain elevator: food for whoever holds it, and a view from the top
    Quarters,          // living barracks: bunks for the men, like AoE's houses
    Workshop,          // a motor pool: vehicles parked by it get repaired
    Hospital,          // a field hospital: the wounded are healed in its beds
    ObservationPost,   // a scout's, on one tile, watching `facing` (see PostKind)
    Count,
};

// What a scout's observation post is, by the ground he made it on: up a tree
// in a wood; a hide of branches in the crops, the reeds, the rubble; out in
// the open, an artificial stump he sits in (as the old field manuals have it).
enum class PostKind : uint8_t { Stump, Tree, Hide };
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

    // A combat position (a trench, a dugout, a gun pit...): ammunition that
    // can be stocked at it, in units of the stock.
    int32_t cache_capacity = 0;
};

const StructureDef& structure_type(StructureType type);

struct UpgradeDef {
    const char* name;
    const char* label;  // short, for the button
    const char* description;
    StructureType building;  // where it's researched
    Stock cost;
    Tick time;
    UpgradeId needs = UpgradeId::Count;  // researched first (Count: nothing)
};

const UpgradeDef& upgrade_def(UpgradeId id);

// What rear troops can build, in button order (1, 2, 3, ...).
inline constexpr StructureType kBuildable[] = {
    StructureType::InfantryBarracks,
    StructureType::ArmorBarracks,
    StructureType::ReconBarracks,
    StructureType::ArtilleryBarracks,
    StructureType::EngineerBarracks,
    StructureType::SignalsBarracks,
    StructureType::AirDefenseBarracks,
    StructureType::Airfield,
    StructureType::Warehouse,
    StructureType::AmmoDepot,
    StructureType::FuelDepot,
    StructureType::Quarters,
    StructureType::Workshop,
    StructureType::Hospital,
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

    // A building researching an upgrade (Count: nothing), and how far along.
    UpgradeId research = UpgradeId::Count;
    Tick research_progress = 0;

    // A village building turned into a depot (Count: it's still a house);
    // while `built` is false it's being turned.
    StructureType converted = StructureType::Count;

    // Ammunition stocked at a combat position, and whose it is: that
    // player's men at the position (or inside) draw on it.
    int32_t cache = 0;
    PlayerId cache_owner = kNoOwner;

    // How a house is drawn (it plays the same): a cottage, a cowshed, a coop.
    HouseLook look = HouseLook::House;
    // Bombs from the air that hit it (up to 3), and where along its long
    // side each fell (0 at its x0 or y0 end, 255 at the other): a block of
    // flats has the section there brought down (how it looks, nothing else).
    uint8_t bombed = 0;
    std::array<uint8_t, 3> bomb_at{};

    // A cell tower: whose direction finder's aerial is up it (kNoOwner: none).
    PlayerId antenna = kNoOwner;
    // An observation post: what it is.
    PostKind post = PostKind::Stump;

    // Where the units it hires go (AoE's rally point): rear troops to work
    // if it's on the wood or the stone, trucks to collect there.
    FixedVec2 rally{};
    bool rally_set = false;
};

// What a structure works as: a house turned into a depot is that depot.
inline StructureType role_of(const Structure& s) {
    return s.converted != StructureType::Count ? s.converted : s.type;
}

// Which depot takes in a resource that trucks bring (food, ammo, fuel).
inline std::optional<StructureType> depot_for(Resource r) {
    switch (r) {
        case Resource::Food: return StructureType::Warehouse;
        case Resource::Ammo: return StructureType::AmmoDepot;
        case Resource::Fuel: return StructureType::FuelDepot;
        default: return std::nullopt;
    }
}

// What a depot takes in from trucks, the other way round.
inline std::optional<Resource> depot_cargo(StructureType t) {
    switch (t) {
        case StructureType::Warehouse: return Resource::Food;
        case StructureType::AmmoDepot: return Resource::Ammo;
        case StructureType::FuelDepot: return Resource::Fuel;
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
    return t == StructureType::House || t == StructureType::Dugout || t == StructureType::Pillbox ||
           t == StructureType::Apartment || t == StructureType::CellTower || t == StructureType::GasStation ||
           t == StructureType::Elevator;
}
// Obstacles: in the way, but no cover.
inline bool is_obstacle(StructureType t) { return t == StructureType::Wire || t == StructureType::Hedgehogs; }

inline uint64_t distance_sq_to(const Structure& s, FixedVec2 p) {
    uint64_t best = UINT64_MAX;
    for (const TilePos& t : s.tiles) best = std::min(best, distance_sq_to_tile(t, p));
    return best;
}

// What each building trains for an axis, in button order.
std::span<const UnitTypeId> roster_of(StructureType building, Axis axis);

// What each building can train for a player (for either axis, without one).
inline bool can_train(StructureType building, UnitTypeId unit, Axis axis) {
    const std::span<const UnitTypeId> r = roster_of(building, axis);
    return std::find(r.begin(), r.end(), unit) != r.end();
}
inline bool can_train(StructureType building, UnitTypeId unit) {
    return can_train(building, unit, Axis::Democratic) || can_train(building, unit, Axis::Authoritarian);
}

}  // namespace engine
