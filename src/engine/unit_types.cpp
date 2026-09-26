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
        .weapon = {.name = "125mm cannon", .damage = 75, .damage_type = DamageType::Explosive,
                   .range = tiles(7), .reload = seconds(4), .projectile_speed = tiles_per_second(14),
                   .splash_radius = tiles(1, 2), .accuracy = 80, .miss_spread = tiles(1)},
        .cost = {3, 0, 150, 100, 100},
        .train_time = seconds(40),
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
};
static_assert(std::size(kUnitTypes) == kUnitTypeCount);

}  // namespace

const UnitTypeDef& unit_type(UnitTypeId id) { return kUnitTypes[static_cast<size_t>(id)]; }

namespace {

// Armor is {Bullet, Explosive, AntiTank}; bullets are ignored anyway.
constexpr StructureDef kStructureTypes[] = {
    // A small house: ~8 tank shells or ~20 RPG rockets bring it down.
    {.name = "House", .max_hp = 600, .armor = {0, 0, 50}, .capacity = 6},
    // Solid: blowing a bridge takes a real effort (sappers will do it faster).
    {.name = "Bridge", .max_hp = 1500, .armor = {0, 10, 60}, .capacity = 0},
    {.name = "Headquarters", .max_hp = 2500, .armor = {0, 20, 80}, .capacity = 0},
};
static_assert(std::size(kStructureTypes) == static_cast<size_t>(StructureType::Count));

}  // namespace

const StructureDef& structure_type(StructureType type) { return kStructureTypes[static_cast<size_t>(type)]; }

}  // namespace engine
