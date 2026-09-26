#pragma once

#include "engine/fixed.h"

// Heights above the ground, in elevation levels, shared by lines of fire and
// lines of sight: whatever hides a man from view also stops a bullet flying
// that low, and from high enough ground both pass over.
namespace engine {

inline constexpr Fixed kInfantryTop = Fixed::from_ratio(1, 2);
inline constexpr Fixed kInfantryMuzzle = Fixed::from_ratio(2, 5);
inline constexpr Fixed kInfantryCenter = Fixed::from_ratio(3, 10);
inline constexpr Fixed kVehicleTop = Fixed::from_ratio(4, 5);
inline constexpr Fixed kVehicleMuzzle = Fixed::from_ratio(7, 10);
inline constexpr Fixed kVehicleCenter = Fixed::from_ratio(1, 2);
inline constexpr Fixed kGroundAim = Fixed::from_ratio(1, 10);  // shooting at a spot on the ground

inline constexpr Fixed kTreeHeight = Fixed::from_ratio(3, 2);
inline constexpr Fixed kHouseHeight = Fixed::from_ratio(6, 5);
inline constexpr Fixed kRockHeight = Fixed::from_ratio(4, 5);
inline constexpr Fixed kBuildingHeight = Fixed::from_ratio(3, 2);
// Garrisoned infantry looks and fires from the windows, a floor up.
inline constexpr Fixed kWindowHeight = Fixed::from_int(1);

}  // namespace engine
