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
    // Direct fire: beyond this (0: nowhere) the aim gets worse with the
    // distance, down to kFarAccuracyPercent at the full range.
    Fixed effective_range{};
    // Against buildings, if not `damage`: a shaped charge punches a hole, no more.
    int32_t structure_damage = 0;
    // A guided missile: one that is going to hit flies after its target, as
    // long as the launcher is there to guide it.
    bool guided = false;
    // An armor-piercing shot that goes through by its speed (a tank's AP
    // round), not a shaped charge (an RPG, a missile): the older kinds of
    // explosive reactive armor don't stop it.
    bool kinetic = false;
    // An aircraft's heavy bomb: a block of flats it hits has a section of it
    // brought down (how it looks; see Structure::bombed).
    bool aerial_bomb = false;
};

// Upgrades researched in buildings, as in AoE II's blacksmith and university.
enum class UpgradeId : uint8_t {
    SmokeGrenades,     // ammunition depot: tanks can lay a smoke screen
    SabotRounds,       // ammunition depot: tank AP rounds hit harder
    ClusterMunitions,  // artillery barracks: cluster shells and rockets
    TrainSchedule,     // station: trains come more often
    TrainCapacity,     // station: trains bring more
    Optics,            // recon barracks: scouts and observation posts see more
    EntrenchingTools,  // infantry barracks: riflemen dig faster
    ReactiveArmor,     // armor barracks: tanks take less from anti-tank hits (Kontakt-1; see kEraLevels)
    FireControl,       // armor barracks: tank guns keep their aim far out
    Atgm,              // armor barracks: IFVs fire guided anti-tank missiles
    FiringTables,      // artillery barracks: ranging in goes faster
    DrilledCrews,      // artillery barracks: guns set up and pack up faster
    LongRangeCharges,  // artillery barracks: howitzers, SPGs and mortars reach farther
    BodyArmor,         // infantry barracks: foot soldiers take less from bullets and fragments
    LoadVests,         // infantry barracks: foot soldiers carry more rounds
    GhillieSuits,      // recon barracks: scouts in cover are harder to make out
    HeavyCharges,      // engineer barracks: mines and demolition charges hit harder
    PrefabPillbox,     // engineer barracks: pillboxes go up faster
    SecureComms,       // signals barracks: enemy direction finders need a third bearing
    MastAntennas,      // signals barracks: the headquarters and command vehicles relay farther
    RadarTracking,     // air defence barracks: better aim at aircraft
    CockpitArmor,      // airfield: attack aircraft take less damage
    TankEngine,        // armor barracks: tanks and IFVs drive faster
    AddOnArmor,        // armor barracks: tanks and IFVs take less from shells, fragments, bullets
    FastReload,        // armor barracks: tank and IFV guns reload faster
    IncendiaryShells,  // artillery barracks: shells that set the ground on fire
    PhosphorusShells,  // artillery barracks: white phosphorus, a burning smoke screen
    Kontakt5,          // armor barracks, after Kontakt-1: heavier reactive armor, against AP rounds too
    Relikt,            // armor barracks, after Kontakt-5: the newest, against tandem charges and AP rounds
    Count,
};
inline constexpr size_t kUpgradeCount = static_cast<size_t>(UpgradeId::Count);

// What the artillery (mortars, howitzers, SPGs, rocket launchers) fires:
// high-explosive fragmentation as standard, the others once researched.
enum class Shell : uint8_t {
    He,          // high-explosive fragmentation
    Cluster,     // opens over the target: bomblets over an area
    Incendiary,  // a weaker burst, and the ground burns
    Phosphorus,  // white phosphorus: a smoke screen that burns
    Count,
};
inline constexpr size_t kShellCount = static_cast<size_t>(Shell::Count);

inline UpgradeId shell_upgrade(Shell shell) {
    switch (shell) {
        case Shell::Cluster: return UpgradeId::ClusterMunitions;
        case Shell::Incendiary: return UpgradeId::IncendiaryShells;
        case Shell::Phosphorus: return UpgradeId::PhosphorusShells;
        default: return UpgradeId::Count;  // standard, nothing to research
    }
}

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
    Atgm,          // IFV: a guided anti-tank missile at an enemy vehicle (needs the launchers)
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

// Which real tank, IFV or APC an armored vehicle is: its look, drawn by the
// renderer. The axes' own lines: the Democratic axis (NATO with Ukraine,
// Japan, Korea, Israel), the Authoritarian one (BRICS with Iran). Standard:
// none of them.
enum class VehicleModel : uint8_t {
    Standard,
    T64BV,       // Ukraine
    T64BM,       // Ukraine: Bulat
    Leopard1A5,  // Germany
    Leopard2A6,  // Germany
    M1A1,        // USA
    Type10,      // Japan
    K2,          // Korea: Black Panther
    Merkava4,    // Israel
    T62M,        // Russia
    T72B3,       // Russia
    T80BVM,      // Russia
    T90M,        // Russia: Proryv
    Type99A,     // China
    Karrar,      // Iran
    // IFVs and APCs.
    Bmp2,     // both: Ukraine's and Russia's
    Bmp1,     // Russia
    Bmp3,     // Russia
    Btr82a,   // Russia
    Mtlb,     // Russia
    Zbd04a,   // China
    Ratel20,  // South Africa
    Boragh,   // Iran
    Btr4e,    // Ukraine: Bucephalus
    M113,     // USA
    Bradley,  // USA: M2A2 ODS
    Marder,   // Germany: 1A3
    Stryker,  // USA: M1126
    Type89,   // Japan
    K21,      // Korea
    Namer,    // Israel
    // Self-propelled guns.
    Gvozdika,  // both: 2S1
    Akatsiya,  // Russia: 2S3
    MstaS,     // Russia: 2S19
    Pion,      // Russia: 2S7
    Plz05,     // China
    M109,      // USA: M109A6 Paladin
    PzH2000,   // Germany
    Caesar,    // France: on wheels
    K9,        // Korea: Thunder
    // Self-propelled anti-aircraft guns.
    Shilka,    // Russia: ZSU-23-4
    Tunguska,  // Russia: 2K22
    Pantsir,   // Russia: Pantsir-S1, on wheels
    Pgz09,     // China
    Gepard,    // Germany
    Type87,    // Japan
    K30,       // Korea: Biho
    // Towed howitzers.
    D30,       // both
    MstaB,     // Russia: 2A65
    Giatsint,  // Russia: 2A36 Giatsint-B
    M777,      // USA
    Fh70,      // Germany and Britain
    // Attack aircraft.
    Su25,      // both
    Su34,      // Russia
    A10,       // USA: A-10C
    Count,
};

// What a unit is among the axes' real vehicles, for grouping them in the
// command grid (each group of a building's in a section of its own).
enum class Family : uint8_t { None, Tank, Apc, Spg, Gun, AntiAir, Aircraft };

// The two sides: the first player is always the Democratic axis, the second
// the Authoritarian one. Each has its own tanks.
enum class Axis : uint8_t { Democratic, Authoritarian };
inline Axis axis_of(PlayerId player) { return player % 2 == 0 ? Axis::Democratic : Axis::Authoritarian; }

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
    // Carries this many foot soldiers (an IFV's squad); 0: nobody.
    int32_t troop_capacity = 0;
    // Guided missiles aboard (an IFV's ATGMs); ammunition trucks bring more.
    int32_t missile_capacity = 0;
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
    // Tanks, each a real one. What sets them apart besides speed, gun, price
    // and fuel: of an anti-tank hit from the front, what the armor there lets
    // through (percent); on soft ground (plough, crops, bog, craters, a dry
    // riverbed) how much of the speed lost there it loses (percent: under 100
    // it copes better); the chance its crew gets out when it's knocked out
    // (the men come back); how far along the reactive armor line it can go
    // (0: none bolted on); how it's drawn.
    bool tank = false;
    // IFVs and APCs, each a real one: the same kinds of difference as the
    // tanks'; `floats`: amphibious, it swims through a bog (a heavy one that
    // doesn't sinks in it, as a tank does).
    bool apc = false;
    bool floats = false;
    int32_t front_percent = 100;
    int32_t soft_ground_percent = 100;
    int32_t crew_survives_percent = 0;
    int32_t era_max = 0;
    VehicleModel model = VehicleModel::Standard;
    Family family = Family::None;
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
    // The tanks of the axes (Tank above is the T-72B3).
    T64BV,
    T64BM,
    Leopard1A5,
    Leopard2A6,
    M1A1,
    Type10,
    K2,
    Merkava4,
    T62M,
    T80BVM,
    T90M,
    Type99A,
    Karrar,
    // The IFVs and APCs of the axes (Ifv above is the BMP-2, both sides').
    Bmp1,
    Bmp3,
    Btr82a,
    Mtlb,
    Zbd04a,
    Ratel20,
    Boragh,
    Btr4e,
    M113,
    Bradley,
    Marder,
    Stryker,
    Type89,
    K21,
    Namer,
    // The self-propelled guns of the axes (Spg above is the 2S1, both sides').
    Akatsiya,
    MstaS,
    Pion,
    Plz05,
    M109,
    PzH2000,
    Caesar,
    K9,
    // The self-propelled anti-aircraft guns (Shilka above).
    Tunguska,
    Pantsir,
    Pgz09,
    Gepard,
    Type87,
    K30,
    // The towed howitzers (Howitzer above is the D-30, both sides').
    MstaB,
    Giatsint,
    M777,
    Fh70,
    // The attack aircraft (Su25 above, both sides').
    Su34,
    A10,
    Count,
};
inline constexpr size_t kUnitTypeCount = static_cast<size_t>(UnitTypeId::Count);

const UnitTypeDef& unit_type(UnitTypeId id);

inline MoveClass move_class(const UnitTypeDef& def) {
    if (!def.vehicle) return MoveClass::Foot;
    return def.wheeled ? MoveClass::Wheeled : MoveClass::Vehicle;
}
// Tanks, IFVs and APCs: armor, each a real one.
inline bool is_armor(const UnitTypeDef& def) { return def.tank || def.apc; }
// Heavy tracks that don't swim: in a bog they sink (tanks, some IFVs, SPGs, AA guns).
inline bool sinks_in_bog(const UnitTypeDef& def) {
    return (def.family == Family::Tank || def.family == Family::Apc || def.family == Family::Spg || def.family == Family::AntiAir) &&
           !def.wheeled && !def.floats;
}
// Tube artillery: mortars, howitzers, SPGs (not a rocket launcher's salvo).
bool is_tube_artillery(const UnitTypeDef& def);
// Trucks carry no weapon: they never pick fights.
inline bool is_armed(const UnitTypeDef& def) { return def.weapon.damage > 0; }
// Foot soldiers ride in an IFV; gun crews walk with their guns.
inline bool can_ride(const UnitTypeDef& def) { return !def.vehicle && !def.aircraft && def.deploy_time == 0; }

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
