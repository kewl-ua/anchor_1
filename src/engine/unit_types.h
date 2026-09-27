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
    // Artillery: lobbed high over everything at a point, bracketing it in
    // (see kRangingChance) instead of accuracy; nothing closer than min_range.
    bool indirect = false;
    Fixed min_range{};
};

// Skills, used with a command-grid button. Each unit type lists its own.
enum class AbilityId : uint8_t {
    AreaShot,    // tank: one HE-FRAG shell with a wide burst at a point
    SwitchAmmo,  // tank: HE <-> armor-piercing; the gun has to be reloaded
    MgSweep,     // IFV: a machine-gun burst along the front in a direction, not at a target
    LobGrenade,  // IFV: grenade launcher at an area, over cover
    DigTrench,     // rifleman: dig a trench along a line
    DigFoxhole,    // rifleman: dig a foxhole where he stands
    BuildParapet,  // rifleman: throw up a parapet facing a direction
    ThrowGrenade,  // assault trooper: a hand grenade, into a trench, a dugout, a house
    Refill,        // fuel tanker, ammo truck: load up at the fuel or ammunition depot
    Deploy,        // guns: set up to fire, or pack up to move
    Count,
};
inline constexpr size_t kAbilityCount = static_cast<size_t>(AbilityId::Count);
inline constexpr size_t kMaxAbilities = 4;

// How a skill is aimed.
enum class AbilityTarget : uint8_t {
    Instant,    // no aiming: the button does it
    Point,      // a point on the ground; the unit walks into range first
    Direction,  // a direction from the unit, given as a point
    Line,       // from one point to another (a trench)
};

struct AbilityDef {
    const char* name;   // full, for tooltips
    const char* label;  // short, for the button
    AbilityTarget target;
    Fixed range;    // Point: how close the unit must come; Direction: how far it reaches
    Tick cooldown;  // after use
    WeaponDef weapon{};  // what it fires, if anything
};

const AbilityDef& ability_def(AbilityId id);

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
    bool wheeled = false;  // a wheeled vehicle: no ditches, no trenches
    WeaponDef weapon;
    Stock cost{};  // Personnel, Food, Materials, Ammo, Fuel
    Tick train_time = 0;
    bool worker = false;  // rear troops: gather, build, unload
    // Fuel and ammunition carried (0: doesn't run out). Fuel is in tiles of driving.
    Fixed fuel_capacity{};
    int32_t rounds_capacity = 0;
    int32_t rounds_per_supply = 1;  // rounds per unit of ammunition from the stock
    // Service vehicles: what they bring to the others (Count: nothing), and how much fits.
    Resource supplies = Resource::Count;
    int32_t cargo_capacity = 0;
    // How close an enemy hiding in cover must be for this unit to make him out.
    Fixed detection = Fixed::from_int(2);
    bool stealthy = false;  // hard to spot in cover (scouts)
    // Reach of an observation post's sector (scouts); 0 = can't hold one.
    Fixed sector_range{};
    // A second kind of round, switched to with AbilityId::SwitchAmmo (damage 0 = none).
    WeaponDef alt_weapon{};
    std::array<AbilityId, kMaxAbilities> abilities{};
    uint8_t ability_count = 0;
    // Guns: set up before firing, packed up before moving, like a trebuchet.
    Tick deploy_time = 0;
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
    Assault,  // assault trooper: close combat, storming trenches and houses
    FuelTanker,  // refuels vehicles; burns when hit
    AmmoTruck,   // brings rounds to vehicles; its load goes off when hit
    Mortar,      // 82mm mortar crew: plunging fire into trenches
    Howitzer,    // 122mm towed howitzer: long range, needs spotters
    Count,
};
inline constexpr size_t kUnitTypeCount = static_cast<size_t>(UnitTypeId::Count);

const UnitTypeDef& unit_type(UnitTypeId id);

inline MoveClass move_class(const UnitTypeDef& def) {
    if (!def.vehicle) return MoveClass::Foot;
    return def.wheeled ? MoveClass::Wheeled : MoveClass::Vehicle;
}
// Trucks carry no weapon: they never pick fights.
inline bool is_armed(const UnitTypeDef& def) { return def.weapon.damage > 0; }

// Where a skill sits in the unit's list (its button), or -1 if it hasn't got it.
inline int ability_slot(const UnitTypeDef& def, AbilityId id) {
    for (uint8_t i = 0; i < def.ability_count; ++i) {
        if (def.abilities[i] == id) return i;
    }
    return -1;
}

// Damage multipliers for shooting down from / up at a higher tile (AoE II values).
inline constexpr int32_t kHighGroundPercent = 125;
inline constexpr int32_t kLowGroundPercent = 75;

}  // namespace engine
