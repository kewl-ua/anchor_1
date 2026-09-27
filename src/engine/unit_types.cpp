#include "engine/unit_types.h"

#include "engine/structures.h"

#include <iterator>

namespace engine {

namespace {

// Stats are written in human units (tiles, seconds) and converted to ticks here.
constexpr Fixed tiles(int32_t num, int32_t den = 1) { return Fixed::from_ratio(num, den); }
constexpr Fixed tiles_per_second(int32_t num, int32_t den = 1) {
    return Fixed::from_ratio(num, den * kTicksPerSecond);
}
constexpr Tick seconds(int32_t num, int32_t den = 1) { return static_cast<Tick>(num * kTicksPerSecond / den); }

constexpr Fixed kInstantHit{};
constexpr Fixed kNoSplash{};

// Armor is {Bullet, Explosive, AntiTank}.
// Balance numbers are a first draft; the relations matter more than values:
//   machine gun   shreds infantry, useless against armor
//   RPG           slow, dodgeable rocket that cracks armor
//   tank          kills anything, but reloads slowly and its shells can be dodged at range
//   IFV           fast, light armor, autocannon great against infantry
//   rear trooper  gathers, builds and unloads; can be retrained as a rifleman
//   scout         makes out men in cover from farther away, hard to spot himself; as an
//                 observation post watches a sector far out
// Costs are {Personnel, Food, Materials, Ammo, Fuel}. Men are the scarce one.
constexpr UnitTypeDef kUnitTypes[] = {
    {
        .name = "Rifleman",
        .short_name = "RIF",
        .max_hp = 40,
        .armor = {0, 0, 45},
        .speed = tiles_per_second(1),
        .radius = tiles(1, 5),
        .sight = tiles(7),
        .mass = 1,
        .vehicle = false,
        .weapon = {.name = "Assault rifle", .damage = 6, .damage_type = DamageType::Bullet,
                   .range = tiles(5), .reload = seconds(1), .projectile_speed = kInstantHit,
                   .splash_radius = kNoSplash, .accuracy = 70, .miss_spread = tiles(1)},
        .cost = {1, 25, 0, 20, 0},
        .train_time = seconds(12),
        .abilities = {AbilityId::DigTrench, AbilityId::DigFoxhole, AbilityId::BuildParapet},
        .ability_count = 3,
    },
    {
        .name = "Machine gunner",
        .short_name = "MG",
        .max_hp = 45,
        .armor = {0, 0, 45},
        .speed = tiles_per_second(4, 5),
        .radius = tiles(1, 5),
        .sight = tiles(7),
        .mass = 1,
        .vehicle = false,
        .weapon = {.name = "Machine gun", .damage = 3, .damage_type = DamageType::Bullet,
                   .range = tiles(6), .reload = seconds(1, 4), .projectile_speed = kInstantHit,
                   .splash_radius = kNoSplash, .accuracy = 55, .miss_spread = tiles(1)},
        .cost = {1, 25, 0, 40, 0},
        .train_time = seconds(15),
    },
    {
        .name = "Grenadier",
        .short_name = "RPG",
        .max_hp = 40,
        .armor = {0, 0, 45},
        .speed = tiles_per_second(19, 20),
        .radius = tiles(1, 5),
        .sight = tiles(6),
        .mass = 1,
        .vehicle = false,
        .weapon = {.name = "RPG-7", .damage = 80, .damage_type = DamageType::AntiTank,
                   .range = tiles(9, 2), .reload = seconds(5), .projectile_speed = tiles_per_second(6),
                   .splash_radius = kNoSplash, .accuracy = 85, .miss_spread = tiles(3, 2)},
        .cost = {1, 25, 0, 60, 0},
        .train_time = seconds(15),
    },
    {
        .name = "Tank",
        .short_name = "TNK",
        .max_hp = 360,
        .armor = {40, 20, 20},
        .speed = tiles_per_second(11, 10),
        .radius = tiles(9, 20),
        .sight = tiles(8),
        .mass = 30,
        .vehicle = true,
        .weapon = {.name = "125mm HE shell", .damage = 75, .damage_type = DamageType::Explosive,
                   .range = tiles(7), .reload = seconds(4), .projectile_speed = tiles_per_second(14),
                   .splash_radius = tiles(1, 2), .accuracy = 80, .miss_spread = tiles(1)},
        .cost = {3, 0, 150, 100, 100},
        .train_time = seconds(40),
        .fuel_capacity = tiles(150),
        .rounds_capacity = 40,
        // Armor-piercing: a fast, hard-hitting round without a burst.
        .alt_weapon = {.name = "125mm AP round", .damage = 130, .damage_type = DamageType::AntiTank,
                       .range = tiles(7), .reload = seconds(4), .projectile_speed = tiles_per_second(24),
                       .splash_radius = kNoSplash, .accuracy = 85, .miss_spread = tiles(1)},
        .abilities = {AbilityId::AreaShot, AbilityId::SwitchAmmo},
        .ability_count = 2,
    },
    {
        .name = "IFV",
        .short_name = "IFV",
        .max_hp = 220,
        .armor = {8, 8, 5},
        .speed = tiles_per_second(7, 5),
        .radius = tiles(2, 5),
        .sight = tiles(8),
        .mass = 15,
        .vehicle = true,
        .weapon = {.name = "30mm autocannon", .damage = 14, .damage_type = DamageType::Bullet,
                   .range = tiles(6), .reload = seconds(3, 5), .projectile_speed = tiles_per_second(25),
                   .splash_radius = kNoSplash, .accuracy = 70, .miss_spread = tiles(1)},
        .cost = {3, 0, 100, 60, 80},
        .train_time = seconds(30),
        .fuel_capacity = tiles(180),
        .rounds_capacity = 150,
        .rounds_per_supply = 5,
        .abilities = {AbilityId::MgSweep, AbilityId::LobGrenade},
        .ability_count = 2,
    },
    {
        .name = "Rear trooper",
        .short_name = "REAR",
        .max_hp = 30,
        .armor = {0, 0, 45},
        .speed = tiles_per_second(1),
        .radius = tiles(1, 5),
        .sight = tiles(6),
        .mass = 1,
        .vehicle = false,
        .weapon = {.name = "Carbine", .damage = 4, .damage_type = DamageType::Bullet,
                   .range = tiles(4), .reload = seconds(5, 4), .projectile_speed = kInstantHit,
                   .splash_radius = kNoSplash, .accuracy = 60, .miss_spread = tiles(1)},
        .cost = {1, 25, 0, 0, 0},
        .train_time = seconds(10),
        .worker = true,
    },
    {
        // Unarmed and thin-skinned: the supply line is a target.
        .name = "Supply truck",
        .short_name = "TRK",
        .max_hp = 120,
        .armor = {3, 0, 0},
        .speed = tiles_per_second(9, 5),
        .radius = tiles(7, 20),
        .sight = tiles(6),
        .mass = 10,
        .vehicle = true,
        .wheeled = true,
        .weapon = {.name = "Unarmed", .damage = 0, .damage_type = DamageType::Bullet, .range = tiles(0),
                   .reload = seconds(1), .projectile_speed = kInstantHit, .splash_radius = kNoSplash,
                   .accuracy = 0, .miss_spread = tiles(0)},
        .cost = {1, 0, 50, 0, 30},
        .train_time = seconds(15),
    },
    {
        // Eyes, not firepower: a carbine for self-defence, binoculars for the rest.
        .name = "Scout",
        .short_name = "SCT",
        .max_hp = 35,
        .armor = {0, 0, 45},
        .speed = tiles_per_second(6, 5),
        .radius = tiles(1, 5),
        .sight = tiles(9),
        .mass = 1,
        .vehicle = false,
        .weapon = {.name = "Carbine", .damage = 5, .damage_type = DamageType::Bullet,
                   .range = tiles(5), .reload = seconds(5, 4), .projectile_speed = kInstantHit,
                   .splash_radius = kNoSplash, .accuracy = 65, .miss_spread = tiles(1)},
        .cost = {1, 25, 0, 15, 0},
        .train_time = seconds(15),
        .detection = tiles(4),
        .stealthy = true,
        .sector_range = tiles(15),
    },
    {
        // Lighter than a rifleman, deadlier up close: storms trenches and houses.
        .name = "Assault trooper",
        .short_name = "AST",
        .max_hp = 32,
        .armor = {0, 0, 45},
        .speed = tiles_per_second(23, 20),
        .radius = tiles(1, 5),
        .sight = tiles(7),
        .mass = 1,
        .vehicle = false,
        .weapon = {.name = "Short assault rifle", .damage = 7, .damage_type = DamageType::Bullet,
                   .range = tiles(4), .reload = seconds(4, 5), .projectile_speed = kInstantHit,
                   .splash_radius = kNoSplash, .accuracy = 75, .miss_spread = tiles(1)},
        .cost = {1, 25, 0, 35, 0},
        .train_time = seconds(15),
        .abilities = {AbilityId::ThrowGrenade},
        .ability_count = 1,
    },
    {
        // Thin-skinned and full of fuel: the enemy's favourite target.
        .name = "Fuel tanker",
        .short_name = "FUEL",
        .max_hp = 60,
        .armor = {0, 0, 0},
        .speed = tiles_per_second(8, 5),
        .radius = tiles(7, 20),
        .sight = tiles(6),
        .mass = 10,
        .vehicle = true,
        .wheeled = true,
        .weapon = {.name = "Unarmed", .damage = 0, .damage_type = DamageType::Bullet, .range = tiles(0),
                   .reload = seconds(1), .projectile_speed = kInstantHit, .splash_radius = kNoSplash,
                   .accuracy = 0, .miss_spread = tiles(0)},
        .cost = {1, 0, 60, 0, 40},
        .train_time = seconds(15),
        .supplies = Resource::Fuel,
        .cargo_capacity = 150,
        .abilities = {AbilityId::Refill},
        .ability_count = 1,
    },
    {
        .name = "Ammunition truck",
        .short_name = "AMMO",
        .max_hp = 100,
        .armor = {3, 0, 0},
        .speed = tiles_per_second(17, 10),
        .radius = tiles(7, 20),
        .sight = tiles(6),
        .mass = 10,
        .vehicle = true,
        .wheeled = true,
        .weapon = {.name = "Unarmed", .damage = 0, .damage_type = DamageType::Bullet, .range = tiles(0),
                   .reload = seconds(1), .projectile_speed = kInstantHit, .splash_radius = kNoSplash,
                   .accuracy = 0, .miss_spread = tiles(0)},
        .cost = {1, 0, 60, 20, 20},
        .train_time = seconds(15),
        .supplies = Resource::Ammo,
        .cargo_capacity = 100,
        .abilities = {AbilityId::Refill},
        .ability_count = 1,
    },
    {
        // Plunging fire: the one that finds men in trenches.
        .name = "Mortar crew",
        .short_name = "MOR",
        .max_hp = 45,
        .armor = {0, 0, 45},
        .speed = tiles_per_second(4, 5),
        .radius = tiles(1, 4),
        .sight = tiles(6),
        .mass = 2,
        .vehicle = false,
        .weapon = {.name = "82mm mortar bomb", .damage = 45, .damage_type = DamageType::Explosive,
                   .range = tiles(14), .reload = seconds(3), .projectile_speed = tiles_per_second(7),
                   .splash_radius = tiles(1), .accuracy = 100, .miss_spread = tiles(0), .indirect = true,
                   .min_range = tiles(3)},
        .cost = {2, 50, 0, 40, 0},
        .train_time = seconds(20),
        .rounds_capacity = 40,
        .abilities = {AbilityId::Deploy},
        .ability_count = 1,
        .deploy_time = seconds(3),
    },
    {
        // Far-reaching and blind: it hits what someone else sees for it.
        .name = "Howitzer D-30",
        .short_name = "HOW",
        .max_hp = 160,
        .armor = {8, 10, 10},
        .speed = tiles_per_second(1),
        .radius = tiles(2, 5),
        .sight = tiles(5),
        .mass = 12,
        .vehicle = true,
        .wheeled = true,  // towed
        .weapon = {.name = "122mm HE shell", .damage = 110, .damage_type = DamageType::Explosive,
                   .range = tiles(26), .reload = seconds(6), .projectile_speed = tiles_per_second(9),
                   .splash_radius = tiles(3, 2), .accuracy = 100, .miss_spread = tiles(0), .indirect = true,
                   .min_range = tiles(5)},
        .cost = {4, 0, 150, 60, 30},
        .train_time = seconds(40),
        .rounds_capacity = 30,
        .abilities = {AbilityId::Deploy},
        .ability_count = 1,
        .deploy_time = seconds(8),
    },
};
static_assert(std::size(kUnitTypes) == kUnitTypeCount);

}  // namespace

const UnitTypeDef& unit_type(UnitTypeId id) { return kUnitTypes[static_cast<size_t>(id)]; }

namespace {

constexpr AbilityDef kAbilities[] = {
    {.name = "Area shot: one HE-FRAG shell with a wide burst", .label = "Area shot", .target = AbilityTarget::Point,
     .range = tiles(7), .cooldown = seconds(20),
     .weapon = {.name = "125mm HE-FRAG shell", .damage = 75, .damage_type = DamageType::Explosive,
                .range = tiles(7), .reload = seconds(4), .projectile_speed = tiles_per_second(14),
                .splash_radius = tiles(5, 4), .accuracy = 85, .miss_spread = tiles(1)}},
    {.name = "Switch rounds: HE / armor-piercing (reloads the gun)", .label = "HE / AP",
     .target = AbilityTarget::Instant, .range = tiles(0), .cooldown = 0},
    // Fired along the front, not aimed at anyone: whoever is in the way gets it.
    {.name = "Machine gun along the front", .label = "MG sweep", .target = AbilityTarget::Direction,
     .range = tiles(8), .cooldown = seconds(12),
     .weapon = {.name = "Coaxial machine gun", .damage = 5, .damage_type = DamageType::Bullet, .range = tiles(8),
                .reload = 0, .projectile_speed = kInstantHit, .splash_radius = kNoSplash, .accuracy = 100,
                .miss_spread = tiles(0)}},
    // Lobbed: flies over cover and comes down among the men in it.
    {.name = "Grenade launcher at an area", .label = "Grenade", .target = AbilityTarget::Point,
     .range = tiles(6), .cooldown = seconds(10),
     .weapon = {.name = "30mm grenade", .damage = 30, .damage_type = DamageType::Explosive, .range = tiles(6),
                .reload = 0, .projectile_speed = tiles_per_second(10), .splash_radius = tiles(1),
                .accuracy = 75, .miss_spread = tiles(1)}},
    {.name = "Dig a trench (drag a line)", .label = "Trench", .target = AbilityTarget::Line,
     .range = tiles(0), .cooldown = 0},
    {.name = "Dig a foxhole here: hits -50%, own accuracy -20%", .label = "Foxhole",
     .target = AbilityTarget::Instant, .range = tiles(0), .cooldown = 0},
    {.name = "Parapet facing a direction: +25% cover from the front", .label = "Parapet",
     .target = AbilityTarget::Direction, .range = tiles(0), .cooldown = 0},
    // Thrown in: into a trench, through a dugout's entrance or a window.
    {.name = "Hand grenade: into a trench, a dugout, a house", .label = "Grenade", .target = AbilityTarget::Point,
     .range = tiles(3), .cooldown = seconds(15),
     .weapon = {.name = "Hand grenade", .damage = 30, .damage_type = DamageType::Explosive, .range = tiles(3),
                .reload = 0, .projectile_speed = tiles_per_second(8), .splash_radius = tiles(1),
                .accuracy = 85, .miss_spread = tiles(1, 2)}},
    {.name = "Refill at the depot (fuel or ammunition, from the stock)", .label = "Refill",
     .target = AbilityTarget::Instant, .range = tiles(0), .cooldown = 0},
    {.name = "Deploy to fire / pack up to move (it also happens by itself)", .label = "Deploy",
     .target = AbilityTarget::Instant, .range = tiles(0), .cooldown = 0},
};
static_assert(std::size(kAbilities) == kAbilityCount);

}  // namespace

const AbilityDef& ability_def(AbilityId id) { return kAbilities[static_cast<size_t>(id)]; }

namespace {

// Armor is {Bullet, Explosive, AntiTank}; bullets are ignored anyway.
// Costs are {Personnel, Food, Materials, Ammo, Fuel}; build times are for one
// rear trooper (two build twice as fast).
constexpr StructureDef kStructureTypes[] = {
    // A small house: ~8 tank shells or ~20 RPG rockets bring it down.
    {.name = "House", .max_hp = 600, .armor = {0, 0, 50}, .capacity = 6},
    // Solid: blowing a bridge takes a real effort (sappers will do it faster).
    {.name = "Bridge", .max_hp = 1500, .armor = {0, 10, 60}},
    {.name = "Headquarters", .max_hp = 2500, .armor = {0, 20, 80},
     .roster = {UnitTypeId::Worker, UnitTypeId::Truck, UnitTypeId::FuelTanker, UnitTypeId::AmmoTruck},
     .roster_size = 4},
    {.name = "Infantry barracks", .max_hp = 1500, .armor = {0, 10, 60},
     .buildable = true, .width = 3, .height = 3, .cost = {0, 0, 150, 0, 0}, .build_time = seconds(30),
     .roster = {UnitTypeId::Rifleman, UnitTypeId::MachineGunner, UnitTypeId::Grenadier, UnitTypeId::Assault},
     .roster_size = 4},
    {.name = "Armor barracks", .max_hp = 2200, .armor = {0, 15, 70},
     .buildable = true, .width = 4, .height = 4, .cost = {0, 0, 250, 0, 50}, .build_time = seconds(45),
     .roster = {UnitTypeId::Tank, UnitTypeId::Ifv}, .roster_size = 2},
    // Put one next to a woodline or a quarry to cut the walk, like an AoE II lumber camp.
    {.name = "Warehouse", .max_hp = 800, .armor = {0, 5, 40},
     .buildable = true, .width = 2, .height = 2, .cost = {0, 0, 75, 0, 0}, .build_time = seconds(20)},
    // The railhead is given, not built: lose it and the trains stop coming.
    {.name = "Railway station", .max_hp = 2000, .armor = {0, 15, 60}},
    {.name = "Ammo depot", .max_hp = 900, .armor = {0, 5, 40},
     .buildable = true, .width = 2, .height = 2, .cost = {0, 0, 100, 0, 0}, .build_time = seconds(25)},
    // Flimsy, and it goes up in flames with the fuel inside.
    {.name = "Fuel depot", .max_hp = 450, .armor = {0, 0, 10},
     .buildable = true, .width = 2, .height = 2, .cost = {0, 0, 100, 0, 0}, .build_time = seconds(25)},
    {.name = "Recon barracks", .max_hp = 1000, .armor = {0, 10, 60},
     .buildable = true, .width = 2, .height = 2, .cost = {0, 0, 100, 0, 0}, .build_time = seconds(25),
     .roster = {UnitTypeId::Scout}, .roster_size = 1},
    // Field works, dug by infantry one tile at a time; shells fill them in.
    {.name = "Trench", .max_hp = 200, .armor = {0, 10, 60}},
    {.name = "Foxhole", .max_hp = 150, .armor = {0, 10, 60}},
    {.name = "Parapet", .max_hp = 150, .armor = {0, 10, 60}},
    // Logs and earth: holds against mortars and light shells, not against heavy ones.
    {.name = "Dugout", .max_hp = 1200, .armor = {0, 30, 80}, .capacity = 8},
    {.name = "Artillery barracks", .max_hp = 1800, .armor = {0, 15, 60},
     .buildable = true, .width = 3, .height = 3, .cost = {0, 0, 200, 0, 50}, .build_time = seconds(40),
     .roster = {UnitTypeId::Mortar, UnitTypeId::Howitzer}, .roster_size = 2},
};
static_assert(std::size(kStructureTypes) == static_cast<size_t>(StructureType::Count));

}  // namespace

const StructureDef& structure_type(StructureType type) { return kStructureTypes[static_cast<size_t>(type)]; }

}  // namespace engine
