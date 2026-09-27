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
    // Lobbed at what the unit sees (a grenade launcher): over cover, into trenches.
    bool lobbed = false;
    // Air defence: can fire at aircraft in the air; `air_only` never at anything else.
    bool anti_air = false;
    bool air_only = false;
};

// Upgrades researched in buildings, as in AoE II's blacksmith and university.
enum class UpgradeId : uint8_t {
    SmokeGrenades,     // ammunition depot: tanks can lay a smoke screen
    SabotRounds,       // ammunition depot: tank AP rounds hit harder
    ClusterRockets,    // ammunition depot: MLRS rockets burst wider
    TrainSchedule,     // station: trains come more often
    TrainCapacity,     // station: trains bring more
    Optics,            // recon barracks: scouts and observation posts see more
    EntrenchingTools,  // infantry barracks: riflemen dig faster
    Count,
};
inline constexpr size_t kUpgradeCount = static_cast<size_t>(UpgradeId::Count);

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
    DigGunPit,     // howitzer: a capunier; mortar: a closed position
    Camouflage,    // howitzer: nets and branches over it
    RapidFire,     // AGS: five grenades in a row along the front
    Salvo,         // MLRS: everything in the launcher at a point
    IndirectFire,  // tank: fire from a covered position, like artillery, wearing out the barrel
    LayApMine,     // sapper: an anti-personnel mine
    LayAtMine,     // sapper: an anti-tank mine
    ClearMines,    // sapper: lift the enemy mines found around a point
    LayWire,       // sapper: barbed wire along a line
    PlaceHedgehogs,  // sapper: anti-tank obstacles along a line
    BuildPillbox,  // sapper: a firing point facing a direction
    Demolish,      // sapper: a charge against a building or a bridge
    Smoke,         // tank: a smoke screen ahead (needs smoke grenades)
    RadioSilence,  // radios: go quiet (direction finders lose it, orders come by courier) or back on air
    CallSupply,    // radios: call the nearest free tanker / ammunition truck over
    Count,
};
inline constexpr size_t kAbilityCount = static_cast<size_t>(AbilityId::Count);
inline constexpr size_t kMaxAbilities = 8;  // the grid's top row and bottom row

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
    UpgradeId needs = UpgradeId::Count;  // researched first (Count: nothing)
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
    bool engineer = false;  // sappers: mines, obstacles, firing points, demolition
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
    // Electronic warfare. A radio on the air gives the unit away to enemy
    // direction finders; keeping silence, it gets its orders by courier
    // unless a relay (a command vehicle, a signaller) is this close; a DF
    // station, set up, takes bearings on radios this far off.
    bool emitter = false;
    Fixed relay_range{};
    Fixed df_range{};
    // Aviation: flies combat missions from an airfield, one mission a sortie.
    bool aircraft = false;
    // Air defence radar, set up and on the air: sees enemy aircraft this far off.
    Fixed radar_range{};
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
    Ags,         // AGS-17 automatic grenade launcher crew
    Mlrs,        // BM-21 multiple rocket launcher
    Sapper,      // mines, wire, obstacles, pillboxes, demolition
    Spg,         // 2S1 self-propelled howitzer: artillery on tracks
    Signaler,    // a radio on his back: relays orders to silent units nearby
    FieldHq,     // command vehicle: a relay on wheels
    DfStation,   // direction finder: bearings on enemy radios; two of them fix one
    Su25,        // attack aircraft: a rocket run at the target of its mission
    Manpads,     // MANPADS crew (Igla): missiles at aircraft, nothing else
    Shilka,      // ZSU-23-4 self-propelled AA guns: aircraft first, infantry too
    AirRadar,    // air defence radar: sees enemy aircraft far out
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
