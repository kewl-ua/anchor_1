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

// Personnel reinforcements (later they will arrive by train).
inline constexpr Tick kReinforcementInterval = 30 * kTicksPerSecond;
inline constexpr int32_t kReinforcementSize = 2;

}  // namespace engine
