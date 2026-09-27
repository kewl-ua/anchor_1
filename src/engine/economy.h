#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/command.h"

namespace engine {

enum class Resource : uint8_t {
    Personnel,  // men: arrive on a schedule, so losses hurt
    Food,
    Materials,  // timber and stone, gathered by rear troops
    Ammo,
    Fuel,
    Count,
};
inline constexpr size_t kResourceCount = static_cast<size_t>(Resource::Count);

// An amount of every resource: a player's stockpile, or a price.
using Stock = std::array<int32_t, kResourceCount>;

inline constexpr size_t kMaxPlayers = 8;

inline const char* resource_name(Resource r) {
    static constexpr const char* kNames[] = {"Men", "Food", "Materials", "Ammo", "Fuel"};
    return kNames[static_cast<size_t>(r)];
}

inline bool can_afford(const Stock& have, const Stock& price) {
    for (size_t i = 0; i < kResourceCount; ++i) {
        if (have[i] < price[i]) return false;
    }
    return true;
}

inline void pay(Stock& have, const Stock& price) {
    for (size_t i = 0; i < kResourceCount; ++i) have[i] -= price[i];
}

// --- Rear troops' work ---
inline constexpr int32_t kCarryCapacity = 10;
inline constexpr Tick kChopTicks = kTicksPerSecond;           // one unit of timber per second
inline constexpr Tick kQuarryTicks = kTicksPerSecond * 3 / 2;  // stone is slower
inline constexpr int32_t kForestMaterials = 150;              // per tile; a cut-down forest is a field
inline constexpr int32_t kRockMaterials = 400;
// Retraining a rear trooper as a rifleman: same man, just a rifle and drill.
inline constexpr Tick kRetrainTicks = 10 * kTicksPerSecond;
inline constexpr size_t kMaxQueue = 5;

// --- Railway ---
// A train comes to every standing station on a schedule. The men walk off
// into the player's personnel pool; the freight waits at the station for
// trucks. (Rail upgrades will make trains come more often and carry more.)
inline constexpr Tick kTrainInterval = 60 * kTicksPerSecond;
inline constexpr Stock kTrainCargo = {4, 120, 0, 80, 60};  // Personnel, Food, Materials, Ammo, Fuel
// How long a train takes to roll in from the map edge, and stands at the
// station: only for drawing, the delivery happens on arrival.
inline constexpr Tick kTrainApproachTicks = 8 * kTicksPerSecond;
inline constexpr Tick kTrainStayTicks = 5 * kTicksPerSecond;

// --- Trucks ---
// What a Haul command tells trucks to carry: nothing new (each keeps its
// assignment), whatever piles up at the station, or one kind of freight.
inline constexpr uint8_t kHaulKeep = 0;
inline constexpr uint8_t kHaulAuto = 0xFF;
inline constexpr uint8_t haul_code(Resource r) { return static_cast<uint8_t>(static_cast<uint8_t>(r) + 1); }
inline constexpr int32_t kTruckCapacity = 40;
inline constexpr Tick kTruckLoadTicks = 2 * kTicksPerSecond;
// Unloading: the driver alone manages one unit a second; every rear trooper
// standing by the depot adds two more (up to four helpers).
inline constexpr int32_t kUnloadWorkPerUnit = kTicksPerSecond;
inline constexpr int32_t kUnloadDriverWork = 1;
inline constexpr int32_t kUnloadHelperWork = 2;
inline constexpr int32_t kMaxUnloadHelpers = 4;

// A fuel depot going up burns this share of the owner's fuel.
inline constexpr int32_t kFuelDepotLossPercent = 30;

}  // namespace engine
