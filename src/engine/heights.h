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
// Men riding on top of armor, sitting on the hull: above it, in the open.
inline constexpr Fixed kRiderCenter = Fixed::from_int(1);
inline constexpr Fixed kRiderTop = Fixed::from_ratio(6, 5);

inline constexpr Fixed kTreeHeight = Fixed::from_ratio(3, 2);
inline constexpr Fixed kHouseHeight = Fixed::from_ratio(6, 5);
inline constexpr Fixed kRockHeight = Fixed::from_ratio(4, 5);
inline constexpr Fixed kBuildingHeight = Fixed::from_ratio(3, 2);
// Garrisoned infantry looks and fires from the windows, a floor up.
inline constexpr Fixed kWindowHeight = Fixed::from_int(1);
// A five-storey block: hides whatever is behind it, and its garrison looks
// and fires from the upper floors. A spotter up a cell tower sees from higher.
inline constexpr Fixed kApartmentHeight = Fixed::from_int(3);
inline constexpr Fixed kApartmentWindow = Fixed::from_ratio(5, 2);
inline constexpr Fixed kTowerEye = Fixed::from_ratio(7, 2);
// A grain elevator's silos stand higher still; its garrison looks from the top.
inline constexpr Fixed kElevatorHeight = Fixed::from_int(4);
inline constexpr Fixed kElevatorWindow = Fixed::from_ratio(7, 2);

}  // namespace engine
