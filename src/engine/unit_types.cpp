#include "engine/unit_types.h"

#include <span>

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
// The T-72B3: the tank the others are measured against.
constexpr UnitTypeDef kT72B3 = {
        .name = "T-72B3",
        .short_name = "T72",
        .max_hp = 360,
        .armor = {40, 20, 20},
        .speed = tiles_per_second(11, 10),
        .radius = tiles(9, 20),
        .sight = tiles(12),
        .mass = 30,
        .vehicle = true,
        // Ten times a rifleman's reach; far out it's less sure (see effective_range),
        // and it takes someone seeing that far.
        .weapon = {.name = "125mm HE shell", .damage = 75, .damage_type = DamageType::Explosive,
                   .range = tiles(50), .reload = seconds(4), .projectile_speed = tiles_per_second(30),
                   .splash_radius = tiles(1, 2), .accuracy = 80, .miss_spread = tiles(1),
                   .effective_range = tiles(15)},
        .cost = {3, 0, 150, 100, 100},
        .train_time = seconds(40),
        .fuel_capacity = tiles(150),
        .rounds_capacity = 40,
        // Armor-piercing: a fast, hard-hitting round without a burst.
        .alt_weapon = {.name = "125mm AP round", .damage = 130, .damage_type = DamageType::AntiTank,
                       .range = tiles(50), .reload = seconds(4), .projectile_speed = tiles_per_second(45),
                       .splash_radius = kNoSplash, .accuracy = 85, .miss_spread = tiles(1),
                       .effective_range = tiles(15), .kinetic = true},
        .abilities = {AbilityId::AreaShot, AbilityId::SwitchAmmo, AbilityId::IndirectFire, AbilityId::Smoke,
                      AbilityId::RadioSilence, AbilityId::CallSupply},
        .ability_count = 6,
        .emitter = true,
        .tank = true,
        .crew_survives_percent = 15,  // the rounds in the carousel under the turret go up
        .era_max = 3,
        .model = VehicleModel::T72B3,
        .family = Family::Tank,
    };

// A real tank, from the T-72B3 (= 100): its speed, its armor in front,
// its aim far out (how far the gun keeps it), its thirst, how its crew
// fares, its price, how it copes with soft ground, its AP round, how far
// along the reactive armor line it can go.
struct TankSpec {
    const char* name;
    const char* short_name;
    VehicleModel model;
    int32_t speed;
    int32_t front;      // armor in front: 150 lets through two thirds of what the T-72B3's does
    int32_t far_aim;    // fire control: the effective range, percent
    int32_t thirst;     // fuel used a tile, percent: the tank goes that much less far
    int32_t crew;       // percent that get out
    int32_t price;
    int32_t soft;       // passability on soft ground: 130 loses a third less there
    int32_t ap_damage;  // its AP round
    int32_t era_max;
};

constexpr UnitTypeDef tank_def(const TankSpec& t) {
    UnitTypeDef d = kT72B3;
    d.name = t.name;
    d.short_name = t.short_name;
    d.model = t.model;
    d.speed = Fixed::from_raw(kT72B3.speed.raw * t.speed / 100);
    d.front_percent = 100 * 100 / t.front;
    d.weapon.effective_range = Fixed::from_raw(kT72B3.weapon.effective_range.raw * t.far_aim / 100);
    d.alt_weapon.effective_range = d.weapon.effective_range;
    d.alt_weapon.damage = t.ap_damage;
    d.fuel_capacity = Fixed::from_raw(kT72B3.fuel_capacity.raw * 100 / t.thirst);
    d.crew_survives_percent = t.crew;
    for (size_t r = static_cast<size_t>(Resource::Materials); r < kResourceCount; ++r) d.cost[r] = kT72B3.cost[r] * t.price / 100;
    d.train_time = kT72B3.train_time * (50 + t.price / 2) / 100;
    d.soft_ground_percent = 100 * 100 / t.soft;
    d.era_max = t.era_max;
    return d;
}

//                  name            short   model                  speed front aim thirst crew price soft  AP  ERA
constexpr TankSpec kTankSpecs[] = {
    {"T-64BV",        "T64", VehicleModel::T64BV,      100,   90,  90, 100, 40,  80, 130, 125, 1},
    {"T-64BM Bulat",  "BLT", VehicleModel::T64BM,       95,  110, 110, 100, 40, 110, 120, 130, 3},
    {"Leopard 1A5",   "LEO1", VehicleModel::Leopard1A5, 110,  60, 120,  90, 25,  70, 120, 115, 0},
    {"Leopard 2A6",   "LEO2", VehicleModel::Leopard2A6, 105, 140, 150, 120, 45, 170,  85, 140, 0},
    {"M1A1 Abrams",   "M1",  VehicleModel::M1A1,       105,  150, 140, 200, 60, 190,  75, 135, 2},
    {"Type 10",       "T10", VehicleModel::Type10,     110,  120, 150, 100, 40, 150, 125, 135, 0},
    {"K2 Black Panther", "K2", VehicleModel::K2,       110,  135, 160, 110, 45, 180, 100, 140, 0},
    {"Merkava Mk4",   "MRK", VehicleModel::Merkava4,    90,  150, 140, 110, 70, 180,  80, 135, 0},
    {"T-62M",         "T62", VehicleModel::T62M,        95,   60,  60, 100, 10,  50, 100, 110, 1},
    {"T-80BVM",       "T80", VehicleModel::T80BVM,     120,  110, 110, 200, 15, 130, 115, 130, 3},
    {"T-90M Proryv",  "T90", VehicleModel::T90M,       100,  140, 140, 110, 25, 170, 100, 135, 3},
    {"Type 99A",      "99A", VehicleModel::Type99A,    100,  140, 150, 120, 25, 180,  90, 135, 3},
    {"Karrar",        "KRR", VehicleModel::Karrar,     100,  110, 115, 100, 15, 110, 100, 130, 3},
};

// The BMP-2: the IFV the others are measured against, both sides have it.
constexpr WeaponDef k30mm = {.name = "30mm autocannon", .damage = 14, .damage_type = DamageType::Bullet,
                             .range = tiles(6), .reload = seconds(3, 5), .projectile_speed = tiles_per_second(25),
                             .splash_radius = kNoSplash, .accuracy = 70, .miss_spread = tiles(1)};
constexpr UnitTypeDef kBmp2 = {
        .name = "BMP-2",
        .short_name = "BMP2",
        .max_hp = 220,
        .armor = {8, 8, 5},
        .speed = tiles_per_second(7, 5),
        .radius = tiles(2, 5),
        .sight = tiles(8),
        .mass = 15,
        .vehicle = true,
        .weapon = k30mm,
        .cost = {3, 0, 100, 60, 80},
        .train_time = seconds(30),
        .fuel_capacity = tiles(180),
        .rounds_capacity = 150,
        .rounds_per_supply = 5,
        .troop_capacity = 7,
        .missile_capacity = 4,
        .abilities = {AbilityId::MgSweep, AbilityId::LobGrenade, AbilityId::RadioSilence, AbilityId::CallSupply,
                      AbilityId::Atgm},
        .ability_count = 5,
        .emitter = true,
        .apc = true,
        .floats = true,
        .soft_ground_percent = 100 * 100 / 120,
        .crew_survives_percent = 30,
        .model = VehicleModel::Bmp2,
        .family = Family::Apc,
    };

// What an IFV or APC fires: a machine gun, an autocannon, the BMP-1's
// low-pressure 73 mm gun (HE-FRAG, or HEAT loaded instead), the 30 mm with
// the 100 mm beside it (the BMP-3's, the ZBD-04A's).
enum class ApcGun : uint8_t { Pkt, Hmg, Auto20, Auto25, Auto30, Auto35, Auto40, Grom, Gun100 };

struct ApcGunDef {
    WeaponDef weapon;
    WeaponDef alt;  // damage 0: none
    int32_t rounds;
    int32_t per_supply;  // rounds a unit of ammunition
};

constexpr ApcGunDef apc_gun(ApcGun gun) {
    auto auto_cannon = [](const char* name, int32_t damage, int32_t range, Tick reload, uint8_t accuracy) {
        return WeaponDef{.name = name, .damage = damage, .damage_type = DamageType::Bullet, .range = tiles(range),
                         .reload = reload, .projectile_speed = tiles_per_second(25), .splash_radius = kNoSplash,
                         .accuracy = accuracy, .miss_spread = tiles(1)};
    };
    switch (gun) {
        case ApcGun::Pkt:
            return {{.name = "7.62mm PKT", .damage = 4, .damage_type = DamageType::Bullet, .range = tiles(6),
                     .reload = seconds(1, 4), .projectile_speed = kInstantHit, .splash_radius = kNoSplash, .accuracy = 55,
                     .miss_spread = tiles(1)},
                    {}, 1500, 50};
        case ApcGun::Hmg:
            return {{.name = "12.7mm machine gun", .damage = 8, .damage_type = DamageType::Bullet, .range = tiles(7),
                     .reload = seconds(3, 10), .projectile_speed = kInstantHit, .splash_radius = kNoSplash, .accuracy = 60,
                     .miss_spread = tiles(1)},
                    {}, 600, 20};
        case ApcGun::Auto20: return {auto_cannon("20mm autocannon", 11, 6, seconds(1, 2), 70), {}, 250, 8};
        case ApcGun::Auto25: return {auto_cannon("25mm Bushmaster", 13, 7, seconds(11, 20), 75), {}, 200, 6};
        case ApcGun::Auto30: return {k30mm, {}, 150, 5};
        case ApcGun::Auto35: return {auto_cannon("35mm autocannon", 17, 7, seconds(7, 10), 72), {}, 120, 4};
        case ApcGun::Auto40: return {auto_cannon("40mm autocannon", 20, 7, seconds(4, 5), 72), {}, 100, 3};
        case ApcGun::Grom:
            return {{.name = "73mm HE-FRAG", .damage = 30, .damage_type = DamageType::Explosive, .range = tiles(7),
                     .reload = seconds(3), .projectile_speed = tiles_per_second(10), .splash_radius = tiles(2, 5),
                     .accuracy = 70, .miss_spread = tiles(1)},
                    {.name = "73mm HEAT", .damage = 110, .damage_type = DamageType::AntiTank, .range = tiles(7),
                     .reload = seconds(3), .projectile_speed = tiles_per_second(10), .splash_radius = kNoSplash,
                     .accuracy = 70, .miss_spread = tiles(1), .structure_damage = 60},
                    40, 1};
        case ApcGun::Gun100:
            return {k30mm,
                    {.name = "100mm HE-FRAG", .damage = 50, .damage_type = DamageType::Explosive, .range = tiles(9),
                     .reload = seconds(4), .projectile_speed = tiles_per_second(12), .splash_radius = tiles(1, 2),
                     .accuracy = 75, .miss_spread = tiles(1)},
                    150, 5};
    }
    return {k30mm, {}, 150, 5};
}

// A real IFV or APC: wheels or tracks, whether it swims, its speed (the
// BMP-2 = 100), its hit points and armor {bullet, explosive, anti-tank},
// its armor in front (as a tank's), its gun, the men it carries, its price
// (the BMP-2 = 100) and crew, how it copes with soft ground, its thirst,
// how its crew fares, whether it takes the ATGM launchers, whether it has
// a grenade launcher.
struct ApcSpec {
    const char* name;
    const char* short_name;
    VehicleModel model;
    bool wheeled;
    bool floats;
    int32_t speed;
    int32_t hp;
    std::array<int32_t, kDamageTypeCount> armor;
    int32_t front;
    ApcGun gun;
    int32_t troops;
    int32_t price;
    int32_t men;
    int32_t soft;
    int32_t thirst;
    int32_t crew;
    bool atgm;
    bool grenades;
};

constexpr UnitTypeDef apc_def(const ApcSpec& a) {
    UnitTypeDef d = kBmp2;
    d.name = a.name;
    d.short_name = a.short_name;
    d.model = a.model;
    d.wheeled = a.wheeled;
    d.floats = a.floats;
    d.speed = Fixed::from_raw(kBmp2.speed.raw * a.speed / 100);
    d.max_hp = a.hp;
    d.armor = a.armor;
    d.front_percent = 100 * 100 / a.front;
    d.mass = kBmp2.mass * a.hp / kBmp2.max_hp;
    d.radius = Fixed::from_raw(kBmp2.radius.raw * (180 + a.hp / 10) / 202);  // the heavy ones are bigger
    const ApcGunDef gun = apc_gun(a.gun);
    d.weapon = gun.weapon;
    d.alt_weapon = gun.alt;
    d.rounds_capacity = gun.rounds;
    d.rounds_per_supply = gun.per_supply;
    d.troop_capacity = a.troops;
    d.cost[static_cast<size_t>(Resource::Personnel)] = a.men;
    for (size_t r = static_cast<size_t>(Resource::Materials); r < kResourceCount; ++r) d.cost[r] = kBmp2.cost[r] * a.price / 100;
    d.train_time = kBmp2.train_time * (50 + a.price / 2) / 100;
    d.soft_ground_percent = 100 * 100 / a.soft;
    d.fuel_capacity = Fixed::from_raw(kBmp2.fuel_capacity.raw * 100 / a.thirst);
    d.crew_survives_percent = a.crew;
    d.missile_capacity = a.atgm ? 4 : 0;
    uint8_t n = 0;
    d.abilities = {};
    d.abilities[n++] = AbilityId::MgSweep;
    if (a.grenades) d.abilities[n++] = AbilityId::LobGrenade;
    if (gun.alt.damage > 0) d.abilities[n++] = AbilityId::SwitchAmmo;
    d.abilities[n++] = AbilityId::RadioSilence;
    d.abilities[n++] = AbilityId::CallSupply;
    if (a.atgm) d.abilities[n++] = AbilityId::Atgm;
    d.ability_count = n;
    return d;
}

//   name               short   model                  wheels floats spd  hp   armor         front gun           men> troops price crew soft thirst survive ATGM  grenades
constexpr ApcSpec kApcSpecs[] = {
    // The Authoritarian axis.
    {"BMP-1",            "BMP1", VehicleModel::Bmp1,    false, true,  100, 190, {6, 6, 3},    100, ApcGun::Grom,   8,  65, 3, 120, 100, 20, true,  false},
    {"BMP-3",            "BMP3", VehicleModel::Bmp3,    false, true,  105, 240, {8, 8, 5},    100, ApcGun::Gun100, 7, 150, 3, 110, 110, 25, true,  false},
    {"BTR-82A",          "BTR",  VehicleModel::Btr82a,  true,  true,  110, 210, {6, 6, 4},    100, ApcGun::Auto30, 7,  90, 3,  80,  80, 30, false, false},
    {"MT-LB",            "MTLB", VehicleModel::Mtlb,    false, true,   90, 150, {4, 5, 2},    100, ApcGun::Pkt,   11,  45, 2, 150,  80, 30, false, false},
    {"ZBD-04A",          "Z04",  VehicleModel::Zbd04a,  false, true,  100, 250, {8, 10, 6},   105, ApcGun::Gun100, 7, 160, 3, 110, 110, 30, true,  false},
    {"Ratel 20",         "RTL",  VehicleModel::Ratel20, true,  false, 110, 210, {6, 6, 4},    100, ApcGun::Auto20, 8,  80, 4,  75,  70, 35, false, false},
    {"Boragh",           "BRG",  VehicleModel::Boragh,  false, true,   95, 200, {5, 6, 3},    100, ApcGun::Hmg,    8,  60, 2, 115, 100, 25, false, false},
    // The Democratic axis.
    {"BTR-4E Bucephalus", "BTR4", VehicleModel::Btr4e,  true,  true,  105, 230, {8, 8, 5},    100, ApcGun::Auto30, 8, 110, 3,  80,  80, 35, true,  true},
    {"M113A3",           "M113", VehicleModel::M113,    false, true,   95, 170, {5, 6, 3},    100, ApcGun::Hmg,   11,  55, 2, 110,  90, 30, false, false},
    {"M2A2 Bradley",     "BRD",  VehicleModel::Bradley, false, false,  95, 300, {10, 12, 8},  115, ApcGun::Auto25, 6, 170, 3,  90, 130, 55, true,  false},
    {"Marder 1A3",       "MRD",  VehicleModel::Marder,  false, false,  95, 280, {10, 12, 6},  110, ApcGun::Auto20, 6, 130, 3,  95, 110, 50, true,  false},
    {"Stryker",          "STR",  VehicleModel::Stryker, true,  false, 110, 240, {8, 8, 5},    100, ApcGun::Hmg,    9, 120, 2,  75,  80, 45, false, true},
    {"Type 89",          "T89",  VehicleModel::Type89,  false, false, 100, 270, {10, 10, 6},  105, ApcGun::Auto35, 7, 160, 3, 100, 110, 45, true,  false},
    {"K21",              "K21",  VehicleModel::K21,     false, true,  105, 270, {10, 12, 7},  110, ApcGun::Auto40, 9, 170, 3, 110, 110, 45, false, false},
    {"Namer",            "NMR",  VehicleModel::Namer,   false, false,  85, 420, {16, 25, 20}, 140, ApcGun::Hmg,    9, 220, 3,  70, 150, 70, false, true},
};

// The 2S1 Gvozdika: the SPG the others are measured against, both sides have it.
constexpr UnitTypeDef kGvozdika = {
        // A howitzer on tracks: sets up in moments, shoots and scoots.
        .name = "2S1 Gvozdika",
        .short_name = "SPG",
        .max_hp = 200,
        .armor = {15, 15, 20},
        .speed = tiles_per_second(6, 5),
        .radius = tiles(9, 20),
        .sight = tiles(6),
        .mass = 14,
        .vehicle = true,
        .weapon = {.name = "122mm HE shell", .damage = 110, .damage_type = DamageType::Explosive,
                   .range = tiles(60), .reload = seconds(6), .projectile_speed = tiles_per_second(9),
                   .splash_radius = tiles(3, 2), .accuracy = 100, .miss_spread = tiles(0), .indirect = true,
                   .min_range = tiles(4)},
        .cost = {4, 0, 200, 60, 60},
        .train_time = seconds(40),
        .fuel_capacity = tiles(160),
        .rounds_capacity = 40,
        .abilities = {AbilityId::Deploy, AbilityId::DigGunPit, AbilityId::Camouflage, AbilityId::RadioSilence,
                      AbilityId::CallSupply},
        .ability_count = 5,
        .deploy_time = seconds(2),
        .emitter = true,
        .floats = true,
        .soft_ground_percent = 100 * 100 / 140,
        .crew_survives_percent = 30,
        .model = VehicleModel::Gvozdika,
        .family = Family::Spg,
    };

// The D-30: the towed howitzer the others are measured against, both sides have it.
constexpr UnitTypeDef kD30 = {
        // Far-reaching and blind: it hits what someone else sees for it.
        .name = "D-30",
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
                   .range = tiles(60), .reload = seconds(6), .projectile_speed = tiles_per_second(9),
                   .splash_radius = tiles(3, 2), .accuracy = 100, .miss_spread = tiles(0), .indirect = true,
                   .min_range = tiles(5)},
        .cost = {4, 0, 150, 60, 30},
        .train_time = seconds(40),
        .rounds_capacity = 30,
        .abilities = {AbilityId::Deploy, AbilityId::DigGunPit, AbilityId::Camouflage, AbilityId::RadioSilence,
                      AbilityId::CallSupply},
        .ability_count = 5,
        .deploy_time = seconds(8),
        .emitter = true,
        .crew_survives_percent = 60,
        .model = VehicleModel::D30,
        .family = Family::Gun,
    };

// The ZSU-23-4 Shilka: the AA gun the others are measured against.
constexpr UnitTypeDef kShilka = {
        // Four radar-laid 23mm barrels: aircraft first, and murder on infantry.
        // Its radar is on the air; switched off, it aims by eye.
        .name = "ZSU-23-4 Shilka",
        .short_name = "ZSU",
        .max_hp = 220,
        .armor = {15, 10, 10},
        .speed = tiles_per_second(6, 5),
        .radius = tiles(9, 20),
        .sight = tiles(8),
        .mass = 20,
        .vehicle = true,
        .weapon = {.name = "23mm quad AA guns", .damage = 14, .damage_type = DamageType::Bullet, .range = tiles(7),
                   .reload = seconds(1, 4), .projectile_speed = kInstantHit, .splash_radius = kNoSplash,
                   .accuracy = 55, .miss_spread = tiles(1), .anti_air = true},
        .cost = {3, 0, 150, 80, 60},
        .train_time = seconds(35),
        .fuel_capacity = tiles(150),
        .rounds_capacity = 200,
        .rounds_per_supply = 10,
        .abilities = {AbilityId::RadioSilence, AbilityId::CallSupply},
        .ability_count = 2,
        .emitter = true,
        .crew_survives_percent = 20,
        .model = VehicleModel::Shilka,
        .family = Family::AntiAir,
    };

// The Su-25: the attack aircraft the others are measured against, both sides fly it. Its pilot ejects.
constexpr UnitTypeDef kSu25 = {
        // Flies only on missions: a rocket run at the target, back to the
        // airfield, rearmed from the stock. Tough: takes two missile hits.
        .name = "Su-25",
        .short_name = "SU25",
        .max_hp = 300,
        .armor = {5, 15, 15},
        .speed = tiles_per_second(5),
        .radius = tiles(1, 2),
        .sight = tiles(9),
        .mass = 20,
        .vehicle = true,
        .weapon = {.name = "S-8 rockets", .damage = 60, .damage_type = DamageType::Explosive, .range = tiles(9),
                   .reload = 0, .projectile_speed = tiles_per_second(25), .splash_radius = tiles(6, 5),
                   .accuracy = 100, .miss_spread = tiles(0)},
        .cost = {1, 0, 250, 80, 120},
        .train_time = seconds(60),
        .fuel_capacity = tiles(240),
        .rounds_capacity = 16,
        .aircraft = true,
        .crew_survives_percent = 50,
        .model = VehicleModel::Su25,
        .family = Family::Aircraft,
    };

// A real SPG or towed howitzer, from the 2S1 (the D-30): its speed, hit
// points and armor, its shell (damage, burst in tenths of a tile), its
// reach, its reload (tenths of a second), rounds aboard, price, crew, how
// long it takes to set up (tenths of a second), soft ground, thirst, how
// its crew fares.
struct GunSpec {
    const char* name;
    const char* short_name;
    VehicleModel model;
    bool wheeled;
    bool floats;
    int32_t speed;
    int32_t hp;
    std::array<int32_t, kDamageTypeCount> armor;
    int32_t damage;
    int32_t burst;
    int32_t range;
    int32_t reload;
    int32_t rounds;
    int32_t price;
    int32_t men;
    int32_t deploy;
    int32_t soft;
    int32_t thirst;
    int32_t crew;
};

constexpr UnitTypeDef gun_def(const UnitTypeDef& base, const GunSpec& g) {
    UnitTypeDef d = base;
    d.name = g.name;
    d.short_name = g.short_name;
    d.model = g.model;
    d.wheeled = g.wheeled;
    d.floats = g.floats;
    d.speed = Fixed::from_raw(base.speed.raw * g.speed / 100);
    d.max_hp = g.hp;
    d.armor = g.armor;
    d.mass = base.mass * g.hp / base.max_hp;
    d.weapon.damage = g.damage;
    d.weapon.splash_radius = tiles(g.burst, 10);
    d.weapon.range = tiles(g.range);
    d.weapon.reload = seconds(g.reload, 10);
    d.rounds_capacity = g.rounds;
    d.cost[static_cast<size_t>(Resource::Personnel)] = g.men;
    for (size_t r = static_cast<size_t>(Resource::Materials); r < kResourceCount; ++r) d.cost[r] = base.cost[r] * g.price / 100;
    d.train_time = base.train_time * (50 + g.price / 2) / 100;
    d.deploy_time = seconds(g.deploy, 10);
    d.soft_ground_percent = 100 * 100 / g.soft;
    if (base.fuel_capacity.raw > 0) d.fuel_capacity = Fixed::from_raw(base.fuel_capacity.raw * 100 / g.thirst);
    d.crew_survives_percent = g.crew;
    return d;
}

//   name                short   model                    wheels floats spd  hp   armor         dmg burst range reload rounds price men deploy soft thirst crew
constexpr GunSpec kSpgSpecs[] = {
    // The Authoritarian axis (and the Democratic one: the 2S3 both).
    {"2S3 Akatsiya",      "2S3",  VehicleModel::Akatsiya, false, false, 100, 240, {18, 18, 22}, 150, 18, 63, 75, 46, 140, 4, 30, 110, 110, 25},
    {"2S19 Msta-S",       "MSTA", VehicleModel::MstaS,    false, false, 100, 300, {20, 20, 25}, 150, 18, 72, 50, 50, 200, 5, 30, 100, 130, 30},
    {"2S7 Pion",          "PION", VehicleModel::Pion,     false, false,  90, 260, {10, 10, 10}, 230, 24, 87, 120, 8, 250, 6, 60,  95, 140, 40},
    {"PLZ-05",            "PLZ",  VehicleModel::Plz05,    false, false, 105, 300, {20, 20, 25}, 150, 18, 88, 50, 30, 210, 5, 30, 100, 120, 30},
    // The Democratic axis.
    {"M109A6 Paladin",    "M109", VehicleModel::M109,     false, false, 100, 280, {18, 18, 22}, 150, 18, 71, 70, 39, 180, 4, 20, 100, 110, 45},
    {"PzH 2000",          "PZH",  VehicleModel::PzH2000,  false, false, 110, 320, {22, 22, 28}, 150, 18, 90, 30, 60, 260, 5, 20,  95, 130, 50},
    {"CAESAR",            "CSR",  VehicleModel::Caesar,   true,  false, 140, 160, {8, 8, 5},    150, 18, 92, 60, 18, 170, 5, 10,  75,  70, 50},
    {"K9 Thunder",        "K9",   VehicleModel::K9,       false, false, 115, 300, {20, 20, 25}, 150, 18, 90, 40, 48, 230, 5, 20, 105, 115, 45},
};

//   name                short   model                    wheels floats spd  hp   armor         dmg burst range reload rounds price men deploy soft thirst crew
constexpr GunSpec kGunSpecs[] = {
    {"2A65 Msta-B",       "MSTB", VehicleModel::MstaB,    true,  false,  85, 190, {8, 10, 10},  150, 18, 72, 70, 30, 150, 5, 100, 100, 100, 60},
    {"2A36 Giatsint-B",   "GIA",  VehicleModel::Giatsint, true,  false,  80, 200, {8, 10, 10},  150, 18, 87, 80, 30, 170, 5, 120, 100, 100, 60},
    {"M777",              "M777", VehicleModel::M777,     true,  false, 100, 150, {8, 10, 10},  150, 18, 71, 60, 30, 170, 5,  60, 100, 100, 60},
    {"FH70",              "FH70", VehicleModel::Fh70,     true,  false,  90, 180, {8, 10, 10},  150, 18, 71, 60, 30, 150, 5,  90, 100, 100, 60},
};

// A real self-propelled AA gun, from the Shilka: its speed, hit points and
// armor, its guns (damage a round, reach, reload in hundredths of a second,
// accuracy), rounds aboard, price, soft ground, thirst, how its crew fares.
struct AaSpec {
    const char* name;
    const char* short_name;
    VehicleModel model;
    bool wheeled;
    int32_t speed;
    int32_t hp;
    std::array<int32_t, kDamageTypeCount> armor;
    int32_t damage;
    int32_t range;
    int32_t reload;
    uint8_t accuracy;
    int32_t rounds;
    int32_t price;
    int32_t soft;
    int32_t thirst;
    int32_t crew;
};

constexpr UnitTypeDef aa_def(const AaSpec& a) {
    UnitTypeDef d = kShilka;
    d.name = a.name;
    d.short_name = a.short_name;
    d.model = a.model;
    d.wheeled = a.wheeled;
    d.speed = Fixed::from_raw(kShilka.speed.raw * a.speed / 100);
    d.max_hp = a.hp;
    d.armor = a.armor;
    d.mass = kShilka.mass * a.hp / kShilka.max_hp;
    d.weapon.damage = a.damage;
    d.weapon.range = tiles(a.range);
    d.weapon.reload = seconds(a.reload, 100);
    d.weapon.accuracy = a.accuracy;
    d.rounds_capacity = a.rounds;
    for (size_t r = static_cast<size_t>(Resource::Materials); r < kResourceCount; ++r) d.cost[r] = kShilka.cost[r] * a.price / 100;
    d.train_time = kShilka.train_time * (50 + a.price / 2) / 100;
    d.soft_ground_percent = 100 * 100 / a.soft;
    d.fuel_capacity = Fixed::from_raw(kShilka.fuel_capacity.raw * 100 / a.thirst);
    d.crew_survives_percent = a.crew;
    return d;
}

//   name                    short   model                    wheels spd  hp   armor         dmg range reload acc rounds price soft thirst crew
constexpr AaSpec kAaSpecs[] = {
    {"2K22 Tunguska",        "TUN",  VehicleModel::Tunguska, false, 110, 280, {15, 12, 10}, 20, 10, 30, 60, 180, 220, 100, 110, 25},
    {"Pantsir-S1",           "PNT",  VehicleModel::Pantsir,  true,  130, 220, {8, 6, 4},    22, 12, 35, 62, 160, 260,  75,  90, 30},
    {"PGZ-09",               "PGZ",  VehicleModel::Pgz09,    false, 105, 280, {18, 14, 14}, 22,  9, 30, 60, 160, 180, 100, 110, 30},
    {"Gepard",               "GEP",  VehicleModel::Gepard,   false, 110, 280, {18, 14, 14}, 22,  9, 30, 60, 160, 180, 110, 110, 40},
    {"Type 87",              "T87",  VehicleModel::Type87,   false, 110, 270, {18, 14, 14}, 22,  9, 30, 60, 160, 190, 105, 110, 40},
    {"K30 Biho",             "K30",  VehicleModel::K30,      false, 115, 240, {15, 12, 10}, 18,  8, 25, 58, 200, 160, 110, 100, 40},
};

// The other attack aircraft: the A-10C, low and slow and tough, many
// rockets; the Su-34, fast, a few heavy bombs.
constexpr UnitTypeDef su25_like(const char* name, const char* short_name, VehicleModel model, int32_t hp,
                                std::array<int32_t, kDamageTypeCount> armor, int32_t speed, WeaponDef weapon, int32_t rounds,
                                int32_t price, int32_t fuel, int32_t crew) {
    UnitTypeDef d = kSu25;
    d.name = name;
    d.short_name = short_name;
    d.model = model;
    d.max_hp = hp;
    d.armor = armor;
    d.speed = Fixed::from_raw(kSu25.speed.raw * speed / 100);
    d.weapon = weapon;
    d.rounds_capacity = rounds;
    for (size_t r = static_cast<size_t>(Resource::Materials); r < kResourceCount; ++r) d.cost[r] = kSu25.cost[r] * price / 100;
    d.train_time = kSu25.train_time * (50 + price / 2) / 100;
    d.fuel_capacity = Fixed::from_raw(static_cast<int32_t>(static_cast<int64_t>(kSu25.fuel_capacity.raw) * fuel / 100));
    d.crew_survives_percent = crew;
    return d;
}
constexpr UnitTypeDef kSu34 =
    su25_like("Su-34", "SU34", VehicleModel::Su34, 380, {5, 15, 15}, 130,
              {.name = "FAB-500 bombs", .damage = 220, .damage_type = DamageType::Explosive, .range = tiles(9), .reload = 0,
               .projectile_speed = tiles_per_second(8), .splash_radius = tiles(12, 5), .accuracy = 100, .miss_spread = tiles(0),
               .aerial_bomb = true},
              6, 170, 150, 50);
constexpr UnitTypeDef kA10 =
    su25_like("A-10C Thunderbolt II", "A10", VehicleModel::A10, 420, {8, 20, 20}, 90,
              {.name = "Hydra 70 rockets", .damage = 60, .damage_type = DamageType::Explosive, .range = tiles(9), .reload = 0,
               .projectile_speed = tiles_per_second(25), .splash_radius = tiles(6, 5), .accuracy = 100, .miss_spread = tiles(0)},
              19, 150, 125, 70);

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
        .rounds_capacity = 120,
        .rounds_per_supply = 30,
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
        .rounds_capacity = 500,
        .rounds_per_supply = 50,
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
        // Twice a rifleman's reach; two hits knock a tank out, one in the side or the rear.
        .weapon = {.name = "RPG-7", .damage = 200, .damage_type = DamageType::AntiTank,
                   .range = tiles(10), .reload = seconds(5), .projectile_speed = tiles_per_second(6),
                   .splash_radius = kNoSplash, .accuracy = 85, .miss_spread = tiles(3, 2),
                   .structure_damage = 80},
        .cost = {1, 25, 0, 60, 0},
        .train_time = seconds(15),
        .rounds_capacity = 6,
    },
    kT72B3,
    kBmp2,
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
        .rounds_capacity = 40,
        .rounds_per_supply = 20,
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
        .rounds_capacity = 60,
        .rounds_per_supply = 30,
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
        .rounds_capacity = 150,
        .rounds_per_supply = 30,
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
                   .range = tiles(20), .reload = seconds(3), .projectile_speed = tiles_per_second(7),
                   .splash_radius = tiles(1), .accuracy = 100, .miss_spread = tiles(0), .indirect = true,
                   .min_range = tiles(3)},
        .cost = {2, 50, 0, 40, 0},
        .train_time = seconds(20),
        .rounds_capacity = 40,
        .abilities = {AbilityId::Deploy, AbilityId::DigGunPit, AbilityId::RadioSilence, AbilityId::CallSupply},
        .ability_count = 4,
        .deploy_time = seconds(3),
        .emitter = true,
    },
    kD30,
    {
        // Grenades lobbed by the belt: over parapets, into trenches.
        .name = "AGS-17 crew",
        .short_name = "AGS",
        .max_hp = 45,
        .armor = {0, 0, 45},
        .speed = tiles_per_second(4, 5),
        .radius = tiles(1, 4),
        .sight = tiles(7),
        .mass = 2,
        .vehicle = false,
        .weapon = {.name = "30mm grenade", .damage = 20, .damage_type = DamageType::Explosive, .range = tiles(8),
                   .reload = seconds(1, 2), .projectile_speed = tiles_per_second(9), .splash_radius = tiles(4, 5),
                   .accuracy = 60, .miss_spread = tiles(3, 2), .lobbed = true},
        .cost = {2, 50, 0, 50, 0},
        .train_time = seconds(20),
        .rounds_capacity = 150,
        .rounds_per_supply = 5,
        .abilities = {AbilityId::RapidFire},
        .ability_count = 1,
    },
    {
        // Forty rockets in twenty seconds: an area, not a target.
        .name = "MLRS BM-21",
        .short_name = "MLRS",
        .max_hp = 140,
        .armor = {5, 0, 10},
        .speed = tiles_per_second(8, 5),
        .radius = tiles(2, 5),
        .sight = tiles(5),
        .mass = 12,
        .vehicle = true,
        .wheeled = true,
        .weapon = {.name = "122mm rocket", .damage = 55, .damage_type = DamageType::Explosive, .range = tiles(70),
                   .reload = seconds(1), .projectile_speed = tiles_per_second(12), .splash_radius = tiles(6, 5),
                   .accuracy = 100, .miss_spread = tiles(0), .indirect = true, .min_range = tiles(8)},
        .cost = {3, 0, 180, 80, 40},
        .train_time = seconds(45),
        .rounds_capacity = 40,
        .abilities = {AbilityId::Deploy, AbilityId::Salvo, AbilityId::RadioSilence, AbilityId::CallSupply},
        .ability_count = 4,
        .deploy_time = seconds(5),
        .emitter = true,
    },
    {
        // Mines, wire, hedgehogs, pillboxes and charges; finds the enemy's mines.
        .name = "Sapper",
        .short_name = "SAP",
        .max_hp = 40,
        .armor = {0, 0, 45},
        .speed = tiles_per_second(1),
        .radius = tiles(1, 5),
        .sight = tiles(7),
        .mass = 1,
        .vehicle = false,
        .weapon = {.name = "Carbine", .damage = 5, .damage_type = DamageType::Bullet, .range = tiles(4),
                   .reload = seconds(5, 4), .projectile_speed = kInstantHit, .splash_radius = kNoSplash,
                   .accuracy = 60, .miss_spread = tiles(1)},
        .cost = {1, 25, 20, 20, 0},
        .train_time = seconds(18),
        .engineer = true,
        .rounds_capacity = 60,
        .rounds_per_supply = 30,
        .abilities = {AbilityId::LayApMine, AbilityId::LayAtMine, AbilityId::ClearMines, AbilityId::LayWire,
                      AbilityId::PlaceHedgehogs, AbilityId::BuildPillbox, AbilityId::Demolish},
        .ability_count = 7,
    },
    kGvozdika,
    {
        // A radio on his back: silent units near him still get their orders at once.
        .name = "Signaller",
        .short_name = "SIG",
        .max_hp = 35,
        .armor = {0, 0, 45},
        .speed = tiles_per_second(1),
        .radius = tiles(1, 5),
        .sight = tiles(7),
        .mass = 1,
        .vehicle = false,
        .weapon = {.name = "Carbine", .damage = 5, .damage_type = DamageType::Bullet, .range = tiles(4),
                   .reload = seconds(5, 4), .projectile_speed = kInstantHit, .splash_radius = kNoSplash,
                   .accuracy = 60, .miss_spread = tiles(1)},
        .cost = {1, 25, 10, 10, 0},
        .train_time = seconds(15),
        .rounds_capacity = 60,
        .rounds_per_supply = 30,
        .abilities = {AbilityId::RadioSilence},
        .ability_count = 1,
        .emitter = true,
        .relay_range = tiles(6),
    },
    {
        // A command vehicle bristling with antennas: a relay on wheels, and a
        // loud one on the air.
        .name = "Command vehicle",
        .short_name = "CMD",
        .max_hp = 180,
        .armor = {8, 5, 5},
        .speed = tiles_per_second(7, 5),
        .radius = tiles(2, 5),
        .sight = tiles(7),
        .mass = 14,
        .vehicle = true,
        .wheeled = true,
        .weapon = {.name = "Unarmed", .damage = 0, .damage_type = DamageType::Bullet, .range = tiles(0),
                   .reload = seconds(1), .projectile_speed = kInstantHit, .splash_radius = kNoSplash,
                   .accuracy = 0, .miss_spread = tiles(0)},
        .cost = {2, 0, 120, 0, 40},
        .train_time = seconds(30),
        .fuel_capacity = tiles(200),
        .abilities = {AbilityId::RadioSilence, AbilityId::CallSupply},
        .ability_count = 2,
        .emitter = true,
        .relay_range = tiles(12),
    },
    {
        // Listens and never talks: a bearing on every radio in reach. Two
        // stations far enough apart cross their bearings into a fix.
        .name = "DF station",
        .short_name = "DF",
        .max_hp = 100,
        .armor = {3, 0, 0},
        .speed = tiles_per_second(8, 5),
        .radius = tiles(7, 20),
        .sight = tiles(6),
        .mass = 10,
        .vehicle = true,
        .wheeled = true,
        .weapon = {.name = "Unarmed", .damage = 0, .damage_type = DamageType::Bullet, .range = tiles(0),
                   .reload = seconds(1), .projectile_speed = kInstantHit, .splash_radius = kNoSplash,
                   .accuracy = 0, .miss_spread = tiles(0)},
        .cost = {2, 0, 120, 20, 30},
        .train_time = seconds(30),
        .abilities = {AbilityId::Deploy},
        .ability_count = 1,
        .deploy_time = seconds(5),
        .df_range = tiles(35),
    },
    kSu25,
    {
        // A missile on the shoulder: deadly to aircraft, useless on the ground.
        .name = "MANPADS crew (Igla)",
        .short_name = "AA",
        .max_hp = 40,
        .armor = {0, 0, 45},
        .speed = tiles_per_second(1),
        .radius = tiles(1, 5),
        .sight = tiles(7),
        .mass = 1,
        .vehicle = false,
        .weapon = {.name = "Igla missile", .damage = 160, .damage_type = DamageType::Explosive, .range = tiles(10),
                   .reload = seconds(10), .projectile_speed = tiles_per_second(20), .splash_radius = kNoSplash,
                   .accuracy = 50, .miss_spread = tiles(0), .anti_air = true, .air_only = true},
        .cost = {1, 25, 0, 60, 0},
        .train_time = seconds(20),
        .rounds_capacity = 4,
    },
    kShilka,
    {
        // Sees aircraft far out for the whole air defence, and is heard as
        // far: the loudest thing on the air.
        .name = "Air defence radar",
        .short_name = "RDR",
        .max_hp = 80,
        .armor = {3, 0, 0},
        .speed = tiles_per_second(7, 5),
        .radius = tiles(2, 5),
        .sight = tiles(6),
        .mass = 10,
        .vehicle = true,
        .wheeled = true,
        .weapon = {.name = "Unarmed", .damage = 0, .damage_type = DamageType::Bullet, .range = tiles(0),
                   .reload = seconds(1), .projectile_speed = kInstantHit, .splash_radius = kNoSplash,
                   .accuracy = 0, .miss_spread = tiles(0)},
        .cost = {2, 0, 150, 0, 40},
        .train_time = seconds(30),
        .abilities = {AbilityId::Deploy, AbilityId::RadioSilence},
        .ability_count = 2,
        .deploy_time = seconds(5),
        .emitter = true,
        .radar_range = tiles(45),
    },
    tank_def(kTankSpecs[0]),
    tank_def(kTankSpecs[1]),
    tank_def(kTankSpecs[2]),
    tank_def(kTankSpecs[3]),
    tank_def(kTankSpecs[4]),
    tank_def(kTankSpecs[5]),
    tank_def(kTankSpecs[6]),
    tank_def(kTankSpecs[7]),
    tank_def(kTankSpecs[8]),
    tank_def(kTankSpecs[9]),
    tank_def(kTankSpecs[10]),
    tank_def(kTankSpecs[11]),
    tank_def(kTankSpecs[12]),
    apc_def(kApcSpecs[0]),
    apc_def(kApcSpecs[1]),
    apc_def(kApcSpecs[2]),
    apc_def(kApcSpecs[3]),
    apc_def(kApcSpecs[4]),
    apc_def(kApcSpecs[5]),
    apc_def(kApcSpecs[6]),
    apc_def(kApcSpecs[7]),
    apc_def(kApcSpecs[8]),
    apc_def(kApcSpecs[9]),
    apc_def(kApcSpecs[10]),
    apc_def(kApcSpecs[11]),
    apc_def(kApcSpecs[12]),
    apc_def(kApcSpecs[13]),
    apc_def(kApcSpecs[14]),
    gun_def(kGvozdika, kSpgSpecs[0]),
    gun_def(kGvozdika, kSpgSpecs[1]),
    gun_def(kGvozdika, kSpgSpecs[2]),
    gun_def(kGvozdika, kSpgSpecs[3]),
    gun_def(kGvozdika, kSpgSpecs[4]),
    gun_def(kGvozdika, kSpgSpecs[5]),
    gun_def(kGvozdika, kSpgSpecs[6]),
    gun_def(kGvozdika, kSpgSpecs[7]),
    aa_def(kAaSpecs[0]),
    aa_def(kAaSpecs[1]),
    aa_def(kAaSpecs[2]),
    aa_def(kAaSpecs[3]),
    aa_def(kAaSpecs[4]),
    aa_def(kAaSpecs[5]),
    gun_def(kD30, kGunSpecs[0]),
    gun_def(kD30, kGunSpecs[1]),
    gun_def(kD30, kGunSpecs[2]),
    gun_def(kD30, kGunSpecs[3]),
    kSu34,
    kA10,
};
static_assert(std::size(kUnitTypes) == kUnitTypeCount);

}  // namespace

const UnitTypeDef& unit_type(UnitTypeId id) { return kUnitTypes[static_cast<size_t>(id)]; }

namespace {

constexpr AbilityDef kAbilities[] = {
    {.name = "Area shot: one HE-FRAG shell with a wide burst", .label = "Area shot", .target = AbilityTarget::Point,
     .range = tiles(50), .cooldown = seconds(20),
     .weapon = {.name = "125mm HE-FRAG shell", .damage = 75, .damage_type = DamageType::Explosive,
                .range = tiles(50), .reload = seconds(4), .projectile_speed = tiles_per_second(30),
                .splash_radius = tiles(5, 4), .accuracy = 85, .miss_spread = tiles(1),
                .effective_range = tiles(15)}},
    {.name = "Switch rounds (reloads the gun): a tank's HE / AP, a BMP-1's HE / HEAT, the 30 mm / the 100 mm", .label = "HE / AP",
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
    {.name = "Dig in the gun: cover and concealment where it stands", .label = "Gun pit",
     .target = AbilityTarget::Instant, .range = tiles(0), .cooldown = 0},
    {.name = "Camouflage the gun: hidden until it moves (a shot still gives it away for a while)",
     .label = "Camouflage", .target = AbilityTarget::Instant, .range = tiles(0), .cooldown = 0},
    {.name = "Rapid fire: five grenades in a row along the front", .label = "Rapid fire",
     .target = AbilityTarget::Point, .range = tiles(8), .cooldown = seconds(15),
     .weapon = {.name = "30mm grenade", .damage = 20, .damage_type = DamageType::Explosive, .range = tiles(8),
                .reload = 0, .projectile_speed = tiles_per_second(9), .splash_radius = tiles(4, 5), .accuracy = 100,
                .miss_spread = tiles(0), .lobbed = true}},
    {.name = "Salvo: every rocket in the launcher at a point", .label = "Salvo", .target = AbilityTarget::Point,
     .range = tiles(70), .cooldown = 0},
    {.name = "Fire from a covered position, like artillery: each shot wears the barrel (1% HP)",
     .label = "Indirect", .target = AbilityTarget::Point, .range = tiles(60), .cooldown = 0,
     .weapon = {.name = "125mm HE shell, indirect", .damage = 75, .damage_type = DamageType::Explosive,
                .range = tiles(60), .reload = seconds(4), .projectile_speed = tiles_per_second(12),
                .splash_radius = tiles(1, 2), .accuracy = 100, .miss_spread = tiles(0), .indirect = true,
                .min_range = tiles(3)}},
    {.name = "Anti-personnel mine (5 ammunition)", .label = "AP mine", .target = AbilityTarget::Point,
     .range = tiles(0), .cooldown = 0},
    {.name = "Anti-tank mine (10 ammunition)", .label = "AT mine", .target = AbilityTarget::Point,
     .range = tiles(0), .cooldown = 0},
    {.name = "Clear the enemy mines found around a point", .label = "Clear", .target = AbilityTarget::Point,
     .range = tiles(0), .cooldown = 0},
    {.name = "Barbed wire along a line (5 materials a tile)", .label = "Wire", .target = AbilityTarget::Line,
     .range = tiles(0), .cooldown = 0},
    {.name = "Anti-tank hedgehogs along a line (10 materials a tile)", .label = "Hedgehogs",
     .target = AbilityTarget::Line, .range = tiles(0), .cooldown = 0},
    {.name = "Pillbox here, its slit facing a direction (80 materials)", .label = "Pillbox",
     .target = AbilityTarget::Direction, .range = tiles(0), .cooldown = 0},
    {.name = "Demolition charge against a building or a bridge", .label = "Charge", .target = AbilityTarget::Point,
     .range = tiles(0), .cooldown = 0},
    {.name = "Smoke screen ahead: nothing is seen through it (needs smoke grenades)", .label = "Smoke",
     .target = AbilityTarget::Instant, .range = tiles(0), .cooldown = seconds(30), .needs = UpgradeId::SmokeGrenades},
    {.name = "Radio silence: enemy direction finders lose it, but orders come by courier (3 s) away from a relay",
     .label = "Radio off", .target = AbilityTarget::Instant, .range = tiles(0), .cooldown = 0},
    {.name = "Call supply by radio: the nearest free tanker / ammunition truck comes over to top it up",
     .label = "Supply", .target = AbilityTarget::Instant, .range = tiles(0), .cooldown = seconds(5)},
    // Wire-guided: slow, but it flies after the vehicle it's aimed at.
    {.name = "ATGM: a guided missile at an enemy vehicle, 25 tiles (needs the launchers)",
     .label = "ATGM", .target = AbilityTarget::Point, .range = tiles(25), .cooldown = seconds(10),
     .weapon = {.name = "ATGM", .damage = 200, .damage_type = DamageType::AntiTank, .range = tiles(25),
                .reload = seconds(4), .projectile_speed = tiles_per_second(5), .splash_radius = kNoSplash,
                .accuracy = 90, .miss_spread = tiles(2), .structure_damage = 80, .guided = true},
     .needs = UpgradeId::Atgm},
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
     .roster = {UnitTypeId::Tank, UnitTypeId::Ifv}, .roster_size = 2},  // see roster_of: each axis its own tanks
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
    {.name = "Trench", .max_hp = 200, .armor = {0, 10, 60}, .cache_capacity = 40},
    {.name = "Foxhole", .max_hp = 150, .armor = {0, 10, 60}, .cache_capacity = 20},
    {.name = "Parapet", .max_hp = 150, .armor = {0, 10, 60}},
    // Logs and earth: holds against mortars and light shells, not against heavy ones.
    {.name = "Dugout", .max_hp = 1200, .armor = {0, 30, 80}, .capacity = 8, .cache_capacity = 150},
    {.name = "Artillery barracks", .max_hp = 1800, .armor = {0, 15, 60},
     .buildable = true, .width = 3, .height = 3, .cost = {0, 0, 200, 0, 50}, .build_time = seconds(40),
     .roster = {UnitTypeId::Mortar, UnitTypeId::Ags, UnitTypeId::Howitzer, UnitTypeId::Spg, UnitTypeId::Mlrs},
     .roster_size = 5},
    {.name = "Gun pit", .max_hp = 250, .armor = {0, 10, 60}, .cache_capacity = 80},
    {.name = "Engineer barracks", .max_hp = 1000, .armor = {0, 10, 60},
     .buildable = true, .width = 2, .height = 2, .cost = {0, 0, 120, 0, 0}, .build_time = seconds(25),
     .roster = {UnitTypeId::Sapper}, .roster_size = 1},
    // Obstacles: a shell or a tank's tracks make short work of wire.
    {.name = "Barbed wire", .max_hp = 60, .armor = {0, 0, 60}},
    {.name = "Hedgehogs", .max_hp = 400, .armor = {0, 30, 80}},
    // Logs and earth with a slit facing the enemy: bullets don't get in.
    {.name = "Pillbox", .max_hp = 1500, .armor = {0, 40, 60}, .capacity = 3, .width = 1, .height = 1,
     .cost = {0, 0, 80, 0, 0}, .build_time = seconds(30), .cache_capacity = 100},
    {.name = "Signals barracks", .max_hp = 1000, .armor = {0, 10, 60},
     .buildable = true, .width = 2, .height = 2, .cost = {0, 0, 120, 0, 0}, .build_time = seconds(25),
     .roster = {UnitTypeId::Signaler, UnitTypeId::FieldHq, UnitTypeId::DfStation}, .roster_size = 3},
    {.name = "Air defence barracks", .max_hp = 1200, .armor = {0, 10, 60},
     .buildable = true, .width = 2, .height = 2, .cost = {0, 0, 150, 0, 0}, .build_time = seconds(30),
     .roster = {UnitTypeId::Manpads, UnitTypeId::Shilka, UnitTypeId::AirRadar}, .roster_size = 3},
    // A runway with a tower: the aircraft park on it. Flat: it hides nothing.
    {.name = "Airfield", .max_hp = 2500, .armor = {0, 10, 60},
     .buildable = true, .width = 6, .height = 3, .cost = {0, 0, 300, 0, 100}, .build_time = seconds(60),
     .roster = {UnitTypeId::Su25}, .roster_size = 1},
    // Concrete panels: it takes a lot to bring down, and holds a company.
    {.name = "Apartment block", .max_hp = 4000, .armor = {0, 40, 90}, .capacity = 12},
    // A lattice mast: a few shells bring it down, and the spotter with it.
    {.name = "Cell tower", .max_hp = 300, .armor = {0, 5, 20}, .capacity = 1},
    // A shop and the pumps under a canopy; with fuel in the tanks, it goes up in flames.
    {.name = "Gas station", .max_hp = 500, .armor = {0, 5, 30}, .capacity = 3},
    // Reinforced concrete silos: very hard to bring down.
    {.name = "Grain elevator", .max_hp = 5000, .armor = {0, 50, 90}, .capacity = 6},
    // A long hut of bunks: cheap, quick, and every one of them counts.
    {.name = "Living quarters", .max_hp = 700, .armor = {0, 10, 50},
     .buildable = true, .width = 2, .height = 2, .cost = {0, 0, 50, 0, 0}, .build_time = seconds(20)},
    // Sheds, a pit, a crane: armor and wheels alike are fixed here.
    {.name = "Workshop", .max_hp = 1500, .armor = {0, 10, 60},
     .buildable = true, .width = 3, .height = 3, .cost = {0, 0, 150, 0, 0}, .build_time = seconds(30)},
    // Ten beds; the red cross on the roof.
    {.name = "Field hospital", .max_hp = 800, .armor = {0, 10, 50}, .capacity = 10,
     .buildable = true, .width = 2, .height = 2, .cost = {0, 50, 100, 0, 0}, .build_time = seconds(25)},
};
static_assert(std::size(kStructureTypes) == static_cast<size_t>(StructureType::Count));

}  // namespace

const StructureDef& structure_type(StructureType type) { return kStructureTypes[static_cast<size_t>(type)]; }

std::span<const UnitTypeId> roster_of(StructureType building, Axis axis) {
    // The armor barracks: each axis's own tanks, IFVs and APCs (the BMP-2 both have).
    static constexpr UnitTypeId kDemocratic[] = {UnitTypeId::T64BV,      UnitTypeId::T64BM,   UnitTypeId::Leopard1A5,
                                                 UnitTypeId::Leopard2A6, UnitTypeId::M1A1,    UnitTypeId::Type10,
                                                 UnitTypeId::K2,         UnitTypeId::Merkava4, UnitTypeId::Ifv,
                                                 UnitTypeId::Btr4e,      UnitTypeId::M113,    UnitTypeId::Bradley,
                                                 UnitTypeId::Marder,     UnitTypeId::Stryker, UnitTypeId::Type89,
                                                 UnitTypeId::K21,        UnitTypeId::Namer};
    static constexpr UnitTypeId kAuthoritarian[] = {UnitTypeId::T62M,   UnitTypeId::Tank,    UnitTypeId::T80BVM,
                                                    UnitTypeId::T90M,   UnitTypeId::Type99A, UnitTypeId::Karrar,
                                                    UnitTypeId::Bmp1,   UnitTypeId::Ifv,     UnitTypeId::Bmp3,
                                                    UnitTypeId::Btr82a, UnitTypeId::Mtlb,    UnitTypeId::Zbd04a,
                                                    UnitTypeId::Ratel20, UnitTypeId::Boragh};
    if (building == StructureType::ArmorBarracks) {
        return axis == Axis::Democratic ? std::span<const UnitTypeId>(kDemocratic) : std::span<const UnitTypeId>(kAuthoritarian);
    }
    // The artillery barracks: mortars, AGS, the towed howitzers, the SPGs, the rocket launchers.
    static constexpr UnitTypeId kDemocraticGuns[] = {UnitTypeId::Mortar, UnitTypeId::Ags,     UnitTypeId::Howitzer, UnitTypeId::M777,
                                                     UnitTypeId::Fh70,   UnitTypeId::Spg,     UnitTypeId::Akatsiya, UnitTypeId::M109,
                                                     UnitTypeId::PzH2000, UnitTypeId::Caesar, UnitTypeId::K9,       UnitTypeId::Mlrs};
    static constexpr UnitTypeId kAuthoritarianGuns[] = {UnitTypeId::Mortar, UnitTypeId::Ags,   UnitTypeId::Howitzer, UnitTypeId::MstaB,
                                                        UnitTypeId::Giatsint, UnitTypeId::Spg, UnitTypeId::Akatsiya, UnitTypeId::MstaS,
                                                        UnitTypeId::Pion,   UnitTypeId::Plz05, UnitTypeId::Mlrs};
    if (building == StructureType::ArtilleryBarracks) {
        return axis == Axis::Democratic ? std::span<const UnitTypeId>(kDemocraticGuns) : std::span<const UnitTypeId>(kAuthoritarianGuns);
    }
    // The air defence barracks: MANPADS, the axis's AA guns, the radar.
    static constexpr UnitTypeId kDemocraticAa[] = {UnitTypeId::Manpads, UnitTypeId::Gepard, UnitTypeId::Type87, UnitTypeId::K30,
                                                   UnitTypeId::AirRadar};
    static constexpr UnitTypeId kAuthoritarianAa[] = {UnitTypeId::Manpads, UnitTypeId::Shilka, UnitTypeId::Tunguska, UnitTypeId::Pantsir,
                                                      UnitTypeId::Pgz09,   UnitTypeId::AirRadar};
    if (building == StructureType::AirDefenseBarracks) {
        return axis == Axis::Democratic ? std::span<const UnitTypeId>(kDemocraticAa) : std::span<const UnitTypeId>(kAuthoritarianAa);
    }
    // The airfield: the Su-25 both, and each axis's own.
    static constexpr UnitTypeId kDemocraticAir[] = {UnitTypeId::Su25, UnitTypeId::A10};
    static constexpr UnitTypeId kAuthoritarianAir[] = {UnitTypeId::Su25, UnitTypeId::Su34};
    if (building == StructureType::Airfield) {
        return axis == Axis::Democratic ? std::span<const UnitTypeId>(kDemocraticAir) : std::span<const UnitTypeId>(kAuthoritarianAir);
    }
    const StructureDef& def = structure_type(building);
    return {def.roster.data(), def.roster_size};
}

namespace {

constexpr UpgradeDef kUpgrades[] = {
    {.name = "Smoke grenades", .label = "Smoke", .description = "Tanks can lay a smoke screen: nothing is seen through it",
     .building = StructureType::AmmoDepot, .cost = {0, 0, 50, 60, 0}, .time = seconds(45)},
    {.name = "Sabot rounds", .label = "Sabot", .description = "Tank armor-piercing rounds hit 30% harder",
     .building = StructureType::AmmoDepot, .cost = {0, 0, 80, 100, 0}, .time = seconds(60)},
    {.name = "Cluster munitions", .label = "Cluster",
     .description = "Cluster shells and rockets: bomblets over a 4x4 area, against men and vehicles in the open",
     .building = StructureType::ArtilleryBarracks, .cost = {0, 0, 60, 120, 0}, .time = seconds(60)},
    {.name = "More trains", .label = "Schedule", .description = "A train every 45 s instead of every 60 s",
     .building = StructureType::Station, .cost = {0, 200, 150, 0, 50}, .time = seconds(90)},
    {.name = "Heavier trains", .label = "Capacity", .description = "Trains bring 50% more",
     .building = StructureType::Station, .cost = {0, 200, 200, 0, 50}, .time = seconds(90)},
    {.name = "Optics", .label = "Optics",
     .description = "Scouts make out men in cover 2 tiles farther; posts watch 3 tiles farther",
     .building = StructureType::ReconBarracks, .cost = {0, 0, 60, 40, 0}, .time = seconds(45)},
    {.name = "Entrenching tools", .label = "Shovels", .description = "Riflemen dig a third faster",
     .building = StructureType::InfantryBarracks, .cost = {0, 0, 60, 0, 0}, .time = seconds(30)},
    {.name = "Reactive armor Kontakt-1", .label = "K-1",
     .description = "Boxes of explosive on the turret and the glacis: tanks take 35% less from RPGs and missiles "
                    "(AP rounds go through it)",
     .building = StructureType::ArmorBarracks, .cost = {0, 0, 100, 60, 0}, .time = seconds(45)},
    {.name = "Fire control system", .label = "FCS",
     .description = "Tank guns keep their aim far out: 65% of it at 50 tiles instead of 35%",
     .building = StructureType::ArmorBarracks, .cost = {0, 0, 120, 60, 0}, .time = seconds(60)},
    {.name = "ATGM launchers", .label = "ATGM",
     .description = "IFVs fire a guided anti-tank missile, 25 tiles (Konkurs, Malyutka, Arkan, Barrier, TOW, Milan, "
                    "Jyu-MAT, HJ-73; not the BTR-82A, MT-LB, Ratel, Boragh, M113, Stryker, K21, Namer); 4 aboard, "
                    "ammunition trucks bring more",
     .building = StructureType::ArmorBarracks, .cost = {0, 0, 100, 150, 0}, .time = seconds(60)},
    {.name = "Firing tables", .label = "Tables",
     .description = "Ranging in goes faster: 30/70/95% on target instead of 17/50/95%",
     .building = StructureType::ArtilleryBarracks, .cost = {0, 0, 80, 40, 0}, .time = seconds(45)},
    {.name = "Drilled crews", .label = "Drill", .description = "Guns set up and pack up twice as fast",
     .building = StructureType::ArtilleryBarracks, .cost = {0, 100, 60, 0, 0}, .time = seconds(45)},
    {.name = "Long-range charges", .label = "Charges",
     .description = "Howitzers, SPGs and mortars reach 25% farther",
     .building = StructureType::ArtilleryBarracks, .cost = {0, 0, 100, 150, 0}, .time = seconds(60)},
    {.name = "Body armor", .label = "Armor", .description = "Foot soldiers take 25% less from bullets and fragments",
     .building = StructureType::InfantryBarracks, .cost = {0, 0, 120, 0, 0}, .time = seconds(50)},
    {.name = "Load-bearing vests", .label = "Vests", .description = "Foot soldiers carry 50% more rounds",
     .building = StructureType::InfantryBarracks, .cost = {0, 50, 80, 0, 0}, .time = seconds(40)},
    {.name = "Ghillie suits", .label = "Ghillie",
     .description = "Scouts in cover are made out only from half as close",
     .building = StructureType::ReconBarracks, .cost = {0, 0, 50, 0, 0}, .time = seconds(30)},
    {.name = "Heavier charges", .label = "Charges", .description = "Mines and demolition charges hit 50% harder",
     .building = StructureType::EngineerBarracks, .cost = {0, 0, 60, 80, 0}, .time = seconds(45)},
    {.name = "Prefab pillboxes", .label = "Prefab", .description = "Pillboxes go up twice as fast",
     .building = StructureType::EngineerBarracks, .cost = {0, 0, 100, 0, 0}, .time = seconds(45)},
    {.name = "Secure radios (ZAS)", .label = "ZAS",
     .description = "Enemy direction finders need three bearings, not two, to fix our radios",
     .building = StructureType::SignalsBarracks, .cost = {0, 0, 100, 0, 0}, .time = seconds(60)},
    {.name = "Mast antennas", .label = "Masts",
     .description = "The headquarters and command vehicles relay orders 50% farther",
     .building = StructureType::SignalsBarracks, .cost = {0, 0, 80, 0, 20}, .time = seconds(45)},
    {.name = "Radar tracking", .label = "Tracking",
     .description = "Self-propelled AA guns and MANPADS are 20% more accurate against aircraft",
     .building = StructureType::AirDefenseBarracks, .cost = {0, 0, 100, 50, 0}, .time = seconds(50)},
    {.name = "Cockpit armor", .label = "Armor", .description = "Attack aircraft take 30% less damage (about 40% tougher)",
     .building = StructureType::Airfield, .cost = {0, 0, 150, 0, 50}, .time = seconds(60)},
    {.name = "Tuned engines", .label = "Engine", .description = "Tanks, IFVs and APCs drive 15% faster",
     .building = StructureType::ArmorBarracks, .cost = {0, 0, 100, 0, 80}, .time = seconds(45)},
    {.name = "Add-on armor", .label = "Screens",
     .description = "Tanks, IFVs and APCs take 20% less from shells, fragments and bullets (slat cages on the IFVs "
                    "and APCs; reactive armor is for the anti-tank ones)",
     .building = StructureType::ArmorBarracks, .cost = {0, 0, 150, 0, 0}, .time = seconds(50)},
    {.name = "Loading drills", .label = "Reload", .description = "Tank, IFV and APC guns reload 25% faster",
     .building = StructureType::ArmorBarracks, .cost = {0, 0, 80, 100, 0}, .time = seconds(50)},
    {.name = "Incendiary shells", .label = "Incend.",
     .description = "Shells and rockets that set the ground on fire for 15 s: men, a garrison, buildings in it burn",
     .building = StructureType::ArtilleryBarracks, .cost = {0, 0, 60, 100, 20}, .time = seconds(60)},
    {.name = "White phosphorus", .label = "WP",
     .description = "A smoke screen for 20 s where it lands (nothing is seen through it), and it burns",
     .building = StructureType::ArtilleryBarracks, .cost = {0, 0, 60, 100, 0}, .time = seconds(60)},
    {.name = "Reactive armor Kontakt-5", .label = "K-5",
     .description = "Heavy wedges on the turret, plates on the glacis, boxes on the skirts: 45% less from RPGs and "
                    "missiles, 25% less from AP rounds",
     .building = StructureType::ArmorBarracks, .cost = {0, 0, 180, 120, 0}, .time = seconds(60),
     .needs = UpgradeId::ReactiveArmor},
    {.name = "Reactive armor Relikt", .label = "Relikt",
     .description = "The newest, against tandem charges too, the skirts covered: 55% less from RPGs and missiles, "
                    "40% less from AP rounds",
     .building = StructureType::ArmorBarracks, .cost = {0, 0, 260, 160, 0}, .time = seconds(75),
     .needs = UpgradeId::Kontakt5},
};
static_assert(std::size(kUpgrades) == kUpgradeCount);

}  // namespace

const UpgradeDef& upgrade_def(UpgradeId id) { return kUpgrades[static_cast<size_t>(id)]; }

bool is_tube_artillery(const UnitTypeDef& def) { return def.weapon.indirect && ability_slot(def, AbilityId::Salvo) < 0; }

}  // namespace engine
