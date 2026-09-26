#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/command.h"
#include "engine/economy.h"
#include "engine/fixed.h"
#include "engine/terrain.h"

namespace engine {

enum class DamageType : uint8_t {
    Bullet,     // rifles, machine guns, autocannons
    Explosive,  // HE shells, grenades, fragments
    AntiTank,   // RPGs, ATGMs, armor-piercing rounds
    Count,
};
inline constexpr size_t kDamageTypeCount = static_cast<size_t>(DamageType::Count);

struct WeaponDef {
    const char* name;
    int32_t damage;
    DamageType damage_type;
    Fixed range;  // tiles, from the shooter's edge to the target's edge
    Tick reload;  // ticks between shots
    // Tiles per tick; 0 means an instant hit (bullets). Anything slower flies
    // to where the target WAS when fired, so a moving target can dodge it.
    Fixed projectile_speed;
    // 0 hits one enemy. Anything bigger hurts everyone in the blast, own units too.
    Fixed splash_radius;
    uint8_t accuracy;  // percent; a miss lands up to miss_spread tiles off
    Fixed miss_spread;
};

struct UnitTypeDef {
    const char* name;
    const char* short_name;  // for compact UI
    int32_t max_hp;
    // Subtracted from incoming damage of each type; at least 1 always gets through.
    std::array<int32_t, kDamageTypeCount> armor;
    Fixed speed;   // tiles per tick
    Fixed radius;  // tiles
    Fixed sight;   // tiles; idle units engage enemies closer than this
    int32_t mass;  // heavier units push lighter ones aside
    bool vehicle;
    WeaponDef weapon;
    Stock cost{};  // Personnel, Food, Materials, Ammo, Fuel
    Tick train_time = 0;
    bool worker = false;  // rear troops: gather, build, unload
    // How close an enemy hiding in cover must be for this unit to make him out.
    Fixed detection = Fixed::from_int(2);
    bool stealthy = false;  // hard to spot in cover (scouts)
    // Reach of an observation post's sector (scouts); 0 = can't hold one.
    Fixed sector_range{};
};

enum class UnitTypeId : uint8_t {
    Rifleman,
    MachineGunner,
    Grenadier,  // RPG
    Tank,
    Ifv,
    Worker,  // rear trooper
    Truck,   // supply truck: station -> depots
    Scout,
    Count,
};
inline constexpr size_t kUnitTypeCount = static_cast<size_t>(UnitTypeId::Count);

const UnitTypeDef& unit_type(UnitTypeId id);

inline MoveClass move_class(const UnitTypeDef& def) { return def.vehicle ? MoveClass::Vehicle : MoveClass::Foot; }
// Trucks carry no weapon: they never pick fights.
inline bool is_armed(const UnitTypeDef& def) { return def.weapon.damage > 0; }

// Damage multipliers for shooting down from / up at a higher tile (AoE II values).
inline constexpr int32_t kHighGroundPercent = 125;
inline constexpr int32_t kLowGroundPercent = 75;

}  // namespace engine
