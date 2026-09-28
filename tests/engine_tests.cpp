// Headless engine tests. No window, no graphics: the engine must run anywhere.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "engine/fixed.h"
#include "engine/rng.h"
#include "engine/scenario.h"
#include "engine/simulation.h"

using namespace engine;

namespace {

int g_failures = 0;

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

FixedVec2 at(int32_t x, int32_t y) { return {Fixed::from_int(x), Fixed::from_int(y)}; }

std::vector<EntityId> units_of(const World& world, PlayerId player) {
    std::vector<EntityId> ids;
    for (const Unit& u : world.units()) {
        if (u.owner == player) ids.push_back(u.id);
    }
    return ids;
}

// Same input delay a real game uses (see net::Lockstep).
void issue(Simulation& sim, Command cmd) { sim.schedule(sim.world().tick() + 2, std::move(cmd)); }

Command make_order(CommandType type, PlayerId player, std::vector<EntityId> units, int32_t x, int32_t y) {
    Command cmd;
    cmd.type = type;
    cmd.player = player;
    cmd.units = std::move(units);
    cmd.target = at(x, y);
    return cmd;
}

Command make_move(PlayerId player, std::vector<EntityId> units, int32_t x, int32_t y) {
    return make_order(CommandType::Move, player, std::move(units), x, y);
}

Command fire_at(PlayerId player, std::vector<EntityId> units, int32_t x, int32_t y);
Fixed abs_fixed(Fixed f);

int32_t hp_of(const Simulation& sim, EntityId id) {
    const Unit* u = sim.world().find_unit(id);
    return u ? u->hp : 0;
}

// Plays a fixed script of orders and records the checksum after every tick:
// both armies march to the middle and clash, then get shuffled around.
std::vector<uint64_t> play_script(uint64_t seed, int ticks) {
    Simulation sim(seed, make_demo_map());
    setup_demo_scenario(sim.world_for_setup());
    const int32_t center = sim.world().map().width() / 2;
    constexpr int kMelee = 2400;  // by now the armies have met
    // Each side's guns shell the middle of the map all along.
    std::array<std::vector<EntityId>, 2> guns;
    std::array<EntityId, 2> rockets{};
    std::array<EntityId, 2> sappers{};
    std::array<EntityId, 2> stations{};
    std::array<EntityId, 2> planes{};
    for (PlayerId p = 0; p < 2; ++p) {
        const FixedVec2 base = demo_base_position(sim.world().map().width(), p);
        const Fixed step = Fixed::from_int(p == 0 ? 8 : -8);
        guns[p].push_back(sim.world_for_setup().spawn_unit(p, UnitTypeId::Howitzer, {base.x + step, base.y - step}));
        guns[p].push_back(sim.world_for_setup().spawn_unit(p, UnitTypeId::Mortar, {base.x + step, base.y}));
        guns[p].push_back(sim.world_for_setup().spawn_unit(p, UnitTypeId::Ags, {base.x, base.y - step}));
        rockets[p] = sim.world_for_setup().spawn_unit(p, UnitTypeId::Mlrs, {base.x + step, base.y + step});
        sappers[p] = sim.world_for_setup().spawn_unit(p, UnitTypeId::Sapper, {base.x + step, base.y - step});
        // Each side listens for the other's radios; its rockets keep silence.
        stations[p] = sim.world_for_setup().spawn_unit(p, UnitTypeId::DfStation, {base.x, base.y + step});
        sim.world_for_setup().unit_for_setup(rockets[p])->silent = true;
        // An airfield behind each base with an attack aircraft on it, and air defence.
        const TilePos b = tile_of(base);
        for (int32_t r = 8; r < 40 && planes[p] == 0; ++r) {
            const TilePos origin{b.x + (p == 0 ? -r : r) - 3, b.y + (p == 0 ? r : -r) - 1};
            if (!sim.world().can_place(StructureType::Airfield, origin)) continue;
            sim.world_for_setup().place_structure(StructureType::Airfield, p, origin, 6, 3);
            planes[p] = sim.world_for_setup().spawn_unit(p, UnitTypeId::Su25, tile_center(origin));
        }
        sim.world_for_setup().spawn_unit(p, UnitTypeId::Manpads, {base.x + step, base.y});
        sim.world_for_setup().spawn_unit(p, UnitTypeId::Shilka, {base.x, base.y + step * 2});
        const EntityId radar = sim.world_for_setup().spawn_unit(p, UnitTypeId::AirRadar, {base.x - step, base.y});
        sim.world_for_setup().unit_for_setup(radar)->deployed = true;
    }

    std::vector<uint64_t> checksums;
    for (int t = 0; t < ticks; ++t) {
        if (t == 0) {
            issue(sim, make_order(CommandType::AttackMove, 0, units_of(sim.world(), 0), center, center));
            issue(sim, make_order(CommandType::AttackMove, 1, units_of(sim.world(), 1), center, center));
            for (PlayerId p = 0; p < 2; ++p) {
                Command deploy = make_order(CommandType::Ability, p, {stations[p]}, 0, 0);
                deploy.ability = static_cast<uint8_t>(AbilityId::Deploy);
                issue(sim, deploy);
            }
        }
        if (t == 30 || t == 1500) {
            for (PlayerId p = 0; p < 2; ++p) {
                if (planes[p]) issue(sim, fire_at(p, {planes[p]}, center + (p == 0 ? 6 : -6), center));
            }
        }
        if (t == 5) {
            for (PlayerId p = 0; p < 2; ++p) {
                issue(sim, make_order(CommandType::AttackGround, p, guns[p], center + (p == 0 ? -10 : 10), center));
                Command salvo = make_order(CommandType::Ability, p, {rockets[p]}, center, center + (p == 0 ? 5 : -5));
                salvo.ability = static_cast<uint8_t>(AbilityId::Salvo);
                issue(sim, salvo);
                // A mine where the armies will clash.
                Command mine = make_order(CommandType::Ability, p, {sappers[p]}, center + (p == 0 ? -3 : 3), center);
                mine.ability = static_cast<uint8_t>(AbilityId::LayApMine);
                issue(sim, mine);
            }
        }
        if (t == 1) {
            // Meanwhile the rear troops cut wood behind each base and the
            // headquarters hire more of them.
            for (PlayerId p = 0; p < 2; ++p) {
                std::vector<EntityId> workers;
                for (const Unit& u : sim.world().units()) {
                    if (u.owner == p && unit_type(u.type).worker) workers.push_back(u.id);
                }
                const int32_t size = sim.world().map().width();
                const int32_t wx = p == 0 ? size * 6 / 100 : size - size * 6 / 100;
                const int32_t wy = p == 0 ? size * 90 / 100 : size - size * 90 / 100;
                issue(sim, make_order(CommandType::Gather, p, workers, wx, wy));
                for (const Structure& s : sim.world().structures()) {
                    if (s.type != StructureType::Headquarters || s.owner != p) continue;
                    Command train{.type = CommandType::Train, .player = p, .target_unit = s.id,
                                  .unit_type = static_cast<uint8_t>(UnitTypeId::Worker)};
                    issue(sim, train);
                    issue(sim, train);
                }
            }
        }
        if (t >= kMelee && t % 40 == 0) {
            const auto player = static_cast<PlayerId>((t / 40) % 2);
            const CommandType type = (t / 80) % 2 ? CommandType::AttackMove : CommandType::Move;
            issue(sim, make_order(type, player, units_of(sim.world(), player), center - 20 + (t * 7) % 40,
                                  center - 20 + (t * 11) % 40));
        }
        if (t >= kMelee && t % 97 == 0) {
            std::vector<EntityId> half = units_of(sim.world(), 0);
            half.resize(half.size() / 2);
            Command stop;
            stop.type = CommandType::Stop;
            stop.player = 0;
            stop.units = half;
            issue(sim, stop);
        }
        sim.step();
        checksums.push_back(sim.world().checksum());
    }
    return checksums;
}

// --- Math --------------------------------------------------------------------

void test_fixed_math() {
    CHECK(Fixed::from_int(3) * Fixed::from_ratio(1, 2) == Fixed::from_ratio(3, 2));
    CHECK(Fixed::from_int(7) / Fixed::from_int(2) == Fixed::from_ratio(7, 2));
    CHECK(Fixed::from_int(-7) / Fixed::from_int(2) == Fixed::from_ratio(-7, 2));
    CHECK((-Fixed::from_ratio(3, 2)).to_int() == -2);  // floor, not truncation
    CHECK((FixedVec2{Fixed::from_int(3), Fixed::from_int(4)}.length() == Fixed::from_int(5)));
    CHECK((FixedVec2{Fixed::from_int(-3000), Fixed::from_int(4000)}.length() == Fixed::from_int(5000)));
    CHECK(isqrt(0) == 0);
    CHECK(isqrt(15) == 3);
    CHECK(isqrt(16) == 4);
    CHECK(isqrt(UINT64_MAX) == 4294967295ULL);
}

void test_rng() {
    Rng a(42), b(42), c(43);
    bool same = true;
    bool differs = false;
    for (int i = 0; i < 1000; ++i) {
        const uint32_t x = a.next_u32();
        same = same && x == b.next_u32();
        differs = differs || x != c.next_u32();
    }
    CHECK(same);
    CHECK(differs);

    Rng r(1);
    bool in_range = true;
    for (int i = 0; i < 10000; ++i) {
        const int32_t v = r.next_range(-5, 5);
        in_range = in_range && v >= -5 && v <= 5;
    }
    CHECK(in_range);
}

// --- Determinism -------------------------------------------------------------

// The core lockstep guarantee: same seed + same commands => same state, every
// tick. The script makes the armies fight, so combat is covered too.
void test_determinism() {
    const std::vector<uint64_t> first = play_script(123, 3500);
    const std::vector<uint64_t> second = play_script(123, 3500);
    const std::vector<uint64_t> other_seed = play_script(124, 3500);
    CHECK(first == second);
    CHECK(first != other_seed);
}

// Peers may receive commands of different players in any order.
void test_command_arrival_order_does_not_matter() {
    auto run = [](bool player0_first) {
        Simulation sim(7, make_demo_map());
        setup_demo_scenario(sim.world_for_setup());
        const int32_t center = sim.world().map().width() / 2;
        Command m0 = make_order(CommandType::AttackMove, 0, units_of(sim.world(), 0), center, center);
        Command m1 = make_order(CommandType::AttackMove, 1, units_of(sim.world(), 1), center, center);
        if (player0_first) {
            sim.schedule(5, m0);
            sim.schedule(5, m1);
        } else {
            sim.schedule(5, m1);
            sim.schedule(5, m0);
        }
        for (int i = 0; i < 800; ++i) sim.step();
        return sim.world().checksum();
    };
    CHECK(run(true) == run(false));
}

// A (possibly hacked) client must not be able to order someone else's units.
void test_foreign_units_are_ignored() {
    Simulation idle(9, make_demo_map());
    setup_demo_scenario(idle.world_for_setup());

    Simulation hacked(9, make_demo_map());
    setup_demo_scenario(hacked.world_for_setup());
    issue(hacked, make_move(1, units_of(hacked.world(), 0), 5, 5));

    for (int i = 0; i < 100; ++i) {
        idle.step();
        hacked.step();
    }
    CHECK(idle.world().checksum() == hacked.world().checksum());
}

// --- Movement ----------------------------------------------------------------

void test_unit_reaches_target() {
    Simulation sim(1, TileMap(32, 32));
    const EntityId id = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(5, 5));
    issue(sim, make_move(0, {id}, 20, 5));
    for (int i = 0; i < 400; ++i) sim.step();

    const Unit* u = sim.world().find_unit(id);
    CHECK(u != nullptr);
    CHECK(u && !u->moving && u->order == Order::Idle);
    CHECK(u && u->pos == at(20, 5));
}

// --- Combat ------------------------------------------------------------------

// Shooting down a hill deals 125%, shooting up deals 75% (AoE II rules).
void test_high_ground() {
    TileMap map(20, 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 8; ++x) map.set_elevation(x, y, 1);
    }
    Simulation sim(3, map);
    // At the edge of the hill, so both have a clear line of fire.
    const EntityId high = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(6, 8));
    const EntityId low = sim.world_for_setup().spawn_unit(1, UnitTypeId::Tank, at(12, 8));

    // Two idle tanks in sight of each other open fire on their own.
    int32_t first_drop_high = 0;
    int32_t first_drop_low = 0;
    int32_t prev_high = hp_of(sim, high);
    int32_t prev_low = hp_of(sim, low);
    for (int i = 0; i < 400 && (!first_drop_high || !first_drop_low); ++i) {
        sim.step();
        if (!first_drop_high && hp_of(sim, high) < prev_high) first_drop_high = prev_high - hp_of(sim, high);
        if (!first_drop_low && hp_of(sim, low) < prev_low) first_drop_low = prev_low - hp_of(sim, low);
        prev_high = hp_of(sim, high);
        prev_low = hp_of(sim, low);
    }

    const WeaponDef& cannon = unit_type(UnitTypeId::Tank).weapon;
    const int32_t base = cannon.damage - unit_type(UnitTypeId::Tank).armor[static_cast<size_t>(cannon.damage_type)];
    CHECK(first_drop_low == base * kHighGroundPercent / 100);
    CHECK(first_drop_high == base * kLowGroundPercent / 100);
}

// A rocket flies to where the target WAS: a tank that keeps driving dodges it,
// one that stands still gets hit. Accuracy is random, so count over many duels.
void test_projectiles_can_be_dodged() {
    auto first_rocket_hits = [](bool target_moves) {
        int hits = 0;
        for (uint64_t seed = 1; seed <= 20; ++seed) {
            Simulation sim(seed, TileMap(40, 40));
            sim.world_for_setup().spawn_unit(0, UnitTypeId::Grenadier, at(10, 10));
            const EntityId target = sim.world_for_setup().spawn_unit(1, UnitTypeId::Tank, at(14, 10));
            if (target_moves) sim.schedule(0, make_move(1, {target}, 14, 38));  // a move order doesn't stop for enemies
            for (int i = 0; i < 25; ++i) sim.step();  // the first rocket lands after ~14 ticks
            if (hp_of(sim, target) < unit_type(UnitTypeId::Tank).max_hp) ++hits;
        }
        return hits;
    };
    CHECK(first_rocket_hits(false) >= 12);
    CHECK(first_rocket_hits(true) <= 3);
}

// Armor can soak almost everything, but every hit does at least 1 damage.
void test_armor_lets_through_at_least_one() {
    Simulation sim(5, TileMap(40, 40));
    sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(10, 10));
    const EntityId tank = sim.world_for_setup().spawn_unit(1, UnitTypeId::Tank, at(14, 10));
    issue(sim, make_move(1, {tank}, 14, 38));  // drive past (firing on the move)

    bool drops_are_one = true;
    int32_t prev = hp_of(sim, tank);
    for (int i = 0; i < 200; ++i) {
        sim.step();
        const int32_t hp = hp_of(sim, tank);
        drops_are_one = drops_are_one && (prev - hp == 0 || prev - hp == 1);
        prev = hp;
    }
    CHECK(drops_are_one);
    CHECK(prev < unit_type(UnitTypeId::Tank).max_hp);
}

// A tank on the move holds its gun on an enemy in reach: once its gunner has
// held it a second (the lock) it fires without stopping, turning or not, its
// turret on the target, its hull the way it drives. Fire control locks sooner.
void test_tanks_fire_on_the_move() {
    auto drive_past = [](bool fire_control) {
        Simulation sim(3, TileMap(60, 20));
        World& w = sim.world_for_setup();
        if (fire_control) w.upgrade_for_setup(0, UpgradeId::FireControl);
        const EntityId tank = w.spawn_unit(0, UnitTypeId::Tank, at(5, 5));
        const EntityId enemy = w.spawn_unit(1, UnitTypeId::Tank, at(13, 12));
        w.unit_for_setup(enemy)->rounds = 0;  // a target, not a duel
        sim.schedule(0, make_move(0, {tank}, 55, 5));
        int locked = -1;
        for (int i = 0; i < 200; ++i) {
            sim.step();
            const Unit* u = sim.world().find_unit(tank);
            if (u->lock == enemy && locked < 0) locked = i;
            if (u->last_shot_tick == kNeverFired) continue;
            CHECK(locked >= 0);
            CHECK(u->order == Order::Move && u->moving);  // still driving
            const FixedVec2 to_enemy = sim.world().find_unit(enemy)->pos - u->pos;
            const int64_t fx = u->facing.x.raw >> 8, fy = u->facing.y.raw >> 8;
            const int64_t ex = to_enemy.x.raw >> 8, ey = to_enemy.y.raw >> 8;
            CHECK(fx * ex + fy * ey > 0 && std::abs(fx * ey - fy * ex) * 50 < fx * ex + fy * ey);  // the turret on it
            CHECK(u->hull.x.raw > 0 && std::abs(u->hull.y.raw) * 4 < u->hull.x.raw);  // the hull down the road
            return i - locked;
        }
        return -1;
    };
    const int held = drive_past(false);
    CHECK(held >= static_cast<int>(kLockTicks) - 1 && held <= static_cast<int>(kLockTicks) + 1);
    const int held_fcs = drive_past(true);
    CHECK(held_fcs >= static_cast<int>(kFireControlLockTicks) - 1 && held_fcs <= static_cast<int>(kFireControlLockTicks) + 1);
}

// A tank killed by a direct hit with more than a quarter of its rounds
// aboard blows up: the turret thrown off, nobody gets out. With its racks
// near empty, or killed by a burst beside it, it doesn't.
void test_tanks_blow_up() {
    enum class Kill { Rocket, Burst };
    auto kill = [](int32_t rounds, Kill how, bool& crew_lost) {
        bool blown = false;
        crew_lost = true;
        for (uint64_t seed = 1; seed <= 20; ++seed) {  // the crew's luck: over a score
            Simulation sim(seed, TileMap(40, 20));
            World& w = sim.world_for_setup();
            const EntityId victim = w.spawn_unit(1, UnitTypeId::Tank, at(18, 10));
            w.unit_for_setup(victim)->rounds = rounds;
            w.unit_for_setup(victim)->hp = 1;
            w.unit_for_setup(victim)->cooldown = 1000;  // not shooting back
            if (how == Kill::Rocket) {
                w.spawn_unit(0, UnitTypeId::Grenadier, at(14, 10));
            } else {
                const EntityId truck = w.spawn_unit(1, UnitTypeId::AmmoTruck, {at(18, 10).x + Fixed::from_ratio(6, 5), at(18, 10).y});
                w.unit_for_setup(truck)->carrying = 20;
                w.unit_for_setup(truck)->hp = 0;  // hit: its load goes off beside the tank
            }
            const int32_t men = sim.world().stock(1)[static_cast<size_t>(Resource::Personnel)];
            for (int i = 0; i < 800 && sim.world().find_unit(victim); ++i) {
                sim.step();
                for (const Impact& im : sim.world().recent_impacts()) blown = blown || im.blown == victim;
            }
            CHECK(!sim.world().find_unit(victim));
            if (sim.world().stock(1)[static_cast<size_t>(Resource::Personnel)] != men) crew_lost = false;
        }
        return blown;
    };
    const int32_t full = unit_type(UnitTypeId::Tank).rounds_capacity;
    bool crew_lost = false;
    CHECK(kill(full, Kill::Rocket, crew_lost));
    CHECK(crew_lost);
    CHECK(kill(full / 4 + 1, Kill::Rocket, crew_lost));
    CHECK(!kill(full / 4, Kill::Rocket, crew_lost));  // a quarter: it only burns
    CHECK(!crew_lost);
    CHECK(!kill(full, Kill::Burst, crew_lost));
    CHECK(!crew_lost);
}

// A tank gun's aim by the range: all but sure point-blank, its accuracy at
// its effective range, less and less past it; and a miss goes wider the
// farther the target (a gun's dispersion is an angle).
void test_aim_by_the_range() {
    auto fire_at_range = [](int32_t distance, int& on_aim, Fixed& widest) {
        on_aim = 0;
        widest = Fixed{};
        for (uint64_t seed = 1; seed <= 60; ++seed) {
            Simulation sim(seed, TileMap(60, 20));
            const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(4, 10));
            sim.schedule(0, fire_at(0, {tank}, 4 + distance, 10));
            for (int i = 0; i < 20; ++i) {
                sim.step();
                const auto& shells = sim.world().projectiles();
                const auto it = std::find_if(shells.begin(), shells.end(), [&](const Projectile& p) { return p.shooter == tank; });
                if (it == shells.end()) continue;
                const Fixed off = max(abs_fixed(it->target.x - at(4 + distance, 10).x), abs_fixed(it->target.y - at(4 + distance, 10).y));
                if (off < Fixed::from_ratio(1, 50)) ++on_aim;
                widest = max(widest, off);
                break;
            }
        }
    };
    int close = 0, mid = 0, far = 0;
    Fixed close_miss, mid_miss, far_miss;
    fire_at_range(3, close, close_miss);
    fire_at_range(15, mid, mid_miss);
    fire_at_range(45, far, far_miss);
    CHECK(close >= 47);                // ~89%
    CHECK(mid >= 40 && mid <= 56);     // ~80%
    CHECK(far <= 30);                  // ~35%
    const Fixed spread = unit_type(UnitTypeId::Tank).weapon.miss_spread;
    CHECK(close_miss <= spread / 4);
    CHECK(mid_miss <= spread);
    CHECK(far_miss > spread * 2 && far_miss <= spread * 3);
}

Command attack_order(PlayerId player, std::vector<EntityId> units, EntityId target) {
    return {.type = CommandType::Attack, .player = player, .units = std::move(units), .target_unit = target};
}

// An explicit attack order hunts the target down, even when it runs.
void test_attack_order_kills_target() {
    Simulation sim(2, TileMap(40, 40));
    const EntityId ifv = sim.world_for_setup().spawn_unit(0, UnitTypeId::Ifv, at(5, 5));
    const EntityId victim = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(10, 5));
    sim.schedule(0, make_move(1, {victim}, 35, 35));  // flees, ignoring the IFV
    issue(sim, attack_order(0, {ifv}, victim));

    for (int i = 0; i < 1000 && sim.world().find_unit(victim); ++i) sim.step();
    CHECK(sim.world().find_unit(victim) == nullptr);
    sim.step();  // the IFV notices on its next update
    const Unit* t = sim.world().find_unit(ifv);
    CHECK(t && t->order == Order::Idle);
}

// Attacking your own unit is not allowed.
void test_cannot_attack_own_units() {
    Simulation sim(2, TileMap(40, 40));
    const EntityId a = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 5));
    const EntityId b = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(8, 5));
    Command attack;
    attack.type = CommandType::Attack;
    attack.player = 0;
    attack.units = {a};
    attack.target_unit = b;
    issue(sim, attack);
    for (int i = 0; i < 200; ++i) sim.step();
    CHECK(hp_of(sim, b) == unit_type(UnitTypeId::Rifleman).max_hp);
}

// Both armies attack-move into each other across the map: there must be casualties.
void test_armies_fight() {
    Simulation sim(4, make_demo_map());
    setup_demo_scenario(sim.world_for_setup());
    const size_t before = sim.world().units().size();
    const int32_t center = sim.world().map().width() / 2;
    issue(sim, make_order(CommandType::AttackMove, 0, units_of(sim.world(), 0), center, center));
    issue(sim, make_order(CommandType::AttackMove, 1, units_of(sim.world(), 1), center, center));
    for (int i = 0; i < 4000; ++i) sim.step();
    CHECK(sim.world().units().size() < before);
}

// Every AoE II preset builds, the map is the same when seen from the other
// player's side (point symmetry), and vehicles can drive from base to base.
void test_demo_map_is_fair() {
    for (const MapSizePreset& preset : kMapSizes) {
        const TileMap map = make_demo_map(preset.tiles);
        CHECK(map.width() == preset.tiles);
        bool symmetric = true;
        uint8_t highest = 0;
        std::array<int, kTerrainCount> counts{};
        for (int32_t y = 0; y < map.height(); ++y) {
            for (int32_t x = 0; x < map.width(); ++x) {
                const int32_t mx = map.width() - 1 - x;
                const int32_t my = map.height() - 1 - y;
                symmetric = symmetric && map.elevation(x, y) == map.elevation(mx, my) &&
                            map.terrain(x, y) == map.terrain(mx, my);
                highest = std::max(highest, map.elevation(x, y));
                ++counts[static_cast<size_t>(map.terrain(x, y))];
            }
        }
        CHECK(symmetric);
        CHECK(highest >= 3);
        for (size_t t = 0; t < kTerrainCount; ++t) {
            // Every kind of natural terrain is there; ruins, buildings and field works come later.
            const auto terrain = static_cast<Terrain>(t);
            const bool made = terrain == Terrain::Ruins || terrain == Terrain::Building || terrain == Terrain::Trench ||
                              terrain == Terrain::Foxhole || terrain == Terrain::Dugout || terrain == Terrain::GunPit ||
                              terrain == Terrain::Wire || terrain == Terrain::Hedgehogs || terrain == Terrain::Pillbox ||
                              terrain == Terrain::Airstrip;
            if (!made) CHECK(counts[t] > 0);
        }
        CHECK(counts[static_cast<size_t>(Terrain::Ruins)] == 0);  // nothing destroyed yet

        const FlowField to_enemy(map, tile_of(demo_base_position(preset.tiles, 1)), MoveClass::Vehicle);
        CHECK(to_enemy.reachable(tile_of(demo_base_position(preset.tiles, 0))));
    }
}

// --- Line of fire ------------------------------------------------------------

Command fire_at(PlayerId player, std::vector<EntityId> units, int32_t x, int32_t y) {
    return make_order(CommandType::AttackGround, player, std::move(units), x, y);
}

FixedVec2 at_half(int32_t x2, int32_t y2) { return {Fixed::from_ratio(x2, 2), Fixed::from_ratio(y2, 2)}; }

// A crew doesn't fire through its own men standing in the open in front of it.
void test_holds_fire_through_own_troops() {
    auto fired = [](bool friend_in_the_way) {
        Simulation sim(1, TileMap(30, 20));
        const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 10));
        if (friend_in_the_way) sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(8, 10));
        issue(sim, fire_at(0, {gun}, 11, 10));
        for (int i = 0; i < 60; ++i) sim.step();
        return sim.world().find_unit(gun)->last_shot_tick != kNeverFired;
    };
    CHECK(fired(false));
    CHECK(!fired(true));
}

// Behind a ridge the target is in range but out of sight: instead of shelling
// the hillside, the tank moves up until it has a line of fire.
void test_moves_up_when_a_hill_is_in_the_way() {
    TileMap map(30, 20);
    for (int y = 0; y < 20; ++y) {
        map.set_elevation(9, y, 3);
        map.set_elevation(10, y, 3);
    }
    Simulation sim(1, map);
    const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(6, 10));
    issue(sim, fire_at(0, {gun}, 13, 10));

    Fixed first_shot_x{};
    for (int i = 0; i < 400 && first_shot_x.raw == 0; ++i) {
        sim.step();
        const Unit* u = sim.world().find_unit(gun);
        if (u->last_shot_tick != kNeverFired) first_shot_x = u->pos.x;
    }
    CHECK(first_shot_x > Fixed::from_int(8));  // fired only once up on the ridge
}

// Whoever stands in the line of fire takes the shell meant for the spot behind him.
void test_bodies_in_the_path_take_the_hit() {
    Simulation sim(2, TileMap(30, 20));
    const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 10));
    const EntityId in_the_way = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(8, 10));
    // Orders from tick 0, so neither side starts by simply shooting the other.
    sim.schedule(0, fire_at(0, {gun}, 11, 10));
    sim.schedule(0, fire_at(1, {in_the_way}, 8, 14));  // busy shooting elsewhere, standing still
    for (int i = 0; i < 40; ++i) sim.step();
    CHECK(hp_of(sim, in_the_way) < unit_type(UnitTypeId::Rifleman).max_hp);
}

// A tree line on the plateau's foot: tile column 7. Our man hides in it.
TileMap plateau_with_tree_line(uint8_t plateau_level) {
    TileMap map(30, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x <= 5; ++x) map.set_elevation(x, y, plateau_level);
        map.set_terrain(7, y, Terrain::Forest);
    }
    return map;
}

// The SPG case: firing from level ground through a tree line where our own
// men sit (unseen, so nobody holds fire) — the shells land among them.
void test_shells_through_a_tree_line_hit_hidden_friends() {
    Simulation sim(3, plateau_with_tree_line(0));
    sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(4, 10));
    const EntityId hidden = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at_half(15, 20));
    issue(sim, fire_at(0, units_of(sim.world(), 0), 11, 10));
    for (int i = 0; i < 600; ++i) sim.step();
    CHECK(hp_of(sim, hidden) < unit_type(UnitTypeId::Rifleman).max_hp);
}

// From a dominant height the line of fire passes over the tree line and our
// men in it, and reaches the target area.
void test_fire_from_high_ground_passes_over_own_troops() {
    Simulation sim(3, plateau_with_tree_line(3));
    const EntityId mg = sim.world_for_setup().spawn_unit(0, UnitTypeId::MachineGunner, at(4, 10));
    const EntityId hidden = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at_half(15, 20));
    issue(sim, fire_at(0, {mg}, 10, 10));

    int reached = 0;
    for (int i = 0; i < 400; ++i) {
        sim.step();
        const Unit* u = sim.world().find_unit(mg);
        if (u->last_shot_tick + 1 == sim.world().tick() && (u->last_shot_at - at(10, 10)).length() < Fixed::from_int(2)) {
            ++reached;
        }
    }
    CHECK(hp_of(sim, hidden) == unit_type(UnitTypeId::Rifleman).max_hp);
    CHECK(reached > 20);
}

// Trees catch a good share of the bullets fired through a forest; in the open
// none stop short.
void test_forest_stops_some_bullets() {
    auto stopped_short = [](bool forest, int& shots) {
        TileMap map(30, 20);
        if (forest) {
            for (int y = 0; y < 20; ++y) {
                map.set_terrain(6, y, Terrain::Forest);
                map.set_terrain(7, y, Terrain::Forest);
            }
        }
        Simulation sim(5, map);
        const EntityId mg = sim.world_for_setup().spawn_unit(0, UnitTypeId::MachineGunner, at(4, 10));
        issue(sim, fire_at(0, {mg}, 10, 10));
        int stopped = 0;
        shots = 0;
        for (int i = 0; i < 400; ++i) {
            sim.step();
            const Unit* u = sim.world().find_unit(mg);
            if (u->last_shot_tick + 1 != sim.world().tick()) continue;
            ++shots;
            // Misses land within a tile of the aim point; anything shorter hit a tree.
            if ((u->last_shot_at - at(10, 10)).length() > Fixed::from_ratio(3, 2)) ++stopped;
        }
        return stopped;
    };
    int open_shots = 0;
    int forest_shots = 0;
    CHECK(stopped_short(false, open_shots) == 0);
    const int stopped = stopped_short(true, forest_shots);
    CHECK(forest_shots > 50);
    CHECK(stopped * 100 >= forest_shots * 15);
    CHECK(stopped * 100 <= forest_shots * 70);
}

// --- Fog of war --------------------------------------------------------------

bool seen(const Simulation& sim, PlayerId player, EntityId id) {
    const Unit* u = sim.world().find_unit(id);
    return u && sim.world().sees(player, *u);
}

// Only what our troops can see is visible; what they once saw stays explored.
void test_fog_of_war() {
    Simulation sim(1, TileMap(40, 40));
    const EntityId rifle = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(5, 5));
    const EntityId enemy = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(30, 30));
    sim.step();
    const World& w = sim.world();
    CHECK(w.visible(0, {5, 5}) && w.visible(0, {12, 5}));  // sight 7
    CHECK(!w.visible(0, {13, 5}));
    CHECK(!w.visible(0, {30, 30}) && !w.explored(0, {30, 30}));
    CHECK(!seen(sim, 0, enemy));
    CHECK(seen(sim, 0, rifle));  // our own, always

    issue(sim, make_move(0, {rifle}, 5, 30));
    for (int i = 0; i < 600; ++i) sim.step();
    CHECK(!w.visible(0, {5, 5}) && w.explored(0, {5, 5}));
    CHECK(w.visible(0, {5, 30}));
}

// A plain with a plateau on x <= 5 and, optionally, a tree line at x = 8 or
// a ridge there. We look from (4.5, 10.5).
bool sees_tile(uint8_t plateau, Terrain at_8, uint8_t ridge, TilePos target) {
    TileMap map(30, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x <= 5; ++x) map.set_elevation(x, y, plateau);
        map.set_terrain(8, y, at_8);
        if (ridge) map.set_elevation(8, y, ridge);
    }
    Simulation sim(1, map);
    sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at_half(9, 21));
    sim.step();
    return sim.world().visible(0, target);
}

// Tree lines and ridges hide what is behind them; from a height you see over
// the trees, and farther.
void test_sight_lines() {
    CHECK(sees_tile(0, Terrain::Grass, 0, {11, 10}));
    CHECK(!sees_tile(0, Terrain::Forest, 0, {11, 10}));  // behind the tree line
    CHECK(!sees_tile(0, Terrain::Grass, 2, {11, 10}));   // behind the ridge
    CHECK(sees_tile(3, Terrain::Forest, 0, {11, 10}));   // over the trees from above
    CHECK(sees_tile(0, Terrain::Forest, 0, {8, 10}));    // the edge of the trees itself
    CHECK(!sees_tile(0, Terrain::Grass, 0, {13, 10}));   // 9 tiles: too far on the plain...
    CHECK(sees_tile(3, Terrain::Grass, 0, {13, 10}));    // ...but not from the plateau
}

// A forest block on x = 12..15; a man at its edge on (12.5, 10.5) watched
// from `distance` tiles to the west, out in the open. Is he seen at the first
// look (or, moving, at the next one)?
bool seen_in_cover(UnitTypeId watcher, Fixed distance, UnitTypeId hider, bool hider_moves, bool ghillie = false) {
    TileMap map(40, 20);
    for (int y = 3; y <= 17; ++y) {
        for (int x = 12; x <= 15; ++x) map.set_terrain(x, y, Terrain::Forest);
    }
    Simulation sim(1, map);
    if (ghillie) sim.world_for_setup().upgrade_for_setup(1, UpgradeId::GhillieSuits);
    const FixedVec2 spot = at_half(25, 21);
    const EntityId hidden = sim.world_for_setup().spawn_unit(1, hider, spot);
    sim.world_for_setup().spawn_unit(0, watcher, {spot.x - distance, spot.y});
    if (hider_moves) sim.schedule(0, make_move(1, {hidden}, 12, 16));
    const Tick ticks = hider_moves ? kVisionInterval + 1 : 1;
    for (Tick i = 0; i < ticks; ++i) sim.step();
    CHECK(sim.world().visible(0, {12, 10}));  // the edge of the forest is in view either way
    return seen(sim, 0, hidden);
}

void test_cover_hides_until_spotted() {
    CHECK(seen_in_cover(UnitTypeId::Rifleman, Fixed::from_int(2), UnitTypeId::Rifleman, false));
    CHECK(!seen_in_cover(UnitTypeId::Rifleman, Fixed::from_int(3), UnitTypeId::Rifleman, false));
    CHECK(seen_in_cover(UnitTypeId::Scout, Fixed::from_int(4), UnitTypeId::Rifleman, false));  // a trained eye
    CHECK(seen_in_cover(UnitTypeId::Rifleman, Fixed::from_int(3), UnitTypeId::Rifleman, true));  // movement shows
    CHECK(!seen_in_cover(UnitTypeId::Rifleman, Fixed::from_ratio(3, 2), UnitTypeId::Scout, false));  // hard to spot
}

// A hidden rifleman gives himself away by firing, and disappears again after.
void test_firing_gives_away_a_hidden_shooter() {
    TileMap map(40, 20);
    for (int y = 3; y <= 17; ++y) {
        for (int x = 12; x <= 15; ++x) map.set_terrain(x, y, Terrain::Forest);
    }
    Simulation sim(1, map);
    const EntityId hider = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at_half(25, 21));
    // Headquarters 8 tiles off watch the forest edge; they don't go after him.
    sim.world_for_setup().place_structure(StructureType::Headquarters, 0, {3, 9}, 3, 3);
    for (int i = 0; i < 10; ++i) sim.step();
    CHECK(sim.world().visible(0, {12, 10}) && !seen(sim, 0, hider));

    issue(sim, fire_at(1, {hider}, 12, 14));
    for (int i = 0; i < 20; ++i) sim.step();
    CHECK(seen(sim, 0, hider));

    issue(sim, {.type = CommandType::Stop, .player = 1, .units = {hider}});
    for (Tick i = 0; i < kRevealTicks + 2 * kVisionInterval; ++i) sim.step();
    CHECK(!seen(sim, 0, hider));
}

// Nobody fires at what they can't see: a tank ignores a man walking through
// the forest right in front of it, but not one walking across the field.
void test_no_shooting_into_the_fog() {
    auto tank_fires = [](bool forest) {
        TileMap map(40, 20);
        if (forest) {
            for (int y = 3; y <= 17; ++y) {
                for (int x = 12; x <= 15; ++x) map.set_terrain(x, y, Terrain::Forest);
            }
        }
        Simulation sim(1, map);
        const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at_half(15, 21));
        const EntityId walker = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at_half(25, 21));
        sim.schedule(0, make_move(1, {walker}, 13, 16));
        for (int i = 0; i < 60; ++i) sim.step();
        return sim.world().find_unit(tank)->last_shot_tick != kNeverFired;
    };
    CHECK(tank_fires(false));
    CHECK(!tank_fires(true));
}

// Attack orders only go to targets in sight; one that ducks into cover is
// sought where it was last seen.
void test_attack_orders_need_the_target_in_sight() {
    Simulation far(2, TileMap(60, 20));
    const EntityId rifle = far.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(5, 10));
    const EntityId distant = far.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(40, 10));
    issue(far, attack_order(0, {rifle}, distant));
    for (int i = 0; i < 10; ++i) far.step();
    CHECK(far.world().find_unit(rifle)->order == Order::Idle);  // ignored: nobody sees it

    TileMap map(40, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 14; x <= 25; ++x) map.set_terrain(x, y, Terrain::Forest);
    }
    Simulation sim(3, map);
    const EntityId hunter = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(6, 10));
    const EntityId victim = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(12, 10));
    sim.schedule(0, make_move(1, {victim}, 16, 10));
    issue(sim, attack_order(0, {hunter}, victim));
    bool sought = false;
    for (int i = 0; i < 600; ++i) {
        sim.step();
        const Unit* h = sim.world().find_unit(hunter);
        sought = sought || (h && h->order == Order::AttackMove);
    }
    CHECK(sought);
    CHECK(hp_of(sim, victim) < unit_type(UnitTypeId::Rifleman).max_hp);
}

Command observe(PlayerId player, std::vector<EntityId> scouts, int32_t x, int32_t y) {
    return make_order(CommandType::Observe, player, std::move(scouts), x, y);
}

// An observation post sees far out, but only within its sector; it holds its
// ground and its fire.
void test_observation_post_watches_its_sector() {
    Simulation sim(1, TileMap(60, 30));
    const EntityId scout = sim.world_for_setup().spawn_unit(0, UnitTypeId::Scout, at_half(11, 31));  // (5.5, 15.5)
    const EntityId rifle = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at_half(61, 55));
    sim.schedule(0, observe(0, {scout, rifle}, 40, 15));
    for (Tick i = 0; i <= kVisionInterval; ++i) sim.step();
    const World& w = sim.world();
    CHECK(w.find_unit(scout)->order == Order::Observe);
    CHECK(w.find_unit(rifle)->order == Order::Idle);  // only scouts hold posts
    CHECK(w.visible(0, {19, 15}));   // 14 tiles out, in the sector
    CHECK(w.visible(0, {17, 20}));   // off to the side, still within 45 degrees
    CHECK(!w.visible(0, {5, 29}));   // 14 tiles to the side: outside it
    CHECK(!w.visible(0, {0, 5}));    // behind
    CHECK(w.visible(0, {9, 12}));    // all around, close by

    const EntityId enemy = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at_half(21, 31));
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(hp_of(sim, enemy) == unit_type(UnitTypeId::Rifleman).max_hp);  // the post doesn't shoot

    issue(sim, make_move(0, {scout}, 5, 25));
    for (Tick i = 0; i < 3 * kVisionInterval; ++i) sim.step();
    CHECK(!w.visible(0, {19, 15}));  // off the post, the sector is gone
}

// A man at a forest edge 7 tiles from our observation posts: one post alone
// doesn't make him out, two crossing their sectors on him do.
void test_crossed_sectors_find_men_in_cover() {
    auto found = [](bool second_post) {
        TileMap map(50, 30);
        for (int y = 5; y <= 25; ++y) {
            for (int x = 30; x <= 40; ++x) map.set_terrain(x, y, Terrain::Forest);
        }
        Simulation sim(1, map);
        const EntityId hider = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at_half(61, 31));
        std::vector<EntityId> posts{sim.world_for_setup().spawn_unit(0, UnitTypeId::Scout, at_half(47, 27))};
        if (second_post) posts.push_back(sim.world_for_setup().spawn_unit(0, UnitTypeId::Scout, at_half(47, 35)));
        sim.schedule(0, observe(0, posts, 30, 15));
        for (Tick i = 0; i <= 2 * kVisionInterval; ++i) sim.step();
        CHECK(sim.world().visible(0, {30, 15}));  // the edge is in view either way
        return seen(sim, 0, hider);
    };
    CHECK(!found(false));
    CHECK(found(true));
}

// --- Skills ------------------------------------------------------------------

Command use_ability(PlayerId player, std::vector<EntityId> units, AbilityId ability, int32_t x, int32_t y) {
    Command cmd = make_order(CommandType::Ability, player, std::move(units), x, y);
    cmd.ability = static_cast<uint8_t>(ability);
    return cmd;
}

Command load_shell(PlayerId player, std::vector<EntityId> units, Shell shell) {
    Command cmd{.type = CommandType::LoadShell, .player = player, .units = std::move(units)};
    cmd.ability = static_cast<uint8_t>(shell);
    return cmd;
}

// Switching to armor-piercing takes a reload, then hits armor much harder.
void test_tank_switches_rounds() {
    auto first_hit = [](bool armor_piercing) {
        Simulation sim(3, TileMap(30, 20));
        const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 10));
        const EntityId target = sim.world_for_setup().spawn_unit(1, UnitTypeId::Tank, at(11, 10));
        if (armor_piercing) sim.schedule(0, use_ability(0, {gun}, AbilityId::SwitchAmmo, 0, 0));
        sim.schedule(0, make_move(1, {target}, 11, 10));  // stays put, holds its fire for a while
        sim.step();
        if (armor_piercing) {
            const Unit* u = sim.world().find_unit(gun);
            CHECK(u->round_type == 1);
            CHECK(u->cooldown + 1 >= unit_type(UnitTypeId::Tank).alt_weapon.reload);  // reloading
        }
        int32_t prev = hp_of(sim, target);
        for (int i = 0; i < 300; ++i) {
            sim.step();
            if (hp_of(sim, target) < prev) return prev - hp_of(sim, target);
            prev = hp_of(sim, target);
        }
        return 0;
    };
    const int32_t he = first_hit(false);
    const int32_t ap = first_hit(true);
    CHECK(he > 0);
    CHECK(ap >= 2 * he);  // 110 against 55 through a tank's armor

    // Switching empties the breech: a full reload before the next shot.
    Simulation alone(1, TileMap(20, 20));
    const EntityId tank = alone.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 5));
    alone.schedule(0, use_ability(0, {tank}, AbilityId::SwitchAmmo, 0, 0));
    alone.step();
    CHECK(alone.world().find_unit(tank)->cooldown + 1 >= unit_type(UnitTypeId::Tank).alt_weapon.reload);
}

// One wide-bursting shell: several men around the spot are hurt, then the
// skill cools down and the tank stands by.
void test_area_shot() {
    int wide = 0;
    for (uint64_t seed = 1; seed <= 10; ++seed) {
        Simulation sim(seed, TileMap(30, 20));
        const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(4, 10));
        std::vector<EntityId> men;
        for (const TilePos& t : {TilePos{10, 10}, TilePos{11, 10}, TilePos{10, 11}, TilePos{9, 10}}) {
            men.push_back(sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(t.x, t.y)));
        }
        sim.schedule(0, make_move(1, men, 10, 10));  // bunched up, not shooting yet
        sim.schedule(0, use_ability(0, {tank}, AbilityId::AreaShot, 10, 10));
        for (int i = 0; i < 30; ++i) sim.step();
        int hurt = 0;
        for (EntityId id : men) hurt += hp_of(sim, id) < unit_type(UnitTypeId::Rifleman).max_hp ? 1 : 0;
        wide += hurt >= 2 ? 1 : 0;
        const Unit* t = sim.world().find_unit(tank);
        CHECK(t->order != Order::Ability);
        CHECK(t->ability_ready[0] > sim.world().tick());  // cooling down
    }
    CHECK(wide >= 6);
}

// A machine-gun sweep along a tree line hits men hidden in it, nobody aimed at.
void test_mg_sweep_hits_along_the_front() {
    int hurt = 0;
    for (uint64_t seed = 1; seed <= 5; ++seed) {
        TileMap map(30, 20);
        for (int y = 4; y <= 16; ++y) {
            for (int x = 8; x <= 11; ++x) map.set_terrain(x, y, Terrain::Forest);
        }
        Simulation sim(seed, map);
        const EntityId ifv = sim.world_for_setup().spawn_unit(0, UnitTypeId::Ifv, at(3, 10));
        // Our own men, standing still: the sweep spares nobody, and none of
        // them is on the line straight ahead.
        std::vector<EntityId> men;
        for (int y = 6; y <= 14; y += 2) {
            men.push_back(sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at_half(17, 2 * y + 1)));
        }
        sim.schedule(0, use_ability(0, {ifv}, AbilityId::MgSweep, 15, 10));
        for (int i = 0; i < 60; ++i) sim.step();
        for (EntityId id : men) hurt += hp_of(sim, id) < unit_type(UnitTypeId::Rifleman).max_hp ? 1 : 0;
        CHECK(sim.world().find_unit(ifv)->order != Order::Ability);
    }
    CHECK(hurt >= 6);
}

// A lobbed grenade flies over the heads of our men in front and comes down
// behind a ridge that stops direct fire.
void test_grenade_goes_over_cover() {
    int hits = 0;
    for (uint64_t seed = 1; seed <= 10; ++seed) {
        TileMap map(30, 20);
        for (int y = 0; y < 20; ++y) map.set_elevation(8, y, 3);
        Simulation sim(seed, map);
        const EntityId ifv = sim.world_for_setup().spawn_unit(0, UnitTypeId::Ifv, at(5, 10));
        const EntityId ours = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at_half(15, 20));
        const EntityId hidden = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(10, 10));
        sim.schedule(0, make_move(1, {hidden}, 10, 10));
        sim.schedule(0, use_ability(0, {ifv}, AbilityId::LobGrenade, 10, 10));
        for (int i = 0; i < 40; ++i) sim.step();
        hits += hp_of(sim, hidden) < unit_type(UnitTypeId::Rifleman).max_hp ? 1 : 0;
        CHECK(hp_of(sim, ours) == unit_type(UnitTypeId::Rifleman).max_hp);
    }
    CHECK(hits >= 5);
}

// Skills belong to unit types, and wait out their cooldown.
void test_skills_need_the_unit_and_the_time() {
    Simulation sim(1, TileMap(30, 20));
    const EntityId rifle = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(5, 10));
    const EntityId ifv = sim.world_for_setup().spawn_unit(0, UnitTypeId::Ifv, at(5, 12));
    sim.schedule(0, use_ability(0, {rifle}, AbilityId::LobGrenade, 8, 10));  // not his
    sim.schedule(0, use_ability(0, {ifv}, AbilityId::LobGrenade, 8, 12));
    for (int i = 0; i < 10; ++i) sim.step();
    CHECK(sim.world().find_unit(rifle)->order == Order::Idle);
    CHECK(sim.world().find_unit(rifle)->last_shot_tick == kNeverFired);
    const Tick ready = sim.world().find_unit(ifv)->ability_ready[1];
    CHECK(ready > sim.world().tick());
    issue(sim, use_ability(0, {ifv}, AbilityId::LobGrenade, 8, 12));  // too soon
    for (int i = 0; i < 5; ++i) sim.step();
    CHECK(sim.world().find_unit(ifv)->ability_ready[1] == ready);
}

// --- Field works ---------------------------------------------------------------

Command dig_trench(std::vector<EntityId> diggers, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    Command cmd = use_ability(0, std::move(diggers), AbilityId::DigTrench, x0, y0);
    cmd.target_end = at(x1, y1);
    return cmd;
}

// The middle of tile (10, 10), where our man stands in these tests.
FixedVec2 post() { return at_half(21, 21); }

// Riflemen dig a trench along a line; tracks cross it slowly, wheels not at all.
void test_riflemen_dig_a_trench() {
    Simulation sim(1, TileMap(30, 20));
    std::vector<EntityId> squad;
    for (int i = 0; i < 2; ++i) squad.push_back(sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(5, 8 + i)));
    const EntityId mg = sim.world_for_setup().spawn_unit(0, UnitTypeId::MachineGunner, at(5, 12));
    issue(sim, dig_trench(squad, 10, 0, 10, 19));
    issue(sim, dig_trench({mg}, 12, 0, 12, 5));  // not his job
    for (int i = 0; i < 6000; ++i) sim.step();

    for (int y = 0; y < kMaxTrenchLength; ++y) CHECK(sim.world().map().terrain(10, y) == Terrain::Trench);
    CHECK(sim.world().map().terrain(10, kMaxTrenchLength) == Terrain::Grass);  // one order digs this much
    CHECK(sim.world().map().terrain(12, 0) == Terrain::Grass);
    for (EntityId id : squad) CHECK(sim.world().find_unit(id)->order == Order::Idle);
    const Structure* t = sim.world().structure_at({10, 5});
    CHECK(t && t->type == StructureType::Trench && t->owner == 0);

    const TileMap& map = sim.world().map();
    CHECK(map.passable({10, 5}, MoveClass::Foot) && map.passable({10, 5}, MoveClass::Vehicle));
    CHECK(!map.passable({10, 5}, MoveClass::Wheeled));
    CHECK(move_class(unit_type(UnitTypeId::Truck)) == MoveClass::Wheeled);
    CHECK(move_class(unit_type(UnitTypeId::Tank)) == MoveClass::Vehicle);
}

// Damage a rifleman on post() takes in 5 s from a machine gun firing at the
// spot from 5 tiles east or west, summed over 20 duels. `prepare` digs him in.
template <typename Prepare>
int32_t damage_taken(bool from_east, Prepare prepare, UnitTypeId who = UnitTypeId::Rifleman) {
    int32_t total = 0;
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        Simulation sim(seed, TileMap(30, 20));
        const EntityId man = sim.world_for_setup().spawn_unit(0, who, post());
        prepare(sim, man);
        const Fixed dx = Fixed::from_int(from_east ? 5 : -5);
        const EntityId gun = sim.world_for_setup().spawn_unit(1, UnitTypeId::MachineGunner, {post().x + dx, post().y});
        Command fire = make_order(CommandType::AttackGround, 1, {gun}, 0, 0);
        fire.target = post();
        sim.schedule(sim.world().tick(), fire);
        for (int i = 0; i < 100; ++i) sim.step();
        total += unit_type(who).max_hp - hp_of(sim, man);
    }
    return total;
}

void in_the_open(Simulation&, EntityId) {}

void settle(Simulation& sim) {
    for (Tick i = 0; i < kSettleTicks + 2; ++i) sim.step();
}

// A foxhole takes half the hits; a trench too, once the man is at a position.
void test_foxholes_and_trenches_give_cover() {
    const int32_t open = damage_taken(false, in_the_open);
    const int32_t foxhole = damage_taken(false, [](Simulation& sim, EntityId) {
        sim.world_for_setup().place_structure(StructureType::Foxhole, 0, {10, 10}, 1, 1);
    });
    const int32_t trench = damage_taken(false, [](Simulation& sim, EntityId) {
        sim.world_for_setup().place_structure(StructureType::Trench, 0, {10, 10}, 1, 1);
        settle(sim);
    });
    CHECK(open > 0);
    for (const int32_t dug_in : {foxhole, trench}) {
        CHECK(dug_in * 100 < open * 70);
        CHECK(dug_in * 100 > open * 30);
    }
}

// A parapet helps only against fire from its front.
void test_parapet_faces_one_way() {
    auto parapet_east = [](Simulation& sim, EntityId man) {
        sim.schedule(sim.world().tick(), use_ability(0, {man}, AbilityId::BuildParapet, 20, 10));
        for (Tick i = 0; i < kParapetWork + 5; ++i) sim.step();
        const Structure* mound = sim.world().structure_at({10, 10});
        CHECK(mound && mound->type == StructureType::Parapet && mound->facing.x > Fixed{});
    };
    const int32_t front = damage_taken(true, parapet_east);
    const int32_t back = damage_taken(false, parapet_east);
    CHECK(back > 0);
    CHECK(front * 100 < back * 90);
}

// A man in a foxhole shoots worse: -20% accuracy.
void test_foxholes_spoil_the_aim() {
    auto hits = [](bool foxhole) {
        int32_t total = 0;
        for (uint64_t seed = 1; seed <= 20; ++seed) {
            Simulation sim(seed, TileMap(30, 20));
            World& w = sim.world_for_setup();
            if (foxhole) w.place_structure(StructureType::Foxhole, 0, {10, 10}, 1, 1);
            w.spawn_unit(0, UnitTypeId::Rifleman, post());
            const EntityId truck = w.spawn_unit(1, UnitTypeId::Truck, {post().x + Fixed::from_int(4), post().y});
            for (int i = 0; i < 200; ++i) sim.step();
            total += unit_type(UnitTypeId::Truck).max_hp - hp_of(sim, truck);
        }
        return total;
    };
    const int32_t open = hits(false);
    const int32_t dug_in = hits(true);
    CHECK(open > 0);
    CHECK(dug_in * 100 < open * 92);
}

// Fresh into a trench and firing at once, before settling at a position:
// small arms at a quarter of the damage.
void test_firing_while_walking_in_a_trench() {
    Simulation sim(1, TileMap(30, 20));
    sim.world_for_setup().place_structure(StructureType::Trench, 0, {10, 10}, 1, 1);
    sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, post());
    const EntityId target = sim.world_for_setup().spawn_unit(1, UnitTypeId::MachineGunner, at_half(29, 21));
    int32_t first = 0;
    for (Tick i = 0; i + 5 < kSettleTicks && first == 0; ++i) {
        sim.step();
        first = unit_type(UnitTypeId::MachineGunner).max_hp - hp_of(sim, target);
    }
    const WeaponDef& rifle = unit_type(UnitTypeId::Rifleman).weapon;
    CHECK(first > 0 && first <= std::max(1, rifle.damage * kTrenchWalkingFirePercent / 100));
}

// A rifleman digs a foxhole and puts a parapet on it; on open ground a
// parapet is a mound of its own.
void test_riflemen_build_foxholes_and_parapets() {
    Simulation sim(1, TileMap(30, 20));
    const EntityId a = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at_half(11, 11));
    const EntityId b = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at_half(21, 11));
    sim.schedule(0, use_ability(0, {a}, AbilityId::DigFoxhole, 0, 0));
    sim.schedule(0, use_ability(0, {b}, AbilityId::BuildParapet, 25, 5));
    for (Tick i = 0; i < kFoxholeWork + 5; ++i) sim.step();
    const Structure* hole = sim.world().structure_at({5, 5});
    CHECK(hole && hole->type == StructureType::Foxhole && !hole->parapet);
    CHECK(sim.world().map().terrain(5, 5) == Terrain::Foxhole);
    const Structure* mound = sim.world().structure_at({10, 5});
    CHECK(mound && mound->type == StructureType::Parapet && mound->parapet && mound->facing.x > Fixed{});
    CHECK(sim.world().map().terrain(10, 5) == Terrain::Grass);  // a mound, still open ground

    issue(sim, use_ability(0, {a}, AbilityId::BuildParapet, 5, 0));
    for (Tick i = 0; i < kParapetWork + 5; ++i) sim.step();
    hole = sim.world().structure_at({5, 5});
    CHECK(hole && hole->type == StructureType::Foxhole && hole->parapet);
}

// Up close in a trench fight the walls don't help: from 2 tiles a rifleman
// hurts a man at a position in a trench about twice as much as from 4.
void test_trench_fight_ignores_cover() {
    auto damage_from = [](Fixed distance) {
        int32_t total = 0;
        for (uint64_t seed = 1; seed <= 20; ++seed) {
            Simulation sim(seed, TileMap(30, 20));
            World& w = sim.world_for_setup();
            w.place_structure(StructureType::Trench, 0, {10, 10}, 1, 1);
            const EntityId target = w.spawn_unit(0, UnitTypeId::MachineGunner, post());
            settle(sim);
            const EntityId rifle = w.spawn_unit(1, UnitTypeId::Rifleman, {post().x + distance, post().y});
            Command fire = make_order(CommandType::AttackGround, 1, {rifle}, 0, 0);
            fire.target = post();
            sim.schedule(sim.world().tick(), fire);
            for (int i = 0; i < 60; ++i) sim.step();
            total += unit_type(UnitTypeId::MachineGunner).max_hp - hp_of(sim, target);
        }
        return total;
    };
    const int32_t close = damage_from(kCloseQuarters);
    const int32_t far = damage_from(Fixed::from_int(4));
    CHECK(far > 0);
    CHECK(close * 100 > far * 150);
}

// Up close the assault trooper hits hardest.
void test_trench_fight() {
    auto first_hit = [](UnitTypeId attacker) {
        Simulation sim(2, TileMap(30, 20));
        World& w = sim.world_for_setup();
        w.place_structure(StructureType::Trench, 0, {10, 10}, 1, 1);
        const EntityId target = w.spawn_unit(0, UnitTypeId::MachineGunner, post());
        settle(sim);
        w.spawn_unit(1, attacker, {post().x + kCloseQuarters, post().y});
        for (int i = 0; i < 400; ++i) {
            sim.step();
            const int32_t lost = unit_type(UnitTypeId::MachineGunner).max_hp - hp_of(sim, target);
            if (lost > 0) return lost;
        }
        return 0;
    };
    const WeaponDef& smg = unit_type(UnitTypeId::Assault).weapon;
    CHECK(first_hit(UnitTypeId::Assault) == smg.damage * kAssaultCloseQuartersPercent / 100);
    CHECK(first_hit(UnitTypeId::Rifleman) == unit_type(UnitTypeId::Rifleman).weapon.damage);
}

Command use_ability_at(PlayerId player, std::vector<EntityId> units, AbilityId ability, FixedVec2 where) {
    Command cmd = use_ability(player, std::move(units), ability, 0, 0);
    cmd.target = where;
    return cmd;
}

// A foxhole dug out into a dugout: materials up front, the men in it do
// the work and end up inside, sheltered and blind, until a grenade comes in.
void test_foxhole_becomes_a_dugout() {
    Simulation sim(1, TileMap(30, 20));
    World& w = sim.world_for_setup();
    Stock two = kDugoutCost;
    for (int32_t& amount : two) amount *= 2;
    w.set_stock(0, two);
    const EntityId hole = w.place_structure(StructureType::Foxhole, 0, {10, 10}, 1, 1);
    const EntityId empty = w.place_structure(StructureType::Foxhole, 0, {10, 16}, 1, 1);  // nobody in it
    std::vector<EntityId> men;
    for (int i = 0; i < 2; ++i) men.push_back(w.spawn_unit(0, UnitTypeId::Rifleman, post()));
    issue(sim, {.type = CommandType::Upgrade, .player = 0, .target_unit = hole});
    issue(sim, {.type = CommandType::Upgrade, .player = 0, .target_unit = hole});  // already under way
    issue(sim, {.type = CommandType::Upgrade, .player = 0, .target_unit = empty});
    for (Tick i = 0; i < kDugoutWork / 2 + 10; ++i) sim.step();
    CHECK(sim.world().find_structure(empty)->type == StructureType::Foxhole);  // no one to dig it
    CHECK(sim.world().find_structure(empty)->upgrading);
    CHECK(!sim.world().visible(0, {14, 10}));  // underground: they see nothing
    const Structure* d = sim.world().find_structure(hole);
    CHECK(d && d->type == StructureType::Dugout);
    CHECK(sim.world().stock(0) == Stock{});  // both paid up front
    CHECK(sim.world().map().terrain(10, 10) == Terrain::Dugout);
    CHECK(d && d->garrison.size() == 2 && d->owner == 0);
    for (EntityId id : men) CHECK(sim.world().find_unit(id)->inside == hole);

    // Bullets don't reach them, and they don't fire out, even at an enemy our
    // observation post sees.
    const EntityId post_scout = w.spawn_unit(0, UnitTypeId::Scout, at_half(21, 7));
    issue(sim, observe(0, {post_scout}, 14, 10));
    const EntityId mg = w.spawn_unit(1, UnitTypeId::MachineGunner, at_half(29, 21));
    for (int i = 0; i < 200; ++i) sim.step();
    for (EntityId id : men) CHECK(hp_of(sim, id) == unit_type(UnitTypeId::Rifleman).max_hp);
    const Unit* enemy = sim.world().find_unit(mg);
    CHECK(enemy && sim.world().sees(0, *enemy));
    CHECK(hp_of(sim, mg) == unit_type(UnitTypeId::MachineGunner).max_hp);

    // An assault trooper's grenade goes in through the entrance.
    const EntityId stormer = w.spawn_unit(1, UnitTypeId::Assault, at_half(27, 21));
    issue(sim, use_ability_at(1, {stormer}, AbilityId::ThrowGrenade, post()));
    for (int i = 0; i < 60; ++i) sim.step();
    int hurt = 0;
    for (EntityId id : men) hurt += hp_of(sim, id) < unit_type(UnitTypeId::Rifleman).max_hp ? 1 : 0;
    CHECK(hurt == 2);

    // Out they come on order.
    issue(sim, {.type = CommandType::Unload, .player = 0, .target_unit = hole});
    for (int i = 0; i < 3; ++i) sim.step();
    for (EntityId id : men) {
        const Unit* u = sim.world().find_unit(id);
        CHECK(!u || u->inside == 0);
    }
}

// A grenade lobbed next to a foxhole: its walls don't stop what falls from above.
void test_grenades_fall_into_foxholes() {
    int hits = 0;
    for (uint64_t seed = 1; seed <= 10; ++seed) {
        Simulation sim(seed, TileMap(30, 20));
        World& w = sim.world_for_setup();
        w.place_structure(StructureType::Foxhole, 0, {10, 10}, 1, 1);
        const EntityId man = w.spawn_unit(0, UnitTypeId::Rifleman, post());
        const EntityId stormer = w.spawn_unit(1, UnitTypeId::Assault, at_half(27, 21));
        sim.schedule(0, use_ability_at(1, {stormer}, AbilityId::ThrowGrenade, at_half(23, 21)));
        for (int i = 0; i < 30; ++i) sim.step();
        // The grenade's hit, not a rifle bullet exchanged afterwards.
        const int32_t grenade = ability_def(AbilityId::ThrowGrenade).weapon.damage;
        hits += unit_type(UnitTypeId::Rifleman).max_hp - hp_of(sim, man) >= grenade ? 1 : 0;
    }
    CHECK(hits >= 7);
}

// --- Structures --------------------------------------------------------------

// A 2x2 house at tiles (10..11, 10..11) and a river at x = 20 with a
// three-tile bridge at y = 9..11.
TileMap village_map() {
    TileMap map(40, 24);
    for (int y = 10; y <= 11; ++y) {
        for (int x = 10; x <= 11; ++x) map.set_terrain(x, y, Terrain::House);
    }
    for (int y = 0; y < 24; ++y) map.set_terrain(20, y, y >= 9 && y <= 11 ? Terrain::Bridge : Terrain::Water);
    return map;
}

Command garrison(PlayerId player, std::vector<EntityId> units, EntityId structure) {
    Command cmd;
    cmd.type = CommandType::Garrison;
    cmd.player = player;
    cmd.units = std::move(units);
    cmd.target_unit = structure;
    return cmd;
}

EntityId house_id(const Simulation& sim) { return sim.world().structure_at({10, 10})->id; }

void test_structures_come_from_the_map() {
    Simulation sim(1, village_map());
    CHECK(sim.world().structures().size() == 2);
    const Structure* house = sim.world().structure_at({11, 11});
    CHECK(house && house->type == StructureType::House && house->tiles.size() == 4);
    CHECK(house && house->hp == structure_type(StructureType::House).max_hp);
    const Structure* bridge = sim.world().structure_at({20, 10});
    CHECK(bridge && bridge->type == StructureType::Bridge && bridge->tiles.size() == 3);
}

// Six soldiers fit; the seventh stays outside; tanks don't go in at all.
void test_infantry_garrisons_a_house() {
    Simulation sim(1, village_map());
    std::vector<EntityId> squad;
    for (int i = 0; i < 7; ++i) squad.push_back(sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(4, 6 + i)));
    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(4, 16));
    squad.push_back(tank);
    issue(sim, garrison(0, squad, house_id(sim)));
    for (int i = 0; i < 400; ++i) sim.step();

    const Structure* house = sim.world().find_structure(house_id(sim));
    CHECK(house->garrison.size() == 6);
    CHECK(house->owner == 0);
    int outside = 0;
    for (EntityId id : squad) {
        const Unit* u = sim.world().find_unit(id);
        if (u->inside == house->id) CHECK(u->pos == house->center);
        if (!u->inside && u->type == UnitTypeId::Rifleman) ++outside;
    }
    CHECK(outside == 1);
    CHECK(sim.world().find_unit(tank)->order == Order::Idle);
}

// Walls stop bullets: the garrison shoots back unharmed.
void test_garrison_is_safe_from_bullets() {
    Simulation sim(1, village_map());
    const EntityId soldier = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(8, 11));
    sim.schedule(0, garrison(0, {soldier}, house_id(sim)));
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(sim.world().find_unit(soldier)->inside == house_id(sim));

    const EntityId mg = sim.world_for_setup().spawn_unit(1, UnitTypeId::MachineGunner, at(16, 11));
    for (int i = 0; i < 400; ++i) sim.step();
    CHECK(hp_of(sim, soldier) == unit_type(UnitTypeId::Rifleman).max_hp);
    CHECK(hp_of(sim, mg) < unit_type(UnitTypeId::MachineGunner).max_hp);  // fired back from the windows
}

// Shelled long enough, the house comes down on everyone inside.
void test_house_collapse_kills_the_garrison() {
    Simulation sim(1, village_map());
    const EntityId soldier = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(8, 11));
    const EntityId house = house_id(sim);
    sim.schedule(0, garrison(0, {soldier}, house));
    for (int i = 0; i < 100; ++i) sim.step();

    const EntityId tank = sim.world_for_setup().spawn_unit(1, UnitTypeId::Tank, at(17, 11));
    issue(sim, fire_at(1, {tank}, 11, 11));
    for (int i = 0; i < 1500 && sim.world().find_structure(house); ++i) sim.step();
    sim.step();

    CHECK(sim.world().find_structure(house) == nullptr);
    CHECK(sim.world().find_unit(soldier) == nullptr);
    CHECK(sim.world().map().terrain(10, 10) == Terrain::Ruins);
    CHECK(sim.world().map().passable({10, 10}, MoveClass::Vehicle));  // tanks can roll over ruins
}

// A blown bridge is river again: no more crossing for vehicles.
void test_bridge_can_be_blown() {
    Simulation sim(1, village_map());
    const EntityId bridge = sim.world().structure_at({20, 10})->id;
    CHECK((FlowField(sim.world().map(), {15, 10}, MoveClass::Vehicle).reachable({25, 10})));

    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(26, 10));
    sim.schedule(0, fire_at(0, {tank}, 20, 10));
    for (int i = 0; i < 4000 && sim.world().find_structure(bridge); ++i) sim.step();

    CHECK(sim.world().find_structure(bridge) == nullptr);
    CHECK(sim.world().map().terrain(20, 10) == Terrain::Water);
    CHECK(!(FlowField(sim.world().map(), {15, 10}, MoveClass::Vehicle).reachable({25, 10})));
}

// Any order to go somewhere takes the soldier out of the house first.
void test_garrison_leaves_on_move() {
    Simulation sim(1, village_map());
    const EntityId soldier = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(8, 11));
    sim.schedule(0, garrison(0, {soldier}, house_id(sim)));
    for (int i = 0; i < 100; ++i) sim.step();
    issue(sim, make_move(0, {soldier}, 4, 4));
    for (int i = 0; i < 10; ++i) sim.step();

    const Unit* u = sim.world().find_unit(soldier);
    CHECK(u->inside == 0 && u->order == Order::Move);
    const Structure* house = sim.world().find_structure(house_id(sim));
    CHECK(house->garrison.empty() && house->owner == kNoOwner);
}

// --- Economy -----------------------------------------------------------------

// A base: headquarters on tiles (5..7, 9..11), a forest strip at x = 12..13,
// a stone outcrop on (5..6, 15..16).
Simulation economy_sim(const Stock& stock, bool forest_strip = true) {
    TileMap map(40, 24);
    if (forest_strip) {
        for (int y = 6; y <= 14; ++y) {
            map.set_terrain(12, y, Terrain::Forest);
            map.set_terrain(13, y, Terrain::Forest);
        }
    }
    for (int y = 15; y <= 16; ++y) {
        for (int x = 5; x <= 6; ++x) map.set_terrain(x, y, Terrain::Rock);
    }
    Simulation sim(1, map);
    sim.world_for_setup().place_structure(StructureType::Headquarters, 0, {5, 9}, 3, 3);
    sim.world_for_setup().set_stock(0, stock);
    return sim;
}

int32_t stock_of(const Simulation& sim, Resource r) { return sim.world().stock(0)[static_cast<size_t>(r)]; }

// The depot of ours a truck standing at `pos` is unloading at.
EntityId nearest_depot_id(const Simulation& sim, FixedVec2 pos) {
    const Structure* s = sim.world().nearest_owned(0, StructureType::AmmoDepot, pos);
    return s ? s->id : 0;
}

std::vector<EntityId> spawn_workers(Simulation& sim, int count, int32_t x, int32_t y) {
    std::vector<EntityId> ids;
    for (int i = 0; i < count; ++i) ids.push_back(sim.world_for_setup().spawn_unit(0, UnitTypeId::Worker, at(x, y + i)));
    return ids;
}

Command gather_at(std::vector<EntityId> units, int32_t x, int32_t y) {
    return make_order(CommandType::Gather, 0, std::move(units), x, y);
}

EntityId headquarters_of(const Simulation& sim) { return sim.world().structure_at({6, 10})->id; }

void test_rear_troops_cut_timber() {
    Simulation sim = economy_sim({});
    issue(sim, gather_at(spawn_workers(sim, 3, 9, 9), 12, 10));
    for (int i = 0; i < 1200; ++i) sim.step();

    const int32_t delivered = stock_of(sim, Resource::Materials);
    int32_t cut = 0;
    for (int y = 6; y <= 14; ++y) {
        for (int x = 12; x <= 13; ++x) cut += kForestMaterials - sim.world().map().resource({x, y});
    }
    CHECK(delivered >= 30);
    CHECK(cut >= delivered);  // what's in hands isn't counted yet
}

// Cut a forest tile down to nothing and it is a field: the cover is gone.
void test_cut_down_forest_becomes_field() {
    TileMap map(40, 24);
    map.set_terrain(12, 10, Terrain::Forest);
    Simulation sim(1, map);
    sim.world_for_setup().place_structure(StructureType::Headquarters, 0, {5, 9}, 3, 3);
    issue(sim, gather_at(spawn_workers(sim, 5, 9, 8), 12, 10));
    for (int i = 0; i < 6000; ++i) sim.step();

    CHECK(sim.world().map().terrain(12, 10) == Terrain::Grass);
    CHECK(stock_of(sim, Resource::Materials) == kForestMaterials);  // every last log delivered
    for (const Unit& u : sim.world().units()) CHECK(u.order == Order::Idle && u.carrying == 0);
}

// A wood far from the headquarters (a forest at x 45..50), three rear
// troops at its edge; `truck`: one of our supply trucks sent to collect
// there. Materials in the stock after `ticks`, and whether any of the men
// walked back near the headquarters.
std::pair<int32_t, bool> timber_run(bool truck, int ticks, int men = 3) {
    TileMap map(60, 30);
    for (int y = 10; y <= 20; ++y) {
        for (int x = 45; x <= 50; ++x) map.set_terrain(x, y, Terrain::Forest);
    }
    Simulation sim(1, map);
    World& w = sim.world_for_setup();
    w.place_structure(StructureType::Headquarters, 0, {3, 12}, 3, 3);
    std::vector<EntityId> crew;
    for (int i = 0; i < men; ++i) crew.push_back(w.spawn_unit(0, UnitTypeId::Worker, at(43, 13 + 2 * i)));
    issue(sim, gather_at(crew, 45, 15));
    if (truck) {
        const EntityId t = w.spawn_unit(0, UnitTypeId::Truck, at(40, 15));
        issue(sim, make_order(CommandType::Collect, 0, {t}, 45, 15));
    }
    bool walked_home = false;
    for (int i = 0; i < ticks; ++i) {
        sim.step();
        for (EntityId id : crew) walked_home = walked_home || sim.world().find_unit(id)->pos.x < Fixed::from_int(20);
    }
    return {stock_of(sim, Resource::Materials), walked_home};
}

// Rear troops cut at the wood and walk their loads to the headquarters; with
// a truck parked by the wood they hand the loads to it instead, and it takes
// them in 40 at a time: more timber in the same time.
void test_trucks_collect_timber() {
    const auto [on_foot, walked] = timber_run(false, 4000);
    const auto [by_truck, walked_too] = timber_run(true, 4000);
    CHECK(on_foot > 0 && walked);
    CHECK(!walked_too);
    CHECK(by_truck > on_foot);
    CHECK(by_truck % kTruckCapacity == 0);  // it waits for a full bed while the men keep bringing
    const auto [one_man, walked_alone] = timber_run(true, 6000, 1);  // slow going, but loads keep coming
    CHECK(one_man > 0 && one_man % kTruckCapacity == 0 && !walked_alone);

    // Nobody bringing anything for a while: it takes in what it has.
    TileMap map(40, 20);
    Simulation sim(1, map);
    sim.world_for_setup().place_structure(StructureType::Headquarters, 0, {3, 8}, 3, 3);
    const EntityId t = sim.world_for_setup().spawn_unit(0, UnitTypeId::Truck, at(30, 10));
    issue(sim, make_order(CommandType::Collect, 0, {t}, 30, 10));
    for (int i = 0; i < 10; ++i) sim.step();
    Unit* truck = sim.world_for_setup().unit_for_setup(t);
    truck->carrying = 15;
    truck->carrying_type = Resource::Materials;
    for (Tick i = 0; i < kCollectPatience + 1000 && stock_of(sim, Resource::Materials) == 0; ++i) sim.step();
    CHECK(stock_of(sim, Resource::Materials) == 15);
    for (int i = 0; i < 1000; ++i) sim.step();
    CHECK((sim.world().find_unit(t)->pos - at(30, 10)).length() < Fixed::from_int(1));  // back at its spot

    // Only supply trucks collect, and not with other freight aboard.
    const EntityId tanker = sim.world_for_setup().spawn_unit(0, UnitTypeId::FuelTanker, at(20, 10));
    sim.world_for_setup().unit_for_setup(tanker)->carrying = 0;  // empty, and still no
    const EntityId loaded = sim.world_for_setup().spawn_unit(0, UnitTypeId::Truck, at(21, 12));
    sim.world_for_setup().unit_for_setup(loaded)->carrying = 20;
    sim.world_for_setup().unit_for_setup(loaded)->carrying_type = Resource::Food;
    issue(sim, make_order(CommandType::Collect, 0, {tanker, loaded}, 30, 10));
    for (int i = 0; i < 5; ++i) sim.step();
    CHECK(sim.world().find_unit(tanker)->order != Order::Collect);
    CHECK(sim.world().find_unit(loaded)->order != Order::Collect);
}

// A rally point: the headquarters sends the rear troops it hires to cut the
// wood the point is on, and trucks to collect there; a barracks sends its
// riflemen to a point. Only for our own buildings that hire anyone.
void test_rally_points() {
    Simulation sim = economy_sim({10, 200, 200, 200, 200});
    World& w = sim.world_for_setup();
    const EntityId hq = sim.world().structure_at({6, 10})->id;
    const EntityId barracks = w.place_structure(StructureType::InfantryBarracks, 0, {20, 16}, 3, 3);
    const EntityId store = w.place_structure(StructureType::Warehouse, 0, {26, 4}, 2, 2);
    Command rally{.type = CommandType::Rally, .player = 0, .target = tile_center({12, 8}), .target_unit = hq};
    issue(sim, rally);
    Command drill{.type = CommandType::Rally, .player = 0, .target = at(30, 20), .target_unit = barracks};
    issue(sim, drill);
    Command foreign{.type = CommandType::Rally, .player = 1, .target = at(2, 2), .target_unit = barracks};
    issue(sim, foreign);
    Command nobody{.type = CommandType::Rally, .player = 0, .target = at(2, 2), .target_unit = store};
    issue(sim, nobody);
    sim.step();
    sim.step();
    sim.step();
    CHECK(sim.world().find_structure(barracks)->rally == at(30, 20));  // not the enemy's point
    CHECK(!sim.world().find_structure(store)->rally_set);              // hires nobody

    auto train = [&](EntityId at_building, UnitTypeId type) {
        issue(sim, Command{.type = CommandType::Train, .player = 0, .target_unit = at_building,
                           .unit_type = static_cast<uint8_t>(type)});
    };
    train(hq, UnitTypeId::Worker);
    train(hq, UnitTypeId::Truck);
    train(barracks, UnitTypeId::Rifleman);
    for (int i = 0; i < 1500; ++i) sim.step();
    int gathering = 0;
    int collecting = 0;
    bool rifle_there = false;
    for (const Unit& u : sim.world().units()) {
        if (u.type == UnitTypeId::Worker && u.order == Order::Gather) ++gathering;
        if (u.type == UnitTypeId::Truck && u.order == Order::Collect) ++collecting;
        if (u.type == UnitTypeId::Rifleman) rifle_there = (u.pos - at(30, 20)).length() < Fixed::from_int(2);
    }
    CHECK(gathering == 1 && collecting == 1 && rifle_there);
}

// Infantry mounts up in our IFV: seven men at most, gun crews walk, the
// enemy's can't get in. Aboard, they ride along unseen and unhurt; told to
// dismount, the IFV stops and they get out at the back.
void test_ifv_carries_squad() {
    TileMap map(50, 30);
    Simulation sim(1, map);
    World& w = sim.world_for_setup();
    const EntityId ifv = w.spawn_unit(0, UnitTypeId::Ifv, at(20, 15));
    std::vector<EntityId> squad;
    for (int i = 0; i < 8; ++i) squad.push_back(w.spawn_unit(0, UnitTypeId::Rifleman, at(12 + (i % 2), 11 + i)));
    const EntityId mortar = w.spawn_unit(0, UnitTypeId::Mortar, at(14, 20));
    const EntityId stranger = w.spawn_unit(1, UnitTypeId::Rifleman, at(2, 28));
    std::vector<EntityId> all = squad;
    all.push_back(mortar);
    issue(sim, Command{.type = CommandType::Garrison, .player = 0, .units = all, .target_unit = ifv});
    issue(sim, Command{.type = CommandType::Garrison, .player = 1, .units = {stranger}, .target_unit = ifv});
    for (int i = 0; i < 10; ++i) sim.step();
    CHECK(sim.world().find_unit(ifv)->passengers.empty());  // they walk up to it first
    for (int i = 0; i < 590; ++i) sim.step();
    const Unit* v = sim.world().find_unit(ifv);
    CHECK(v->passengers.size() == 7);
    int aboard = 0;
    for (EntityId id : squad) aboard += sim.world().find_unit(id)->inside == ifv ? 1 : 0;
    CHECK(aboard == 7);  // the eighth: no room, he waits beside it
    CHECK(sim.world().find_unit(mortar)->inside == 0);
    CHECK(sim.world().find_unit(stranger)->inside == 0);
    const std::vector<EntityId> riders = v->passengers;

    // Riding along, past an enemy machine gunner shooting at it: the squad unhurt.
    const EntityId gunner = sim.world_for_setup().spawn_unit(1, UnitTypeId::MachineGunner, at(30, 19));
    issue(sim, make_move(0, {ifv}, 38, 15));
    for (int i = 0; i < 700; ++i) sim.step();
    v = sim.world().find_unit(ifv);
    CHECK((v->pos - at(38, 15)).length() < Fixed::from_int(2));
    CHECK(v->hp < unit_type(UnitTypeId::Ifv).max_hp);  // it was under fire
    for (EntityId id : riders) {
        const Unit* p = sim.world().find_unit(id);
        CHECK(p && p->pos == v->pos && p->hp == unit_type(UnitTypeId::Rifleman).max_hp);
        CHECK(p && p->last_shot_tick == kNeverFired);  // they don't fire from inside
    }
    if (Unit* g = sim.world_for_setup().unit_for_setup(gunner)) g->hp = 0;

    // Dismount on the move: it stops, they get out behind it.
    issue(sim, make_move(0, {ifv}, 45, 15));
    for (int i = 0; i < 20; ++i) sim.step();
    issue(sim, Command{.type = CommandType::Unload, .player = 1, .units = {ifv}});
    for (int i = 0; i < 3; ++i) sim.step();
    CHECK(sim.world().find_unit(ifv)->passengers.size() == 7);  // not the enemy's to order
    issue(sim, Command{.type = CommandType::Unload, .player = 0, .units = {ifv}});
    for (int i = 0; i < 3; ++i) sim.step();
    v = sim.world().find_unit(ifv);
    CHECK(v->passengers.empty() && v->order == Order::Idle);
    const FixedVec2 stop = v->pos;
    for (EntityId id : riders) {
        const Unit* p = sim.world().find_unit(id);
        CHECK(p && p->inside == 0 && p->order == Order::Idle);
        if (!p) continue;
        const FixedVec2 off = p->pos - v->pos;
        CHECK((off.x * v->facing.x + off.y * v->facing.y).raw < 0);  // behind it
    }
    for (int i = 0; i < 60; ++i) sim.step();
    CHECK((sim.world().find_unit(ifv)->pos - stop).length() < Fixed::from_ratio(1, 2));

    // Seats free now, but the mortar crew walks with its mortar.
    issue(sim, Command{.type = CommandType::Garrison, .player = 0, .units = {mortar}, .target_unit = ifv});
    for (int i = 0; i < 1500; ++i) sim.step();
    CHECK(sim.world().find_unit(mortar)->inside == 0 && sim.world().find_unit(ifv)->passengers.empty());
}

// Mounting up with an IFV that drives off: they follow and get in. Knocked
// out with them aboard, the squad bails out, each losing half his health;
// a wounded man doesn't make it.
void test_ifv_bail_out() {
    TileMap map(50, 30);
    Simulation sim(1, map);
    World& w = sim.world_for_setup();
    const EntityId ifv = w.spawn_unit(0, UnitTypeId::Ifv, at(20, 15));
    const EntityId fit = w.spawn_unit(0, UnitTypeId::Rifleman, at(10, 14));
    const EntityId hurt = w.spawn_unit(0, UnitTypeId::Rifleman, at(10, 16));
    w.unit_for_setup(hurt)->hp = 15;
    issue(sim, Command{.type = CommandType::Garrison, .player = 0, .units = {fit, hurt}, .target_unit = ifv});
    issue(sim, make_move(0, {ifv}, 30, 20));
    for (int i = 0; i < 1200; ++i) sim.step();
    CHECK(sim.world().find_unit(ifv)->passengers.size() == 2);
    CHECK((sim.world().find_unit(ifv)->pos - at(30, 20)).length() < Fixed::from_int(2));

    sim.world_for_setup().unit_for_setup(ifv)->hp = 0;
    sim.step();
    CHECK(sim.world().find_unit(ifv) == nullptr);
    const Unit* survivor = sim.world().find_unit(fit);
    CHECK(survivor && survivor->inside == 0 && survivor->hp == unit_type(UnitTypeId::Rifleman).max_hp / 2);
    CHECK(sim.world().find_unit(hurt) == nullptr);
}

// Housing, like AoE's houses: the headquarters has bunks for 30 men, each
// living quarters (built by rear troops) for 10 more, up to 200. Hiring
// waits for a free bunk; the one in training has his already.
void test_quarters_house_the_men() {
    CHECK(std::find(std::begin(kBuildable), std::end(kBuildable), StructureType::Quarters) != std::end(kBuildable));
    Simulation sim = economy_sim({50, 2000, 2000, 2000, 2000}, false);
    World& w = sim.world_for_setup();
    const EntityId hq = sim.world().structure_at({6, 10})->id;
    const EntityId barracks = w.place_structure(StructureType::InfantryBarracks, 0, {20, 16}, 3, 3);
    CHECK(sim.world().bunks(0) == kHeadquartersBunks && sim.world().population(0) == 0);
    for (int i = 0; i < 28; ++i) w.spawn_unit(0, UnitTypeId::Rifleman, at(30 + i % 5, 2 + i / 5));
    w.spawn_unit(0, UnitTypeId::Worker, at(10, 20));
    CHECK(sim.world().population(0) == 29);

    auto hire = [&](EntityId at_building, UnitTypeId type) {
        issue(sim, Command{.type = CommandType::Train, .player = 0, .target_unit = at_building,
                           .unit_type = static_cast<uint8_t>(type)});
    };
    auto count = [&](UnitTypeId type) {
        int n = 0;
        for (const Unit& u : sim.world().units()) n += u.type == type ? 1 : 0;
        return n;
    };
    // One bunk left: the headquarters' rear trooper takes it; the barracks'
    // rifleman, hired at the same time, and the second rear trooper wait.
    hire(hq, UnitTypeId::Worker);
    hire(hq, UnitTypeId::Worker);
    hire(barracks, UnitTypeId::Rifleman);
    for (int i = 0; i < 3; ++i) sim.step();
    CHECK(sim.world().population(0) == 30);  // the one in training has his bunk
    CHECK(!sim.world().waits_for_bunks(*sim.world().find_structure(hq)));  // it's training him
    CHECK(sim.world().waits_for_bunks(*sim.world().find_structure(barracks)));
    for (Tick i = 0; i < 3 * unit_type(UnitTypeId::Rifleman).train_time; ++i) sim.step();
    CHECK(count(UnitTypeId::Worker) == 2 && count(UnitTypeId::Rifleman) == 28);
    CHECK(sim.world().population(0) == 30);  // the ones waiting have none yet
    CHECK(sim.world().waits_for_bunks(*sim.world().find_structure(hq)));
    CHECK(sim.world().waits_for_bunks(*sim.world().find_structure(barracks)));

    // Living quarters: ten more bunks, and both come out.
    const EntityId quarters = w.place_structure(StructureType::Quarters, 0, {26, 16}, 2, 2);
    CHECK(sim.world().bunks(0) == kHeadquartersBunks + kQuartersBunks);
    for (Tick i = 0; i < unit_type(UnitTypeId::Rifleman).train_time + 5; ++i) sim.step();
    CHECK(count(UnitTypeId::Worker) == 3 && count(UnitTypeId::Rifleman) == 29);
    CHECK(!sim.world().waits_for_bunks(*sim.world().find_structure(hq)));

    // Burnt down: nobody is sent away, but no more hiring.
    w.structure_for_setup(quarters)->hp = 0;
    sim.step();
    CHECK(sim.world().bunks(0) == kHeadquartersBunks && count(UnitTypeId::Worker) == 3);
    hire(barracks, UnitTypeId::Rifleman);
    for (Tick i = 0; i < 2 * unit_type(UnitTypeId::Rifleman).train_time; ++i) sim.step();
    CHECK(count(UnitTypeId::Rifleman) == 29);

    // Quarters still going up don't house anyone; the enemy's don't house ours.
    const EntityId worker = w.spawn_unit(0, UnitTypeId::Worker, at(30, 20));
    issue(sim, Command{.type = CommandType::Build, .player = 0, .units = {worker}, .target = tile_center({31, 20}),
                       .structure_type = static_cast<uint8_t>(StructureType::Quarters)});
    w.place_structure(StructureType::Quarters, 1, {36, 20}, 2, 2);
    for (int i = 0; i < 100; ++i) sim.step();
    bool site = false;
    for (const Structure& s : sim.world().structures()) site = site || (s.type == StructureType::Quarters && !s.built);
    CHECK(site && sim.world().bunks(0) == kHeadquartersBunks);

    // Up to 200 at most.
    for (int i = 0; i < 20; ++i) w.place_structure(StructureType::Quarters, 0, {2 + 2 * (i % 10), 20 + 2 * (i / 10)}, 2, 2);
    CHECK(sim.world().bunks(0) == kMaxPopulation);
}

// A tank at (5, 10) fires armor-piercing rounds at an enemy tank `distance`
// tiles east that can't answer, spotted by a scout of ours: its hits out
// of `shots`, and whether it fired from where it stands.
std::pair<int, bool> tank_hits(int32_t distance, int shots, bool fire_control = false) {
    TileMap map(70, 20);
    Simulation sim(1, map);
    World& w = sim.world_for_setup();
    if (fire_control) w.upgrade_for_setup(0, UpgradeId::FireControl);
    const EntityId tank = w.spawn_unit(0, UnitTypeId::Tank, at(5, 10));
    w.unit_for_setup(tank)->round_type = 1;  // armor-piercing: no burst to count
    const EntityId target = w.spawn_unit(1, UnitTypeId::Tank, at(5 + distance, 10));
    w.unit_for_setup(target)->rounds = 0;
    w.unit_for_setup(target)->hp = 1000000;
    // Spotting, not shooting, and off to the side, clear of the misses.
    const EntityId scout = w.spawn_unit(0, UnitTypeId::Scout, at(5 + distance - 6, 15));
    w.unit_for_setup(scout)->rounds = 0;
    issue(sim, attack_order(0, {tank}, target));
    int hits = 0;
    int32_t hp = 1000000;
    auto step = [&] {
        sim.step();
        const int32_t now = hp_of(sim, target);
        if (hp - now >= 100) ++hits;
        hp = now;
    };
    const int32_t rounds = unit_type(UnitTypeId::Tank).rounds_capacity;
    for (int i = 0; i < 5000 && rounds - sim.world().find_unit(tank)->rounds < shots; ++i) step();
    for (int i = 0; i < 100; ++i) step();  // the last one lands
    return {hits, (sim.world().find_unit(tank)->pos - at(5, 10)).length() < Fixed::from_int(1)};
}

// A tank reaches ten times as far as a rifleman: with someone spotting, it
// fires 45 tiles out from where it stands. Past 15 tiles its aim falls off:
// far fewer hits than close in. It sees 12 tiles itself.
void test_tank_reaches_far() {
    CHECK(unit_type(UnitTypeId::Tank).weapon.range == unit_type(UnitTypeId::Rifleman).weapon.range * 10);
    CHECK(unit_type(UnitTypeId::Tank).alt_weapon.range == unit_type(UnitTypeId::Tank).weapon.range);
    const auto [near, stood_near] = tank_hits(10, 16);
    const auto [far, stood_far] = tank_hits(45, 16);
    CHECK(stood_near && stood_far);
    CHECK(far > 0);
    CHECK(far * 3 < near * 2);

    // An enemy in the open 11 tiles off: the tank sees him and opens up.
    TileMap map(40, 20);
    Simulation sim(1, map);
    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 10));
    sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(16, 10));
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(sim.world().find_unit(tank)->last_shot_tick != kNeverFired);

    // New ones stand facing the middle of the map; the hull turns the way it drives.
    const EntityId east = sim.world_for_setup().spawn_unit(1, UnitTypeId::Tank, at(35, 3));
    CHECK(sim.world().find_unit(east)->hull.x.raw < 0);
    issue(sim, make_move(1, {east}, 35, 18));
    for (int i = 0; i < 60; ++i) sim.step();
    const Unit* driven = sim.world().find_unit(east);
    CHECK(driven->hull.y.raw > 2 * std::abs(driven->hull.x.raw));  // south
}

// A grenadier with a spotter by him, 9 tiles from an enemy tank that can't
// answer, its hull pointing `hull`; hidden in the trees if `in_trees`. The
// tank's health after each hit (0: knocked out), and whether he fired from
// where he stands.
std::pair<std::vector<int32_t>, bool> rpg_hits(FixedVec2 hull, bool in_trees, int32_t distance,
                                               UpgradeId upgrade = UpgradeId::Count) {
    TileMap map(40, 20);
    if (in_trees) {
        for (int y = 8; y <= 12; ++y) {
            for (int x = 8; x <= 11; ++x) map.set_terrain(x, y, Terrain::Forest);
        }
    }
    Simulation sim(1, map);
    World& w = sim.world_for_setup();
    const EntityId rpg = w.spawn_unit(0, UnitTypeId::Grenadier, at(10, 10));
    const EntityId scout = w.spawn_unit(0, UnitTypeId::Scout, at(13, 12));  // out of the trees, to see
    w.unit_for_setup(scout)->rounds = 0;
    const EntityId tank = w.spawn_unit(1, UnitTypeId::Tank, at(10 + distance, 10));
    w.unit_for_setup(tank)->rounds = 0;
    w.unit_for_setup(tank)->hull = hull;
    if (upgrade != UpgradeId::Count) w.upgrade_for_setup(1, upgrade);
    issue(sim, attack_order(0, {rpg}, tank));
    std::vector<int32_t> after;
    int32_t hp = unit_type(UnitTypeId::Tank).max_hp;
    for (int i = 0; i < 2000 && hp > 0; ++i) {
        sim.step();
        const int32_t now = sim.world().find_unit(tank) ? hp_of(sim, tank) : 0;
        if (now < hp) after.push_back(std::max(0, now));
        hp = now;
    }
    return {after, (sim.world().find_unit(rpg)->pos - at(10, 10)).length() < Fixed::from_int(1)};
}

// A grenadier reaches twice as far as a rifleman: he has to catch a tank.
// Two hits in the front knock it out; from the trees by its road, one into
// its side does. Against a house a shaped charge punches a hole, no more.
void test_rpg_catches_tanks() {
    CHECK(unit_type(UnitTypeId::Grenadier).weapon.range == unit_type(UnitTypeId::Rifleman).weapon.range * 2);
    const auto [front, stood] = rpg_hits({Fixed::from_int(-1), Fixed{}}, false, 9);  // facing him
    CHECK(stood);
    CHECK(front.size() == 2 && front[0] == unit_type(UnitTypeId::Tank).max_hp / 2 && front[1] == 0);
    const auto [side, stood_side] = rpg_hits({Fixed{}, Fixed::from_int(-1)}, true, 5);  // driving past
    CHECK(side.size() == 1 && side[0] == 0);
    const auto [rear, stood_rear] = rpg_hits({Fixed::from_int(1), Fixed{}}, true, 5);  // driving away
    CHECK(rear.size() == 1 && rear[0] == 0);

    TileMap map(40, 20);
    Simulation sim(1, map);
    World& w = sim.world_for_setup();
    const EntityId rpg = w.spawn_unit(0, UnitTypeId::Grenadier, at(10, 5));
    const EntityId store = w.place_structure(StructureType::Warehouse, 1, {15, 4}, 2, 2);
    issue(sim, fire_at(0, {rpg}, 16, 5));
    const int32_t full = sim.world().find_structure(store)->hp;
    int32_t first = 0;
    for (int i = 0; i < 1000 && first == 0; ++i) {
        sim.step();
        first = full - sim.world().find_structure(store)->hp;
    }
    const WeaponDef& rocket = unit_type(UnitTypeId::Grenadier).weapon;
    CHECK(first == rocket.structure_damage -
                       structure_type(StructureType::Warehouse).armor[static_cast<size_t>(DamageType::AntiTank)]);
}

void test_rear_troops_quarry_stone() {
    Simulation sim = economy_sim({}, false);
    issue(sim, gather_at(spawn_workers(sim, 2, 9, 13), 5, 15));
    for (int i = 0; i < 1500; ++i) sim.step();
    CHECK(stock_of(sim, Resource::Materials) >= 10);
    CHECK(sim.world().map().resource({5, 15}) < kRockMaterials);
    CHECK(sim.world().map().terrain(5, 15) == Terrain::Rock);  // stone is impassable: worked from beside it
}

// Hiring costs men and food up front; without them nothing is queued.
void test_headquarters_trains_rear_troops() {
    const Stock one_worker = unit_type(UnitTypeId::Worker).cost;
    Simulation sim = economy_sim(one_worker);
    Command train{.type = CommandType::Train, .player = 0, .target_unit = headquarters_of(sim),
                  .unit_type = static_cast<uint8_t>(UnitTypeId::Worker)};
    issue(sim, train);
    issue(sim, train);  // can't afford a second one
    for (int i = 0; i < 3; ++i) sim.step();
    CHECK(sim.world().find_structure(headquarters_of(sim))->queue.size() == 1);
    CHECK(sim.world().stock(0) == Stock{});

    for (Tick i = 0; i < unit_type(UnitTypeId::Worker).train_time; ++i) sim.step();
    CHECK(sim.world().units().size() == 1);
    const Unit& fresh = sim.world().units().front();
    CHECK(fresh.type == UnitTypeId::Worker && fresh.owner == 0);
    CHECK(sim.world().map().passable(tile_of(fresh.pos), MoveClass::Foot));  // out of the door

    // Headquarters don't train tanks.
    Command tank = train;
    tank.unit_type = static_cast<uint8_t>(UnitTypeId::Tank);
    sim.world_for_setup().set_stock(0, unit_type(UnitTypeId::Tank).cost);
    issue(sim, tank);
    for (int i = 0; i < 3; ++i) sim.step();
    CHECK(sim.world().find_structure(headquarters_of(sim))->queue.empty());
}

// The same man comes out a rifleman; it costs ammunition, not personnel.
void test_rear_troops_retrain_as_riflemen() {
    Stock ammo_for_one{};
    ammo_for_one[static_cast<size_t>(Resource::Ammo)] = 20;
    Simulation sim = economy_sim(ammo_for_one);
    const std::vector<EntityId> pair = spawn_workers(sim, 2, 10, 9);
    Command retrain{.type = CommandType::Retrain, .player = 0, .units = pair};
    issue(sim, retrain);
    for (int i = 0; i < 600; ++i) sim.step();

    int riflemen = 0;
    int workers = 0;
    for (EntityId id : pair) {
        const Unit* u = sim.world().find_unit(id);
        CHECK(u && u->inside == 0 && u->order == Order::Idle);
        if (u && u->type == UnitTypeId::Rifleman) ++riflemen;
        if (u && u->type == UnitTypeId::Rifleman) CHECK(u->rounds == unit_type(UnitTypeId::Rifleman).rounds_capacity);
        if (u && u->type == UnitTypeId::Worker) ++workers;
    }
    CHECK(riflemen == 1 && workers == 1);  // ammo for one only
    CHECK(sim.world().stock(0) == Stock{});
}

Stock stock_with(std::initializer_list<std::pair<Resource, int32_t>> amounts) {
    Stock s{};
    for (const auto& [r, amount] : amounts) s[static_cast<size_t>(r)] = amount;
    return s;
}

Command build_at(std::vector<EntityId> builders, StructureType type, int32_t x, int32_t y) {
    Command cmd = make_order(CommandType::Build, 0, std::move(builders), x, y);
    cmd.structure_type = static_cast<uint8_t>(type);
    return cmd;
}

// Ticks until the structure on `tile` is finished (or max_ticks).
int build_until_done(Simulation& sim, TilePos tile, int max_ticks) {
    int ticks = 0;
    for (; ticks < max_ticks; ++ticks) {
        const Structure* s = sim.world().structure_at(tile);
        if (s && s->built) break;
        sim.step();
    }
    return ticks;
}

// Rear troops put up an infantry barracks; then it hires riflemen (not tanks).
void test_rear_troops_build_barracks() {
    Simulation sim = economy_sim(stock_with({{Resource::Materials, 200},
                                             {Resource::Personnel, 5},
                                             {Resource::Food, 100},
                                             {Resource::Ammo, 100}}),
                                 false);
    const std::vector<EntityId> crew = spawn_workers(sim, 2, 12, 12);
    issue(sim, build_at(crew, StructureType::InfantryBarracks, 15, 12));
    for (int i = 0; i < 3; ++i) sim.step();

    const Structure* site = sim.world().structure_at({16, 13});
    CHECK(site && !site->built && site->hp < structure_type(StructureType::InfantryBarracks).max_hp);
    CHECK(stock_of(sim, Resource::Materials) == 50);  // paid up front
    CHECK(!sim.world().map().passable({16, 13}, MoveClass::Foot));

    build_until_done(sim, {16, 13}, 3000);
    sim.step();  // the builders notice on their next update
    const Structure* barracks = sim.world().structure_at({16, 13});
    CHECK(barracks && barracks->built);
    CHECK(barracks && barracks->hp == structure_type(StructureType::InfantryBarracks).max_hp);
    for (EntityId id : crew) CHECK(sim.world().find_unit(id)->order == Order::Idle);

    const EntityId id = barracks->id;
    Command hire{.type = CommandType::Train, .player = 0, .target_unit = id,
                 .unit_type = static_cast<uint8_t>(UnitTypeId::Rifleman)};
    issue(sim, hire);
    hire.unit_type = static_cast<uint8_t>(UnitTypeId::Tank);  // not in this barracks
    issue(sim, hire);
    for (Tick i = 0; i < unit_type(UnitTypeId::Rifleman).train_time + 5; ++i) sim.step();
    int riflemen = 0;
    for (const Unit& u : sim.world().units()) riflemen += u.type == UnitTypeId::Rifleman ? 1 : 0;
    CHECK(riflemen == 1);
    CHECK(sim.world().find_structure(id)->queue.empty());
}

// Two rear troops build twice as fast as one.
void test_more_builders_build_faster() {
    auto time_with = [](int builders) {
        Simulation sim = economy_sim(stock_with({{Resource::Materials, 100}}), false);
        issue(sim, build_at(spawn_workers(sim, builders, 14, 14), StructureType::Warehouse, 16, 14));
        return build_until_done(sim, {16, 14}, 4000);
    };
    const int one = time_with(1);
    const int two = time_with(2);
    CHECK(one < 4000);
    CHECK(two * 10 < one * 7);
}

void test_building_placement_rules() {
    Simulation sim = economy_sim(stock_with({{Resource::Materials, 1000}}));
    const World& w = sim.world();
    CHECK(w.can_place(StructureType::InfantryBarracks, {20, 5}));
    CHECK(!w.can_place(StructureType::InfantryBarracks, {11, 8}));  // into the forest
    CHECK(!w.can_place(StructureType::InfantryBarracks, {4, 8}));   // over the headquarters
    CHECK(!w.can_place(StructureType::InfantryBarracks, {38, 21})); // off the map
    CHECK(!w.can_place(StructureType::House, {20, 5}));             // not something you build

    // No materials, no foundation.
    Simulation broke = economy_sim({});
    issue(broke, build_at(spawn_workers(broke, 1, 18, 5), StructureType::Warehouse, 20, 5));
    for (int i = 0; i < 5; ++i) broke.step();
    CHECK(broke.world().structure_at({20, 5}) == nullptr);
}

// A warehouse by the woodline takes in materials: no more walking to the far headquarters.
void test_warehouse_takes_in_materials() {
    Simulation sim = economy_sim(stock_with({{Resource::Materials, 75}}));
    const std::vector<EntityId> crew = spawn_workers(sim, 2, 15, 12);
    issue(sim, build_at(crew, StructureType::Warehouse, 15, 9));
    build_until_done(sim, {15, 9}, 2000);
    issue(sim, gather_at(crew, 13, 10));

    Fixed westmost = Fixed::from_int(100);
    for (int i = 0; i < 1500; ++i) {
        sim.step();
        for (EntityId id : crew) westmost = min(westmost, sim.world().find_unit(id)->pos.x);
    }
    CHECK(stock_of(sim, Resource::Materials) >= 30);
    CHECK(westmost > Fixed::from_int(10));  // the headquarters is at x = 5..7
}

// --- Logistics ---------------------------------------------------------------

// The economy base plus our railway station on (26..29, 3..4).
Simulation logistics_sim(const Stock& stock) {
    Simulation sim = economy_sim(stock, false);
    sim.world_for_setup().place_structure(StructureType::Station, 0, {26, 3}, 4, 2);
    return sim;
}

constexpr auto kMen = static_cast<size_t>(Resource::Personnel);
constexpr auto kAmmo = static_cast<size_t>(Resource::Ammo);

Command haul(std::vector<EntityId> trucks) { return {.type = CommandType::Haul, .player = 0, .units = std::move(trucks)}; }

// Trains come on schedule: the men join at once, the freight waits at the
// station for trucks.
void test_trains_bring_men_and_freight() {
    Simulation sim = logistics_sim({});
    for (Tick i = 0; i + 1 < kTrainInterval; ++i) sim.step();
    CHECK(stock_of(sim, Resource::Personnel) == 0);  // not yet
    sim.step();
    sim.step();
    Stock freight = kTrainCargo;
    freight[kMen] = 0;
    CHECK(stock_of(sim, Resource::Personnel) == kTrainCargo[kMen]);
    CHECK(sim.world().station_of(0)->cargo == freight);
    CHECK(stock_of(sim, Resource::Food) == 0);  // not ours until it's in a depot
    CHECK(sim.world().stock(1)[kMen] == 0);     // no station, no trains

    for (Tick i = 0; i < kTrainInterval; ++i) sim.step();
    CHECK(stock_of(sim, Resource::Personnel) == 2 * kTrainCargo[kMen]);
    CHECK(sim.world().station_of(0)->cargo[kAmmo] == 2 * kTrainCargo[kAmmo]);
}

// Ticks from the first train until a truck has unloaded a full load of
// ammunition at the depot, with `helpers` rear troops standing by it.
int haul_one_load(int helpers) {
    Simulation sim = logistics_sim({});
    World& w = sim.world_for_setup();
    w.place_structure(StructureType::AmmoDepot, 0, {20, 12}, 2, 2);
    const EntityId truck = w.spawn_unit(0, UnitTypeId::Truck, at(22, 8));
    for (int i = 0; i < helpers; ++i) w.spawn_unit(0, UnitTypeId::Worker, at(19, 12 + i));
    issue(sim, haul({truck}));
    for (Tick i = 0; i <= kTrainInterval; ++i) sim.step();

    int ticks = 0;
    for (; ticks < 3000 && stock_of(sim, Resource::Ammo) < kTruckCapacity; ++ticks) {
        sim.step();
        // Nothing gets lost or made up on the way.
        const Unit* u = sim.world().find_unit(truck);
        const int32_t aboard = u->carrying_type == Resource::Ammo ? u->carrying : 0;
        CHECK(stock_of(sim, Resource::Ammo) + aboard + sim.world().station_of(0)->cargo[kAmmo] ==
              kTrainCargo[kAmmo]);
    }
    // Only ammunition moved: there is no depot for food or fuel.
    CHECK(sim.world().station_of(0)->cargo[static_cast<size_t>(Resource::Food)] ==
          kTrainCargo[static_cast<size_t>(Resource::Food)]);
    return ticks;
}

void test_trucks_haul_freight_to_depots() {
    const int ticks = haul_one_load(0);
    CHECK(ticks < 3000);
}

Command haul_cargo(std::vector<EntityId> trucks, uint8_t cargo, EntityId depot = 0) {
    Command c = haul(std::move(trucks));
    c.cargo = cargo;
    c.target_unit = depot;
    return c;
}

// What a truck takes on at the station after the first train: on auto
// whatever there's most of (food), assigned a freight only that one, and
// with no depot for its freight nothing at all.
void test_trucks_haul_what_they_are_told() {
    auto first_load = [](std::vector<Command> orders) {
        Simulation sim = logistics_sim({});
        World& w = sim.world_for_setup();
        w.place_structure(StructureType::Warehouse, 0, {20, 12}, 2, 2);
        w.place_structure(StructureType::AmmoDepot, 0, {24, 12}, 2, 2);
        const EntityId truck = w.spawn_unit(0, UnitTypeId::Truck, at(22, 8));
        for (Command& c : orders) {
            c.units = {truck};
            issue(sim, c);
            sim.step();
        }
        for (Tick i = 0; i <= kTrainInterval + 200 && sim.world().find_unit(truck)->carrying == 0; ++i) sim.step();
        const Unit* u = sim.world().find_unit(truck);
        return u->carrying > 0 ? u->carrying_type : Resource::Count;
    };
    CHECK(first_load({haul_cargo({}, kHaulAuto)}) == Resource::Food);
    CHECK(first_load({haul_cargo({}, haul_code(Resource::Ammo))}) == Resource::Ammo);
    CHECK(first_load({haul_cargo({}, haul_code(Resource::Fuel))}) == Resource::Count);  // no fuel depot
    // Back on the run as it was (the station, or no cargo given); auto again.
    CHECK(first_load({haul_cargo({}, haul_code(Resource::Ammo)), haul_cargo({}, kHaulKeep)}) == Resource::Ammo);
    CHECK(first_load({haul_cargo({}, haul_code(Resource::Ammo)), haul_cargo({}, kHaulAuto)}) == Resource::Food);
    // Nobody hauls men or materials by truck.
    CHECK(first_load({haul_cargo({}, haul_code(Resource::Ammo)), haul_cargo({}, haul_code(Resource::Materials))}) ==
          Resource::Ammo);
}

// Sent to a depot of ours, a truck hauls its freight there, not to a nearer
// one; if that depot goes, to the nearest again. The enemy's depots and our
// unfinished ones don't count.
void test_trucks_assigned_to_a_depot() {
    Simulation sim = logistics_sim({});
    World& w = sim.world_for_setup();
    const EntityId near = w.place_structure(StructureType::AmmoDepot, 0, {24, 8}, 2, 2);
    const EntityId far = w.place_structure(StructureType::AmmoDepot, 0, {34, 20}, 2, 2);
    const EntityId enemy = w.place_structure(StructureType::Warehouse, 1, {10, 20}, 2, 2);
    const EntityId truck = w.spawn_unit(0, UnitTypeId::Truck, at(22, 6));
    const EntityId other = w.spawn_unit(0, UnitTypeId::Truck, at(22, 5));
    issue(sim, haul_cargo({truck}, kHaulKeep, far));
    issue(sim, haul_cargo({other}, haul_code(Resource::Ammo)));
    sim.step();
    issue(sim, haul_cargo({truck}, kHaulKeep, enemy));  // not ours: nothing changes
    for (int i = 0; i < 3; ++i) sim.step();
    CHECK(sim.world().find_unit(truck)->haul_cargo == Resource::Ammo && sim.world().find_unit(truck)->haul_depot == far);

    // Where each one unloads its first load.
    auto unloads_at = [&](EntityId id) {
        for (Tick i = 0; i < kTrainInterval + 3000; ++i) {
            const int32_t before = sim.world().find_unit(id)->carrying;
            sim.step();
            const Unit* u = sim.world().find_unit(id);
            if (before > 0 && u->carrying < before && u->carrying_type == Resource::Ammo) {
                return nearest_depot_id(sim, u->pos);
            }
        }
        return EntityId{0};
    };
    CHECK(unloads_at(truck) == far);
    CHECK(unloads_at(other) == near);

    sim.world_for_setup().structure_for_setup(far)->hp = 0;
    sim.step();
    for (int i = 0; i < 3000 && sim.world().find_unit(truck)->carrying > 0; ++i) sim.step();
    CHECK(unloads_at(truck) == near);

    // Assigned to the ammunition depot with food aboard: the food still goes
    // to the warehouse.
    Simulation food = logistics_sim({});
    World& fw = food.world_for_setup();
    const EntityId warehouse = fw.place_structure(StructureType::Warehouse, 0, {10, 18}, 2, 2);
    const EntityId depot = fw.place_structure(StructureType::AmmoDepot, 0, {34, 18}, 2, 2);
    const EntityId loaded = fw.spawn_unit(0, UnitTypeId::Truck, at(22, 6));
    issue(food, haul_cargo({loaded}, haul_code(Resource::Food)));
    for (Tick i = 0; i < kTrainInterval + 400 && food.world().find_unit(loaded)->carrying == 0; ++i) food.step();
    CHECK(food.world().find_unit(loaded)->carrying_type == Resource::Food);
    issue(food, haul_cargo({loaded}, kHaulKeep, depot));
    for (int i = 0; i < 3000 && food.world().find_unit(loaded)->carrying == kTruckCapacity; ++i) food.step();
    const FixedVec2 unloading = food.world().find_unit(loaded)->pos;
    CHECK((unloading - food.world().find_structure(warehouse)->center).length() < Fixed::from_int(3));
    CHECK(food.world().find_unit(loaded)->haul_depot == depot);
}

// --- Ammunition at combat positions ---

Command attach(PlayerId player, std::vector<EntityId> vehicles, EntityId unit);  // below

// A rifleman carries so many rounds; once they're gone he holds fire.
void test_infantry_runs_out_of_rounds() {
    Simulation sim(1, TileMap(40, 20));
    const EntityId rifle = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(10, 10));
    const EntityId truck = sim.world_for_setup().spawn_unit(1, UnitTypeId::Truck, at(14, 10));
    sim.world_for_setup().unit_for_setup(truck)->hp = 100000;
    sim.world_for_setup().unit_for_setup(rifle)->rounds = 5;
    issue(sim, attack_order(0, {rifle}, truck));
    for (int i = 0; i < 400; ++i) sim.step();
    const Unit* r = sim.world().find_unit(rifle);
    CHECK(r->rounds == 0);
    const Tick last = r->last_shot_tick;
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(sim.world().find_unit(rifle)->last_shot_tick == last);  // silent since
}

// A trench tile at (20, 10), a gun pit at (30, 10); an ammunition depot and
// ammunition in stock.
Simulation positions_sim() {
    TileMap map(48, 24);
    map.set_terrain(20, 10, Terrain::Trench);
    Simulation sim(1, map);
    World& w = sim.world_for_setup();
    w.place_structure(StructureType::Trench, 0, {20, 10}, 1, 1);
    w.place_structure(StructureType::GunPit, 0, {30, 10}, 1, 1);
    w.place_structure(StructureType::AmmoDepot, 0, {4, 4}, 2, 2);
    w.set_stock(0, {0, 100, 0, 200, 0});
    return sim;
}

// An ammunition truck keeps a position stocked: its own load first, then
// from the depot, until the position is full; men at it (but not farther
// off, and not the enemy's) take their rounds from it.
void test_positions_hold_ammunition() {
    Simulation sim = positions_sim();
    World& w = sim.world_for_setup();
    const EntityId trench = sim.world().structure_at({20, 10})->id;
    const EntityId pit = sim.world().structure_at({30, 10})->id;
    const EntityId truck = w.spawn_unit(0, UnitTypeId::AmmoTruck, at(16, 14));
    w.unit_for_setup(truck)->carrying = 10;
    const int32_t trench_room = structure_type(StructureType::Trench).cache_capacity;
    issue(sim, attach(0, {truck}, trench));
    for (int i = 0; i < 3000 && sim.world().find_structure(trench)->cache < trench_room; ++i) sim.step();
    CHECK(sim.world().find_structure(trench)->cache == trench_room);
    CHECK(sim.world().find_structure(trench)->cache_owner == 0);
    CHECK(sim.world().find_unit(truck)->order == Order::Supply);
    CHECK(stock_of(sim, Resource::Ammo) < 200);  // it went back to the depot for more

    // A rifleman out of rounds in the trench fills up from it; one farther off doesn't.
    const EntityId in = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, tile_center({20, 10}));
    const EntityId out = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(20, 17));
    const EntityId enemy = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(22, 11));
    sim.world_for_setup().unit_for_setup(in)->rounds = 0;
    sim.world_for_setup().unit_for_setup(out)->rounds = 0;
    sim.world_for_setup().unit_for_setup(enemy)->rounds = 0;
    issue(sim, make_order(CommandType::Stop, 0, {in, out}, 0, 0));
    for (Tick i = 0; i < 10 * kRearmInterval; ++i) sim.step();
    CHECK(sim.world().find_unit(in)->rounds == unit_type(UnitTypeId::Rifleman).rounds_capacity);
    CHECK(sim.world().find_unit(out)->rounds == 0);
    CHECK(!sim.world().find_unit(enemy) || sim.world().find_unit(enemy)->rounds == 0);
    // The truck stands by and tops the position up again.
    for (int i = 0; i < 3000 && sim.world().find_structure(trench)->cache < trench_room; ++i) sim.step();
    CHECK(sim.world().find_structure(trench)->cache == trench_room);

    // A mortar at its gun pit takes its bombs from the pit's stock.
    Simulation guns = positions_sim();
    const EntityId stocker = guns.world_for_setup().spawn_unit(0, UnitTypeId::AmmoTruck, at(26, 14));
    const EntityId mortar = guns.world_for_setup().spawn_unit(0, UnitTypeId::Mortar, tile_center({30, 10}));
    guns.world_for_setup().unit_for_setup(mortar)->rounds = 0;
    guns.world_for_setup().unit_for_setup(stocker)->carrying = 100;
    issue(guns, attach(0, {stocker}, guns.world().structure_at({30, 10})->id));
    for (int i = 0; i < 1500 && guns.world().find_unit(mortar)->rounds < unit_type(UnitTypeId::Mortar).rounds_capacity; ++i) {
        guns.step();
    }
    CHECK(guns.world().find_unit(mortar)->rounds == unit_type(UnitTypeId::Mortar).rounds_capacity);
    (void)pit;

    // What's drawn is gone from the position: four units for a rifleman's 120 rounds.
    Simulation drawn = positions_sim();
    Structure* t = drawn.world_for_setup().structure_for_setup(drawn.world().structure_at({20, 10})->id);
    t->cache = 10;
    t->cache_owner = 0;
    const EntityId empty = drawn.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, tile_center({20, 10}));
    drawn.world_for_setup().unit_for_setup(empty)->rounds = 0;
    for (Tick i = 0; i < 10 * kRearmInterval; ++i) drawn.step();
    const int32_t per_supply = unit_type(UnitTypeId::Rifleman).rounds_per_supply;
    CHECK(drawn.world().structure_at({20, 10})->cache == 10 - unit_type(UnitTypeId::Rifleman).rounds_capacity / per_supply);
}

// Only combat positions hold ammunition, and not ones the enemy holds or has
// stocked; lose the position and the truck is free.
void test_stocking_rules() {
    Simulation sim(1, village_map());
    World& w = sim.world_for_setup();
    const EntityId house = house_id(sim);
    const EntityId dugout = w.place_structure(StructureType::Dugout, 0, {30, 5}, 1, 1);
    const EntityId truck = w.spawn_unit(0, UnitTypeId::AmmoTruck, at(25, 8));
    const EntityId enemy = w.spawn_unit(1, UnitTypeId::Rifleman, at(31, 7));
    CHECK(!sim.world().can_stock(*sim.world().find_structure(house), 0));  // a house is no position
    issue(sim, garrison(1, {enemy}, dugout));
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(sim.world().find_structure(dugout)->owner == 1);
    CHECK(!sim.world().can_stock(*sim.world().find_structure(dugout), 0) && sim.world().can_stock(*sim.world().find_structure(dugout), 1));
    issue(sim, attach(0, {truck}, dugout));
    issue(sim, attach(0, {truck}, house));
    for (int i = 0; i < 5; ++i) sim.step();
    CHECK(sim.world().find_unit(truck)->order == Order::Idle);

    // Ours: stocked, then shelled flat: the truck is free again.
    Simulation lost = positions_sim();
    const EntityId trench = lost.world().structure_at({20, 10})->id;
    const EntityId stocker = lost.world_for_setup().spawn_unit(0, UnitTypeId::AmmoTruck, at(16, 14));
    issue(lost, attach(0, {stocker}, trench));
    for (int i = 0; i < 400; ++i) lost.step();
    CHECK(lost.world().find_structure(trench)->cache > 0);
    CHECK(!lost.world().can_stock(*lost.world().find_structure(trench), 1));  // our stock in it
    // An empty dugout with our stock in it is held by nobody, but it's ours to stock, not theirs.
    const EntityId empty_dugout = lost.world_for_setup().place_structure(StructureType::Dugout, 0, {40, 5}, 1, 1);
    Structure* d = lost.world_for_setup().structure_for_setup(empty_dugout);
    d->owner = kNoOwner;
    d->cache = 5;
    d->cache_owner = 0;
    CHECK(lost.world().can_stock(*d, 0) && !lost.world().can_stock(*d, 1));
    lost.world_for_setup().structure_for_setup(trench)->hp = 0;
    for (int i = 0; i < 3; ++i) lost.step();
    CHECK(lost.world().find_unit(stocker)->order == Order::Idle);
}

// --- Rations ---

// Every ration time each man takes his ration from the stock: a rifleman
// one, a tank's crew three. Short of it, the army goes hungry until the
// next full ration.
void test_rations() {
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    for (int i = 0; i < 4; ++i) w.spawn_unit(0, UnitTypeId::Rifleman, at(5, 5 + i));
    w.spawn_unit(0, UnitTypeId::Tank, at(10, 10));
    w.spawn_unit(1, UnitTypeId::Rifleman, at(35, 15));
    w.set_stock(0, {0, 100, 0, 0, 0});
    w.set_stock(1, {0, 0, 0, 0, 0});
    CHECK(sim.world().mouths(0) == 4 + 3 && sim.world().mouths(1) == 1);
    for (Tick i = 0; i + 1 < kRationInterval; ++i) sim.step();
    CHECK(stock_of(sim, Resource::Food) == 100);  // not yet
    for (int i = 0; i < 2; ++i) sim.step();
    CHECK(stock_of(sim, Resource::Food) == 100 - 7 * kRationPerMan);
    CHECK(!sim.world().hungry(0) && sim.world().hungry(1));

    sim.world_for_setup().set_stock(0, {0, 5, 0, 0, 0});
    for (Tick i = 0; i < kRationInterval; ++i) sim.step();
    CHECK(sim.world().hungry(0) && stock_of(sim, Resource::Food) == 0);
    sim.world_for_setup().set_stock(0, {0, 50, 0, 0, 0});
    for (Tick i = 0; i < kRationInterval; ++i) sim.step();
    CHECK(!sim.world().hungry(0) && stock_of(sim, Resource::Food) == 43);
}

// Hungry men move slower and shoot worse.
void test_hunger_weakens_the_army() {
    auto walked = [](bool fed) {
        Simulation sim(1, TileMap(60, 20));
        const EntityId rifle = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(5, 10));
        sim.world_for_setup().set_stock(0, {0, fed ? 100 : 0, 0, 0, 0});
        for (Tick i = 0; i <= kRationInterval; ++i) sim.step();
        CHECK(sim.world().hungry(0) != fed);
        issue(sim, make_move(0, {rifle}, 55, 10));
        for (int i = 0; i < 200; ++i) sim.step();
        return sim.world().find_unit(rifle)->pos.x;
    };
    const Fixed fed = walked(true) - Fixed::from_int(5);
    const Fixed hungry = walked(false) - Fixed::from_int(5);
    CHECK(hungry < fed && hungry * 100 >= fed * (kHungrySpeedPercent - 3) && hungry * 100 <= fed * (kHungrySpeedPercent + 3));

    // Shots at a truck that can't answer, over many seeds: hungry, fewer hit.
    auto damage = [](bool fed) {
        int32_t total = 0;
        for (uint64_t seed = 1; seed <= 20; ++seed) {
            Simulation sim(seed, TileMap(40, 20));
            const EntityId mg = sim.world_for_setup().spawn_unit(0, UnitTypeId::MachineGunner, at(10, 10));
            sim.world_for_setup().set_stock(0, {0, fed ? 100 : 0, 0, 0, 0});
            for (Tick i = 0; i <= kRationInterval; ++i) sim.step();
            const EntityId truck = sim.world_for_setup().spawn_unit(1, UnitTypeId::Truck, at(15, 10));
            sim.world_for_setup().unit_for_setup(truck)->hp = 100000;
            issue(sim, attack_order(0, {mg}, truck));
            for (int i = 0; i < 400; ++i) sim.step();
            total += 100000 - sim.world().find_unit(truck)->hp;
        }
        return total;
    };
    const int32_t fed_damage = damage(true);
    const int32_t hungry_damage = damage(false);
    // A quarter fewer aimed hits; a miss this close still often finds the truck.
    CHECK(hungry_damage > 0 && hungry_damage * 100 <= fed_damage * 93);
}

// --- Service vehicles attached and called over the radio ---

Command attach(PlayerId player, std::vector<EntityId> vehicles, EntityId unit) {
    return {.type = CommandType::Supply, .player = player, .units = std::move(vehicles), .target_unit = unit};
}

// An ammunition truck attached to a tank tops it up, follows it about, and
// when it runs dry fetches more from the depot and comes back; with the tank
// gone it's free. Only for one of ours that uses what it carries.
void test_attached_supply() {
    Simulation sim(1, TileMap(60, 30));
    World& w = sim.world_for_setup();
    w.set_stock(0, {0, 0, 0, 200, 0});
    w.place_structure(StructureType::AmmoDepot, 0, {4, 4}, 2, 2);
    const EntityId tank = w.spawn_unit(0, UnitTypeId::Tank, at(20, 15));
    const EntityId truck = w.spawn_unit(0, UnitTypeId::AmmoTruck, at(12, 20));
    const EntityId scout = w.spawn_unit(0, UnitTypeId::Truck, at(14, 20));  // no gun, nothing to load
    const EntityId enemy = w.spawn_unit(1, UnitTypeId::Tank, at(50, 25));
    w.unit_for_setup(tank)->rounds = 0;
    w.unit_for_setup(enemy)->rounds = 0;
    w.unit_for_setup(truck)->carrying = 10;

    // Not the enemy's, not one without a gun to load.
    issue(sim, attach(0, {truck}, enemy));
    issue(sim, attach(0, {truck}, scout));
    for (int i = 0; i < 5; ++i) sim.step();
    CHECK(sim.world().find_unit(truck)->order == Order::Idle);

    issue(sim, attach(0, {truck}, tank));
    const int32_t full = unit_type(UnitTypeId::Tank).rounds_capacity;
    for (int i = 0; i < 3000 && sim.world().find_unit(tank)->rounds < full; ++i) sim.step();
    CHECK(sim.world().find_unit(tank)->rounds == full);  // 10 aboard, then a trip to the depot for the rest
    CHECK(sim.world().find_unit(truck)->carrying >= unit_type(UnitTypeId::AmmoTruck).cargo_capacity - full);  // it filled up
    CHECK(stock_of(sim, Resource::Ammo) < 200);
    CHECK(sim.world().find_unit(truck)->order == Order::Supply);

    issue(sim, make_move(0, {tank}, 45, 10));
    for (int i = 0; i < 900; ++i) sim.step();
    const Fixed apart = (sim.world().find_unit(tank)->pos - sim.world().find_unit(truck)->pos).length();
    CHECK(apart <= kEscortDistance + Fixed::from_int(1));

    sim.world_for_setup().unit_for_setup(tank)->hp = 0;
    sim.step();
    sim.step();
    CHECK(sim.world().find_unit(truck)->order == Order::Idle && sim.world().find_unit(truck)->serves == 0);
}

// A call over the radio brings the nearest free tanker and ammunition
// truck with something aboard, not ones busy elsewhere or empty; they top
// the caller up and are free again. Radio off and no relay: nobody hears.
void test_radio_call_for_supply() {
    Simulation sim(1, TileMap(60, 30));
    World& w = sim.world_for_setup();
    const EntityId tank = w.spawn_unit(0, UnitTypeId::Tank, at(30, 15));
    const EntityId other = w.spawn_unit(0, UnitTypeId::Tank, at(30, 25));
    w.unit_for_setup(tank)->rounds = 0;
    w.unit_for_setup(tank)->fuel = Fixed::from_int(20);
    w.unit_for_setup(other)->rounds = 20;
    const EntityId busy = w.spawn_unit(0, UnitTypeId::AmmoTruck, at(32, 22));  // nearest, but attached elsewhere
    const EntityId empty = w.spawn_unit(0, UnitTypeId::AmmoTruck, at(34, 15));
    const EntityId near = w.spawn_unit(0, UnitTypeId::AmmoTruck, at(40, 15));
    const EntityId far = w.spawn_unit(0, UnitTypeId::AmmoTruck, at(55, 15));
    const EntityId tanker = w.spawn_unit(0, UnitTypeId::FuelTanker, at(50, 5));
    w.unit_for_setup(empty)->carrying = 0;
    issue(sim, attach(0, {busy}, other));
    issue(sim, use_ability(0, {tank}, AbilityId::CallSupply, 0, 0));
    for (int i = 0; i < 4; ++i) sim.step();
    CHECK(sim.world().find_unit(near)->order == Order::Supply && sim.world().find_unit(near)->serves == tank);
    CHECK(sim.world().find_unit(tanker)->order == Order::Supply && sim.world().find_unit(tanker)->serves == tank);
    CHECK(sim.world().find_unit(far)->order == Order::Idle && sim.world().find_unit(empty)->order == Order::Idle);
    CHECK(sim.world().find_unit(busy)->serves == other);
    // Calling again while they're on the way sends nobody else.
    for (Tick i = 0; i < ability_def(AbilityId::CallSupply).cooldown; ++i) sim.step();
    issue(sim, use_ability(0, {tank}, AbilityId::CallSupply, 0, 0));
    for (int i = 0; i < 4; ++i) sim.step();
    CHECK(sim.world().find_unit(far)->order == Order::Idle);

    for (int i = 0; i < 3000 && sim.world().find_unit(near)->order == Order::Supply; ++i) sim.step();
    const Unit* t = sim.world().find_unit(tank);
    CHECK(t->rounds == unit_type(UnitTypeId::Tank).rounds_capacity || sim.world().find_unit(near)->carrying == 0);
    CHECK(t->rounds > 0);
    for (int i = 0; i < 3000 && sim.world().find_unit(tanker)->order == Order::Supply; ++i) sim.step();
    CHECK(sim.world().find_unit(tank)->fuel == unit_type(UnitTypeId::Tank).fuel_capacity);
    CHECK(sim.world().find_unit(near)->order == Order::Idle && sim.world().find_unit(tanker)->order == Order::Idle);

    // Radio silence, no relay near: the call goes nowhere.
    Simulation quiet(1, TileMap(60, 30));
    World& q = quiet.world_for_setup();
    const EntityId silent = q.spawn_unit(0, UnitTypeId::Tank, at(30, 15));
    const EntityId truck = q.spawn_unit(0, UnitTypeId::AmmoTruck, at(40, 15));
    q.unit_for_setup(silent)->rounds = 0;
    q.unit_for_setup(silent)->silent = true;
    issue(quiet, use_ability(0, {silent}, AbilityId::CallSupply, 0, 0));
    for (Tick i = 0; i < kCourierTicks + 20; ++i) quiet.step();
    CHECK(quiet.world().find_unit(truck)->order == Order::Idle);
}

// --- Village buildings as forward depots ---

// A plain with a barn (4 x 2 House tiles at (20..23, 10..11)) and a cottage
// (2 x 2 at (30..31, 10..11)), our headquarters, materials and fuel.
Simulation farm_sim() {
    TileMap map(48, 24);
    for (int y = 10; y <= 11; ++y) {
        for (int x = 20; x <= 23; ++x) map.set_terrain(x, y, Terrain::House);
        for (int x = 30; x <= 31; ++x) map.set_terrain(x, y, Terrain::House);
    }
    Simulation sim(1, map);
    sim.world_for_setup().place_structure(StructureType::Headquarters, 0, {3, 9}, 3, 3);
    sim.world_for_setup().set_stock(0, {0, 0, 100, 100, 100});
    return sim;
}

EntityId barn_of(const Simulation& sim) { return sim.world().structure_at({20, 10})->id; }

Command take_over(PlayerId player, std::vector<EntityId> crew, EntityId building, StructureType depot) {
    return {.type = CommandType::Build, .player = player, .units = std::move(crew), .target_unit = building,
            .structure_type = static_cast<uint8_t>(depot)};
}

std::vector<EntityId> farm_crew(Simulation& sim) {
    std::vector<EntityId> crew;
    for (int i = 0; i < 2; ++i) crew.push_back(sim.world_for_setup().spawn_unit(0, UnitTypeId::Worker, at(18, 13 + i)));
    return crew;
}

// The plain lies a level above the rivers and the dry riverbeds in their
// gullies: a way in unseen from the fields, unless one looks from the edge.
void test_demo_map_relief() {
    for (const MapSizePreset& preset : kMapSizes) {
        const TileMap map = make_demo_map(preset.tiles);
        int riverbed = 0;
        bool plain_raised = true;
        for (int32_t y = 0; y < map.height(); ++y) {
            for (int32_t x = 0; x < map.width(); ++x) {
                const Terrain t = map.terrain(x, y);
                if (t == Terrain::Riverbed) {
                    ++riverbed;
                    CHECK(map.elevation(x, y) == 0);
                }
                if (t == Terrain::Grass || t == Terrain::Road || t == Terrain::Plowed) {
                    plain_raised = plain_raised && map.elevation(x, y) >= 1;
                }
            }
        }
        CHECK(riverbed > 0 && plain_raised);
    }
    // A man down in a gully three tiles wide: hidden from the field some way off, seen from its edge.
    auto seen_from = [](int32_t x) {
        TileMap map(40, 20);
        for (int y = 0; y < 20; ++y) {
            for (int tx = 0; tx < 40; ++tx) map.set_elevation(tx, y, tx >= 19 && tx <= 21 ? 0 : 1);
        }
        Simulation sim(1, map);
        const EntityId man = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, tile_center({20, 10}));
        sim.world_for_setup().spawn_unit(0, UnitTypeId::Scout, tile_center({x, 10}));
        sim.step();
        return seen(sim, 0, man);
    };
    CHECK(!seen_from(12));
    CHECK(seen_from(18));
}

// The demo map has spacious buildings well away from both bases: barns near
// the middle, sheds in the industrial zone and the dairy farm's cowsheds,
// two of each a side; a town with two apartment blocks and two cell towers a side.
void test_demo_map_has_barns() {
    for (const MapSizePreset& preset : kMapSizes) {
        const World world(1, make_demo_map(preset.tiles));
        int barns = 0;
        for (const Structure& s : world.structures()) {
            if (s.type != StructureType::House || s.tiles.size() < kSpaciousTiles) continue;
            ++barns;
            for (PlayerId p = 0; p < 2; ++p) {
                CHECK((s.center - demo_base_position(preset.tiles, p)).length() > Fixed::from_int(preset.tiles / 5));
            }
        }
        CHECK(barns == 12);
        int blocks = 0;
        int towers = 0;
        for (const Structure& s : world.structures()) {
            blocks += s.type == StructureType::Apartment ? 1 : 0;
            towers += s.type == StructureType::CellTower ? 1 : 0;
        }
        CHECK(blocks == 4 && towers == 4);
        // A gas station by the highway and a grain elevator a side, stocked.
        int stations = 0;
        int elevators = 0;
        for (const Structure& s : world.structures()) {
            if (s.type == StructureType::GasStation) {
                ++stations;
                CHECK(s.cargo[static_cast<size_t>(Resource::Fuel)] == kGasStationFuel);
                bool by_road = false;
                for (int dy = -3; dy <= 3; ++dy) {
                    for (int dx = -3; dx <= 3; ++dx) {
                        const TilePos t{s.tiles.front().x + dx, s.tiles.front().y + dy};
                        by_road = by_road || (world.map().contains(t) && world.map().terrain(t) == Terrain::Road);
                    }
                }
                CHECK(by_road);
            }
            if (s.type == StructureType::Elevator) {
                ++elevators;
                CHECK(s.cargo[static_cast<size_t>(Resource::Food)] == kElevatorFood);
            }
        }
        CHECK(stations == 2 && elevators == 2);
    }
}

// Rear troops turn a spacious village building into a depot: paid up front,
// worked on like a building site, then it's a depot like any other, nearer
// the front. A cottage is too small; a depot is no shelter.
void test_take_over_a_village_building() {
    Simulation sim = farm_sim();
    const std::vector<EntityId> crew = farm_crew(sim);
    const EntityId barn = barn_of(sim);
    const EntityId cottage = sim.world().structure_at({30, 10})->id;
    CHECK(sim.world().can_convert(*sim.world().find_structure(barn), 0));
    CHECK(!sim.world().can_convert(*sim.world().find_structure(cottage), 0));
    CHECK(!sim.world().can_convert(*sim.world().structure_at({3, 9}), 0));  // our headquarters: not a village building

    issue(sim, take_over(0, crew, cottage, StructureType::AmmoDepot));
    issue(sim, take_over(0, crew, barn, StructureType::Headquarters));  // not a depot
    for (int i = 0; i < 5; ++i) sim.step();
    CHECK(stock_of(sim, Resource::Materials) == 100);
    CHECK(sim.world().find_structure(cottage)->converted == StructureType::Count);
    CHECK(sim.world().find_structure(barn)->converted == StructureType::Count);

    issue(sim, take_over(0, crew, barn, StructureType::AmmoDepot));
    for (int i = 0; i < 3; ++i) sim.step();
    const Structure* b = sim.world().find_structure(barn);
    CHECK(b->converted == StructureType::AmmoDepot && b->owner == 0 && !b->built);
    CHECK(stock_of(sim, Resource::Materials) == 100 - kConversionCost[static_cast<size_t>(Resource::Materials)]);
    CHECK(sim.world().nearest_owned(0, StructureType::AmmoDepot, b->center) == nullptr);  // not until it's done
    for (Tick i = 0; i < kConversionWork / 4; ++i) sim.step();
    CHECK(!b->built && b->build_progress > 0);  // at work on it
    for (Tick i = 0; i < kConversionWork / 4 + 100; ++i) sim.step();  // two of them: half the time, and the walk
    CHECK(b->built && sim.world().nearest_owned(0, StructureType::AmmoDepot, at(0, 0)) == b);
    CHECK(b->hp == structure_type(StructureType::House).max_hp);  // the building was standing all along
    CHECK(!sim.world().can_convert(*b, 0) && !sim.world().can_convert(*b, 1));

    // A depot keeps a lookout: with the rear troops gone, its yard is still in view.
    issue(sim, make_move(0, crew, 4, 20));
    for (int i = 0; i < 400; ++i) sim.step();
    CHECK(sim.world().visible(0, {21, 15}));

    // An ammunition truck loads up there, not back at the base.
    const EntityId truck = sim.world_for_setup().spawn_unit(0, UnitTypeId::AmmoTruck, at(28, 16));
    sim.world_for_setup().unit_for_setup(truck)->carrying = 0;
    issue(sim, use_ability(0, {truck}, AbilityId::Refill, 0, 0));
    for (int i = 0; i < 1500 && sim.world().find_unit(truck)->carrying == 0; ++i) sim.step();
    CHECK(sim.world().find_unit(truck)->carrying > 0);
    CHECK((sim.world().find_unit(truck)->pos - b->center).length() < Fixed::from_int(4));

    // No shelter: nobody goes in, and it stays ours standing empty.
    const EntityId rifle = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(19, 8));
    issue(sim, garrison(0, {rifle}, barn));
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(sim.world().find_unit(rifle)->inside == 0 && sim.world().find_structure(barn)->owner == 0);
}

// Held by the enemy it can't be taken over; our own men inside come out; a
// fuel depot in a barn burns like any other.
void test_taking_over_rules() {
    Simulation held = farm_sim();
    const std::vector<EntityId> crew = farm_crew(held);
    const EntityId enemy = held.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(19, 9));
    issue(held, garrison(1, {enemy}, barn_of(held)));
    for (int i = 0; i < 100; ++i) held.step();
    CHECK(held.world().find_unit(enemy)->inside == barn_of(held));
    issue(held, take_over(0, crew, barn_of(held), StructureType::Warehouse));
    for (int i = 0; i < 5; ++i) held.step();
    CHECK(held.world().find_structure(barn_of(held))->converted == StructureType::Count);
    CHECK(stock_of(held, Resource::Materials) == 100);

    Simulation ours = farm_sim();
    const std::vector<EntityId> hands = farm_crew(ours);
    const EntityId rifle = ours.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(19, 9));
    issue(ours, garrison(0, {rifle}, barn_of(ours)));
    for (int i = 0; i < 100; ++i) ours.step();
    CHECK(ours.world().find_unit(rifle)->inside == barn_of(ours));
    ours.world_for_setup().set_stock(0, {});
    issue(ours, take_over(0, hands, barn_of(ours), StructureType::FuelDepot));  // can't afford it
    for (int i = 0; i < 5; ++i) ours.step();
    CHECK(ours.world().find_structure(barn_of(ours))->converted == StructureType::Count);
    ours.world_for_setup().set_stock(0, {0, 0, 100, 0, 100});
    issue(ours, take_over(0, hands, barn_of(ours), StructureType::FuelDepot));
    for (int i = 0; i < 5; ++i) ours.step();
    CHECK(ours.world().find_unit(rifle)->inside == 0);
    for (Tick i = 0; i < kConversionWork; ++i) ours.step();
    CHECK(ours.world().find_structure(barn_of(ours))->built);
    ours.world_for_setup().structure_for_setup(barn_of(ours))->hp = 0;
    ours.step();
    CHECK(stock_of(ours, Resource::Fuel) == 100 - 100 * kFuelDepotLossPercent / 100);
}

// A tanker or an ammunition truck put on the supply run hauls only its own
// freight, a whole tank or bed of it at a time, whatever it's told; with no
// depot for it, it waits.
void test_service_vehicles_on_the_rail_run() {
    Simulation sim = logistics_sim({});
    World& w = sim.world_for_setup();
    w.place_structure(StructureType::Warehouse, 0, {20, 12}, 2, 2);
    w.place_structure(StructureType::FuelDepot, 0, {24, 12}, 2, 2);
    const EntityId tanker = w.spawn_unit(0, UnitTypeId::FuelTanker, at(22, 8));
    const EntityId ammo = w.spawn_unit(0, UnitTypeId::AmmoTruck, at(23, 8));
    w.unit_for_setup(tanker)->carrying = 0;
    w.unit_for_setup(ammo)->carrying = 0;
    issue(sim, haul_cargo({tanker}, haul_code(Resource::Food)));  // told food: still fuel
    issue(sim, haul_cargo({ammo}, kHaulAuto));
    for (Tick i = 0; i < kTrainInterval + 400 && sim.world().find_unit(tanker)->carrying == 0; ++i) sim.step();
    const Unit* t = sim.world().find_unit(tanker);
    CHECK(t->order == Order::Haul && t->carrying_type == Resource::Fuel);
    CHECK(t->carrying == kTrainCargo[static_cast<size_t>(Resource::Fuel)]);  // the whole 60 at once, more than a truck's 40
    for (int i = 0; i < 3000 && sim.world().find_unit(tanker)->carrying > 0; ++i) sim.step();
    CHECK(stock_of(sim, Resource::Fuel) == kTrainCargo[static_cast<size_t>(Resource::Fuel)]);
    CHECK(stock_of(sim, Resource::Ammo) == 0 && sim.world().station_of(0)->cargo[kAmmo] > 0);  // no ammo depot: none moved
    CHECK(sim.world().find_unit(ammo)->carrying == 0 && sim.world().find_unit(ammo)->order == Order::Haul);
    CHECK(stock_of(sim, Resource::Food) == 0);  // nobody hauls food here
}

// A five-storey block and a cell tower on a plain: the block (8 tiles) at
// (20..23, 10..11), the tower at (30, 10).
Simulation town_sim() {
    TileMap map(48, 24);
    for (int y = 10; y <= 11; ++y) {
        for (int x = 20; x <= 23; ++x) map.set_terrain(x, y, Terrain::Apartment);
    }
    map.set_terrain(30, 10, Terrain::Tower);
    return Simulation(1, map);
}

// The block holds twelve men, hides what's behind it, and its garrison sees
// farther and fires down as from higher ground.
void test_apartment_blocks() {
    Simulation sim = town_sim();
    const Structure* block = sim.world().structure_at({20, 10});
    CHECK(block && block->type == StructureType::Apartment && block->tiles.size() == 8);
    CHECK(structure_type(StructureType::Apartment).capacity == 12);

    // Behind the block, unseen from the other side.
    const EntityId hider = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(22, 14));
    sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(22, 7));
    sim.step();
    CHECK(!seen(sim, 0, hider));

    // Up the stairs: 7 tiles of sight become 10.
    auto sees_from = [](bool apartment) {
        Simulation s = town_sim();
        const EntityId in = s.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(21, 13));
        const EntityId far = s.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(22, 20));
        if (apartment) {
            issue(s, garrison(0, {in}, s.world().structure_at({20, 10})->id));
            for (int i = 0; i < 60; ++i) s.step();
            CHECK(s.world().find_unit(in)->inside != 0);
        }
        for (int i = 0; i < 6; ++i) s.step();
        return seen(s, 0, far);
    };
    CHECK(!sees_from(false));
    CHECK(sees_from(true));

    // Fire from the upper floors hits as from high ground: a rifle's 6 becomes 7.
    Simulation fire = town_sim();
    const EntityId shooter = fire.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(21, 13));
    const EntityId target = fire.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(22, 16));
    fire.world_for_setup().unit_for_setup(target)->hp = 100000;
    fire.world_for_setup().unit_for_setup(target)->rounds = 0;
    issue(fire, garrison(0, {shooter}, fire.world().structure_at({20, 10})->id));
    for (int i = 0; i < 300; ++i) fire.step();
    const int32_t lost = 100000 - hp_of(fire, target);
    CHECK(lost > 0 && lost % 7 == 0);
}

// A spotter up a cell tower sees far; held by our men it relays our radio.
void test_cell_towers() {
    Simulation sim = town_sim();
    const Structure* tower = sim.world().structure_at({30, 10});
    CHECK(tower && tower->type == StructureType::CellTower);
    const EntityId scout = sim.world_for_setup().spawn_unit(0, UnitTypeId::Scout, at(30, 12));
    const EntityId far = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(30, 23));
    const EntityId other = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(31, 12));
    issue(sim, garrison(0, {scout, other}, tower->id));  // room for one
    for (int i = 0; i < 80; ++i) sim.step();
    CHECK(sim.world().find_unit(scout)->inside == tower->id);
    CHECK(sim.world().find_unit(other)->inside == 0);
    CHECK(seen(sim, 0, far));  // 11 tiles off, beyond a scout's 9 on the ground

    Unit* tank = sim.world_for_setup().unit_for_setup(sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(40, 18)));
    tank->silent = true;
    CHECK(sim.world().in_touch(*tank));  // 12.8 tiles from the tower: relayed
    tank->pos = at(46, 22);
    CHECK(!sim.world().in_touch(*tank));  // 20 tiles: out of reach
    issue(sim, make_order(CommandType::Move, 0, {scout}, 30, 16));  // down from the mast
    for (int i = 0; i < 5; ++i) sim.step();
    tank->pos = at(40, 18);
    CHECK(!sim.world().in_touch(*tank));  // nobody up there: no relay
}

// A gas station (2 x 2 at (20..21, 10..11)) and a grain elevator (3 x 2 at
// (30..32, 10..11)) on a plain, with a warehouse and a fuel depot of ours.
Simulation spoils_sim() {
    TileMap map(48, 24);
    for (int y = 10; y <= 11; ++y) {
        for (int x = 20; x <= 21; ++x) map.set_terrain(x, y, Terrain::GasStation);
        for (int x = 30; x <= 32; ++x) map.set_terrain(x, y, Terrain::Elevator);
    }
    Simulation sim(1, map);
    sim.world_for_setup().place_structure(StructureType::Warehouse, 0, {4, 4}, 2, 2);
    sim.world_for_setup().place_structure(StructureType::FuelDepot, 0, {8, 4}, 2, 2);
    return sim;
}

void hold(Simulation& sim, TilePos building, PlayerId player, int32_t x, int32_t y) {
    const EntityId man = sim.world_for_setup().spawn_unit(player, UnitTypeId::Rifleman, at(x, y));
    issue(sim, garrison(player, {man}, sim.world().structure_at(building)->id));
    for (int i = 0; i < 80; ++i) sim.step();
    CHECK(sim.world().find_unit(man)->inside == sim.world().structure_at(building)->id);
}

// Held by our men, a gas station fills our vehicles up at the pumps from its
// tanks; nobody's, it doesn't. Destroyed with fuel in it, it goes up in
// flames, and our stock of fuel doesn't burn with it.
void test_gas_stations() {
    Simulation sim = spoils_sim();
    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(20, 14));
    sim.world_for_setup().unit_for_setup(tank)->fuel = Fixed::from_int(10);
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(sim.world().find_unit(tank)->fuel == Fixed::from_int(10));  // nobody holds it
    hold(sim, {20, 10}, 0, 23, 12);
    for (int i = 0; i < 100; ++i) sim.step();
    const Structure* station = sim.world().structure_at({20, 10});
    CHECK(sim.world().find_unit(tank)->fuel > Fixed::from_int(10));
    CHECK(station->cargo[static_cast<size_t>(Resource::Fuel)] < kGasStationFuel);

    // Up in flames: the man nearby burns, our stock doesn't.
    sim.world_for_setup().set_stock(0, {0, 0, 0, 0, 100});
    const EntityId near = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(22, 13));
    sim.world_for_setup().structure_for_setup(station->id)->hp = 0;
    sim.step();
    sim.step();
    CHECK(sim.world().structure_at({20, 10}) == nullptr);
    CHECK(hp_of(sim, near) < unit_type(UnitTypeId::Rifleman).max_hp);
    CHECK(stock_of(sim, Resource::Fuel) == 100);
}

// Held by our men, an elevator's grain is ours: trucks haul it to the
// warehouse, with no railway station at all. Its garrison sees far.
void test_grain_elevators() {
    Simulation sim = spoils_sim();
    const EntityId truck = sim.world_for_setup().spawn_unit(0, UnitTypeId::Truck, at(26, 16));
    issue(sim, haul_cargo({truck}, haul_code(Resource::Food)));
    for (int i = 0; i < 200; ++i) sim.step();
    CHECK(sim.world().find_unit(truck)->carrying == 0);  // nobody holds it yet
    hold(sim, {30, 10}, 0, 33, 13);
    for (int i = 0; i < 2000 && stock_of(sim, Resource::Food) == 0; ++i) sim.step();
    CHECK(stock_of(sim, Resource::Food) > 0);
    CHECK(sim.world().structure_at({30, 10})->cargo[static_cast<size_t>(Resource::Food)] < kElevatorFood);

    // From the top: a rifleman's 7 tiles of sight become 13.
    const EntityId far = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(31, 22));
    for (int i = 0; i < 6; ++i) sim.step();
    CHECK(seen(sim, 0, far));
}

// Rear troops and trucks with nothing to do are idle; on the supply run or at work they aren't.
void test_idle_hands() {
    Simulation sim = logistics_sim({});
    World& w = sim.world_for_setup();
    const EntityId truck = w.spawn_unit(0, UnitTypeId::Truck, at(22, 8));
    const EntityId worker = w.spawn_unit(0, UnitTypeId::Worker, at(20, 12));
    const EntityId rifle = w.spawn_unit(0, UnitTypeId::Rifleman, at(20, 14));
    CHECK(idle_hand(*sim.world().find_unit(truck)) && idle_hand(*sim.world().find_unit(worker)));
    CHECK(!idle_hand(*sim.world().find_unit(rifle)));
    issue(sim, haul({truck}));
    for (int i = 0; i < 3; ++i) sim.step();
    CHECK(!idle_hand(*sim.world().find_unit(truck)));
}

// The driver alone unloads at a crawl; rear troops by the depot make it quick.
void test_rear_troops_unload_faster() {
    const int alone = haul_one_load(0);
    const int crew = haul_one_load(2);
    CHECK(crew * 2 < alone);
}

// Shell the station flat and the trains stop: no more men, no more freight.
void test_no_trains_without_the_station() {
    Simulation sim = logistics_sim({});
    const EntityId station = sim.world().station_of(0)->id;
    std::vector<EntityId> tanks;
    for (int i = 0; i < 3; ++i) tanks.push_back(sim.world_for_setup().spawn_unit(1, UnitTypeId::Tank, at(26 + i, 9)));
    issue(sim, fire_at(1, tanks, 28, 4));
    for (int i = 0; i < 8000 && sim.world().find_structure(station); ++i) sim.step();
    CHECK(sim.world().find_structure(station) == nullptr);
    CHECK(sim.world().station_of(0) == nullptr);

    const int32_t men = stock_of(sim, Resource::Personnel);
    for (Tick i = 0; i <= 2 * kTrainInterval; ++i) sim.step();
    CHECK(stock_of(sim, Resource::Personnel) == men);
}

// A fuel depot going up burns a good part of the fuel and whatever stands by it.
void test_fuel_depot_burns() {
    Simulation sim = economy_sim(stock_with({{Resource::Fuel, 100}}), false);
    World& w = sim.world_for_setup();
    const EntityId depot = w.place_structure(StructureType::FuelDepot, 0, {20, 12}, 2, 2);
    // Next to the depot, but out of reach of the shells aimed at its middle.
    const EntityId bystander = w.spawn_unit(0, UnitTypeId::Truck, {Fixed::from_ratio(37, 2), Fixed::from_int(12)});
    const EntityId tank = w.spawn_unit(1, UnitTypeId::Tank, at(27, 13));
    issue(sim, fire_at(1, {tank}, 21, 13));
    for (int i = 0; i < 3000 && sim.world().find_structure(depot); ++i) sim.step();
    CHECK(sim.world().find_structure(depot) == nullptr);

    sim.step();  // the fire's damage lands on the next tick
    CHECK(stock_of(sim, Resource::Fuel) == 100 - 100 * kFuelDepotLossPercent / 100);
    CHECK(sim.world().find_unit(bystander) == nullptr);
}

// Supply trucks carry no guns: they don't take attack orders.
void test_trucks_are_unarmed() {
    Simulation sim = logistics_sim({});
    World& w = sim.world_for_setup();
    const EntityId truck = w.spawn_unit(0, UnitTypeId::Truck, at(10, 5));
    const EntityId enemy = w.spawn_unit(1, UnitTypeId::Rifleman, at(12, 5));
    issue(sim, {.type = CommandType::Attack, .player = 0, .units = {truck}, .target_unit = enemy});
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(hp_of(sim, enemy) == unit_type(UnitTypeId::Rifleman).max_hp);
    CHECK(sim.world().find_unit(truck)->order == Order::Idle);
}

// --- Vehicle supply ------------------------------------------------------------

// Out of fuel a tank stops where it is, but its gun still works.
void test_out_of_fuel_but_still_shooting() {
    Simulation sim(1, TileMap(40, 20));
    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 10));
    sim.world_for_setup().unit_for_setup(tank)->fuel = Fixed::from_int(3);
    issue(sim, make_move(0, {tank}, 30, 10));
    for (int i = 0; i < 400; ++i) sim.step();
    const Unit* t = sim.world().find_unit(tank);
    CHECK(t->fuel == Fixed{});
    CHECK(t->pos.x > Fixed::from_int(7) && t->pos.x < Fixed::from_int(9));  // 3 tiles and no more
    CHECK(t->order == Order::Idle);

    const FixedVec2 stuck = t->pos;
    sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, {stuck.x + Fixed::from_int(6), stuck.y});
    for (int i = 0; i < 100; ++i) sim.step();
    t = sim.world().find_unit(tank);
    CHECK(t->last_shot_tick != kNeverFired);
    CHECK(t->pos == stuck);
}

// A tanker standing by fills up our vehicles close to it, not the far ones.
void test_tanker_refuels_vehicles_nearby() {
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    const EntityId near = w.spawn_unit(0, UnitTypeId::Tank, at(10, 10));
    const EntityId far = w.spawn_unit(0, UnitTypeId::Ifv, at(25, 10));
    w.unit_for_setup(near)->fuel = Fixed{};
    w.unit_for_setup(far)->fuel = Fixed{};
    const EntityId tanker = w.spawn_unit(0, UnitTypeId::FuelTanker, at(7, 10));
    const int32_t cargo = sim.world().find_unit(tanker)->carrying;
    CHECK(cargo == unit_type(UnitTypeId::FuelTanker).cargo_capacity);
    for (int i = 0; i < 500; ++i) sim.step();

    const Unit* t = sim.world().find_unit(near);
    const int32_t handed = cargo - sim.world().find_unit(tanker)->carrying;
    CHECK(t->fuel == unit_type(UnitTypeId::Tank).fuel_capacity);  // full
    CHECK(t->fuel == Fixed::from_int(kTilesPerFuel * handed));   // nothing lost, nothing made up
    CHECK(sim.world().find_unit(far)->fuel == Fixed{});           // out of its reach

    const FixedVec2 filled_at = t->pos;
    issue(sim, make_move(0, {near}, 20, 15));
    for (int i = 0; i < 20; ++i) sim.step();
    CHECK(sim.world().find_unit(near)->pos != filled_at);  // driving again
}

// Out of rounds a tank holds its fire until an ammunition truck rearms it.
void test_out_of_rounds_until_rearmed() {
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    const EntityId tank = w.spawn_unit(0, UnitTypeId::Tank, at(10, 10));
    w.unit_for_setup(tank)->rounds = 0;
    w.spawn_unit(1, UnitTypeId::Rifleman, at(16, 10));
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(sim.world().find_unit(tank)->last_shot_tick == kNeverFired);

    const EntityId truck = w.spawn_unit(0, UnitTypeId::AmmoTruck, at(7, 10));
    for (int i = 0; i < 300; ++i) sim.step();
    const Unit* t = sim.world().find_unit(tank);
    CHECK(t->last_shot_tick != kNeverFired);
    CHECK(t->rounds > 0 && t->rounds <= unit_type(UnitTypeId::Tank).rounds_capacity);
    CHECK(sim.world().find_unit(truck)->carrying < unit_type(UnitTypeId::AmmoTruck).cargo_capacity);


    // Rearmed to the brim, not beyond: an IFV takes five rounds a load.
    Simulation rack(3, TileMap(40, 20));
    const EntityId ifv = rack.world_for_setup().spawn_unit(0, UnitTypeId::Ifv, at(10, 10));
    rack.world_for_setup().unit_for_setup(ifv)->rounds = 3;
    rack.world_for_setup().spawn_unit(0, UnitTypeId::AmmoTruck, at(7, 10));
    for (int i = 0; i < 1000; ++i) rack.step();
    CHECK(rack.world().find_unit(ifv)->rounds == unit_type(UnitTypeId::Ifv).rounds_capacity);

    // Every shot takes a round: two in the racks, two shots, then silence.
    Simulation duel(2, TileMap(40, 20));
    const EntityId gun = duel.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(10, 10));
    duel.world_for_setup().unit_for_setup(gun)->rounds = 2;
    duel.world_for_setup().spawn_unit(1, UnitTypeId::Tank, at(16, 10));
    for (int i = 0; i < 400; ++i) duel.step();
    const Unit* g = duel.world().find_unit(gun);
    CHECK(g && g->rounds == 0);
    CHECK(g && g->last_shot_tick != kNeverFired && g->last_shot_tick < 2 * unit_type(UnitTypeId::Tank).weapon.reload + 5);
}

// An empty tanker loads up at the fuel depot from the stock; hired ones
// come with what was paid for aboard.
void test_service_vehicles_refill_at_depots() {
    const Stock stock = stock_with({{Resource::Fuel, 50 + 40}, {Resource::Personnel, 1}, {Resource::Materials, 60}});
    Simulation sim = economy_sim(stock, false);
    World& w = sim.world_for_setup();
    w.place_structure(StructureType::FuelDepot, 0, {20, 12}, 2, 2);
    const EntityId tanker = w.spawn_unit(0, UnitTypeId::FuelTanker, at(24, 8));
    w.unit_for_setup(tanker)->carrying = 0;
    const EntityId ammo = w.spawn_unit(0, UnitTypeId::AmmoTruck, at(24, 6));
    w.unit_for_setup(ammo)->carrying = 5;
    issue(sim, {.type = CommandType::Train, .player = 0, .target_unit = headquarters_of(sim),
                .unit_type = static_cast<uint8_t>(UnitTypeId::FuelTanker)});
    for (int i = 0; i < 3; ++i) sim.step();
    issue(sim, use_ability(0, {tanker, ammo}, AbilityId::Refill, 0, 0));
    for (int i = 0; i < 500; ++i) sim.step();

    CHECK(sim.world().find_unit(tanker)->carrying == 50);  // all the stock had
    CHECK(stock_of(sim, Resource::Fuel) == 0);
    CHECK(sim.world().find_unit(tanker)->order == Order::Idle);
    CHECK(sim.world().find_unit(ammo)->carrying == 5);  // no ammunition depot to load at
    int hired = 0;
    for (const Unit& u : sim.world().units()) {
        if (u.type != UnitTypeId::FuelTanker || u.id == tanker) continue;
        ++hired;
        CHECK(u.carrying == unit_type(UnitTypeId::FuelTanker).cost[static_cast<size_t>(Resource::Fuel)]);
    }
    CHECK(hired == 1);
}

// A tanker hit with fuel aboard goes up and burns whatever stands next to it;
// an empty one just stops. So does a supply truck with fuel or ammunition
// on it (the rounds cooking off); with food, timber, it just burns out.
void test_tanker_goes_up_in_flames() {
    // (The neighbour a step off on the diagonal: burning fuel reaches it, a cook-off doesn't; `close`: right beside it.)
    auto neighbour_hurt = [](int32_t fuel_aboard, UnitTypeId type = UnitTypeId::FuelTanker, Resource load = Resource::Fuel, bool close = false) {
        Simulation sim(1, TileMap(40, 20));
        World& w = sim.world_for_setup();
        const EntityId tanker = w.spawn_unit(0, type, at(10, 10));
        w.unit_for_setup(tanker)->hp = 1;
        w.unit_for_setup(tanker)->carrying = fuel_aboard;
        w.unit_for_setup(tanker)->carrying_type = load;
        const EntityId neighbour = w.spawn_unit(0, UnitTypeId::Truck, at(11, close ? 10 : 11));
        w.spawn_unit(1, UnitTypeId::Rifleman, at(10, 5));
        for (int i = 0; i < 200 && sim.world().find_unit(tanker); ++i) sim.step();
        CHECK(sim.world().find_unit(tanker) == nullptr);
        sim.step();  // the fire's damage lands on the next tick
        sim.step();
        // Burnt, not just shot at by the rifleman who goes on firing.
        return unit_type(UnitTypeId::Truck).max_hp - hp_of(sim, neighbour) >= 30;
    };
    CHECK(neighbour_hurt(100));
    CHECK(!neighbour_hurt(0));
    CHECK(neighbour_hurt(20, UnitTypeId::Truck, Resource::Fuel));
    CHECK(neighbour_hurt(20, UnitTypeId::Truck, Resource::Ammo, true));
    CHECK(!neighbour_hurt(20, UnitTypeId::Truck, Resource::Ammo));  // (a cook-off reaches less far than burning fuel)
    CHECK(!neighbour_hurt(20, UnitTypeId::Truck, Resource::Food, true));
    CHECK(!neighbour_hurt(20, UnitTypeId::Truck, Resource::Materials, true));
}

// --- Artillery -----------------------------------------------------------------

Fixed abs_fixed(Fixed f) { return f.raw < 0 ? Fixed::from_raw(-f.raw) : f; }

// Where a fire mission's shells come down, in firing order.
std::vector<FixedVec2> shell_landings(Simulation& sim, int ticks) {
    std::vector<FixedVec2> landings;
    uint32_t last = 0;
    for (int i = 0; i < ticks; ++i) {
        sim.step();
        for (const Projectile& p : sim.world().projectiles()) {
            if (p.lobbed && p.id > last) {
                landings.push_back(p.target);
                last = p.id;
            }
        }
    }
    return landings;
}

bool on_the_spot(FixedVec2 landing, FixedVec2 aim) {
    const Fixed close = kOnTargetSpread + Fixed::from_ratio(1, 100);
    return abs_fixed(landing.x - aim.x) <= close && abs_fixed(landing.y - aim.y) <= close;
}

// On-target shells by shot number (1st, 2nd, 3rd) over 100 fire missions of
// a mortar at (5, 10) on (15, 10); `scout` puts one of ours next to the
// target; `silent`, the mortar keeps radio silence, `relay` with a
// signaller next to it.
std::array<int, 3> bracketing(bool scout, bool silent = false, bool relay = false) {
    std::array<int, 3> on{};
    for (uint64_t seed = 1; seed <= 100; ++seed) {
        Simulation sim(seed, TileMap(30, 20));
        const EntityId mortar = sim.world_for_setup().spawn_unit(0, UnitTypeId::Mortar, at(5, 10));
        if (scout) sim.world_for_setup().spawn_unit(0, UnitTypeId::Scout, at(17, 12));
        sim.world_for_setup().unit_for_setup(mortar)->silent = silent;
        if (relay) sim.world_for_setup().spawn_unit(0, UnitTypeId::Signaler, at(5, 12));
        sim.schedule(0, fire_at(0, {mortar}, 15, 10));
        const std::vector<FixedVec2> landings = shell_landings(sim, 300 + static_cast<int>(kCourierTicks));
        CHECK(landings.size() >= 3);
        for (size_t i = 0; i < 3 && i < landings.size(); ++i) on[i] += on_the_spot(landings[i], at(15, 10)) ? 1 : 0;
    }
    return on;
}

// Bracketing: 17%, 50%, then 95% of the shells on the aim point.
void test_artillery_brackets_its_target() {
    const std::array<int, 3> on = bracketing(false);
    CHECK(on[0] >= 5 && on[0] <= 30);
    CHECK(on[1] >= 35 && on[1] <= 65);
    CHECK(on[2] >= 85);
    // A scout looking at the target corrects the fire: a step ahead.
    const std::array<int, 3> spotted = bracketing(true);
    CHECK(spotted[0] >= 35 && spotted[0] <= 65);
    CHECK(spotted[1] >= 85);
    // Keeping radio silence alone, the gun doesn't hear the corrections; a
    // signaller by it passes them on.
    const std::array<int, 3> silent = bracketing(true, true);
    CHECK(silent[0] >= 5 && silent[0] <= 30);
    CHECK(silent[1] >= 35 && silent[1] <= 65);
    const std::array<int, 3> relayed = bracketing(true, true, true);
    CHECK(relayed[0] >= 35 && relayed[0] <= 65);
}

// The same target (a new aim point close to the last) keeps the ranging; a
// new one starts over; the first misses land far, the later ones close.
void test_ranging_follows_the_target() {
    Simulation sim(4, TileMap(40, 20));
    const EntityId mortar = sim.world_for_setup().spawn_unit(0, UnitTypeId::Mortar, at(5, 10));
    sim.schedule(0, fire_at(0, {mortar}, 15, 10));
    shell_landings(sim, 300);
    CHECK(sim.world().find_unit(mortar)->ranging_shots == 3);
    issue(sim, fire_at(0, {mortar}, 16, 10));  // a tile along: still the same target
    shell_landings(sim, 80);
    CHECK(sim.world().find_unit(mortar)->ranging_shots == 3);
    issue(sim, fire_at(0, {mortar}, 12, 16));  // somewhere else: from scratch
    const std::vector<FixedVec2> landings = shell_landings(sim, 70);
    CHECK(sim.world().find_unit(mortar)->ranging_shots == 1);
    CHECK(!landings.empty());

    // Misses of a first shot scatter up to 4 tiles, of a third up to 1.
    Fixed widest_first{};
    Fixed widest_third{};
    for (uint64_t seed = 1; seed <= 30; ++seed) {
        Simulation s(seed, TileMap(30, 20));
        const EntityId m = s.world_for_setup().spawn_unit(0, UnitTypeId::Mortar, at(5, 10));
        s.schedule(0, fire_at(0, {m}, 15, 10));
        const std::vector<FixedVec2> l = shell_landings(s, 300);
        if (l.size() < 3) continue;
        widest_first = max(widest_first, max(abs_fixed(l[0].x - Fixed::from_int(15)), abs_fixed(l[0].y - Fixed::from_int(10))));
        widest_third = max(widest_third, max(abs_fixed(l[2].x - Fixed::from_int(15)), abs_fixed(l[2].y - Fixed::from_int(10))));
    }
    CHECK(widest_first > Fixed::from_int(2) && widest_first <= kRangingSpread[0]);
    CHECK(widest_third <= kRangingSpread[2]);

    // Nearer, the scatter's tighter; farther, wider (at 10 tiles, half the
    // mortar's range, it's as above).
    auto widest_at = [](int32_t x) {
        Fixed widest{};
        for (uint64_t seed = 1; seed <= 30; ++seed) {
            Simulation s(seed, TileMap(40, 20));
            const EntityId m = s.world_for_setup().spawn_unit(0, UnitTypeId::Mortar, at(5, 10));
            s.schedule(0, fire_at(0, {m}, x, 10));
            const std::vector<FixedVec2> l = shell_landings(s, 120);
            if (l.empty()) continue;
            widest = max(widest, max(abs_fixed(l[0].x - Fixed::from_int(x)), abs_fixed(l[0].y - Fixed::from_int(10))));
        }
        return widest;
    };
    const Fixed near = widest_at(10);
    const Fixed far = widest_at(23);
    CHECK(near <= kRangingSpread[0] / 2 + Fixed::from_ratio(1, 100));
    CHECK(far > kRangingSpread[0] && far <= kRangingSpread[0] * 2);
}

// A howitzer sets up before it fires and packs up before it moves.
void test_guns_deploy_and_pack_up() {
    Simulation sim(1, TileMap(40, 20));
    const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Howitzer, at(5, 10));
    const Tick setup = unit_type(UnitTypeId::Howitzer).deploy_time;
    sim.schedule(0, fire_at(0, {gun}, 25, 10));
    for (Tick i = 0; i + 2 < setup; ++i) sim.step();
    CHECK(sim.world().find_unit(gun)->last_shot_tick == kNeverFired);
    CHECK(!sim.world().find_unit(gun)->deployed);
    for (int i = 0; i < 10 + kLayStepTicks; ++i) sim.step();  // set up, then laid
    CHECK(sim.world().find_unit(gun)->deployed);
    CHECK(sim.world().find_unit(gun)->last_shot_tick != kNeverFired);

    const FixedVec2 set_up_at = sim.world().find_unit(gun)->pos;
    issue(sim, make_move(0, {gun}, 5, 18));
    for (Tick i = 0; i + 2 < setup; ++i) sim.step();
    CHECK(sim.world().find_unit(gun)->pos == set_up_at);  // still packing up
    for (int i = 0; i < 40; ++i) sim.step();
    CHECK(!sim.world().find_unit(gun)->deployed);
    CHECK(sim.world().find_unit(gun)->pos != set_up_at);
}

// Nothing closer than the minimum range, and no firing at whatever shows up:
// guns fire when told to.
void test_guns_hold_fire_unless_ordered() {
    Simulation sim(1, TileMap(40, 20));
    const EntityId mortar = sim.world_for_setup().spawn_unit(0, UnitTypeId::Mortar, at(5, 10));
    sim.schedule(0, fire_at(0, {mortar}, 6, 10));  // too close to lob at
    for (int i = 0; i < 200; ++i) sim.step();
    CHECK(sim.world().find_unit(mortar)->last_shot_tick == kNeverFired);

    Simulation idle(1, TileMap(40, 20));
    const EntityId gun = idle.world_for_setup().spawn_unit(0, UnitTypeId::Mortar, at(5, 10));
    idle.world_for_setup().unit_for_setup(gun)->deployed = true;
    idle.world_for_setup().spawn_unit(1, UnitTypeId::Truck, at(10, 10));  // in plain sight and range
    for (int i = 0; i < 200; ++i) idle.step();
    CHECK(idle.world().find_unit(gun)->last_shot_tick == kNeverFired);
}

// A gun far behind the lines stays hidden when it fires; from a village the
// locals report it for a while; an observation post facing it sees the
// flash from twice its reach.
void test_firing_guns_give_themselves_away() {
    auto seen_after_firing = [](bool village, bool post, Tick wait_after) {
        TileMap map(60, 20);
        if (village) {
            for (int y = 8; y <= 12; ++y) {
                for (int x = 38; x <= 42; ++x) map.set_terrain(x, y, Terrain::Urban);
            }
        }
        Simulation sim(1, map);
        World& w = sim.world_for_setup();
        const EntityId mortar = w.spawn_unit(0, UnitTypeId::Mortar, at_half(81, 21));  // (40.5, 10.5)
        w.spawn_unit(1, UnitTypeId::Rifleman, at(3, 3));  // the enemy, far away
        if (post) {
            const EntityId scout = w.spawn_unit(1, UnitTypeId::Scout, at_half(31, 21));  // 25 tiles off
            sim.schedule(0, observe(1, {scout}, 40, 10));
        }
        sim.schedule(0, fire_at(0, {mortar}, 52, 10));
        bool before = false;
        for (int i = 0; i < 400 && sim.world().find_unit(mortar)->last_shot_tick == kNeverFired; ++i) {
            sim.step();
            before = before || sim.world().sees(1, *sim.world().find_unit(mortar));
        }
        CHECK(!before);
        issue(sim, {.type = CommandType::Stop, .player = 0, .units = {mortar}});  // one shot, then quiet
        for (Tick i = 0; i < wait_after; ++i) sim.step();
        return sim.world().sees(1, *sim.world().find_unit(mortar));
    };
    CHECK(!seen_after_firing(false, false, 2 * kVisionInterval));
    CHECK(seen_after_firing(true, false, 2 * kVisionInterval));   // the locals tell
    CHECK(!seen_after_firing(true, false, kReportedTicks + 2 * kVisionInterval));
    CHECK(seen_after_firing(false, true, 2 * kVisionInterval));   // the flash
    CHECK(!seen_after_firing(false, true, kGunRevealTicks + 2 * kVisionInterval));
}

// A mortar crew digs a closed position: cover from fire, and out of sight
// of an enemy a few tiles off.
void test_gun_pits() {
    Simulation sim(1, TileMap(30, 20));
    const EntityId mortar = sim.world_for_setup().spawn_unit(0, UnitTypeId::Mortar, post());
    sim.schedule(0, use_ability(0, {mortar}, AbilityId::DigGunPit, 0, 0));
    for (Tick i = 0; i < kGunPitWork + 5; ++i) sim.step();
    const Structure* pit = sim.world().structure_at({10, 10});
    CHECK(pit && pit->type == StructureType::GunPit);
    CHECK(sim.world().map().terrain(10, 10) == Terrain::GunPit);
    CHECK(sim.world().map().passable({10, 10}, MoveClass::Wheeled));  // a towed gun can stand in one

    const EntityId watcher = sim.world_for_setup().spawn_unit(1, UnitTypeId::Truck, at(14, 10));
    for (Tick i = 0; i < 2 * kVisionInterval; ++i) sim.step();
    CHECK(!seen(sim, 1, mortar));
    CHECK(sim.world().find_unit(watcher));

    const int32_t open = damage_taken(false, in_the_open, UnitTypeId::Mortar);
    const int32_t dug_in = damage_taken(false, [](Simulation& s, EntityId) {
        s.world_for_setup().place_structure(StructureType::GunPit, 0, {10, 10}, 1, 1);
    }, UnitTypeId::Mortar);
    CHECK(open > 0);
    CHECK(dug_in * 100 < open * 70);
}

// Camouflage hides a howitzer in the open until it moves.
void test_camouflaged_guns() {
    Simulation sim(1, TileMap(40, 20));
    const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Howitzer, post());
    sim.world_for_setup().spawn_unit(1, UnitTypeId::Truck, {post().x + Fixed::from_int(5), post().y});
    for (Tick i = 0; i < 2 * kVisionInterval; ++i) sim.step();
    CHECK(seen(sim, 1, gun));
    issue(sim, use_ability(0, {gun}, AbilityId::Camouflage, 0, 0));
    for (Tick i = 0; i < kCamouflageWork + 2 * kVisionInterval; ++i) sim.step();
    CHECK(sim.world().find_unit(gun)->camouflaged);
    CHECK(!seen(sim, 1, gun));
    issue(sim, make_move(0, {gun}, 10, 15));
    for (Tick i = 0; i < 30; ++i) sim.step();
    CHECK(!sim.world().find_unit(gun)->camouflaged);
    CHECK(seen(sim, 1, gun));
}

// Rapid fire: five grenades in a row across the line of fire, the middle one
// on the point; about one burst in twenty lands them dead in line.
void test_ags_rapid_fire() {
    int perfect = 0;
    for (uint64_t seed = 1; seed <= 100; ++seed) {
        Simulation sim(seed, TileMap(30, 20));
        const EntityId ags = sim.world_for_setup().spawn_unit(0, UnitTypeId::Ags, at(5, 10));
        sim.schedule(0, use_ability(0, {ags}, AbilityId::RapidFire, 12, 10));
        const std::vector<FixedVec2> grenades = shell_landings(sim, 40);
        CHECK(grenades.size() == static_cast<size_t>(kBurstGrenades));
        if (grenades.size() != static_cast<size_t>(kBurstGrenades)) continue;
        bool in_line = true;
        for (int k = 0; k < kBurstGrenades; ++k) {
            const Fixed ideal_y = Fixed::from_int(10) + kBurstSpacing * (k - kBurstGrenades / 2);
            const FixedVec2 g = grenades[static_cast<size_t>(k)];
            CHECK(abs_fixed(g.x - Fixed::from_int(12)) <= kBurstJitter + Fixed::from_ratio(1, 20));
            CHECK(abs_fixed(g.y - ideal_y) <= kBurstJitter + Fixed::from_ratio(1, 20));
            in_line = in_line && abs_fixed(g.x - Fixed::from_int(12)) <= Fixed::from_ratio(1, 50) &&
                      abs_fixed(g.y - ideal_y) <= Fixed::from_ratio(1, 50);
        }
        perfect += in_line ? 1 : 0;
        CHECK(sim.world().find_unit(ags)->rounds == unit_type(UnitTypeId::Ags).rounds_capacity - kBurstGrenades);
    }
    CHECK(perfect >= 1 && perfect <= 12);
}

// The AGS lobs its grenades over a ridge and the parapet, from where it
// stands: a man at a position in a trench behind the ridge gets hurt all the same.
void test_ags_reaches_into_trenches() {
    int hurt = 0;
    for (uint64_t seed = 1; seed <= 10; ++seed) {
        TileMap map(30, 20);
        for (int y = 0; y < 20; ++y) map.set_elevation(13, y, 3);
        Simulation sim(seed, map);
        World& w = sim.world_for_setup();
        w.place_structure(StructureType::Trench, 0, {10, 10}, 1, 1);
        const EntityId man = w.spawn_unit(0, UnitTypeId::Rifleman, post());
        settle(sim);
        const EntityId ags = w.spawn_unit(1, UnitTypeId::Ags, {post().x + Fixed::from_int(6), post().y});
        Command fire = make_order(CommandType::AttackGround, 1, {ags}, 0, 0);
        fire.target = post();
        sim.schedule(sim.world().tick(), fire);
        const FixedVec2 stands = sim.world().find_unit(ags)->pos;
        for (int i = 0; i < 100; ++i) sim.step();
        hurt += hp_of(sim, man) < unit_type(UnitTypeId::Rifleman).max_hp ? 1 : 0;
        CHECK(sim.world().find_unit(ags)->pos == stands);  // no need to climb the ridge
    }
    CHECK(hurt >= 8);
}

// A salvo: everything in the launcher over an area, then empty.
void test_mlrs_salvo() {
    Simulation sim(1, TileMap(40, 30));
    const EntityId mlrs = sim.world_for_setup().spawn_unit(0, UnitTypeId::Mlrs, at(5, 15));
    sim.schedule(0, use_ability(0, {mlrs}, AbilityId::Salvo, 25, 15));
    const std::vector<FixedVec2> rockets =
        shell_landings(sim, static_cast<int>(unit_type(UnitTypeId::Mlrs).deploy_time + 100));
    CHECK(rockets.size() == static_cast<size_t>(unit_type(UnitTypeId::Mlrs).rounds_capacity));
    Fixed widest{};
    for (const FixedVec2& r : rockets) {
        widest = max(widest, max(abs_fixed(r.x - Fixed::from_int(25)), abs_fixed(r.y - Fixed::from_int(15))));
    }
    // At 20 tiles, not quite a third of its range: tighter than at half of it.
    CHECK(widest <= kSalvoSpread * 3 / 5 && widest > Fixed::from_int(1));
    const Unit* u = sim.world().find_unit(mlrs);
    CHECK(u->rounds == 0 && u->order == Order::Idle);

    Simulation near(1, TileMap(40, 30));
    const EntityId close = near.world_for_setup().spawn_unit(0, UnitTypeId::Mlrs, at(5, 15));
    near.schedule(0, use_ability(0, {close}, AbilityId::Salvo, 10, 15));  // inside the minimum range
    CHECK(shell_landings(near, 200).empty());
}

// A tank fires from behind a ridge like artillery, out to twice its direct
// range; each shot wears the barrel by 1% of its HP.
void test_tank_indirect_fire() {
    TileMap map(40, 20);
    for (int y = 0; y < 20; ++y) map.set_elevation(9, y, 3);
    Simulation sim(1, map);
    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 10));
    sim.schedule(0, use_ability(0, {tank}, AbilityId::IndirectFire, 17, 10));  // 12 tiles: past direct range
    const std::vector<FixedVec2> shells = shell_landings(sim, 300);
    const Unit* t = sim.world().find_unit(tank);
    CHECK(shells.size() >= 3);
    const int32_t wear = unit_type(UnitTypeId::Tank).max_hp * kBarrelWearPercent / 100;
    CHECK(t->hp == unit_type(UnitTypeId::Tank).max_hp - wear * static_cast<int32_t>(shells.size()));
    CHECK(t->ranging_shots == 3);
    CHECK(t->pos == at(5, 10));  // stays behind its ridge
    CHECK(t->rounds == unit_type(UnitTypeId::Tank).rounds_capacity - static_cast<int32_t>(shells.size()));
}

// A self-propelled howitzer: sets up in two seconds, fires like artillery,
// burns fuel driving and crosses trenches on its tracks.
void test_self_propelled_howitzer() {
    const UnitTypeDef& def = unit_type(UnitTypeId::Spg);
    CHECK(def.deploy_time < unit_type(UnitTypeId::Howitzer).deploy_time);
    CHECK(move_class(def) == MoveClass::Vehicle);
    CHECK(can_train(StructureType::ArtilleryBarracks, UnitTypeId::Spg));

    TileMap map(40, 20);
    for (int y = 0; y < 20; ++y) map.set_terrain(8, y, Terrain::Trench);
    Simulation sim(1, map);
    const EntityId spg = sim.world_for_setup().spawn_unit(0, UnitTypeId::Spg, at(5, 10));
    issue(sim, make_move(0, {spg}, 12, 10));  // over the trench
    for (int i = 0; i < 300; ++i) sim.step();
    const Unit* u = sim.world().find_unit(spg);
    CHECK(u->pos.x > Fixed::from_int(10));
    CHECK(u->fuel < def.fuel_capacity);

    issue(sim, fire_at(0, {spg}, 30, 10));
    const std::vector<FixedVec2> shells = shell_landings(sim, static_cast<int>(def.deploy_time + 10));
    CHECK(shells.size() == 1);  // set up and firing within moments
    CHECK(sim.world().find_unit(spg)->deployed);
}

// --- Upgrades ------------------------------------------------------------------

// Research in the ammunition depot: paid up front, done after its time, not
// twice; until then the tank's smoke screen isn't there.
void test_research() {
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    const UpgradeDef& smoke = upgrade_def(UpgradeId::SmokeGrenades);
    Stock twice = smoke.cost;
    for (int32_t& amount : twice) amount *= 2;
    w.set_stock(0, twice);
    const EntityId depot = w.place_structure(StructureType::AmmoDepot, 0, {20, 5}, 2, 2);
    const EntityId barracks = w.place_structure(StructureType::InfantryBarracks, 0, {25, 5}, 3, 3);
    const EntityId tank = w.spawn_unit(0, UnitTypeId::Tank, at(10, 10));
    Command research{.type = CommandType::Research, .player = 0, .target_unit = depot,
                     .upgrade = static_cast<uint8_t>(UpgradeId::SmokeGrenades)};
    Command wrong = research;
    wrong.target_unit = barracks;  // not researched there
    issue(sim, wrong);
    issue(sim, research);
    for (int i = 0; i < 5; ++i) sim.step();
    CHECK(sim.world().stock(0) == smoke.cost);  // paid once
    CHECK(sim.world().find_structure(barracks)->research == UpgradeId::Count);
    CHECK(sim.world().find_structure(depot)->research == UpgradeId::SmokeGrenades);
    issue(sim, use_ability(0, {tank}, AbilityId::Smoke, 0, 0));
    for (int i = 0; i < 5; ++i) sim.step();
    CHECK(sim.world().smokes().empty());  // not yet researched

    for (Tick i = 0; i < smoke.time; ++i) sim.step();
    CHECK(sim.world().has_upgrade(0, UpgradeId::SmokeGrenades));
    CHECK(!sim.world().has_upgrade(1, UpgradeId::SmokeGrenades));
    issue(sim, research);  // already have it
    for (int i = 0; i < 5; ++i) sim.step();
    CHECK(sim.world().stock(0) == smoke.cost);
    issue(sim, use_ability(0, {tank}, AbilityId::Smoke, 0, 0));
    for (int i = 0; i < 5; ++i) sim.step();
    CHECK(sim.world().smokes().size() == 1);
}

// Nothing is seen into or through a smoke screen, until it clears.
void test_smoke_screen() {
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    w.upgrade_for_setup(0, UpgradeId::SmokeGrenades);
    const EntityId tank = w.spawn_unit(0, UnitTypeId::Tank, at(10, 10));
    w.unit_for_setup(tank)->rounds = 0;  // it only hides here, it doesn't shoot the enemy's eyes out
    const EntityId enemy = w.spawn_unit(1, UnitTypeId::Truck, at(15, 10));
    w.spawn_unit(0, UnitTypeId::Truck, at(10, 6));  // another pair of eyes, beside the screen
    // One of ours just inside the screen's edge, as an enemy tank to the side sees it (a screen isn't thin smoke).
    const EntityId edge = w.spawn_unit(0, UnitTypeId::Truck, at(12, 11));
    const EntityId side = w.spawn_unit(1, UnitTypeId::Tank, at(12, 16));
    w.unit_for_setup(side)->rounds = 0;
    for (Tick i = 0; i < 2 * kVisionInterval; ++i) sim.step();
    CHECK(seen(sim, 0, enemy) && seen(sim, 1, tank));
    issue(sim, {.type = CommandType::Stop, .player = 0, .units = {tank}});
    w.unit_for_setup(tank)->facing = {Fixed::from_int(1), Fixed{}};  // towards the enemy
    issue(sim, use_ability(0, {tank}, AbilityId::Smoke, 0, 0));
    for (Tick i = 0; i < 2 * kVisionInterval + 2; ++i) sim.step();
    CHECK(sim.world().smokes().size() == 1);
    CHECK(!seen(sim, 1, tank));
    CHECK(!seen(sim, 1, edge));
    for (Tick i = 0; i < kSmokeTicks; ++i) sim.step();
    CHECK(sim.world().smokes().empty());
    CHECK(seen(sim, 1, tank));
}

// A knocked-out vehicle burns: its smoke hides what's behind it until it
// burns out. It's thin smoke: a line of sight gets through a little of it
// (a man just inside its edge is seen), not through the thick of it.
void test_burning_wreck_smoke() {
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    const EntityId eyes = w.spawn_unit(0, UnitTypeId::Tank, at(4, 10));
    w.unit_for_setup(eyes)->rounds = 0;
    const EntityId far = w.spawn_unit(1, UnitTypeId::Truck, at(14, 10));
    // Its smoke blown off it (see kPlumeDrift): the line of sight passes 0.85 of a tile off its middle.
    const EntityId wreck = w.spawn_unit(1, UnitTypeId::Truck, {Fixed::from_int(9), Fixed::from_ratio(1168, 100)});
    for (Tick i = 0; i < 2 * kVisionInterval; ++i) sim.step();
    CHECK(seen(sim, 0, far));
    w.unit_for_setup(wreck)->hp = 0;  // knocked out
    sim.step();
    CHECK(sim.world().smokes().size() == 1 && sim.world().smokes().front().kind == SmokeKind::Plume);
    const EntityId near = w.spawn_unit(1, UnitTypeId::Truck, at(9, 10));  // just inside its edge
    for (Tick i = 0; i < 2 * kVisionInterval; ++i) sim.step();
    CHECK(!seen(sim, 0, far));
    CHECK(seen(sim, 0, near));
    for (Tick i = 0; i < kPlumeTicks; ++i) sim.step();
    CHECK(sim.world().smokes().empty());
    CHECK(seen(sim, 0, far));

    // Nor does an observation post see through it.
    Simulation post(1, TileMap(40, 20));
    World& pw = post.world_for_setup();
    const EntityId scout = pw.spawn_unit(0, UnitTypeId::Scout, at(2, 10));
    const EntityId beyond = pw.spawn_unit(1, UnitTypeId::Truck, at(15, 10));  // out of his own sight, in his sector's
    const EntityId burning = pw.spawn_unit(1, UnitTypeId::Truck, {Fixed::from_int(9), Fixed::from_ratio(1100, 100)});
    issue(post, observe(0, {scout}, 20, 10));
    for (Tick i = 0; i < 4 * kVisionInterval; ++i) post.step();
    CHECK(seen(post, 0, beyond));
    pw.unit_for_setup(burning)->hp = 0;
    for (Tick i = 0; i < 2 * kVisionInterval; ++i) post.step();
    CHECK(!seen(post, 0, beyond));
}

// A burst of a tile and more raises dust and smoke: three quarters as wide,
// a few seconds, hiding what's behind it.
void test_burst_dust() {
    {  // a tank's shell, a small burst: none
        Simulation shot(1, TileMap(40, 20));
        const EntityId tank = shot.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 10));
        shot.schedule(0, fire_at(0, {tank}, 12, 10));
        bool burst = false;
        for (int i = 0; i < 40; ++i) {
            shot.step();
            burst = burst || !shot.world().recent_impacts().empty();
        }
        CHECK(burst && shot.world().smokes().empty());
    }
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    const EntityId mortar = w.spawn_unit(0, UnitTypeId::Mortar, at(5, 10));
    issue(sim, fire_at(0, {mortar}, 15, 10));
    const Smoke* dust = nullptr;
    for (int i = 0; i < 400 && !dust; ++i) {
        sim.step();
        for (const Smoke& s : sim.world().smokes()) {
            if (s.kind == SmokeKind::Dust) dust = &s;
        }
    }
    CHECK(dust != nullptr);
    if (!dust) return;
    issue(sim, {.type = CommandType::Stop, .player = 0, .units = {mortar}});
    const Fixed splash = unit_type(UnitTypeId::Mortar).weapon.splash_radius;
    CHECK(dust->radius == splash * kDustPercent / 100);
    CHECK(dust->clears - sim.world().tick() + 1 == kDustTicks + static_cast<Tick>((splash * kDustTicksPerTile).to_int()));
    const Tick clears = dust->clears;
    // Either side of it, three tiles off.
    const TilePos mid = tile_of(dust->center);
    const EntityId eyes = w.spawn_unit(0, UnitTypeId::Tank, {Fixed::from_int(mid.x - 3), Fixed::from_int(mid.y)});
    w.unit_for_setup(eyes)->rounds = 0;
    const EntityId other = w.spawn_unit(1, UnitTypeId::Truck, {Fixed::from_int(mid.x + 3), Fixed::from_int(mid.y)});
    for (Tick i = 0; i < 2 * kVisionInterval; ++i) sim.step();
    CHECK(!seen(sim, 0, other));
    while (sim.world().tick() <= clears + kVisionInterval) sim.step();
    CHECK(seen(sim, 0, other));
}

// The crew lays the gun for the range before it fires, a step of
// elevation at a time (see elevation_step); set up, it stands ready at the second.
void test_guns_lay_before_firing() {
    auto first_shot = [](int32_t x) {  // a D-30 set up at (5, 10), told to fire at (x, 10)
        Simulation sim(1, TileMap(80, 20));
        const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Howitzer, at(5, 10));
        Unit* u = sim.world_for_setup().unit_for_setup(gun);
        u->deployed = true;
        u->laid = u->laying_to = kReadyStep;
        sim.schedule(0, fire_at(0, {gun}, x, 10));
        for (int i = 0; i < 100; ++i) {
            sim.step();
            if (sim.world().find_unit(gun)->last_shot_tick != kNeverFired) return i;
        }
        return -1;
    };
    // Its reach is 60: half of it as it stands (the 2nd step), a quarter (the 1st), nearly all (the 4th).
    const int ready = first_shot(35);
    const int lower = first_shot(20);
    const int higher = first_shot(62);
    CHECK(ready >= 0);
    CHECK(lower == ready + kLayStepTicks - 1);
    CHECK(higher == ready + 2 * kLayStepTicks - 1);

    Simulation sim(1, TileMap(40, 20));
    const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Howitzer, at(5, 10));
    issue(sim, use_ability(0, {gun}, AbilityId::Deploy, 0, 0));
    for (Tick i = 0; i < unit_type(UnitTypeId::Howitzer).deploy_time + 5; ++i) sim.step();
    CHECK(sim.world().find_unit(gun)->deployed && sim.world().find_unit(gun)->laid == kReadyStep);
}

// Trains: a new schedule brings them every 45 s, heavier ones bring half as much again.
void test_train_upgrades() {
    Simulation sim = logistics_sim({});
    sim.world_for_setup().upgrade_for_setup(0, UpgradeId::TrainSchedule);
    sim.world_for_setup().upgrade_for_setup(0, UpgradeId::TrainCapacity);
    for (Tick i = 0; i <= kTrainInterval; ++i) sim.step();  // the first one, on the old timetable
    CHECK(stock_of(sim, Resource::Personnel) == kTrainCargo[kMen] * kTrainCapacityPercent / 100);
    for (Tick i = 0; i < kTrainIntervalUpgraded; ++i) sim.step();
    CHECK(stock_of(sim, Resource::Personnel) == 2 * (kTrainCargo[kMen] * kTrainCapacityPercent / 100));
}

// Sabot rounds hit harder; optics let scouts make men out farther; shovels
// dig faster; cluster rockets burst wider.
void test_upgrade_effects() {
    auto ap_hit = [](bool sabot) {
        Simulation sim(3, TileMap(30, 20));
        if (sabot) sim.world_for_setup().upgrade_for_setup(0, UpgradeId::SabotRounds);
        const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 10));
        const EntityId target = sim.world_for_setup().spawn_unit(1, UnitTypeId::Tank, at(11, 10));
        sim.world_for_setup().unit_for_setup(gun)->round_type = 1;
        sim.world_for_setup().unit_for_setup(target)->hull = {Fixed::from_int(-1), Fixed{}};  // front on
        sim.schedule(0, make_move(1, {target}, 11, 10));
        for (int i = 0; i < 300; ++i) {
            sim.step();
            if (hp_of(sim, target) < unit_type(UnitTypeId::Tank).max_hp) break;
        }
        return unit_type(UnitTypeId::Tank).max_hp - hp_of(sim, target);
    };
    const WeaponDef& ap = unit_type(UnitTypeId::Tank).alt_weapon;
    const int32_t armor = unit_type(UnitTypeId::Tank).armor[static_cast<size_t>(DamageType::AntiTank)];
    CHECK(ap_hit(false) == ap.damage - armor);
    CHECK(ap_hit(true) == ap.damage * kSabotPercent / 100 - armor);

    auto scout_sees = [](bool optics) {
        TileMap map(40, 20);
        for (int y = 3; y <= 17; ++y) {
            for (int x = 12; x <= 15; ++x) map.set_terrain(x, y, Terrain::Forest);
        }
        Simulation sim(1, map);
        if (optics) sim.world_for_setup().upgrade_for_setup(0, UpgradeId::Optics);
        const EntityId hidden = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at_half(25, 21));
        sim.world_for_setup().spawn_unit(0, UnitTypeId::Scout, at_half(15, 21));  // 5 tiles off
        sim.step();
        return seen(sim, 0, hidden);
    };
    CHECK(!scout_sees(false));
    CHECK(scout_sees(true));

    auto foxhole_ticks = [](bool shovels) {
        Simulation sim(1, TileMap(20, 20));
        if (shovels) sim.world_for_setup().upgrade_for_setup(0, UpgradeId::EntrenchingTools);
        const EntityId man = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, post());
        sim.schedule(0, use_ability(0, {man}, AbilityId::DigFoxhole, 0, 0));
        int ticks = 0;
        for (; ticks < 1000 && !sim.world().structure_at({10, 10}); ++ticks) sim.step();
        return ticks;
    };
    CHECK(foxhole_ticks(true) * 100 < foxhole_ticks(false) * 75);

    // Cluster munitions: a rocket launcher loaded with them fires cluster rockets.
    Simulation rockets(1, TileMap(40, 30));
    rockets.world_for_setup().upgrade_for_setup(0, UpgradeId::ClusterMunitions);
    const EntityId mlrs = rockets.world_for_setup().spawn_unit(0, UnitTypeId::Mlrs, at(5, 15));
    rockets.schedule(0, load_shell(0, {mlrs}, Shell::Cluster));
    rockets.schedule(1, use_ability(0, {mlrs}, AbilityId::Salvo, 25, 15));
    Shell fired = Shell::He;
    bool any = false;
    for (int i = 0; i < 400 && !any; ++i) {
        rockets.step();
        for (const Projectile& p : rockets.world().projectiles()) {
            fired = p.shell;
            any = true;
        }
    }
    CHECK(any && fired == Shell::Cluster);
}

// --- Engineering ---------------------------------------------------------------

// A sapper of `player` lays a mine on (x, y), with the ammunition for it.
void lay_a_mine(Simulation& sim, PlayerId player, bool anti_tank, int32_t x, int32_t y) {
    World& w = sim.world_for_setup();
    Stock stock = w.stock(player);
    stock[static_cast<size_t>(Resource::Ammo)] += 10;
    w.set_stock(player, stock);
    const EntityId sapper = w.spawn_unit(player, UnitTypeId::Sapper, at_half(2 * x + 1, 2 * y + 3));
    issue(sim, use_ability_at(player, {sapper}, anti_tank ? AbilityId::LayAtMine : AbilityId::LayApMine,
                              at_half(2 * x + 1, 2 * y + 1)));
    for (Tick i = 0; i < kMineWork + 60; ++i) sim.step();
    // Out of the way, so it neither triggers nor finds anything.
    w.unit_for_setup(sapper)->hp = 0;
    sim.step();
}

// A mine goes off under the first enemy of its kind to come onto its tile;
// the side that laid it walks over it safely.
void test_mines() {
    auto crossing = [](UnitTypeId who, PlayerId owner_of_walker, bool anti_tank) {
        Simulation sim(1, TileMap(40, 20));
        lay_a_mine(sim, 0, anti_tank, 15, 10);
        CHECK(sim.world().mines().size() == 1);
        const EntityId walker = sim.world_for_setup().spawn_unit(owner_of_walker, who, at_half(21, 21));
        issue(sim, make_move(owner_of_walker, {walker}, 25, 10));
        for (int i = 0; i < 600; ++i) sim.step();
        struct Result {
            bool blown;
            int32_t hp_lost;
        };
        return Result{sim.world().mines().empty(), unit_type(who).max_hp - hp_of(sim, walker)};
    };
    const auto enemy_on_ap = crossing(UnitTypeId::Rifleman, 1, false);
    CHECK(enemy_on_ap.blown && enemy_on_ap.hp_lost > 0);
    const auto own_on_ap = crossing(UnitTypeId::Rifleman, 0, false);
    CHECK(!own_on_ap.blown && own_on_ap.hp_lost == 0);
    const auto tank_on_ap = crossing(UnitTypeId::Tank, 1, false);
    CHECK(!tank_on_ap.blown && tank_on_ap.hp_lost == 0);  // too little to set off
    const auto tank_on_at = crossing(UnitTypeId::Tank, 1, true);
    CHECK(tank_on_at.blown && tank_on_at.hp_lost >= 100);
    const auto man_on_at = crossing(UnitTypeId::Rifleman, 1, true);
    CHECK(!man_on_at.blown);

    // Paid for from the stock: 5 ammunition an AP mine.
    Simulation sim(1, TileMap(40, 20));
    lay_a_mine(sim, 0, false, 15, 10);
    CHECK(sim.world().stock(0)[static_cast<size_t>(Resource::Ammo)] == 10 - kApMineCost[static_cast<size_t>(Resource::Ammo)]);
}

// The enemy doesn't know a mine is there until one of his sappers comes
// close; then a sapper can lift it.
void test_sappers_find_and_clear_mines() {
    Simulation sim(1, TileMap(40, 20));
    lay_a_mine(sim, 1, true, 15, 10);
    CHECK(!sim.world().knows(0, sim.world().mines().front()));
    CHECK(sim.world().knows(1, sim.world().mines().front()));

    sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(14, 10));  // right next to it
    for (Tick i = 0; i < 2 * kVisionInterval; ++i) sim.step();
    CHECK(!sim.world().knows(0, sim.world().mines().front()));

    const EntityId far_sapper = sim.world_for_setup().spawn_unit(0, UnitTypeId::Sapper, at(12, 13));
    for (Tick i = 0; i < 2 * kVisionInterval; ++i) sim.step();
    CHECK(!sim.world().knows(0, sim.world().mines().front()));  // too far to find it
    const EntityId sapper = sim.world_for_setup().spawn_unit(0, UnitTypeId::Sapper, at(14, 12));
    for (Tick i = 0; i < 2 * kVisionInterval; ++i) sim.step();
    CHECK(sim.world().knows(0, sim.world().mines().front()));
    CHECK(sim.world().find_unit(far_sapper));

    issue(sim, use_ability(0, {sapper}, AbilityId::ClearMines, 15, 10));
    for (Tick i = 0; i < kClearWork + 100; ++i) sim.step();
    CHECK(sim.world().mines().empty());
    CHECK(hp_of(sim, sapper) == unit_type(UnitTypeId::Sapper).max_hp);  // lifted, not set off
}

// Wire: infantry crawls through, wheels stop, tracks roll it flat.
// Hedgehogs: no vehicle gets through.
void test_wire_and_hedgehogs() {
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    w.set_stock(0, stock_with({{Resource::Materials, 15 + 10}}));
    const EntityId sapper = w.spawn_unit(0, UnitTypeId::Sapper, at(8, 10));
    Command wire = use_ability(0, {sapper}, AbilityId::LayWire, 10, 9);
    wire.target_end = at(10, 11);
    issue(sim, wire);
    for (int i = 0; i < 600; ++i) sim.step();
    for (int y = 9; y <= 11; ++y) CHECK(sim.world().map().terrain(10, y) == Terrain::Wire);
    CHECK(sim.world().stock(0)[static_cast<size_t>(Resource::Materials)] == 10);
    const TileMap& map = sim.world().map();
    CHECK(map.speed_percent({10, 10}, MoveClass::Foot) < 50);
    CHECK(!map.passable({10, 10}, MoveClass::Wheeled));

    Command posts = use_ability(0, {sapper}, AbilityId::PlaceHedgehogs, 14, 9);
    posts.target_end = at(14, 12);
    issue(sim, posts);
    for (int i = 0; i < 800; ++i) sim.step();
    int hedgehogs = 0;
    for (int y = 9; y <= 12; ++y) {
        if (sim.world().map().terrain(14, y) != Terrain::Hedgehogs) continue;
        ++hedgehogs;
        CHECK(!sim.world().map().passable({14, y}, MoveClass::Vehicle));
        CHECK(sim.world().map().passable({14, y}, MoveClass::Foot));
    }
    CHECK(hedgehogs == 1);  // the stock ran out after one

    // A tank drives through the wire and flattens it.
    const EntityId tank = w.spawn_unit(0, UnitTypeId::Tank, at(6, 10));
    issue(sim, make_move(0, {tank}, 12, 10));
    for (int i = 0; i < 200; ++i) sim.step();
    CHECK(sim.world().map().terrain(10, 10) == Terrain::Grass);
    CHECK(sim.world().map().terrain(10, 9) == Terrain::Wire);
}

// A pillbox: its garrison fires through the slit only, bullets don't get
// in, a rocket through the slit does.
void test_pillbox() {
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    w.set_stock(0, structure_type(StructureType::Pillbox).cost);
    const EntityId sapper = w.spawn_unit(0, UnitTypeId::Sapper, post());
    issue(sim, use_ability(0, {sapper}, AbilityId::BuildPillbox, 20, 10));  // facing east
    for (Tick i = 0; i < structure_type(StructureType::Pillbox).build_time + 60; ++i) sim.step();
    const Structure* box = sim.world().structure_at({10, 10});
    CHECK(box && box->type == StructureType::Pillbox && box->built);
    CHECK(sim.world().stock(0) == Stock{});
    const EntityId id = box->id;
    w.unit_for_setup(sapper)->hp = 0;  // gone, so only the pillbox fires
    sim.step();

    const EntityId gunner = w.spawn_unit(0, UnitTypeId::MachineGunner, at(9, 12));
    issue(sim, garrison(0, {gunner}, id));
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(sim.world().find_unit(gunner)->inside == id);

    // Behind it (west): no way to fire at them. In front (east): fire.
    const EntityId behind = w.spawn_unit(1, UnitTypeId::Truck, at(6, 10));
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(hp_of(sim, behind) == unit_type(UnitTypeId::Truck).max_hp);
    const EntityId ahead = w.spawn_unit(1, UnitTypeId::Rifleman, at(15, 10));
    for (int i = 0; i < 200; ++i) sim.step();
    CHECK(hp_of(sim, ahead) < unit_type(UnitTypeId::Rifleman).max_hp);
    CHECK(hp_of(sim, gunner) == unit_type(UnitTypeId::MachineGunner).max_hp);  // bullets don't get in

    // An RPG round through the slit.
    const EntityId rpg = w.spawn_unit(1, UnitTypeId::Grenadier, at(14, 11));
    issue(sim, fire_at(1, {rpg}, 10, 10));
    for (int i = 0; i < 300 && hp_of(sim, gunner) == unit_type(UnitTypeId::MachineGunner).max_hp; ++i) sim.step();
    CHECK(hp_of(sim, gunner) < unit_type(UnitTypeId::MachineGunner).max_hp);
}

// A demolition charge: planted, the sapper runs, it goes off. Two bring
// down a bridge.
void test_demolition_charges() {
    Simulation sim(1, village_map());
    const EntityId bridge = sim.world().structure_at({20, 10})->id;
    std::vector<EntityId> sappers;
    for (int i = 0; i < 2; ++i) sappers.push_back(sim.world_for_setup().spawn_unit(0, UnitTypeId::Sapper, at(17, 9 + 2 * i)));
    issue(sim, use_ability(0, {sappers[0]}, AbilityId::Demolish, 20, 10));
    for (Tick i = 0; i < kPlantWork + 60; ++i) sim.step();
    CHECK(sim.world().charges().size() == 1);
    for (Tick i = 0; i < kFuseTicks + 5; ++i) sim.step();
    CHECK(sim.world().charges().empty());
    const Structure* b = sim.world().find_structure(bridge);
    CHECK(b && b->hp < structure_type(StructureType::Bridge).max_hp);
    CHECK(hp_of(sim, sappers[0]) == unit_type(UnitTypeId::Sapper).max_hp);  // got away in time

    issue(sim, use_ability(0, {sappers[1]}, AbilityId::Demolish, 20, 10));
    for (Tick i = 0; i < kPlantWork + kFuseTicks + 100; ++i) sim.step();
    CHECK(sim.world().find_structure(bridge) == nullptr);
    CHECK(sim.world().map().terrain(20, 10) == Terrain::Water);
}

// --- Electronic warfare ------------------------------------------------------

// How many ticks after its order a tank at (10, 10) starts moving; `prepare`
// sets the scene (radio silence, relays around it).
template <typename Prepare>
Tick order_delay(Prepare prepare) {
    Simulation sim(1, TileMap(60, 30));
    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(10, 10));
    prepare(sim, tank);
    sim.step();
    const Tick ordered = sim.world().tick() + 2;
    issue(sim, make_move(0, {tank}, 40, 10));
    const FixedVec2 start = sim.world().find_unit(tank)->pos;
    for (Tick i = 0; i < kCourierTicks + 40; ++i) {
        sim.step();
        if (sim.world().find_unit(tank)->pos != start) return sim.world().tick() - ordered;
    }
    return kNeverFired;
}

// On the air, orders arrive at once; keeping silence, by courier, unless a
// relay of ours on the air is close: the headquarters, a command vehicle, a
// signaller.
void test_couriers_reach_silent_units() {
    auto silent = [](Simulation& sim, EntityId tank) { sim.world_for_setup().unit_for_setup(tank)->silent = true; };
    CHECK(order_delay([](Simulation&, EntityId) {}) <= 2);
    const Tick by_courier = order_delay(silent);
    CHECK(by_courier >= kCourierTicks && by_courier <= kCourierTicks + 2);

    auto relayed_by = [&](PlayerId owner, UnitTypeId relay, int32_t x, bool quiet) {
        return order_delay([&](Simulation& sim, EntityId tank) {
            silent(sim, tank);
            const EntityId r = sim.world_for_setup().spawn_unit(owner, relay, at(x, 14));
            sim.world_for_setup().unit_for_setup(r)->silent = quiet;
        });
    };
    CHECK(relayed_by(0, UnitTypeId::FieldHq, 19, false) <= 2);              // 9.8 tiles
    CHECK(relayed_by(0, UnitTypeId::FieldHq, 25, false) >= kCourierTicks);  // 15.5: too far
    CHECK(relayed_by(0, UnitTypeId::FieldHq, 19, true) >= kCourierTicks);   // silent itself
    CHECK(relayed_by(1, UnitTypeId::FieldHq, 19, false) >= kCourierTicks);  // the enemy's
    CHECK(relayed_by(0, UnitTypeId::Signaler, 13, false) <= 2);             // 5 tiles
    CHECK(relayed_by(0, UnitTypeId::Signaler, 16, false) >= kCourierTicks); // 7.2
    auto headquarters_at = [&](int32_t x) {
        return order_delay([&](Simulation& sim, EntityId tank) {
            silent(sim, tank);
            sim.world_for_setup().place_structure(StructureType::Headquarters, 0, {x, 9}, 3, 3);
        });
    };
    CHECK(headquarters_at(18) <= 2);              // its center 9.5 tiles off
    CHECK(headquarters_at(26) >= kCourierTicks);  // 17.5

    // A group: whoever is on the air goes at once, the silent ones later.
    Simulation sim(1, TileMap(60, 30));
    const EntityId quiet = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(10, 10));
    const EntityId loud = sim.world_for_setup().spawn_unit(0, UnitTypeId::Ifv, at(10, 16));
    silent(sim, quiet);
    const FixedVec2 quiet_start = sim.world().find_unit(quiet)->pos;
    const FixedVec2 loud_start = sim.world().find_unit(loud)->pos;
    issue(sim, make_move(0, {quiet, loud}, 40, 13));
    for (int i = 0; i < 5; ++i) sim.step();
    CHECK(sim.world().find_unit(loud)->pos != loud_start);
    CHECK(sim.world().find_unit(quiet)->pos == quiet_start);
    CHECK(sim.world().couriers().size() == 1 && sim.world().couriers().front().cmd.units == std::vector<EntityId>{quiet});
    for (Tick i = 0; i < kCourierTicks; ++i) sim.step();
    CHECK(sim.world().find_unit(quiet)->pos != quiet_start);
    CHECK(sim.world().couriers().empty());

    // Back on the air takes a courier too; going quiet is at once.
    issue(sim, use_ability(0, {quiet}, AbilityId::RadioSilence, 0, 0));
    for (int i = 0; i < 5; ++i) sim.step();
    CHECK(sim.world().find_unit(quiet)->silent);
    for (Tick i = 0; i < kCourierTicks; ++i) sim.step();
    CHECK(!sim.world().find_unit(quiet)->silent);
    issue(sim, use_ability(0, {quiet, loud}, AbilityId::RadioSilence, 0, 0));
    for (int i = 0; i < 3; ++i) sim.step();
    CHECK(sim.world().find_unit(quiet)->silent && sim.world().find_unit(loud)->silent);
}

int bearings_on(const World& w, EntityId target) {
    return static_cast<int>(std::count_if(w.bearings().begin(), w.bearings().end(),
                                          [&](const Bearing& b) { return b.target == target; }));
}

// A DF station, set up, takes a bearing on every enemy radio on the air in
// its reach. Two bearings crossing at a wide enough angle fix the radio: the
// enemy is seen there. From nearly the same spot they only give a direction.
void test_direction_finding() {
    Simulation sim(1, TileMap(80, 60));
    World& w = sim.world_for_setup();
    const EntityId tank = w.spawn_unit(1, UnitTypeId::Tank, at(45, 30));
    const EntityId rifle = w.spawn_unit(1, UnitTypeId::Rifleman, at(45, 33));  // no radio
    const EntityId far = w.spawn_unit(1, UnitTypeId::Ifv, at(78, 30));        // out of reach
    const EntityId a = w.spawn_unit(0, UnitTypeId::DfStation, at(12, 30));    // 33 tiles off
    for (int i = 0; i < 10; ++i) sim.step();
    CHECK(sim.world().bearings().empty());  // packed up: deaf

    issue(sim, use_ability(0, {a}, AbilityId::Deploy, 0, 0));
    for (Tick i = 0; i < unit_type(UnitTypeId::DfStation).deploy_time + 10; ++i) sim.step();
    CHECK(sim.world().find_unit(a)->deployed);
    CHECK(bearings_on(sim.world(), tank) == 1 && sim.world().bearings().size() == 1);
    CHECK(sim.world().bearings().front().station == a && sim.world().bearings().front().owner == 0);
    CHECK(!seen(sim, 0, tank));

    const EntityId b = w.spawn_unit(0, UnitTypeId::DfStation, at(12, 33));  // 5 degrees off a's bearing
    w.unit_for_setup(b)->deployed = true;
    for (int i = 0; i < 6; ++i) sim.step();
    CHECK(bearings_on(sim.world(), tank) == 2);
    CHECK(!seen(sim, 0, tank));

    const EntityId c = w.spawn_unit(0, UnitTypeId::DfStation, at(45, 5));  // square across
    w.unit_for_setup(c)->deployed = true;
    for (int i = 0; i < 6; ++i) sim.step();
    CHECK(bearings_on(sim.world(), tank) == 3);
    CHECK(seen(sim, 0, tank));
    CHECK(!seen(sim, 0, rifle) && bearings_on(sim.world(), rifle) == 0);
    CHECK(!seen(sim, 0, far) && bearings_on(sim.world(), far) == 0);

    // Radio silence: nothing to take a bearing on.
    issue(sim, use_ability(1, {tank}, AbilityId::RadioSilence, 0, 0));
    for (int i = 0; i < 8; ++i) sim.step();
    CHECK(sim.world().bearings().empty());
    CHECK(!seen(sim, 0, tank));
}

// The signals barracks hires the signallers, command vehicles and DF
// stations. Every radio can go quiet; the DF station has none to switch off.
void test_signals_barracks() {
    CHECK(std::find(std::begin(kBuildable), std::end(kBuildable), StructureType::SignalsBarracks) != std::end(kBuildable));
    for (UnitTypeId t : {UnitTypeId::Signaler, UnitTypeId::FieldHq, UnitTypeId::DfStation}) {
        CHECK(can_train(StructureType::SignalsBarracks, t));
    }
    for (size_t i = 0; i < kUnitTypeCount; ++i) {
        const UnitTypeDef& def = unit_type(static_cast<UnitTypeId>(i));
        CHECK(def.emitter == (ability_slot(def, AbilityId::RadioSilence) >= 0));
        CHECK(def.relay_range.raw == 0 || def.emitter);
    }
    CHECK(unit_type(UnitTypeId::Tank).emitter && !unit_type(UnitTypeId::DfStation).emitter);
}

// --- Aviation and air defence ---------------------------------------------------

// A plain 80 x 40 with our airfield at (4..9, 18..20), an attack aircraft
// on it and plenty of ammunition and fuel for rearming.
struct AirSetup {
    Simulation sim;
    EntityId airfield = 0;
    EntityId plane = 0;
};

AirSetup air_setup(uint64_t seed = 1, TileMap map = TileMap(80, 40)) {
    AirSetup a{Simulation(seed, std::move(map))};
    World& w = a.sim.world_for_setup();
    a.airfield = w.place_structure(StructureType::Airfield, 0, {4, 18}, 6, 3);
    a.plane = w.spawn_unit(0, UnitTypeId::Su25, tile_center({4, 18}));
    w.set_stock(0, {0, 0, 0, 100, 100});
    return a;
}

bool on_airfield(const Simulation& sim, EntityId plane, EntityId airfield) {
    const Unit* u = sim.world().find_unit(plane);
    const Structure* s = u ? sim.world().structure_at(tile_of(u->pos)) : nullptr;
    return u && !u->airborne && s && s->id == airfield;
}

// Steps until the aircraft, sent on a mission, is back on its airfield.
void fly_sortie(AirSetup& a) {
    for (int i = 0; i < 5; ++i) a.sim.step();
    for (int i = 0; i < 800 && !on_airfield(a.sim, a.plane, a.airfield); ++i) a.sim.step();
    CHECK(on_airfield(a.sim, a.plane, a.airfield) || a.sim.world().find_unit(a.plane) == nullptr);
}

// A bomb from the air (the Su-34's FAB-500) that hits a building brings
// down the section of it where it fell (up to three: how it looks); a
// shell doesn't, not even the heaviest. What's brought down leaves rubble
// that looks like what stood there.
void test_bombs_and_rubble() {
    AirSetup a = air_setup();
    Simulation& sim = a.sim;
    World& w = sim.world_for_setup();
    // Long along y, across the bombers' run (along x, down its middle).
    const EntityId block = w.place_structure(StructureType::Apartment, kNoOwner, {48, 17}, 2, 4);
    const FixedVec2 at = sim.world().find_structure(block)->center;
    // A mortar's bursts on it: no section down.
    const EntityId mortar = w.spawn_unit(0, UnitTypeId::Mortar, {at.x - Fixed::from_int(12), at.y});
    issue(sim, fire_at(0, {mortar}, at.x.to_int(), at.y.to_int()));
    for (int i = 0; i < 400; ++i) sim.step();
    const Structure* s = sim.world().find_structure(block);
    CHECK(s && s->hp < structure_type(StructureType::Apartment).max_hp && s->bombed == 0);
    issue(sim, {.type = CommandType::Stop, .player = 0, .units = {mortar}});
    // A 2S7 Pion's 203 mm shells (heavier than a bomb, their burst as wide): no section down either.
    const int32_t shelled = s ? s->hp : 0;
    const EntityId pion = w.spawn_unit(0, UnitTypeId::Pion, {at.x - Fixed::from_int(20), at.y});
    issue(sim, fire_at(0, {pion}, at.x.to_int(), at.y.to_int()));
    for (int i = 0; i < 900; ++i) sim.step();
    s = sim.world().find_structure(block);
    CHECK(s && s->hp < shelled && s->bombed == 0);
    issue(sim, {.type = CommandType::Stop, .player = 0, .units = {pion}});
    // An Su-34's bombs on it.
    const EntityId bomber = w.spawn_unit(0, UnitTypeId::Su34, tile_center({6, 19}));
    issue(sim, fire_at(0, {bomber}, at.x.to_int(), at.y.to_int()));
    bool bombed = false;
    for (int i = 0; i < 800 && sim.world().find_structure(block); ++i) {
        sim.step();
        if (const Structure* b = sim.world().find_structure(block)) bombed = bombed || b->bombed > 0;
    }
    CHECK(bombed);
    // The section down is the one the run went over: the middle of its length, not an end.
    if (const Structure* b = sim.world().find_structure(block)) CHECK(b->bomb_at[0] > 64 && b->bomb_at[0] < 192);
    // Brought down (by hand, whatever's left of it): rubble of a block of flats.
    if (Structure* b = w.structure_for_setup(block)) b->hp = 0;
    sim.step();
    CHECK(sim.world().map().terrain(49, 18) == Terrain::Ruins);
    CHECK(sim.world().map().ruin(49, 18) == RuinKind::Apartment);
}

// Aircraft fly missions only: one mission a sortie. It takes off, makes its
// rocket run along the target, flies home, lands and rearms from the stock.
// In the air, new orders don't reach it.
void test_aircraft_fly_missions() {
    AirSetup a = air_setup();
    Simulation& sim = a.sim;
    const FixedVec2 parked = sim.world().find_unit(a.plane)->pos;
    issue(sim, make_move(0, {a.plane}, 30, 30));  // not a mission
    for (int i = 0; i < 20; ++i) sim.step();
    CHECK(sim.world().find_unit(a.plane)->pos == parked && !sim.world().find_unit(a.plane)->airborne);
    CHECK(sim.world().find_unit(a.plane)->order == Order::Idle);

    issue(sim, fire_at(0, {a.plane}, 50, 20));
    for (int i = 0; i < 12; ++i) sim.step();
    CHECK(sim.world().find_unit(a.plane)->airborne);
    issue(sim, fire_at(0, {a.plane}, 50, 35));  // a new mission in the air: ignored
    Command stop = make_order(CommandType::Stop, 0, {a.plane}, 0, 0);
    issue(sim, stop);
    const std::vector<FixedVec2> rockets = shell_landings(sim, 400);
    CHECK(rockets.size() == static_cast<size_t>(unit_type(UnitTypeId::Su25).rounds_capacity));
    for (const FixedVec2& r : rockets) CHECK((r - at(50, 20)).length() <= Fixed::from_int(6));

    for (int i = 0; i < 600 && !on_airfield(sim, a.plane, a.airfield); ++i) sim.step();
    CHECK(on_airfield(sim, a.plane, a.airfield));
    CHECK(sim.world().find_unit(a.plane)->order == Order::Idle);  // the mission is over
    const Fixed flown = sim.world().find_unit(a.plane)->fuel;
    CHECK(flown < unit_type(UnitTypeId::Su25).fuel_capacity);
    for (Tick i = 0; i < 40 * kAirRearmInterval; ++i) sim.step();
    const Unit* u = sim.world().find_unit(a.plane);
    CHECK(u->rounds == unit_type(UnitTypeId::Su25).rounds_capacity && u->fuel == unit_type(UnitTypeId::Su25).fuel_capacity);
    CHECK(stock_of(sim, Resource::Ammo) == 100 - unit_type(UnitTypeId::Su25).rounds_capacity);
    CHECK(stock_of(sim, Resource::Fuel) < 100);
    CHECK(u->hp == unit_type(UnitTypeId::Su25).max_hp);
}

// The run is made lined up on the target: a target off to the side of the
// runway gets the rockets once the aircraft has come round to it.
void test_run_lines_up() {
    AirSetup a = air_setup();
    issue(a.sim, fire_at(0, {a.plane}, 8, 25));
    const std::vector<FixedVec2> rockets = shell_landings(a.sim, 200);
    CHECK(!rockets.empty());
    bool close = false;
    for (const FixedVec2& r : rockets) close = close || (r - at(8, 25)).length() <= Fixed::from_ratio(5, 2);
    CHECK(close);
}

// A mission against an enemy we see follows him while we see him.
void test_strike_follows_the_target() {
    AirSetup a = air_setup();
    Simulation& sim = a.sim;
    World& w = sim.world_for_setup();
    const EntityId tank = w.spawn_unit(1, UnitTypeId::Tank, at(55, 8));
    for (int y = 6; y <= 34; y += 7) w.spawn_unit(0, UnitTypeId::Scout, at(49, y));  // watching the road south
    sim.step();
    issue(sim, attack_order(0, {a.plane}, tank));
    issue(sim, make_move(1, {tank}, 55, 34));
    const std::vector<FixedVec2> rockets = shell_landings(sim, 400);
    CHECK(rockets.size() == static_cast<size_t>(unit_type(UnitTypeId::Su25).rounds_capacity));
    Fixed y{};
    for (const FixedVec2& r : rockets) y += r.y;
    CHECK(!rockets.empty() && y / static_cast<int32_t>(rockets.size()) > Fixed::from_int(14));  // not where he was
}

// Short of fuel for the way there and back, the aircraft turns home early,
// its rockets unfired.
void test_bingo_fuel() {
    AirSetup a = air_setup();
    Simulation& sim = a.sim;
    sim.world_for_setup().unit_for_setup(a.plane)->fuel = Fixed::from_int(80);
    sim.world_for_setup().set_stock(0, {});
    issue(sim, fire_at(0, {a.plane}, 70, 20));
    const std::vector<FixedVec2> rockets = shell_landings(sim, 300);
    CHECK(rockets.empty());
    for (int i = 0; i < 600 && !on_airfield(sim, a.plane, a.airfield); ++i) sim.step();
    CHECK(on_airfield(sim, a.plane, a.airfield));
    CHECK(sim.world().find_unit(a.plane)->fuel > Fixed{});
    // Empty racks or next to no fuel: it doesn't take off at all.
    for (const bool racks : {false, true}) {
        Unit* p = sim.world_for_setup().unit_for_setup(a.plane);
        p->rounds = racks ? unit_type(UnitTypeId::Su25).rounds_capacity : 0;
        p->fuel = racks ? Fixed{} : unit_type(UnitTypeId::Su25).fuel_capacity;
        issue(sim, fire_at(0, {a.plane}, 40, 20));
        for (int i = 0; i < 20; ++i) sim.step();
        CHECK(sim.world().find_unit(a.plane) && !sim.world().find_unit(a.plane)->airborne &&
              sim.world().find_unit(a.plane)->order == Order::Idle);
    }
    // Off the runway (set down there for the test) it isn't rearmed.
    Unit* p = sim.world_for_setup().unit_for_setup(a.plane);
    p->pos = at(30, 30);
    p->rounds = 0;
    sim.world_for_setup().set_stock(0, {0, 0, 0, 100, 100});
    for (Tick i = 0; i < 10 * kAirRearmInterval; ++i) sim.step();
    CHECK(sim.world().find_unit(a.plane)->rounds == 0 && stock_of(sim, Resource::Ammo) == 100);
}

// Nothing but air defence reaches an aircraft in the air: a firefight under
// its path doesn't touch it, and no one shoots at it but the AA.
void test_only_air_defence_reaches_aircraft() {
    AirSetup a = air_setup();
    Simulation& sim = a.sim;
    World& w = sim.world_for_setup();
    const EntityId tank = w.spawn_unit(1, UnitTypeId::Tank, at(30, 22));
    const EntityId mg = w.spawn_unit(1, UnitTypeId::MachineGunner, at(30, 19));
    w.spawn_unit(0, UnitTypeId::Rifleman, at(33, 20));
    w.spawn_unit(0, UnitTypeId::Rifleman, at(27, 20));
    issue(sim, fire_at(0, {a.plane}, 60, 20));
    bool told = false;
    for (int i = 0; i < 300; ++i) {
        sim.step();
        if (told || !seen(sim, 1, a.plane)) continue;
        // Seen overhead, but a tank or a machine gun can't be told to shoot at it.
        issue(sim, attack_order(1, {tank, mg}, a.plane));
        for (int k = 0; k < 3; ++k) sim.step();
        for (const EntityId id : {tank, mg}) {
            const Unit* gun = sim.world().find_unit(id);
            CHECK(!gun || gun->order != Order::Attack);
        }
        told = true;
    }
    CHECK(told);
    const Unit* u = sim.world().find_unit(a.plane);
    CHECK(u && u->hp == unit_type(UnitTypeId::Su25).max_hp);

    // Alone under its path, with nothing else to shoot at, the ground troops
    // hold fire; a machine gun firing along the path doesn't touch it.
    AirSetup c = air_setup(3);
    World& cw = c.sim.world_for_setup();
    // Under the path, out of sight of the airfield and of the tank being struck.
    const EntityId lone_tank = cw.spawn_unit(1, UnitTypeId::Tank, at(48, 22));
    const EntityId gunner = cw.spawn_unit(1, UnitTypeId::MachineGunner, at_half(72, 37));
    const EntityId target = cw.spawn_unit(0, UnitTypeId::Tank, at_half(62, 37));
    cw.unit_for_setup(target)->rounds = 0;  // takes it, can't answer
    issue(c.sim, fire_at(0, {c.plane}, 60, 18));
    bool overhead = false;
    for (int i = 0; i < 400 && !overhead; ++i) {
        c.sim.step();
        const Unit* p = c.sim.world().find_unit(c.plane);
        overhead = p && (p->pos - c.sim.world().find_unit(lone_tank)->pos).length() < Fixed::from_int(5);
    }
    CHECK(overhead);
    CHECK(c.sim.world().find_unit(lone_tank)->engaged != c.plane);  // not even taken aim at
    fly_sortie(c);
    CHECK(c.sim.world().find_unit(lone_tank)->last_shot_tick == kNeverFired);
    CHECK(c.sim.world().find_unit(gunner)->last_shot_tick != kNeverFired);
    CHECK(hp_of(c.sim, c.plane) == unit_type(UnitTypeId::Su25).max_hp);

    // Two strikes crossing: neither is hurt by the other's rockets bursting below.
    AirSetup b = air_setup(2);
    World& bw = b.sim.world_for_setup();
    bw.place_structure(StructureType::Airfield, 1, {70, 18}, 6, 3);
    const EntityId other = bw.spawn_unit(1, UnitTypeId::Su25, tile_center({75, 20}));
    issue(b.sim, fire_at(0, {b.plane}, 40, 20));
    issue(b.sim, fire_at(1, {other}, 40, 20));
    const std::vector<FixedVec2> rockets = shell_landings(b.sim, 300);
    CHECK(rockets.size() == 2 * static_cast<size_t>(unit_type(UnitTypeId::Su25).rounds_capacity));
    CHECK(hp_of(b.sim, b.plane) == unit_type(UnitTypeId::Su25).max_hp);
    CHECK(hp_of(b.sim, other) == unit_type(UnitTypeId::Su25).max_hp);
}

// A MANPADS crew under the aircraft's path: a missile hits about every
// other time, and it never fires at anything on the ground.
void test_manpads() {
    int damaged = 0;
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        AirSetup a = air_setup(seed);
        const EntityId crew = a.sim.world_for_setup().spawn_unit(1, UnitTypeId::Manpads, at(40, 21));
        issue(a.sim, fire_at(0, {a.plane}, 60, 20));
        fly_sortie(a);
        const int32_t lost = unit_type(UnitTypeId::Su25).max_hp - hp_of(a.sim, a.plane);
        const int32_t per_hit = unit_type(UnitTypeId::Manpads).weapon.damage - unit_type(UnitTypeId::Su25).armor[1];
        CHECK(lost % per_hit == 0);  // whole missiles
        damaged += lost > 0 ? 1 : 0;
        CHECK(a.sim.world().find_unit(crew)->last_shot_tick != kNeverFired);
    }
    CHECK(damaged >= 5 && damaged <= 19);

    Simulation sim(1, TileMap(40, 20));
    const EntityId crew = sim.world_for_setup().spawn_unit(1, UnitTypeId::Manpads, at(10, 10));
    const EntityId rifle = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(14, 10));
    issue(sim, attack_order(1, {crew}, rifle));
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(sim.world().find_unit(crew) == nullptr || sim.world().find_unit(crew)->last_shot_tick == kNeverFired);
    CHECK(hp_of(sim, rifle) == unit_type(UnitTypeId::Rifleman).max_hp);

    // Nor from a window.
    Simulation village(1, village_map());
    const EntityId inside = village.world_for_setup().spawn_unit(1, UnitTypeId::Manpads, at(9, 12));
    issue(village, garrison(1, {inside}, house_id(village)));
    for (int i = 0; i < 100; ++i) village.step();
    CHECK(village.world().find_unit(inside)->inside != 0);
    const EntityId passer = village.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(14, 11));
    for (int i = 0; i < 100; ++i) village.step();
    CHECK(village.world().find_unit(inside)->last_shot_tick == kNeverFired);
    CHECK(hp_of(village, passer) == unit_type(UnitTypeId::Rifleman).max_hp);
}

// An aircraft in the air is seen by whoever has it within his sight, over
// the trees too; an air defence radar, set up and on the air, sees it far out.
void test_radar_sees_aircraft() {
    auto seen_at_nine = [](bool radar, bool deployed, bool quiet) {
        AirSetup a = air_setup();
        World& w = a.sim.world_for_setup();
        w.spawn_unit(1, UnitTypeId::Manpads, at(40, 20));
        if (radar) {
            const EntityId r = w.spawn_unit(1, UnitTypeId::AirRadar, at(70, 35));
            w.unit_for_setup(r)->deployed = deployed;
            w.unit_for_setup(r)->silent = quiet;
        }
        Unit* p = w.unit_for_setup(a.plane);
        p->airborne = true;
        p->pos = at(31, 20);
        p->order = Order::AttackGround;
        p->order_point = at(60, 20);
        a.sim.step();
        return seen(a.sim, 1, a.plane);
    };
    CHECK(!seen_at_nine(false, false, false));
    CHECK(seen_at_nine(true, true, false));
    CHECK(!seen_at_nine(true, false, false));  // packed up
    CHECK(!seen_at_nine(true, true, true));    // switched off

    // Out of the radar's reach: not seen.
    AirSetup far = air_setup();
    far.sim.world_for_setup().spawn_unit(1, UnitTypeId::Manpads, at(40, 20));
    Unit* r = far.sim.world_for_setup().unit_for_setup(far.sim.world_for_setup().spawn_unit(1, UnitTypeId::AirRadar, at(79, 39)));
    r->deployed = true;
    Unit* p = far.sim.world_for_setup().unit_for_setup(far.plane);
    p->airborne = true;
    p->pos = at(31, 20);
    p->order = Order::AttackGround;
    p->order_point = at(60, 20);
    far.sim.step();
    CHECK(!seen(far.sim, 1, far.plane));

    // Over a forest an aircraft is still in the open sky.
    TileMap woods(80, 40);
    for (int y = 17; y <= 23; ++y) {
        for (int x = 27; x <= 33; ++x) woods.set_terrain(x, y, Terrain::Forest);
    }
    AirSetup forest = air_setup(1, woods);
    World& fw = forest.sim.world_for_setup();
    fw.spawn_unit(1, UnitTypeId::Manpads, at(36, 20));
    Unit* q = fw.unit_for_setup(forest.plane);
    q->airborne = true;
    q->pos = at(30, 20);
    q->order = Order::AttackGround;
    q->order_point = at(60, 20);
    forest.sim.step();
    CHECK(seen(forest.sim, 1, forest.plane));
    // Seen by a building's lookouts too: over the trees, out in the open sky.
    TileMap grove(80, 40);
    for (int y = 16; y <= 18; ++y) {
        for (int x = 40; x <= 43; ++x) grove.set_terrain(x, y, Terrain::Forest);
    }
    AirSetup hq = air_setup(1, grove);
    hq.sim.world_for_setup().place_structure(StructureType::Headquarters, 1, {40, 10}, 3, 3);
    Unit* h = hq.sim.world_for_setup().unit_for_setup(hq.plane);
    h->airborne = true;
    h->pos = at_half(83, 33);
    h->order = Order::AttackGround;
    h->order_point = at(60, 20);
    hq.sim.step();
    CHECK(seen(hq.sim, 1, hq.plane));
}

// The Shilka fires at aircraft and at the ground; with its radar off it
// aims by eye and hits aircraft half as often.
void test_shilka() {
    int32_t radar_on = 0;
    int32_t radar_off = 0;
    for (uint64_t seed = 1; seed <= 10; ++seed) {
        for (const bool quiet : {false, true}) {
            AirSetup a = air_setup(seed);
            const EntityId zsu = a.sim.world_for_setup().spawn_unit(1, UnitTypeId::Shilka, at(40, 22));
            a.sim.world_for_setup().unit_for_setup(zsu)->silent = quiet;
            issue(a.sim, fire_at(0, {a.plane}, 60, 20));
            fly_sortie(a);
            (quiet ? radar_off : radar_on) += unit_type(UnitTypeId::Su25).max_hp - hp_of(a.sim, a.plane);
        }
    }
    CHECK(radar_off > 0 && radar_on > 0);
    CHECK(radar_off * 100 <= radar_on * 75);

    Simulation sim(1, TileMap(40, 20));
    sim.world_for_setup().spawn_unit(1, UnitTypeId::Shilka, at(10, 10));
    const EntityId rifle = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(15, 10));
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(hp_of(sim, rifle) < unit_type(UnitTypeId::Rifleman).max_hp);
}

// On the ground an aircraft is a target like any other, and the runway
// takes the ones parked on it with it; an aircraft in the air with no
// airfield left to land on is lost to the fight.
void test_airfield_losses() {
    AirSetup a = air_setup();
    Simulation& sim = a.sim;
    World& w = sim.world_for_setup();
    const EntityId tank = w.spawn_unit(1, UnitTypeId::Tank, at(11, 19));
    sim.step();
    issue(sim, attack_order(1, {tank}, a.plane));
    for (int i = 0; i < 200; ++i) sim.step();
    CHECK(hp_of(sim, a.plane) < unit_type(UnitTypeId::Su25).max_hp);

    AirSetup b = air_setup();
    const EntityId parked = b.sim.world_for_setup().spawn_unit(0, UnitTypeId::Su25, tile_center({6, 19}));
    issue(b.sim, fire_at(0, {b.plane}, 60, 20));
    for (int i = 0; i < 20; ++i) b.sim.step();
    CHECK(b.sim.world().find_unit(b.plane)->airborne);
    b.sim.world_for_setup().structure_for_setup(b.airfield)->hp = 0;
    b.sim.step();
    CHECK(b.sim.world().find_structure(b.airfield) == nullptr);
    CHECK(b.sim.world().find_unit(parked) == nullptr);
    CHECK(b.sim.world().find_unit(b.plane) != nullptr);
    CHECK(b.sim.world().map().terrain(6, 19) == Terrain::Grass);
    for (int i = 0; i < 400; ++i) b.sim.step();
    CHECK(b.sim.world().find_unit(b.plane) == nullptr);
}

// The airfield (a runway anyone can cross) and the air defence barracks
// are built by rear troops and hire the aircraft and the air defence.
void test_aviation_buildings() {
    for (StructureType t : {StructureType::Airfield, StructureType::AirDefenseBarracks}) {
        CHECK(std::find(std::begin(kBuildable), std::end(kBuildable), t) != std::end(kBuildable));
    }
    CHECK(can_train(StructureType::Airfield, UnitTypeId::Su25));
    for (UnitTypeId t : {UnitTypeId::Manpads, UnitTypeId::Shilka, UnitTypeId::AirRadar}) {
        CHECK(can_train(StructureType::AirDefenseBarracks, t));
    }
    for (MoveClass c : {MoveClass::Foot, MoveClass::Vehicle, MoveClass::Wheeled}) {
        CHECK(terrain_def(Terrain::Airstrip).speed_percent[static_cast<size_t>(c)] == 100);
    }
    // A new aircraft is rolled out onto a free spot of the runway.
    AirSetup a = air_setup();
    a.sim.world_for_setup().set_stock(0, {10, 0, 1000, 1000, 1000});
    a.sim.world_for_setup().place_structure(StructureType::Quarters, 0, {4, 30}, 2, 2);  // bunks for the pilots
    Command train{.type = CommandType::Train, .player = 0, .target_unit = a.airfield,
                  .unit_type = static_cast<uint8_t>(UnitTypeId::Su25)};
    issue(a.sim, train);
    issue(a.sim, train);
    for (Tick i = 0; i < 2 * unit_type(UnitTypeId::Su25).train_time + 5; ++i) a.sim.step();
    std::vector<FixedVec2> spots;
    for (const Unit& u : a.sim.world().units()) {
        if (u.type == UnitTypeId::Su25 && on_airfield(a.sim, u.id, a.airfield)) spots.push_back(u.pos);
    }
    CHECK(spots.size() == 3);
    for (size_t i = 0; i < spots.size(); ++i) {
        for (size_t j = i + 1; j < spots.size(); ++j) CHECK(spots[i] != spots[j]);
    }
    CHECK(a.sim.world().find_unit(a.plane)->pos == tile_center({4, 18}));
}

// --- Terrain and pathfinding -------------------------------------------------

// A forest wall down the middle of the map with one trail through it.
TileMap forest_wall_map() {
    TileMap map(40, 40);
    for (int32_t y = 0; y < 40; ++y) {
        for (int32_t x = 19; x <= 21; ++x) map.set_terrain(x, y, y == 30 ? Terrain::Trail : Terrain::Forest);
    }
    return map;
}

// Whether `id` reached `goal` within `ticks`; also reports if it ever stood on forest.
struct Trip {
    bool arrived = false;
    int ticks = 0;
    bool touched_forest = false;
};

Trip drive(Simulation& sim, EntityId id, int32_t x, int32_t y, int max_ticks) {
    issue(sim, make_move(sim.world().find_unit(id)->owner, {id}, x, y));
    Trip trip;
    for (trip.ticks = 0; trip.ticks < max_ticks; ++trip.ticks) {
        sim.step();
        const Unit* u = sim.world().find_unit(id);
        if (!u) return trip;  // lost on the way (sunk in a bog)
        trip.touched_forest = trip.touched_forest || sim.world().map().terrain_at(u->pos) == Terrain::Forest;
        if (trip.ticks > 3 && u->order == Order::Idle) break;
    }
    const Unit* u = sim.world().find_unit(id);
    trip.arrived = u && (u->pos - at(x, y)).length() < Fixed::from_ratio(1, 2);
    return trip;
}

// A 40 x 10 strip of one terrain; how long a unit takes to cross 30 tiles of it.
int crossing(Terrain ground, UnitTypeId who) {
    TileMap map(40, 10);
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 40; ++x) map.set_terrain(x, y, ground);
    }
    Simulation sim(1, map);
    const EntityId id = sim.world_for_setup().spawn_unit(0, who, at(4, 5));
    const Trip trip = drive(sim, id, 34, 5, 20000);
    return trip.arrived ? trip.ticks : 1000000;
}

// Roads are fast (concrete faster than dirt), plowed land slows wheels,
// a swamp swallows tracks and stops wheels; the route finder takes the road.
bool lasts_ifv_ok() {
    TileMap map(20, 10);
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 20; ++x) map.set_terrain(x, y, Terrain::Swamp);
    }
    Simulation sim(1, map);
    const EntityId id = sim.world_for_setup().spawn_unit(0, UnitTypeId::Ifv, at(10, 5));
    for (int i = 0; i < kBogSeconds * kTicksPerSecond * 2; ++i) sim.step();
    return sim.world().find_unit(id) != nullptr && sim.world().find_unit(id)->mired == 0;
}

void test_roads_fields_and_swamps() {
    const int tank_field = crossing(Terrain::Grass, UnitTypeId::Tank);
    const int tank_dirt = crossing(Terrain::DirtRoad, UnitTypeId::Tank);
    const int tank_road = crossing(Terrain::Road, UnitTypeId::Tank);
    CHECK(tank_road < tank_dirt && tank_dirt < tank_field);
    CHECK(crossing(Terrain::Road, UnitTypeId::Truck) * 100 < crossing(Terrain::Grass, UnitTypeId::Truck) * 70);
    CHECK(crossing(Terrain::Plowed, UnitTypeId::Truck) > crossing(Terrain::Grass, UnitTypeId::Truck));
    CHECK(crossing(Terrain::Swamp, UnitTypeId::Tank) == 1000000);  // thirty tiles of bog: it sinks on the way (see test_bogs)
    CHECK(crossing(Terrain::Swamp, UnitTypeId::Truck) == 1000000);  // never gets in
    CHECK(crossing(Terrain::Swamp, UnitTypeId::Rifleman) > 2 * crossing(Terrain::Grass, UnitTypeId::Rifleman));

    // Across a plowed field, a road looping round is the quicker way for a truck.
    TileMap map(40, 30);
    for (int y = 0; y < 30; ++y) {
        for (int x = 0; x < 40; ++x) map.set_terrain(x, y, Terrain::Plowed);
    }
    for (int x = 2; x <= 36; ++x) map.set_terrain(x, 20, Terrain::Road);
    for (int y = 5; y <= 20; ++y) {
        map.set_terrain(2, y, Terrain::Road);
        map.set_terrain(36, y, Terrain::Road);
    }
    Simulation sim(1, map);
    const EntityId truck = sim.world_for_setup().spawn_unit(0, UnitTypeId::Truck, tile_center({2, 5}));
    issue(sim, make_move(0, {truck}, 36, 5));
    bool on_road_far = false;
    for (int i = 0; i < 2000; ++i) {
        sim.step();
        on_road_far = on_road_far || sim.world().find_unit(truck)->pos.y > Fixed::from_int(15);
    }
    CHECK(on_road_far);
}

// A tank in a bog sinks, slowly: through a narrow one it gets, slower and
// slower; standing in it, it's lost twice as soon, a heavy one sooner than a
// light one; lost, its crew comes back; out on firm ground it's free again.
void test_bogs() {
    auto strip = [](int width, UnitTypeId type) {
        TileMap map(40, 10);
        for (int y = 0; y < 10; ++y) {
            for (int x = 15; x < 15 + width; ++x) map.set_terrain(x, y, Terrain::Swamp);
        }
        Simulation sim(1, map);
        const EntityId id = sim.world_for_setup().spawn_unit(0, type, at(4, 5));
        return drive(sim, id, 34, 5, 20000);
    };
    const Trip narrow = strip(3, UnitTypeId::Tank);
    CHECK(narrow.arrived);
    CHECK(!strip(20, UnitTypeId::Tank).arrived);  // too wide: lost in it

    // How long each lasts standing in a bog, and moving.
    auto lasts = [](UnitTypeId type, bool moving) {
        TileMap map(60, 10);
        for (int y = 0; y < 10; ++y) {
            for (int x = 0; x < 60; ++x) map.set_terrain(x, y, Terrain::Swamp);
        }
        Simulation sim(1, map);
        World& w = sim.world_for_setup();
        w.set_stock(0, {});
        const EntityId id = w.spawn_unit(0, type, at(3, 5));
        if (moving) issue(sim, make_move(0, {id}, 58, 5));
        int t = 0;
        while (sim.world().find_unit(id) && t < 20000) {
            sim.step();
            ++t;
        }
        CHECK(sim.world().stock(0)[static_cast<size_t>(Resource::Personnel)] == unit_type(type).cost[static_cast<size_t>(Resource::Personnel)]);
        return t;
    };
    const int standing = lasts(UnitTypeId::Tank, false);
    CHECK(std::abs(standing - kBogSeconds * kTicksPerSecond / 2) <= 2);
    const int driving = lasts(UnitTypeId::Tank, true);
    CHECK(driving > standing * 18 / 10 && driving <= kBogSeconds * kTicksPerSecond + 5);
    CHECK(lasts(UnitTypeId::M1A1, false) < standing && lasts(UnitTypeId::T64BV, false) > standing);

    // Sinking, it slows down; out of it, it's free.
    TileMap map(40, 10);
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 12; ++x) map.set_terrain(x, y, Terrain::Swamp);
    }
    Simulation sim(1, map);
    const EntityId id = sim.world_for_setup().spawn_unit(0, UnitTypeId::T64BV, at(8, 5));
    for (int i = 0; i < 400; ++i) sim.step();  // standing in it for a while
    CHECK(sim.world().find_unit(id)->mired > kBogLimit / 3);
    issue(sim, make_move(0, {id}, 30, 5));
    Fixed first{};
    for (int i = 0; i < 40; ++i) sim.step();
    first = sim.world().find_unit(id)->pos.x;
    CHECK(sim.world().find_unit(id)->mired > 0);
    for (int i = 0; i < 600; ++i) sim.step();
    CHECK(sim.world().find_unit(id)->mired == 0);  // out on firm ground
    CHECK(sim.world().find_unit(id)->pos.x > first + Fixed::from_int(3));
    // Only tanks: an IFV floats.
    CHECK(lasts_ifv_ok());

    // One that has sat in it a while crawls, next to one just got in.
    auto crawl = [](int sat) {
        TileMap map(40, 10);
        for (int y = 0; y < 10; ++y) {
            for (int x = 0; x < 40; ++x) map.set_terrain(x, y, Terrain::Swamp);
        }
        Simulation sim(1, map);
        const EntityId id = sim.world_for_setup().spawn_unit(0, UnitTypeId::T64BV, at(5, 5));
        for (int i = 0; i < sat; ++i) sim.step();
        const Fixed x0 = sim.world().find_unit(id)->pos.x;
        issue(sim, make_move(0, {id}, 35, 5));
        for (int i = 0; i < 30; ++i) sim.step();
        return sim.world().find_unit(id)->pos.x - x0;
    };
    const Fixed fresh = crawl(0);
    CHECK(fresh > Fixed{} && crawl(300) < fresh * 9 / 10);

    // How far it's sunk is part of the game's state.
    auto sum = [](int32_t mired) {
        Simulation sim(1, TileMap(20, 10));
        const EntityId id = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 5));
        sim.world_for_setup().unit_for_setup(id)->mired = mired;
        return sim.world().checksum();
    };
    CHECK(sum(0) != sum(1000));
}

// Sunflowers, reeds, an orchard and a crater hide a man, not a vehicle (wheat
// and kitchen gardens hide nobody); a crater also takes some of the hits for
// him. No mine goes into concrete.
void test_crops_swamps_and_craters() {
    auto hidden = [](Terrain ground, UnitTypeId who) {
        TileMap map(40, 20);
        map.set_terrain(20, 10, ground);
        Simulation sim(1, map);
        const EntityId man = sim.world_for_setup().spawn_unit(1, who, tile_center({20, 10}));
        sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(15, 10));  // 5 tiles off: in sight
        sim.step();
        return !seen(sim, 0, man);
    };
    CHECK(!hidden(Terrain::Grass, UnitTypeId::Rifleman));
    CHECK(hidden(Terrain::Crops, UnitTypeId::Rifleman));
    CHECK(hidden(Terrain::Swamp, UnitTypeId::Rifleman));
    CHECK(hidden(Terrain::Crater, UnitTypeId::Rifleman));
    CHECK(!hidden(Terrain::Crops, UnitTypeId::Tank));
    CHECK(hidden(Terrain::Orchard, UnitTypeId::Rifleman));
    CHECK(!hidden(Terrain::Orchard, UnitTypeId::Tank));
    CHECK(!hidden(Terrain::Wheat, UnitTypeId::Rifleman));
    CHECK(!hidden(Terrain::Garden, UnitTypeId::Rifleman));

    auto damage = [](Terrain ground, CraterKind kind = CraterKind::None) {
        int32_t total = 0;
        for (uint64_t seed = 1; seed <= 20; ++seed) {
            TileMap map(30, 20);
            map.set_terrain(15, 10, ground);
            if (kind != CraterKind::None) map.set_crater(15, 10, kind, 0);
            Simulation sim(seed, map);
            const EntityId man = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, tile_center({15, 10}));
            sim.world_for_setup().unit_for_setup(man)->hp = 100000;
            sim.world_for_setup().unit_for_setup(man)->rounds = 0;  // just takes it
            const EntityId gun = sim.world_for_setup().spawn_unit(1, UnitTypeId::MachineGunner, at(21, 10));
            Command fire = make_order(CommandType::AttackGround, 1, {gun}, 0, 0);
            fire.target = tile_center({15, 10});
            sim.schedule(0, fire);
            for (int i = 0; i < 200; ++i) sim.step();
            total += 100000 - hp_of(sim, man);
        }
        return total;
    };
    const int32_t open = damage(Terrain::Grass);
    const int32_t crater = damage(Terrain::Crater);
    CHECK(crater < open && crater * 100 >= open * (100 - kCraterCover - 12) && crater * 100 <= open * (100 - kCraterCover + 12));
    // The deeper the crater, the better the cover.
    auto covered = [&](CraterKind kind, int32_t cover) {
        const int32_t got = damage(Terrain::Crater, kind);
        return got * 100 >= open * (100 - cover - 10) && got * 100 <= open * (100 - cover + 10);
    };
    CHECK(covered(CraterKind::Small, kSmallCraterCover));
    CHECK(covered(CraterKind::Rocket, kRocketCraterCover));
    CHECK(covered(CraterKind::Shell, kCraterCover));
    CHECK(covered(CraterKind::Heavy, kHeavyCraterCover));
    static_assert(kSmallCraterCover < kRocketCraterCover && kRocketCraterCover < kCraterCover && kCraterCover < kHeavyCraterCover);

    TileMap road(30, 20);
    for (int x = 0; x < 30; ++x) road.set_terrain(x, 10, Terrain::Road);
    Simulation sim(1, road);
    const EntityId sapper = sim.world_for_setup().spawn_unit(0, UnitTypeId::Sapper, at(10, 12));
    sim.world_for_setup().set_stock(0, {0, 0, 0, 100, 0});
    issue(sim, use_ability(0, {sapper}, AbilityId::LayApMine, 10, 10));
    for (int i = 0; i < 400; ++i) sim.step();
    issue(sim, use_ability(0, {sapper}, AbilityId::LayApMine, 12, 12));
    for (int i = 0; i < 400; ++i) sim.step();
    CHECK(sim.world().mines().size() == 1 && (sim.world().mines().front().tile == TilePos{12, 12}));
}

// Heavy shells bursting on open ground leave craters behind.
void test_shelling_leaves_craters() {
    Simulation sim(1, TileMap(40, 20));
    const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Howitzer, at(4, 10));
    sim.world_for_setup().unit_for_setup(gun)->rounds = 30;
    issue(sim, fire_at(0, {gun}, 30, 10));
    for (int i = 0; i < 2400; ++i) sim.step();
    int craters = 0;
    for (int y = 0; y < 20; ++y) {
        for (int x = 20; x < 40; ++x) craters += sim.world().map().terrain(x, y) == Terrain::Crater ? 1 : 0;
    }
    CHECK(craters >= 3);
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 15; ++x) CHECK(sim.world().map().terrain(x, y) == Terrain::Grass);  // only where they burst
    }

    // Into a wheat field, the same.
    TileMap field(40, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 20; x < 40; ++x) field.set_terrain(x, y, Terrain::Wheat);
    }
    Simulation wheat(1, field);
    const EntityId third = wheat.world_for_setup().spawn_unit(0, UnitTypeId::Howitzer, at(4, 10));
    wheat.world_for_setup().unit_for_setup(third)->rounds = 30;
    issue(wheat, fire_at(0, {third}, 30, 10));
    for (int i = 0; i < 2400; ++i) wheat.step();
    int in_wheat = 0;
    for (int y = 0; y < 20; ++y) {
        for (int x = 20; x < 40; ++x) in_wheat += wheat.world().map().terrain(x, y) == Terrain::Crater ? 1 : 0;
    }
    CHECK(in_wheat >= 3);

    // Into a wood, none: craters are for open ground and roads.
    TileMap wood(40, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 22; x < 40; ++x) wood.set_terrain(x, y, Terrain::Forest);
    }
    Simulation forest(1, wood);
    const EntityId other = forest.world_for_setup().spawn_unit(0, UnitTypeId::Howitzer, at(4, 10));
    issue(forest, fire_at(0, {other}, 30, 10));
    for (int i = 0; i < 2400; ++i) forest.step();
    CHECK(forest.world().find_unit(other)->rounds < unit_type(UnitTypeId::Howitzer).rounds_capacity);
    for (int y = 0; y < 20; ++y) {
        for (int x = 22; x < 40; ++x) CHECK(forest.world().map().terrain(x, y) == Terrain::Forest);
    }
}

// Each gun leaves its own crater, the shell's way in pointing back at the gun:
// a mortar bomb a small one, a 122 mm shell a deep one, rockets long ones. A
// bigger one swallows a smaller one, never the other way round. The old
// craters on the demo map are of every kind, the same on both halves, their
// shells from the other side.
void test_crater_kinds() {
    auto fire = [](UnitTypeId type, int gun_x, CraterKind before) {
        TileMap map(40, 20);
        if (before != CraterKind::None) {
            for (int y = 0; y < 20; ++y) {
                for (int x = 16; x < 40; ++x) {
                    map.set_terrain(x, y, Terrain::Crater);
                    map.set_crater(x, y, before, 0);
                }
            }
        }
        Simulation sim(1, map);
        const EntityId gun = sim.world_for_setup().spawn_unit(0, type, at(gun_x, 10));
        sim.world_for_setup().unit_for_setup(gun)->rounds = 30;
        issue(sim, fire_at(0, {gun}, 30, 10));
        for (int i = 0; i < 2400; ++i) sim.step();
        std::array<int, 5> kinds{};
        int back = 0;
        int total = 0;
        for (int y = 0; y < 20; ++y) {
            for (int x = 0; x < 40; ++x) {
                const TileMap& m = sim.world().map();
                if (m.terrain(x, y) != Terrain::Crater) continue;
                if (before != CraterKind::None && m.crater_kind(x, y) == before) continue;
                ++kinds[static_cast<size_t>(m.crater_kind(x, y))];
                ++total;
                back += m.crater_from(x, y) == 4 ? 1 : 0;  // towards -x, where the gun is
            }
        }
        CHECK(back == total);
        return std::pair{kinds, total};
    };
    const auto [mortar, mortars] = fire(UnitTypeId::Mortar, 12, CraterKind::None);
    CHECK(mortars >= 2 && mortar[static_cast<size_t>(CraterKind::Small)] == mortars);
    const auto [shell, shells] = fire(UnitTypeId::Howitzer, 4, CraterKind::None);
    CHECK(shells >= 2 && shell[static_cast<size_t>(CraterKind::Shell)] == shells);
    const auto [rocket, rockets] = fire(UnitTypeId::Mlrs, 4, CraterKind::None);
    CHECK(rockets >= 2 && rocket[static_cast<size_t>(CraterKind::Rocket)] == rockets);
    CHECK(fire(UnitTypeId::Howitzer, 4, CraterKind::Small).second >= 2);  // deeper over the small ones
    CHECK(fire(UnitTypeId::Mortar, 12, CraterKind::Heavy).second == 0);   // the big ones stay
    const auto [pion, pions] = fire(UnitTypeId::Pion, 4, CraterKind::None);
    CHECK(pions >= 2 && pion[static_cast<size_t>(CraterKind::Heavy)] == pions);  // the 203 mm's
    const auto [m777, m777s] = fire(UnitTypeId::M777, 4, CraterKind::None);
    CHECK(m777s >= 2 && m777[static_cast<size_t>(CraterKind::Shell)] == m777s);

    const TileMap map = make_demo_map();
    std::array<int, 5> old{};
    const int size = map.width();
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (map.terrain(x, y) != Terrain::Crater) continue;
            ++old[static_cast<size_t>(map.crater_kind(x, y))];
            const int mx = size - 1 - x;
            const int my = size - 1 - y;
            CHECK(map.terrain(mx, my) == Terrain::Crater && map.crater_kind(mx, my) == map.crater_kind(x, y) &&
                  map.crater_from(mx, my) == (map.crater_from(x, y) + 4) % 8);
        }
    }
    CHECK(old[0] == 0 && old[1] > 0 && old[2] > 0 && old[3] > 0 && old[4] > 0);
}

// The highway runs straight along the diagonal, the same three tiles across
// on both halves, so it crosses the river on a bridge as wide as the road.
// The side bridges are two rows wide, a dirt road up to them from either
// bank; a tank drives over one to the other side.
void test_highway_and_bridges() {
    for (const MapSizePreset& preset : kMapSizes) {
        const TileMap map = make_demo_map(preset.tiles);
        const int size = map.width();
        int road = 0;
        int central = 0;
        int side = 0;
        int approached = 0;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const Terrain t = map.terrain(x, y);
                const int a = x + y + 1 - size;  // along the river from the center
                if (t == Terrain::Road) {
                    ++road;
                    CHECK(std::abs(a) <= 1);
                }
                if (t != Terrain::Bridge) continue;
                if (std::abs(a) <= 1) {
                    ++central;
                    continue;
                }
                ++side;
                const int offset = size * 25 / 100;
                CHECK(std::abs(a) == offset || std::abs(a) == offset + 1);
                bool dirt = false;
                for (int k = 1; k <= 8; ++k) {
                    for (const int d : {-1, 1}) {
                        const int tx = x + d * k;
                        const int ty = y - d * k;
                        dirt = dirt || (map.contains_tile(tx, ty) && map.terrain(tx, ty) == Terrain::DirtRoad);
                    }
                }
                approached += dirt ? 1 : 0;
            }
        }
        CHECK(road > size && central >= 6 && side >= 6);
        CHECK(approached == side);
    }
    // Over a side bridge, bank to bank.
    Simulation sim(1, make_demo_map());
    const int size = sim.world().map().width();
    const int a = size * 25 / 100;
    int bx = -1;
    int by = -1;
    for (int y = 0; y < size && bx < 0; ++y) {
        for (int x = 0; x < size; ++x) {
            if (sim.world().map().terrain(x, y) == Terrain::Bridge && x + y + 1 - size == a) {
                bx = x;
                by = y;
                break;
            }
        }
    }
    CHECK(bx >= 0);
    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, tile_center({bx + 6, by - 6}));
    const Trip trip = drive(sim, tank, bx - 6, by + 6, 3000);
    CHECK(trip.arrived && trip.ticks < 900);
}

// Shelling cuts up the trees round each burst, the worse the more bursts
// and the heavier they are, worst where it burst, up to snapped-off trunks;
// open ground and the woods out of reach stay as they were.
void test_shelling_shreds_trees() {
    TileMap map(40, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 22; x < 40; ++x) map.set_terrain(x, y, Terrain::Forest);
    }
    Simulation sim(1, map);
    const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Howitzer, at(4, 10));
    sim.world_for_setup().unit_for_setup(gun)->rounds = 30;
    issue(sim, fire_at(0, {gun}, 30, 10));
    int most = 0;
    for (int i = 0; i < 400 && most == 0; ++i) {
        sim.step();
        for (int y = 0; y < 20; ++y) {
            for (int x = 22; x < 40; ++x) most = std::max<int>(most, sim.world().map().shred(x, y));
        }
    }
    CHECK(most == 3);  // one 122 mm shell: 2 round it, 3 where it burst
    for (int i = 0; i < 4000; ++i) sim.step();
    int top = 0;
    int cut = 0;
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 40; ++x) {
            const int s = sim.world().map().shred(x, y);
            top = std::max(top, s);
            cut += s > 0 ? 1 : 0;
            if (x < 22) CHECK(s == 0);  // no trees there
        }
    }
    CHECK(top == TileMap::kMaxShred && cut >= 6);
    CHECK(sim.world().map().shred(38, 1) == 0);  // out of reach

    // A mortar bomb's a light burst: a tree hit by one is only thinned.
    TileMap wood(40, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 22; x < 40; ++x) wood.set_terrain(x, y, Terrain::Forest);
    }
    Simulation light(1, wood);
    const EntityId mortar = light.world_for_setup().spawn_unit(0, UnitTypeId::Mortar, at(12, 10));
    issue(light, fire_at(0, {mortar}, 28, 10));
    int first = 0;
    for (int i = 0; i < 600 && first == 0; ++i) {
        light.step();
        for (int y = 0; y < 20; ++y) {
            for (int x = 22; x < 40; ++x) first = std::max<int>(first, light.world().map().shred(x, y));
        }
    }
    CHECK(first == 2);
    int thinned = 0;
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 40; ++x) thinned += light.world().map().shred(x, y) > 0 ? 1 : 0;
    }
    CHECK(thinned <= 9);  // only round the burst

    // Only where there are trees: every other tile open ground, it stays as it was.
    TileMap patchy(40, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 22; x < 40; ++x) patchy.set_terrain(x, y, (x + y) % 2 == 0 ? Terrain::Grass : Terrain::Forest);
    }
    Simulation mixed(1, patchy);
    const EntityId howitzer = mixed.world_for_setup().spawn_unit(0, UnitTypeId::Howitzer, at(4, 10));
    mixed.world_for_setup().unit_for_setup(howitzer)->rounds = 10;
    issue(mixed, fire_at(0, {howitzer}, 30, 10));
    for (int i = 0; i < 2000; ++i) mixed.step();
    int in_wood = 0;
    for (int y = 0; y < 20; ++y) {
        for (int x = 22; x < 40; ++x) {
            const int sh = mixed.world().map().shred(x, y);
            if ((x + y) % 2 == 0) CHECK(sh == 0);
            in_wood += sh;
        }
    }
    CHECK(in_wood > 0);
}

void test_vehicle_drives_around_forest() {
    Simulation sim(1, forest_wall_map());
    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(10, 5));
    const Trip trip = drive(sim, tank, 30, 5, 3000);
    CHECK(trip.arrived);
    CHECK(!trip.touched_forest);  // it went along the trail at y = 30
}

void test_infantry_walks_through_forest_slower() {
    Simulation open(1, TileMap(40, 40));
    const EntityId a = open.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(10, 5));
    const Trip on_grass = drive(open, a, 30, 5, 3000);

    Simulation forest(1, forest_wall_map());
    const EntityId b = forest.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(10, 5));
    const Trip through_forest = drive(forest, b, 30, 5, 3000);

    CHECK(on_grass.arrived);
    CHECK(through_forest.arrived);
    CHECK(through_forest.touched_forest);  // straight through, no detour
    CHECK(through_forest.ticks > on_grass.ticks);
}

void test_vehicle_stops_at_the_shore() {
    TileMap map(40, 40);
    for (int32_t y = 15; y < 25; ++y) {
        for (int32_t x = 15; x < 25; ++x) map.set_terrain(x, y, Terrain::Water);
    }
    Simulation sim(1, map);
    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 5));
    const Trip trip = drive(sim, tank, 20, 20, 3000);
    const Unit* u = sim.world().find_unit(tank);
    CHECK(u->order == Order::Idle);  // gave up instead of pushing forever
    CHECK(sim.world().map().terrain_at(u->pos) != Terrain::Water);
    CHECK((u->pos - at(20, 20)).length() < Fixed::from_int(8));  // but got as close as it could
    CHECK(trip.ticks < 3000);
}

// A mixed group keeps together at the pace of its slowest member.
void test_group_moves_at_slowest_speed() {
    Simulation sim(1, TileMap(60, 60));
    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 10));
    sim.world_for_setup().spawn_unit(0, UnitTypeId::MachineGunner, at(5, 12));
    issue(sim, make_move(0, units_of(sim.world(), 0), 50, 11));
    const FixedVec2 start = sim.world().find_unit(tank)->pos;
    for (int i = 0; i < 202; ++i) sim.step();  // 2 ticks of input delay + 200 of marching

    const Fixed travelled = (sim.world().find_unit(tank)->pos - start).length();
    const Fixed mg_pace = unit_type(UnitTypeId::MachineGunner).speed * 200;
    CHECK(travelled <= mg_pace + Fixed::from_ratio(1, 2));
    CHECK(travelled >= mg_pace - Fixed::from_int(1));
}

}  // namespace

// --- Upgrades in the barracks ------------------------------------------------

// Armor barracks. Reactive armor: a tank takes 35% less from an RPG. Fire
// control: tank guns hit more far out.
void test_armor_upgrades() {
    const int32_t full = unit_type(UnitTypeId::Tank).max_hp;
    const int32_t hit = unit_type(UnitTypeId::Grenadier).weapon.damage -
                        unit_type(UnitTypeId::Tank).armor[static_cast<size_t>(DamageType::AntiTank)];
    const auto [plain, stood] = rpg_hits({Fixed::from_int(-1), Fixed{}}, false, 9);
    const auto [era, stood_era] = rpg_hits({Fixed::from_int(-1), Fixed{}}, false, 9, UpgradeId::ReactiveArmor);
    CHECK(!plain.empty() && plain[0] == full - hit);
    CHECK(!era.empty() && era[0] == full - hit * kReactiveArmorPercent / 100);
    CHECK(era.size() == 4);  // four hits in the front now

    const auto [far, stood_far] = tank_hits(45, 30);
    const auto [far_fcs, stood_fcs] = tank_hits(45, 30, true);
    CHECK(far_fcs > far);
}

// Explosive reactive armor comes in a line, each after the one before:
// Kontakt-1 against shaped charges only, Kontakt-5 and Relikt against AP
// rounds as well, Relikt the most.
void test_reactive_armor_line() {
    // What a tank takes from the first hit of an enemy tank's AP round, and of an RPG.
    auto ap_hit = [](int era) {
        Simulation sim(1, TileMap(40, 20));
        World& w = sim.world_for_setup();
        const EntityId gun = w.spawn_unit(0, UnitTypeId::Tank, at(10, 10));
        w.unit_for_setup(gun)->round_type = 1;  // armor-piercing
        const EntityId target = w.spawn_unit(1, UnitTypeId::Tank, at(18, 10));
        w.unit_for_setup(target)->rounds = 0;
        w.unit_for_setup(target)->hull = {Fixed::from_int(-1), Fixed{}};  // facing it
        const UpgradeId line[] = {UpgradeId::ReactiveArmor, UpgradeId::Kontakt5, UpgradeId::Relikt};
        for (int i = 0; i < era; ++i) w.upgrade_for_setup(1, line[i]);
        issue(sim, attack_order(0, {gun}, target));
        const int32_t full = unit_type(UnitTypeId::Tank).max_hp;
        for (int i = 0; i < 3000; ++i) {
            sim.step();
            if (hp_of(sim, target) < full) return full - hp_of(sim, target);
        }
        return 0;
    };
    const int32_t bare = ap_hit(0);
    CHECK(bare > 0);
    CHECK(ap_hit(1) == bare);  // Kontakt-1: AP rounds go through it
    CHECK(ap_hit(2) == bare * kEraLevels[1].kinetic_percent / 100);
    CHECK(ap_hit(3) == bare * kEraLevels[2].kinetic_percent / 100);
    const int32_t full = unit_type(UnitTypeId::Tank).max_hp;
    const int32_t rpg = full - rpg_hits({Fixed::from_int(-1), Fixed{}}, false, 9).first[0];
    CHECK(full - rpg_hits({Fixed::from_int(-1), Fixed{}}, false, 9, UpgradeId::ReactiveArmor).first[0] ==
          rpg * kEraLevels[0].shaped_percent / 100);
    static_assert(kEraLevels[0].shaped_percent > kEraLevels[1].shaped_percent &&
                  kEraLevels[1].shaped_percent > kEraLevels[2].shaped_percent);
    static_assert(kEraLevels[1].kinetic_percent > kEraLevels[2].kinetic_percent);

    // Kontakt-5 only after Kontakt-1, Relikt only after Kontakt-5.
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    w.set_stock(0, {0, 0, 5000, 5000, 0});
    const EntityId barracks = w.place_structure(StructureType::ArmorBarracks, 0, {20, 5}, 3, 3);
    auto research = [&](UpgradeId id) {
        issue(sim, Command{.type = CommandType::Research, .player = 0, .target_unit = barracks,
                           .upgrade = static_cast<uint8_t>(id)});
        for (int i = 0; i < 5; ++i) sim.step();
        return sim.world().find_structure(barracks)->research;
    };
    CHECK(research(UpgradeId::Relikt) == UpgradeId::Count);
    CHECK(research(UpgradeId::Kontakt5) == UpgradeId::Count);
    CHECK(research(UpgradeId::ReactiveArmor) == UpgradeId::ReactiveArmor);
    for (Tick i = 0; i < upgrade_def(UpgradeId::ReactiveArmor).time + 5; ++i) sim.step();
    CHECK(sim.world().era_level(0) == 1);
    CHECK(research(UpgradeId::Relikt) == UpgradeId::Count);
    CHECK(research(UpgradeId::Kontakt5) == UpgradeId::Kontakt5);
    for (Tick i = 0; i < upgrade_def(UpgradeId::Kontakt5).time + 5; ++i) sim.step();
    CHECK(research(UpgradeId::Relikt) == UpgradeId::Relikt);
    for (Tick i = 0; i < upgrade_def(UpgradeId::Relikt).time + 5; ++i) sim.step();
    CHECK(sim.world().era_level(0) == 3);
}

// The axes' tanks. The first player is the Democratic axis, the second the
// Authoritarian one: each hires only its own tanks in the armor barracks, and
// starts with its usual one (the T-64BV, the T-72B3).
void test_axes_hire_their_own_tanks() {
    CHECK(axis_of(0) == Axis::Democratic && axis_of(1) == Axis::Authoritarian);
    for (const UnitTypeId t : {UnitTypeId::T64BV, UnitTypeId::T64BM, UnitTypeId::Leopard1A5, UnitTypeId::Leopard2A6,
                               UnitTypeId::M1A1, UnitTypeId::Type10, UnitTypeId::K2, UnitTypeId::Merkava4}) {
        CHECK(unit_type(t).tank);
        CHECK(can_train(StructureType::ArmorBarracks, t, Axis::Democratic));
        CHECK(!can_train(StructureType::ArmorBarracks, t, Axis::Authoritarian));
    }
    for (const UnitTypeId t : {UnitTypeId::T62M, UnitTypeId::Tank, UnitTypeId::T80BVM, UnitTypeId::T90M, UnitTypeId::Type99A,
                               UnitTypeId::Karrar}) {
        CHECK(unit_type(t).tank);
        CHECK(can_train(StructureType::ArmorBarracks, t, Axis::Authoritarian));
        CHECK(!can_train(StructureType::ArmorBarracks, t, Axis::Democratic));
    }
    CHECK(can_train(StructureType::ArmorBarracks, UnitTypeId::Ifv, Axis::Democratic) &&
          can_train(StructureType::ArmorBarracks, UnitTypeId::Ifv, Axis::Authoritarian));

    // A hire of the other axis's tank does nothing.
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    w.set_stock(0, {20, 0, 5000, 5000, 5000});
    const EntityId barracks = w.place_structure(StructureType::ArmorBarracks, 0, {20, 5}, 3, 3);
    auto hire = [&](UnitTypeId t) {
        issue(sim, Command{.type = CommandType::Train, .player = 0, .target_unit = barracks, .unit_type = static_cast<uint8_t>(t)});
        for (int i = 0; i < 3; ++i) sim.step();
        return sim.world().find_structure(barracks)->queue.size();
    };
    CHECK(hire(UnitTypeId::Tank) == 0);
    CHECK(hire(UnitTypeId::T64BV) == 1);

    Simulation demo(1, make_demo_map());
    setup_demo_scenario(demo.world_for_setup());
    int ours = 0;
    int theirs = 0;
    for (const Unit& u : demo.world().units()) {
        ours += u.owner == 0 && u.type == UnitTypeId::T64BV ? 1 : 0;
        theirs += u.owner == 1 && u.type == UnitTypeId::Tank ? 1 : 0;
    }
    CHECK(ours == 2 && theirs == 2);
}

// The IFVs and APCs of the axes, as their tanks: each side hires its own
// from the armor barracks (the BMP-2 both); every one carries a squad and
// is drawn as the real one it is, each armored vehicle its own.
const UnitTypeId kDemocraticApcs[] = {UnitTypeId::Btr4e,   UnitTypeId::M113,   UnitTypeId::Bradley, UnitTypeId::Marder,
                                      UnitTypeId::Stryker, UnitTypeId::Type89, UnitTypeId::K21,     UnitTypeId::Namer};
const UnitTypeId kAuthoritarianApcs[] = {UnitTypeId::Bmp1,   UnitTypeId::Bmp3,    UnitTypeId::Btr82a, UnitTypeId::Mtlb,
                                         UnitTypeId::Zbd04a, UnitTypeId::Ratel20, UnitTypeId::Boragh};

void test_axes_hire_their_own_apcs() {
    for (const UnitTypeId t : kDemocraticApcs) {
        CHECK(unit_type(t).apc && !unit_type(t).tank && unit_type(t).troop_capacity > 0);
        CHECK(can_train(StructureType::ArmorBarracks, t, Axis::Democratic));
        CHECK(!can_train(StructureType::ArmorBarracks, t, Axis::Authoritarian));
    }
    for (const UnitTypeId t : kAuthoritarianApcs) {
        CHECK(unit_type(t).apc && !unit_type(t).tank && unit_type(t).troop_capacity > 0);
        CHECK(can_train(StructureType::ArmorBarracks, t, Axis::Authoritarian));
        CHECK(!can_train(StructureType::ArmorBarracks, t, Axis::Democratic));
    }
    CHECK(unit_type(UnitTypeId::Ifv).apc && is_armor(unit_type(UnitTypeId::Ifv)));
    std::vector<VehicleModel> models;
    for (size_t i = 0; i < kUnitTypeCount; ++i) {
        const UnitTypeDef& d = unit_type(static_cast<UnitTypeId>(i));
        CHECK(is_armor(d) == (d.family == Family::Tank || d.family == Family::Apc));
        if (d.family == Family::None) continue;  // tanks, IFVs, guns, AA guns, aircraft: each a real one
        CHECK(d.model != VehicleModel::Standard);
        CHECK(std::find(models.begin(), models.end(), d.model) == models.end());
        models.push_back(d.model);
    }
    CHECK(models.size() == static_cast<size_t>(VehicleModel::Count) - 1);
}

// Real IFVs and APCs, from the BMP-2 (= 100). The Namer, on a Merkava's
// hull, takes an RPG in the face better than a T-72B3; the BMP-2 holds one,
// the thin M113 doesn't. The MT-LB with its wide tracks is the best of them on soft
// ground, and the cheapest. The wheeled ones drive as wheels do; the ones
// that don't swim are lost in a bog as a tank is (their crews get out), the
// others swim. Only some take the ATGM launchers. The BMP-1 loads HEAT
// instead of its HE-FRAG, the BMP-3 its 100 mm (no sabot for it: a tank's).
void test_real_apcs() {
    auto rpg_in_face = [](UnitTypeId type) {
        Simulation sim(1, TileMap(40, 20));
        World& w = sim.world_for_setup();
        const EntityId rpg = w.spawn_unit(0, UnitTypeId::Grenadier, at(10, 10));
        const EntityId scout = w.spawn_unit(0, UnitTypeId::Scout, at(13, 12));  // to see it
        w.unit_for_setup(scout)->rounds = 0;
        const EntityId target = w.spawn_unit(1, type, at(17, 10));
        w.unit_for_setup(target)->rounds = 0;
        w.unit_for_setup(target)->hull = {Fixed::from_int(-1), Fixed{}};
        issue(sim, attack_order(0, {rpg}, target));
        for (int i = 0; i < 2000; ++i) {
            sim.step();
            if (hp_of(sim, target) < unit_type(type).max_hp) return unit_type(type).max_hp - hp_of(sim, target);
        }
        return 0;
    };
    const int32_t bmp = rpg_in_face(UnitTypeId::Ifv);
    CHECK(bmp > 0);
    CHECK(rpg_in_face(UnitTypeId::Namer) < rpg_in_face(UnitTypeId::Tank));
    CHECK(bmp < unit_type(UnitTypeId::Ifv).max_hp);                                // it holds one
    CHECK(rpg_in_face(UnitTypeId::M113) == unit_type(UnitTypeId::M113).max_hp);  // it doesn't

    std::vector<UnitTypeId> all{UnitTypeId::Ifv};
    all.insert(all.end(), std::begin(kDemocraticApcs), std::end(kDemocraticApcs));
    all.insert(all.end(), std::begin(kAuthoritarianApcs), std::end(kAuthoritarianApcs));
    const size_t materials = static_cast<size_t>(Resource::Materials);
    for (const UnitTypeId t : all) {
        const UnitTypeDef& d = unit_type(t);
        if (t != UnitTypeId::Mtlb) {
            CHECK(unit_type(UnitTypeId::Mtlb).soft_ground_percent < d.soft_ground_percent);
            CHECK(unit_type(UnitTypeId::Mtlb).cost[materials] < d.cost[materials]);
        }
        const bool launchers = t == UnitTypeId::Ifv || t == UnitTypeId::Bmp1 || t == UnitTypeId::Bmp3 || t == UnitTypeId::Zbd04a ||
                               t == UnitTypeId::Btr4e || t == UnitTypeId::Bradley || t == UnitTypeId::Marder || t == UnitTypeId::Type89;
        CHECK((d.missile_capacity > 0) == launchers);
        CHECK((ability_slot(d, AbilityId::Atgm) >= 0) == launchers);
        const bool two_rounds = t == UnitTypeId::Bmp1 || t == UnitTypeId::Bmp3 || t == UnitTypeId::Zbd04a;
        CHECK((d.alt_weapon.damage > 0) == two_rounds);
        CHECK((ability_slot(d, AbilityId::SwitchAmmo) >= 0) == two_rounds);
    }
    for (const UnitTypeId t : {UnitTypeId::Btr82a, UnitTypeId::Btr4e, UnitTypeId::Stryker, UnitTypeId::Ratel20}) {
        CHECK(move_class(unit_type(t)) == MoveClass::Wheeled);
    }
    CHECK(move_class(unit_type(UnitTypeId::Bradley)) == MoveClass::Vehicle);

    auto lost_in_bog = [](UnitTypeId type) {
        TileMap map(30, 10);
        for (int y = 0; y < 10; ++y) {
            for (int x = 0; x < 30; ++x) map.set_terrain(x, y, Terrain::Swamp);
        }
        Simulation sim(1, map);
        World& w = sim.world_for_setup();
        w.set_stock(0, {});
        const EntityId id = w.spawn_unit(0, type, at(15, 5));
        for (int i = 0; i < kBogSeconds * kTicksPerSecond * 2; ++i) sim.step();
        const int32_t men = sim.world().stock(0)[static_cast<size_t>(Resource::Personnel)];
        return sim.world().find_unit(id) == nullptr && men == unit_type(type).cost[static_cast<size_t>(Resource::Personnel)];
    };
    CHECK(lost_in_bog(UnitTypeId::Bradley) && lost_in_bog(UnitTypeId::Namer));
    CHECK(!lost_in_bog(UnitTypeId::Bmp3) && !lost_in_bog(UnitTypeId::Mtlb) && !lost_in_bog(UnitTypeId::M113));

    // The other round: the first hit on a T-72B3 six tiles off.
    auto first_hit = [](UnitTypeId type, bool other, bool sabot) {
        Simulation sim(3, TileMap(30, 20));
        World& w = sim.world_for_setup();
        if (sabot) w.upgrade_for_setup(0, UpgradeId::SabotRounds);
        const EntityId gun = w.spawn_unit(0, type, at(5, 10));
        const EntityId target = w.spawn_unit(1, UnitTypeId::Tank, at(11, 10));
        if (other) sim.schedule(0, use_ability(0, {gun}, AbilityId::SwitchAmmo, 0, 0));
        sim.schedule(0, make_move(1, {target}, 11, 10));  // stays put, holds its fire for a while
        int32_t prev = hp_of(sim, target);
        for (int i = 0; i < 400; ++i) {
            sim.step();
            if (hp_of(sim, target) < prev) return prev - hp_of(sim, target);
            prev = hp_of(sim, target);
        }
        return 0;
    };
    const int32_t frag = first_hit(UnitTypeId::Bmp1, false, false);
    CHECK(frag > 0 && first_hit(UnitTypeId::Bmp1, true, false) >= 5 * frag);
    const int32_t hundred = first_hit(UnitTypeId::Bmp3, true, false);
    CHECK(hundred > first_hit(UnitTypeId::Bmp3, false, false));
    CHECK(first_hit(UnitTypeId::Bmp3, true, true) == hundred);
    CHECK(unit_type(UnitTypeId::Bmp3).alt_weapon.range > unit_type(UnitTypeId::Bmp3).weapon.range);
}

// The rest of the axes' real vehicles, as their tanks: each side hires its
// own SPGs and towed howitzers from the artillery barracks, AA guns from the
// air defence barracks, aircraft from the airfield (the 2S1, 2S3, D-30 and
// Su-25 both sides); each of a family, grouped in the command grid by it.
void test_axes_hire_their_own_guns() {
    struct Line {
        StructureType building;
        Family family;
        std::vector<UnitTypeId> democratic;
        std::vector<UnitTypeId> authoritarian;
        std::vector<UnitTypeId> both;
    };
    const Line lines[] = {
        {StructureType::ArtilleryBarracks, Family::Spg, {UnitTypeId::M109, UnitTypeId::PzH2000, UnitTypeId::Caesar, UnitTypeId::K9},
         {UnitTypeId::MstaS, UnitTypeId::Pion, UnitTypeId::Plz05}, {UnitTypeId::Spg, UnitTypeId::Akatsiya}},
        {StructureType::ArtilleryBarracks, Family::Gun, {UnitTypeId::M777, UnitTypeId::Fh70}, {UnitTypeId::MstaB, UnitTypeId::Giatsint},
         {UnitTypeId::Howitzer}},
        {StructureType::AirDefenseBarracks, Family::AntiAir, {UnitTypeId::Gepard, UnitTypeId::Type87, UnitTypeId::K30},
         {UnitTypeId::Shilka, UnitTypeId::Tunguska, UnitTypeId::Pantsir, UnitTypeId::Pgz09}, {}},
        {StructureType::Airfield, Family::Aircraft, {UnitTypeId::A10}, {UnitTypeId::Su34}, {UnitTypeId::Su25}},
    };
    for (const Line& line : lines) {
        for (const UnitTypeId t : line.democratic) {
            CHECK(unit_type(t).family == line.family);
            CHECK(can_train(line.building, t, Axis::Democratic) && !can_train(line.building, t, Axis::Authoritarian));
        }
        for (const UnitTypeId t : line.authoritarian) {
            CHECK(unit_type(t).family == line.family);
            CHECK(can_train(line.building, t, Axis::Authoritarian) && !can_train(line.building, t, Axis::Democratic));
        }
        for (const UnitTypeId t : line.both) {
            CHECK(unit_type(t).family == line.family);
            CHECK(can_train(line.building, t, Axis::Authoritarian) && can_train(line.building, t, Axis::Democratic));
        }
    }
    // Mortars, AGS, rocket launchers, MANPADS and the radar stay as they were, for both.
    for (const UnitTypeId t : {UnitTypeId::Mortar, UnitTypeId::Ags, UnitTypeId::Mlrs}) {
        CHECK(can_train(StructureType::ArtilleryBarracks, t, Axis::Democratic) && can_train(StructureType::ArtilleryBarracks, t, Axis::Authoritarian));
    }
    for (const UnitTypeId t : {UnitTypeId::Manpads, UnitTypeId::AirRadar}) {
        CHECK(can_train(StructureType::AirDefenseBarracks, t, Axis::Democratic) && can_train(StructureType::AirDefenseBarracks, t, Axis::Authoritarian));
    }
}

// Real guns, from the 2S1 (the D-30, the Shilka, the Su-25): the PzH 2000
// reloads fastest of the SPGs, CAESAR drives on wheels, the Pion's 203 mm
// hits hardest and digs the biggest craters; the 2S1 swims through a bog, a
// 2S19 sinks in it as a tank does and its crew gets out; long-range charges
// carry every tube gun farther, not a rocket launcher; every AA gun shoots
// at aircraft, the Pantsir farthest; the Su-34's bombs hit hardest, the A-10
// is the toughest.
void test_real_guns() {
    const UnitTypeId spgs[] = {UnitTypeId::Spg, UnitTypeId::Akatsiya, UnitTypeId::MstaS, UnitTypeId::Pion, UnitTypeId::Plz05,
                               UnitTypeId::M109, UnitTypeId::PzH2000, UnitTypeId::Caesar, UnitTypeId::K9};
    for (const UnitTypeId t : spgs) {
        CHECK(unit_type(t).weapon.indirect && is_tube_artillery(unit_type(t)));
        if (t != UnitTypeId::PzH2000) CHECK(unit_type(UnitTypeId::PzH2000).weapon.reload < unit_type(t).weapon.reload);
        if (t != UnitTypeId::Pion) CHECK(unit_type(UnitTypeId::Pion).weapon.damage > unit_type(t).weapon.damage);
    }
    CHECK(move_class(unit_type(UnitTypeId::Caesar)) == MoveClass::Wheeled);
    CHECK(unit_type(UnitTypeId::MstaS).weapon.range > unit_type(UnitTypeId::Spg).weapon.range);

    auto lost_in_bog = [](UnitTypeId type) {
        TileMap map(30, 10);
        for (int y = 0; y < 10; ++y) {
            for (int x = 0; x < 30; ++x) map.set_terrain(x, y, Terrain::Swamp);
        }
        Simulation sim(1, map);
        World& w = sim.world_for_setup();
        w.set_stock(0, {});
        const EntityId id = w.spawn_unit(0, type, at(15, 5));
        for (int i = 0; i < kBogSeconds * kTicksPerSecond * 2; ++i) sim.step();
        const int32_t men = sim.world().stock(0)[static_cast<size_t>(Resource::Personnel)];
        return sim.world().find_unit(id) == nullptr && men == unit_type(type).cost[static_cast<size_t>(Resource::Personnel)];
    };
    CHECK(lost_in_bog(UnitTypeId::MstaS) && lost_in_bog(UnitTypeId::Shilka));
    CHECK(!lost_in_bog(UnitTypeId::Spg));

    // A shot a little past its reach: it fires from where it stands only with the charges.
    auto far_shot = [](UnitTypeId type, bool charges) {
        const int32_t range = unit_type(type).weapon.range.raw / Fixed::kOneRaw;
        Simulation sim(1, TileMap(range + 20, 20));
        if (charges) sim.world_for_setup().upgrade_for_setup(0, UpgradeId::LongRangeCharges);
        const EntityId gun = sim.world_for_setup().spawn_unit(0, type, at(5, 10));
        issue(sim, fire_at(0, {gun}, 5 + range + 3, 10));
        for (Tick i = 0; i < unit_type(type).deploy_time + unit_type(type).weapon.reload + 100; ++i) sim.step();
        const Unit* u = sim.world().find_unit(gun);
        return (u->pos - at(5, 10)).length() < Fixed::from_int(1) && u->last_shot_tick != kNeverFired;
    };
    for (const UnitTypeId t : {UnitTypeId::M777, UnitTypeId::Giatsint, UnitTypeId::PzH2000, UnitTypeId::Caesar, UnitTypeId::Mortar}) {
        CHECK(far_shot(t, true) && !far_shot(t, false));
    }
    CHECK(!far_shot(UnitTypeId::Mlrs, true));

    const UnitTypeId aa[] = {UnitTypeId::Shilka, UnitTypeId::Tunguska, UnitTypeId::Pantsir, UnitTypeId::Pgz09,
                             UnitTypeId::Gepard, UnitTypeId::Type87, UnitTypeId::K30};
    for (const UnitTypeId t : aa) {
        CHECK(unit_type(t).weapon.anti_air && !unit_type(t).weapon.air_only);
        if (t != UnitTypeId::Pantsir) CHECK(unit_type(UnitTypeId::Pantsir).weapon.range > unit_type(t).weapon.range);
    }
    for (const UnitTypeId t : {UnitTypeId::Su25, UnitTypeId::Su34, UnitTypeId::A10}) CHECK(unit_type(t).aircraft);
    CHECK(unit_type(UnitTypeId::Su34).weapon.damage > unit_type(UnitTypeId::Su25).weapon.damage);
    CHECK(unit_type(UnitTypeId::A10).max_hp > unit_type(UnitTypeId::Su25).max_hp &&
          unit_type(UnitTypeId::A10).max_hp > unit_type(UnitTypeId::Su34).max_hp);
}

// Real tanks, not newer = better. Armor in front: of an RPG in the face, a
// Leopard 2A6 takes less than a T-72B3, a T-62M more. On soft ground the
// light T-64BV gets on better than the heavy Abrams, though the Abrams is the
// faster on firm ground. A Merkava's crew gets out far oftener than a
// T-62M's. The reactive armor a tank carries goes only as far as it can
// (a T-64BV Kontakt-1, a Leopard 2A6 none). The Abrams' turbine drinks.
void test_real_tanks() {
    auto rpg_in_face = [](UnitTypeId type, bool side = false) {
        Simulation sim(1, TileMap(40, 20));
        World& w = sim.world_for_setup();
        const EntityId rpg = w.spawn_unit(0, UnitTypeId::Grenadier, at(10, 10));
        const EntityId scout = w.spawn_unit(0, UnitTypeId::Scout, at(13, 12));  // to see it
        w.unit_for_setup(scout)->rounds = 0;
        const EntityId tank = w.spawn_unit(1, type, at(17, 10));
        w.unit_for_setup(tank)->rounds = 0;
        w.unit_for_setup(tank)->hull = side ? FixedVec2{Fixed{}, Fixed::from_int(-1)} : FixedVec2{Fixed::from_int(-1), Fixed{}};
        issue(sim, attack_order(0, {rpg}, tank));
        for (int i = 0; i < 2000; ++i) {
            sim.step();
            if (hp_of(sim, tank) < unit_type(type).max_hp) return unit_type(type).max_hp - hp_of(sim, tank);
        }
        return 0;
    };
    const int32_t t72 = rpg_in_face(UnitTypeId::Tank);
    CHECK(t72 > 0);
    CHECK(rpg_in_face(UnitTypeId::Leopard2A6) == t72 * unit_type(UnitTypeId::Leopard2A6).front_percent / 100);
    CHECK(rpg_in_face(UnitTypeId::Leopard2A6) < t72 && rpg_in_face(UnitTypeId::T62M) > t72);
    CHECK(rpg_in_face(UnitTypeId::Leopard2A6, true) == rpg_in_face(UnitTypeId::Tank, true));  // the armor in front, not in the side

    auto distance = [](UnitTypeId type, Terrain ground) {
        TileMap map(60, 10);
        for (int y = 0; y < 10; ++y) {
            for (int x = 0; x < 60; ++x) map.set_terrain(x, y, ground);
        }
        Simulation sim(1, map);
        const EntityId tank = sim.world_for_setup().spawn_unit(0, type, at(3, 5));
        issue(sim, make_move(0, {tank}, 55, 5));
        const Fixed start = sim.world().find_unit(tank)->pos.x;
        for (int i = 0; i < 200; ++i) sim.step();
        return (sim.world().find_unit(tank)->pos.x - start).raw;
    };
    CHECK(distance(UnitTypeId::M1A1, Terrain::Grass) > distance(UnitTypeId::T64BV, Terrain::Grass));
    CHECK(distance(UnitTypeId::T64BV, Terrain::Plowed) > distance(UnitTypeId::M1A1, Terrain::Plowed));
    CHECK(distance(UnitTypeId::T64BV, Terrain::Swamp) > distance(UnitTypeId::M1A1, Terrain::Swamp) * 2);

    auto crews_back = [](UnitTypeId type) {
        int back = 0;
        for (uint64_t seed = 1; seed <= 30; ++seed) {
            Simulation sim(seed, TileMap(30, 10));
            World& w = sim.world_for_setup();
            w.set_stock(1, {});
            const EntityId tank = w.spawn_unit(1, type, at(15, 5));
            w.unit_for_setup(tank)->hp = 1;
            w.unit_for_setup(tank)->rounds = 0;
            const EntityId rifle = w.spawn_unit(0, UnitTypeId::Rifleman, at(12, 5));
            issue(sim, attack_order(0, {rifle}, tank));
            for (int i = 0; i < 300 && sim.world().find_unit(tank); ++i) sim.step();
            back += sim.world().stock(1)[static_cast<size_t>(Resource::Personnel)] > 0 ? 1 : 0;
        }
        return back;
    };
    const int merkava = crews_back(UnitTypeId::Merkava4);
    const int t62 = crews_back(UnitTypeId::T62M);
    CHECK(merkava >= 14 && t62 <= 8 && merkava > t62);

    auto ap_hit = [](UnitTypeId type) {
        Simulation sim(1, TileMap(40, 20));
        World& w = sim.world_for_setup();
        const EntityId gun = w.spawn_unit(0, UnitTypeId::Tank, at(10, 10));
        w.unit_for_setup(gun)->round_type = 1;
        const EntityId target = w.spawn_unit(1, type, at(18, 10));
        w.unit_for_setup(target)->rounds = 0;
        w.unit_for_setup(target)->hull = {Fixed::from_int(-1), Fixed{}};
        for (const UpgradeId u : {UpgradeId::ReactiveArmor, UpgradeId::Kontakt5, UpgradeId::Relikt}) w.upgrade_for_setup(1, u);
        issue(sim, attack_order(0, {gun}, target));
        for (int i = 0; i < 3000; ++i) {
            sim.step();
            if (hp_of(sim, target) < unit_type(type).max_hp) return unit_type(type).max_hp - hp_of(sim, target);
        }
        return 0;
    };
    // Relikt researched: the T-72B3 carries it; the T-64BV only Kontakt-1 (no help against AP); the Leopard nothing.
    const int32_t bare = unit_type(UnitTypeId::Tank).alt_weapon.damage - unit_type(UnitTypeId::Tank).armor[static_cast<size_t>(DamageType::AntiTank)];
    CHECK(ap_hit(UnitTypeId::Tank) == bare * kEraLevels[2].kinetic_percent / 100);
    CHECK(ap_hit(UnitTypeId::T64BV) == bare * unit_type(UnitTypeId::T64BV).front_percent / 100);
    CHECK(ap_hit(UnitTypeId::Leopard2A6) == bare * unit_type(UnitTypeId::Leopard2A6).front_percent / 100);

    CHECK(unit_type(UnitTypeId::M1A1).fuel_capacity * 2 == unit_type(UnitTypeId::Tank).fuel_capacity);
    CHECK(unit_type(UnitTypeId::Leopard2A6).weapon.effective_range > unit_type(UnitTypeId::Tank).weapon.effective_range);
    CHECK(unit_type(UnitTypeId::T62M).cost[static_cast<size_t>(Resource::Materials)] <
          unit_type(UnitTypeId::Tank).cost[static_cast<size_t>(Resource::Materials)]);
}

// ATGM launchers: an IFV fires a guided missile at an enemy tank 20 tiles
// off, which drives away across: the missile flies after it and hits. Not
// researched, the skill does nothing. An ammunition truck brings a new one.
void test_atgm() {
    auto run = [](bool researched, uint64_t seed) {
        Simulation sim(seed, TileMap(50, 30));
        World& w = sim.world_for_setup();
        if (researched) w.upgrade_for_setup(0, UpgradeId::Atgm);
        const EntityId ifv = w.spawn_unit(0, UnitTypeId::Ifv, at(5, 15));
        const EntityId tank = w.spawn_unit(1, UnitTypeId::Tank, at(25, 8));
        w.unit_for_setup(tank)->rounds = 0;
        const EntityId scout = w.spawn_unit(0, UnitTypeId::Scout, at(21, 10));
        w.unit_for_setup(scout)->rounds = 0;
        issue(sim, make_move(1, {tank}, 25, 26));
        for (int i = 0; i < 10; ++i) sim.step();
        issue(sim, use_ability(0, {ifv}, AbilityId::Atgm, 25, 8));
        for (int i = 0; i < 400; ++i) sim.step();
        const int32_t lost = sim.world().find_unit(tank) ? unit_type(UnitTypeId::Tank).max_hp - hp_of(sim, tank)
                                                          : unit_type(UnitTypeId::Tank).max_hp;
        const bool stood = (sim.world().find_unit(ifv)->pos - at(5, 15)).length() < Fixed::from_int(1);
        const int32_t missiles = sim.world().find_unit(ifv)->missiles;
        int32_t refilled = missiles;
        if (researched) {
            w.spawn_unit(0, UnitTypeId::AmmoTruck, at(7, 16));
            for (Tick i = 0; i < 4 * kRearmInterval; ++i) sim.step();
            refilled = sim.world().find_unit(ifv)->missiles;
        }
        return std::tuple{lost, stood, missiles, refilled};
    };
    const int32_t aboard = unit_type(UnitTypeId::Ifv).missile_capacity;
    int hits = 0;
    for (uint64_t seed = 1; seed <= 4; ++seed) {
        const auto [lost, stood, missiles, refilled] = run(true, seed);
        hits += lost > 0 ? 1 : 0;  // hit on the move (nine in ten do)
        CHECK(stood && missiles == aboard - 1 && refilled == aboard);
    }
    CHECK(hits >= 3);
    const auto [untouched, stood_idle, kept, same] = run(false, 2);
    CHECK(untouched == 0 && kept == aboard);
}

// Artillery barracks. Firing tables: more first shots on target. Drilled
// crews: set up and pack up in half the time. Long-range charges: a
// howitzer fires 70 tiles out from where it stands.
int first_shots_on_target(bool tables) {
    Simulation sim(5, TileMap(50, 20));
    World& w = sim.world_for_setup();
    if (tables) w.upgrade_for_setup(0, UpgradeId::FiringTables);
    const EntityId mortar = w.spawn_unit(0, UnitTypeId::Mortar, at(5, 10));
    issue(sim, fire_at(0, {mortar}, 20, 10));
    int on = 0;
    for (int i = 0; i < 4000 && sim.world().find_unit(mortar)->rounds > 0; ++i) {
        w.unit_for_setup(mortar)->ranging_shots = 0;  // every shot a first one
        const Tick now = sim.world().tick();
        sim.step();
        for (const Impact& imp : sim.world().recent_impacts()) {
            if (imp.tick == now && imp.shooter_type == UnitTypeId::Mortar &&
                (imp.pos - at(20, 10)).length() < Fixed::from_ratio(1, 2)) {
                ++on;
            }
        }
    }
    return on;
}

void test_artillery_upgrades() {
    CHECK(first_shots_on_target(true) > first_shots_on_target(false));
    Simulation plain(1, TileMap(20, 20));
    plain.world_for_setup().upgrade_for_setup(1, UpgradeId::FiringTables);
    CHECK(plain.world().ranging_chance(0, 1) == kRangingChance[0]);
    CHECK(plain.world().ranging_chance(1, 1) == kTabledRangingChance[0]);

    auto set_up = [](bool drilled) {
        Simulation sim(1, TileMap(60, 20));
        if (drilled) sim.world_for_setup().upgrade_for_setup(0, UpgradeId::DrilledCrews);
        const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Howitzer, at(5, 10));
        issue(sim, fire_at(0, {gun}, 40, 10));
        const Tick time = unit_type(UnitTypeId::Howitzer).deploy_time;
        for (Tick i = 0; i < time * 6 / 10; ++i) sim.step();
        const bool deployed = sim.world().find_unit(gun)->deployed;
        issue(sim, make_move(0, {gun}, 5, 15));
        for (Tick i = 0; i < time * 6 / 10 + 2; ++i) sim.step();
        return std::pair{deployed, !sim.world().find_unit(gun)->deployed};
    };
    CHECK(set_up(true) == std::pair(true, true));
    CHECK(set_up(false).first == false);
    CHECK(unit_type(UnitTypeId::Howitzer).weapon.range == Fixed::from_int(60));

    auto far_shot = [](bool charges) {
        Simulation sim(1, TileMap(90, 20));
        if (charges) sim.world_for_setup().upgrade_for_setup(0, UpgradeId::LongRangeCharges);
        const EntityId gun = sim.world_for_setup().spawn_unit(0, UnitTypeId::Howitzer, at(5, 10));
        issue(sim, fire_at(0, {gun}, 75, 10));  // 70 tiles
        for (Tick i = 0; i < unit_type(UnitTypeId::Howitzer).deploy_time + 100; ++i) sim.step();
        const Unit* u = sim.world().find_unit(gun);
        return (u->pos - at(5, 10)).length() < Fixed::from_int(1) && u->last_shot_tick != kNeverFired;
    };
    CHECK(far_shot(true));
    CHECK(!far_shot(false));
}

// Infantry barracks. Body armor: a rifle hit takes a quarter less. Vests:
// half as many rounds again, from the barracks, a stocked trench, a truck.
void test_infantry_upgrades() {
    auto first_hit = [](bool armor) {
        Simulation sim(1, TileMap(30, 20));
        if (armor) sim.world_for_setup().upgrade_for_setup(0, UpgradeId::BodyArmor);
        const EntityId target = sim.world_for_setup().spawn_unit(0, UnitTypeId::Rifleman, at(14, 10));
        sim.world_for_setup().unit_for_setup(target)->rounds = 0;
        sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(10, 10));
        for (int i = 0; i < 300 && hp_of(sim, target) == unit_type(UnitTypeId::Rifleman).max_hp; ++i) sim.step();
        return unit_type(UnitTypeId::Rifleman).max_hp - hp_of(sim, target);
    };
    const int32_t rifle = unit_type(UnitTypeId::Rifleman).weapon.damage;
    CHECK(first_hit(false) == rifle);
    CHECK(first_hit(true) == rifle * kBodyArmorPercent / 100);

    Simulation sim(1, TileMap(30, 20));
    World& w = sim.world_for_setup();
    w.upgrade_for_setup(0, UpgradeId::LoadVests);
    const int32_t vest = unit_type(UnitTypeId::Rifleman).rounds_capacity * kVestRoundsPercent / 100;
    const EntityId fresh = w.spawn_unit(0, UnitTypeId::Rifleman, at(3, 3));
    CHECK(sim.world().find_unit(fresh)->rounds == vest);
    CHECK(sim.world().rack(*sim.world().find_unit(fresh)) == vest);
    const EntityId tank = w.spawn_unit(0, UnitTypeId::Tank, at(3, 6));
    CHECK(sim.world().find_unit(tank)->rounds == unit_type(UnitTypeId::Tank).rounds_capacity);
    // At a stocked trench, and by an ammunition truck: topped up past the old rack.
    const EntityId trench = w.place_structure(StructureType::Trench, 0, {10, 10}, 1, 1);
    w.structure_for_setup(trench)->cache = 20;
    w.structure_for_setup(trench)->cache_owner = 0;
    const EntityId dug_in = w.spawn_unit(0, UnitTypeId::Rifleman, tile_center({10, 10}));
    w.unit_for_setup(dug_in)->rounds = vest - 30;
    const EntityId by_truck = w.spawn_unit(0, UnitTypeId::Rifleman, at(20, 15));
    w.unit_for_setup(by_truck)->rounds = vest - 30;
    w.spawn_unit(0, UnitTypeId::AmmoTruck, at(21, 16));
    for (Tick i = 0; i < 20 * kRearmInterval; ++i) sim.step();
    CHECK(sim.world().find_unit(dug_in)->rounds == vest);
    CHECK(sim.world().find_unit(by_truck)->rounds == vest);
}

// Recon: in ghillie suits a scout in cover is made out only from half as
// close. Engineers: heavier mines and charges; prefab pillboxes go up in
// half the time.
void test_recon_and_engineer_upgrades() {
    const Fixed close = Fixed::from_ratio(4, 5);
    CHECK(seen_in_cover(UnitTypeId::Rifleman, close, UnitTypeId::Scout, false));
    CHECK(!seen_in_cover(UnitTypeId::Rifleman, close, UnitTypeId::Scout, false, true));

    auto mine_on_tank = [](bool heavy, bool era = false) {
        Simulation sim(1, TileMap(40, 20));
        if (heavy) sim.world_for_setup().upgrade_for_setup(0, UpgradeId::HeavyCharges);
        if (era) sim.world_for_setup().upgrade_for_setup(1, UpgradeId::ReactiveArmor);
        lay_a_mine(sim, 0, true, 15, 10);
        const EntityId tank = sim.world_for_setup().spawn_unit(1, UnitTypeId::Tank, at_half(21, 21));
        issue(sim, make_move(1, {tank}, 25, 10));
        for (int i = 0; i < 600; ++i) sim.step();
        return unit_type(UnitTypeId::Tank).max_hp - hp_of(sim, tank);
    };
    const int32_t plain = mine_on_tank(false);
    CHECK(plain > 0 && mine_on_tank(true) == plain * kHeavyChargePercent / 100);
    CHECK(mine_on_tank(false, true) == plain);  // reactive armor is for what hits it, not a mine under it

    auto charge_on_bridge = [](bool heavy) {
        Simulation sim(1, village_map());
        if (heavy) sim.world_for_setup().upgrade_for_setup(0, UpgradeId::HeavyCharges);
        const EntityId bridge = sim.world().structure_at({20, 10})->id;
        const EntityId sapper = sim.world_for_setup().spawn_unit(0, UnitTypeId::Sapper, at(17, 9));
        issue(sim, use_ability(0, {sapper}, AbilityId::Demolish, 20, 10));
        for (Tick i = 0; i < kPlantWork + kFuseTicks + 70; ++i) sim.step();
        const Structure* b = sim.world().find_structure(bridge);
        return b ? structure_type(StructureType::Bridge).max_hp - b->hp : -1;
    };
    const int32_t armor = structure_type(StructureType::Bridge).armor[static_cast<size_t>(DamageType::AntiTank)];
    const int32_t charge = charge_on_bridge(false) + armor;  // the charge's damage
    CHECK(charge > armor && charge_on_bridge(true) == charge * kHeavyChargePercent / 100 - armor);

    auto pillbox_up = [](bool prefab) {
        Simulation sim(1, TileMap(40, 20));
        World& w = sim.world_for_setup();
        if (prefab) w.upgrade_for_setup(0, UpgradeId::PrefabPillbox);
        w.set_stock(0, structure_type(StructureType::Pillbox).cost);
        const EntityId sapper = w.spawn_unit(0, UnitTypeId::Sapper, post());
        issue(sim, use_ability(0, {sapper}, AbilityId::BuildPillbox, 20, 10));
        for (Tick i = 0; i < structure_type(StructureType::Pillbox).build_time * 6 / 10; ++i) sim.step();
        const Structure* box = sim.world().structure_at({10, 10});
        return box && box->built;
    };
    CHECK(pillbox_up(true));
    CHECK(!pillbox_up(false));
}

// Signals. Secure radios: two crossing bearings no longer fix a tank, a
// third does. Mast antennas: the headquarters and a command vehicle relay
// half as far again.
void test_signals_upgrades() {
    auto fixed = [](bool secure, int stations) {
        Simulation sim(1, TileMap(80, 60));
        World& w = sim.world_for_setup();
        if (secure) w.upgrade_for_setup(1, UpgradeId::SecureComms);
        const EntityId tank = w.spawn_unit(1, UnitTypeId::Tank, at(45, 30));
        const TilePos spots[] = {{12, 30}, {45, 5}, {70, 10}};
        for (int i = 0; i < stations; ++i) {
            const EntityId df = w.spawn_unit(0, UnitTypeId::DfStation, tile_center(spots[i]));
            w.unit_for_setup(df)->deployed = true;
        }
        for (int i = 0; i < 8; ++i) sim.step();
        return seen(sim, 0, tank);
    };
    CHECK(fixed(false, 2));
    CHECK(!fixed(true, 2));
    CHECK(fixed(true, 3));

    Simulation sim(1, TileMap(80, 40));
    World& w = sim.world_for_setup();
    w.place_structure(StructureType::Headquarters, 0, {4, 4}, 3, 3);
    const EntityId near_hq = w.spawn_unit(0, UnitTypeId::Tank, at(20, 6));    // 14.5 tiles off
    const EntityId command = w.spawn_unit(0, UnitTypeId::FieldHq, at(50, 30));
    const EntityId near_car = w.spawn_unit(0, UnitTypeId::Tank, at(65, 30));  // 15 tiles off
    w.unit_for_setup(near_hq)->silent = true;
    w.unit_for_setup(near_car)->silent = true;
    CHECK(!sim.world().in_touch(*sim.world().find_unit(near_hq)));
    CHECK(!sim.world().in_touch(*sim.world().find_unit(near_car)));
    w.upgrade_for_setup(0, UpgradeId::MastAntennas);
    CHECK(sim.world().in_touch(*sim.world().find_unit(near_hq)));
    CHECK(sim.world().in_touch(*sim.world().find_unit(near_car)));
    CHECK(sim.world().relay_reach(*sim.world().find_unit(command)) ==
          unit_type(UnitTypeId::FieldHq).relay_range * kMastRelayPercent / 100);
}

// Air defence: radar tracking puts more rounds into an aircraft. Airfield:
// cockpit armor takes 30% off every missile.
void test_air_upgrades() {
    auto shilka_damage = [](bool tracking) {
        AirSetup a = air_setup(4);
        World& w = a.sim.world_for_setup();
        if (tracking) w.upgrade_for_setup(1, UpgradeId::RadarTracking);
        w.spawn_unit(1, UnitTypeId::Shilka, at(40, 21));
        w.unit_for_setup(a.plane)->hp = 100000;
        issue(a.sim, fire_at(0, {a.plane}, 60, 20));
        fly_sortie(a);
        return 100000 - hp_of(a.sim, a.plane);
    };
    const int32_t plain = shilka_damage(false);
    CHECK(plain > 0 && shilka_damage(true) > plain);

    const int32_t per_hit = (unit_type(UnitTypeId::Manpads).weapon.damage - unit_type(UnitTypeId::Su25).armor[1]) *
                            kCockpitArmorPercent / 100;
    int damaged = 0;
    for (uint64_t seed = 1; seed <= 10; ++seed) {
        AirSetup a = air_setup(seed);
        a.sim.world_for_setup().upgrade_for_setup(0, UpgradeId::CockpitArmor);
        a.sim.world_for_setup().spawn_unit(1, UnitTypeId::Manpads, at(40, 21));
        issue(a.sim, fire_at(0, {a.plane}, 60, 20));
        fly_sortie(a);
        const int32_t lost = unit_type(UnitTypeId::Su25).max_hp - hp_of(a.sim, a.plane);
        CHECK(lost % per_hit == 0);
        damaged += lost > 0 ? 1 : 0;
    }
    CHECK(damaged > 0);
}

// Armor barracks, more: tuned engines drive faster; add-on armor takes a
// fifth off an HE shell but nothing off an RPG; loading drills reload the
// tank's and the IFV's guns a quarter faster, nobody else's.
void test_armor_upgrades_more() {
    auto drive = [](bool tuned, UnitTypeId type) {
        Simulation sim(1, TileMap(60, 20));
        if (tuned) sim.world_for_setup().upgrade_for_setup(0, UpgradeId::TankEngine);
        const EntityId unit = sim.world_for_setup().spawn_unit(0, type, at(5, 10));
        issue(sim, make_move(0, {unit}, 55, 10));
        for (int i = 0; i < 300; ++i) sim.step();
        return sim.world().find_unit(unit)->pos.x - Fixed::from_int(5);
    };
    CHECK(drive(true, UnitTypeId::Tank) * 100 > drive(false, UnitTypeId::Tank) * 110);
    CHECK(drive(true, UnitTypeId::Ifv) * 100 > drive(false, UnitTypeId::Ifv) * 110);
    CHECK(drive(true, UnitTypeId::Btr82a) * 100 > drive(false, UnitTypeId::Btr82a) * 110);
    CHECK(drive(true, UnitTypeId::Truck) == drive(false, UnitTypeId::Truck));  // only the armor

    auto he_hit = [](bool screens, UnitTypeId type = UnitTypeId::Tank) {
        Simulation sim(3, TileMap(30, 20));
        World& w = sim.world_for_setup();
        if (screens) w.upgrade_for_setup(1, UpgradeId::AddOnArmor);
        w.spawn_unit(0, UnitTypeId::Tank, at(5, 10));
        const EntityId target = w.spawn_unit(1, type, at(11, 10));
        w.unit_for_setup(target)->rounds = 0;
        for (int i = 0; i < 400 && hp_of(sim, target) == unit_type(type).max_hp; ++i) sim.step();
        return unit_type(type).max_hp - hp_of(sim, target);
    };
    const int32_t he = unit_type(UnitTypeId::Tank).weapon.damage -
                       unit_type(UnitTypeId::Tank).armor[static_cast<size_t>(DamageType::Explosive)];
    CHECK(he_hit(false) == he);
    CHECK(he_hit(true) == he * kAddOnArmorPercent / 100);
    const int32_t he_m113 = unit_type(UnitTypeId::Tank).weapon.damage -
                            unit_type(UnitTypeId::M113).armor[static_cast<size_t>(DamageType::Explosive)];
    CHECK(he_hit(true, UnitTypeId::M113) == he_m113 * kAddOnArmorPercent / 100);  // the slat cages
    const auto [rpg, stood] = rpg_hits({Fixed::from_int(-1), Fixed{}}, false, 9, UpgradeId::AddOnArmor);
    CHECK(!rpg.empty() && rpg[0] == unit_type(UnitTypeId::Tank).max_hp / 2);  // the screens don't stop a rocket

    auto reload = [](bool drills, UnitTypeId type) {
        Simulation sim(1, TileMap(30, 20));
        World& w = sim.world_for_setup();
        if (drills) w.upgrade_for_setup(0, UpgradeId::FastReload);
        const EntityId gun = w.spawn_unit(0, type, at(5, 10));
        const EntityId target = w.spawn_unit(1, UnitTypeId::Truck, at(9, 10));
        w.unit_for_setup(target)->hp = 100000;
        for (int i = 0; i < 300; ++i) {
            sim.step();
            if (sim.world().find_unit(gun)->last_shot_tick != kNeverFired) return sim.world().find_unit(gun)->cooldown;
        }
        return Tick{0};
    };
    CHECK(reload(false, UnitTypeId::Tank) == unit_type(UnitTypeId::Tank).weapon.reload);
    CHECK(reload(true, UnitTypeId::Tank) == unit_type(UnitTypeId::Tank).weapon.reload * kFastReloadPercent / 100);
    CHECK(reload(true, UnitTypeId::Ifv) == unit_type(UnitTypeId::Ifv).weapon.reload * kFastReloadPercent / 100);
    CHECK(reload(true, UnitTypeId::Stryker) == unit_type(UnitTypeId::Stryker).weapon.reload * kFastReloadPercent / 100);
    CHECK(reload(true, UnitTypeId::Rifleman) == unit_type(UnitTypeId::Rifleman).weapon.reload);
}

// A mortar at (5, 10) loaded with `shell` fires at (18, 10) until the first
// one lands; then it has nothing left to fire. The impacts of that landing.
struct ShellRun {
    Simulation sim;
    EntityId gun = 0;
    std::vector<Impact> landing;
};

ShellRun fire_shell(Shell shell) {
    ShellRun r{Simulation(2, TileMap(40, 20))};
    World& w = r.sim.world_for_setup();
    if (shell != Shell::He) w.upgrade_for_setup(0, shell_upgrade(shell));
    r.gun = w.spawn_unit(0, UnitTypeId::Mortar, at(5, 10));
    r.sim.schedule(0, load_shell(0, {r.gun}, shell));
    r.sim.schedule(1, fire_at(0, {r.gun}, 18, 10));
    for (int i = 0; i < 1000 && r.landing.empty(); ++i) {
        const Tick now = r.sim.world().tick();
        r.sim.step();
        for (const Impact& imp : r.sim.world().recent_impacts()) {
            if (imp.tick == now && imp.shooter_type == UnitTypeId::Mortar) r.landing.push_back(imp);
        }
    }
    w.unit_for_setup(r.gun)->rounds = 0;
    return r;
}

// Artillery shells: HE until the others are researched. Cluster: bomblets
// over an area. Incendiary: the ground burns; a man in it burns every second,
// a building too, until it burns out. White phosphorus: a smoke screen, and
// a smaller fire under it.
void test_artillery_shells() {
    Simulation sim(1, TileMap(30, 20));
    World& w = sim.world_for_setup();
    const EntityId mortar = w.spawn_unit(0, UnitTypeId::Mortar, at(5, 10));
    const EntityId tank = w.spawn_unit(0, UnitTypeId::Tank, at(5, 14));
    issue(sim, load_shell(0, {mortar}, Shell::Cluster));
    for (int i = 0; i < 3; ++i) sim.step();
    CHECK(sim.world().find_unit(mortar)->shell == Shell::He);  // not researched
    w.upgrade_for_setup(0, UpgradeId::ClusterMunitions);
    issue(sim, load_shell(0, {mortar, tank}, Shell::Cluster));
    issue(sim, load_shell(1, {mortar}, Shell::He));  // not the enemy's to load
    for (int i = 0; i < 3; ++i) sim.step();
    CHECK(sim.world().find_unit(mortar)->shell == Shell::Cluster);
    CHECK(sim.world().find_unit(tank)->shell == Shell::He);  // not a gun

    ShellRun cluster = fire_shell(Shell::Cluster);
    CHECK(cluster.landing.size() == static_cast<size_t>(kClusterBomblets));
    bool spread = false;
    for (const Impact& imp : cluster.landing) spread = spread || imp.pos != cluster.landing.front().pos;
    CHECK(spread);
    CHECK(cluster.sim.world().fires().empty());
    CHECK(fire_shell(Shell::He).landing.size() == 1);

    ShellRun fire = fire_shell(Shell::Incendiary);
    CHECK(fire.landing.size() == 1 && fire.sim.world().fires().size() == 1);
    const Fire burning = fire.sim.world().fires().front();
    CHECK(burning.radius == kFireRadius);
    World& fw = fire.sim.world_for_setup();
    const EntityId man = fw.spawn_unit(1, UnitTypeId::Rifleman, burning.center);
    const TilePos t = tile_of(burning.center);
    const EntityId store = fw.place_structure(StructureType::Warehouse, 1, {t.x + 1, t.y}, 2, 2);
    const int32_t store_hp = fire.sim.world().find_structure(store)->hp;
    for (Tick i = 0; i < 3 * kFireInterval; ++i) fire.sim.step();
    const int32_t burnt = unit_type(UnitTypeId::Rifleman).max_hp - hp_of(fire.sim, man);
    CHECK(burnt >= 2 * kFireBurn && burnt % kFireBurn == 0);
    const int32_t charred = store_hp - fire.sim.world().find_structure(store)->hp;
    CHECK(charred >= 2 * kFireStructureBurn && charred % kFireStructureBurn == 0);
    for (Tick i = 0; i < kFireTicks; ++i) fire.sim.step();
    CHECK(fire.sim.world().fires().empty());
    const int32_t out = fire.sim.world().find_structure(store)->hp;
    for (Tick i = 0; i < 3 * kFireInterval; ++i) fire.sim.step();
    CHECK(fire.sim.world().find_structure(store)->hp == out);  // burnt out

    ShellRun wp = fire_shell(Shell::Phosphorus);
    const std::vector<Smoke>& wp_smoke = wp.sim.world().smokes();  // a screen (and the burst's dust)
    const auto screen = std::find_if(wp_smoke.begin(), wp_smoke.end(), [](const Smoke& s) { return s.kind == SmokeKind::Screen; });
    CHECK(screen != wp_smoke.end() && screen->radius == kPhosphorusSmokeRadius);
    CHECK(std::count_if(wp_smoke.begin(), wp_smoke.end(), [](const Smoke& s) { return s.kind == SmokeKind::Screen; }) == 1);
    CHECK(wp.sim.world().fires().size() == 1 && wp.sim.world().fires().front().radius == kPhosphorusFireRadius);
}

// A workshop fixes our vehicles parked by it: three at a time, the worst
// damaged first, a material a second each, up to full health. Not the
// enemy's, not men, not one driving past, not without materials, not while
// it's still going up.
void test_workshop() {
    CHECK(std::find(std::begin(kBuildable), std::end(kBuildable), StructureType::Workshop) != std::end(kBuildable));
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    w.set_stock(0, {0, 0, 1000, 0, 0});
    const EntityId shop = w.place_structure(StructureType::Workshop, 0, {10, 10}, 3, 3);
    // Four of our tanks by it, the first the least damaged; worse off than
    // any of them: an enemy truck by it, one of ours far off, one of ours
    // driving past it.
    std::vector<EntityId> tanks;
    for (int i = 0; i < 4; ++i) {
        tanks.push_back(w.spawn_unit(0, UnitTypeId::Tank, at_half(19 + 2 * i, 28)));
        w.unit_for_setup(tanks.back())->rounds = 0;
        w.unit_for_setup(tanks.back())->hp = 130 - 10 * i;
    }
    const EntityId truck = w.spawn_unit(1, UnitTypeId::Truck, at_half(27, 23));
    const EntityId far = w.spawn_unit(0, UnitTypeId::Truck, at(30, 4));
    const EntityId passing = w.spawn_unit(0, UnitTypeId::Truck, at_half(15, 17));
    for (EntityId id : {truck, far, passing}) w.unit_for_setup(id)->hp = 10;
    const EntityId man = w.spawn_unit(0, UnitTypeId::Rifleman, at_half(18, 23));
    w.unit_for_setup(man)->hp = 10;
    w.unit_for_setup(man)->rounds = 0;
    issue(sim, make_move(0, {passing}, 38, 8));
    for (Tick i = 0; i < 2 * kRepairInterval; ++i) sim.step();
    CHECK(hp_of(sim, tanks[0]) == 130);  // the least damaged waits its turn
    for (int i = 1; i < 4; ++i) CHECK(hp_of(sim, tanks[static_cast<size_t>(i)]) == 130 - 10 * i + 2 * kRepairPerInterval);
    CHECK(hp_of(sim, truck) == 10 && hp_of(sim, far) == 10 && hp_of(sim, passing) == 10);
    CHECK(hp_of(sim, man) == 10);
    CHECK(sim.world().stock(0)[static_cast<size_t>(Resource::Materials)] ==
          1000 - 2 * 3 * kRepairCost[static_cast<size_t>(Resource::Materials)]);

    // Out of spare parts: nothing more.
    const int32_t before = hp_of(sim, tanks[0]);
    w.set_stock(0, {});
    for (Tick i = 0; i < 3 * kRepairInterval; ++i) sim.step();
    CHECK(hp_of(sim, tanks[0]) == before);
    // With them again, all the way up and no further.
    w.set_stock(0, {0, 0, 1000, 0, 0});
    for (int i = 0; i < 60 * kRepairInterval; ++i) sim.step();
    for (EntityId id : tanks) CHECK(hp_of(sim, id) == unit_type(UnitTypeId::Tank).max_hp);
    // A workshop still going up repairs nothing.
    w.structure_for_setup(shop)->built = false;
    w.unit_for_setup(tanks[0])->hp = 200;
    for (Tick i = 0; i < 3 * kRepairInterval; ++i) sim.step();
    CHECK(hp_of(sim, tanks[0]) == 200);
}

// A field hospital: our wounded go into its beds (vehicles and the enemy's
// men don't), heal there without firing a shot, and come out when well.
// "Leave" lets them out at once.
void test_field_hospital() {
    CHECK(std::find(std::begin(kBuildable), std::end(kBuildable), StructureType::Hospital) != std::end(kBuildable));
    Simulation sim(1, TileMap(40, 20));
    World& w = sim.world_for_setup();
    const EntityId ward = w.place_structure(StructureType::Hospital, 0, {10, 10}, 2, 2);
    const EntityId hurt = w.spawn_unit(0, UnitTypeId::Rifleman, at(8, 11));
    w.unit_for_setup(hurt)->hp = 10;
    const EntityId other = w.spawn_unit(0, UnitTypeId::MachineGunner, at(8, 13));
    w.unit_for_setup(other)->hp = 5;
    const EntityId tank = w.spawn_unit(0, UnitTypeId::Tank, at(7, 15));
    w.unit_for_setup(tank)->rounds = 0;
    const EntityId stranger = w.spawn_unit(1, UnitTypeId::Rifleman, at(35, 3));
    issue(sim, garrison(0, {hurt, other, tank}, ward));
    issue(sim, garrison(1, {stranger}, ward));
    for (int i = 0; i < 100; ++i) sim.step();
    CHECK(sim.world().find_unit(hurt)->inside == ward && sim.world().find_unit(other)->inside == ward);
    CHECK(sim.world().find_unit(tank)->inside == 0);
    CHECK(sim.world().find_unit(stranger)->inside == 0 && sim.world().find_unit(stranger)->order != Order::Garrison);
    CHECK(sim.world().find_structure(ward)->owner == 0);

    // An enemy walks by: the patients don't fire.
    const EntityId passer = w.spawn_unit(1, UnitTypeId::Rifleman, at(14, 11));
    w.unit_for_setup(passer)->rounds = 0;
    const int32_t healing = hp_of(sim, hurt);
    for (Tick i = 0; i < 5 * kTicksPerSecond; ++i) sim.step();
    CHECK(sim.world().find_unit(hurt)->last_shot_tick == kNeverFired);
    CHECK(hp_of(sim, hurt) == healing + static_cast<int32_t>(5 * kTicksPerSecond / kHealTicks));
    w.unit_for_setup(passer)->hp = 0;

    // Well again: out by himself, and the bed is free.
    for (Tick i = 0; i < 40 * kTicksPerSecond && sim.world().find_unit(hurt)->inside; ++i) sim.step();
    CHECK(sim.world().find_unit(hurt)->inside == 0);
    CHECK(hp_of(sim, hurt) == unit_type(UnitTypeId::Rifleman).max_hp);
    CHECK(sim.world().find_structure(ward)->garrison.size() == 1);
    CHECK(sim.world().find_structure(ward)->owner == 0);  // an empty hospital stays ours

    // "Leave": out at once, not yet well.
    issue(sim, Command{.type = CommandType::Unload, .player = 0, .target_unit = ward});
    for (int i = 0; i < 3; ++i) sim.step();
    CHECK(sim.world().find_unit(other)->inside == 0);
    CHECK(hp_of(sim, other) < unit_type(UnitTypeId::MachineGunner).max_hp);
    CHECK(sim.world().find_structure(ward)->owner == 0);
}

// The Donbas on the demo map: spoil tips of black rock, the highest ground
// about, that wheels can't climb; a chalk ridge along the river; tree lines
// down both sides of the concrete highway, with gaps to cross.
void test_donbas_landmarks() {
    const TileMap map = make_demo_map();
    int slag = 0;
    int chalk = 0;
    int top_slag = 0;
    int top_else = 0;
    int road = 0;
    int lined = 0;
    int gaps = 0;
    int steep = 0;
    int scree = 0;
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const Terrain t = map.terrain(x, y);
            slag += t == Terrain::Slag ? 1 : 0;
            if (t == Terrain::Slag) {
                // Two levels down to the next tile out; the scree at the foot on the plain.
                steep += x + 1 < map.width() && map.terrain(x + 1, y) == Terrain::Slag &&
                                 std::abs(map.elevation(x + 1, y) - map.elevation(x, y)) >= 2 ? 1 : 0;
                scree += map.elevation(x, y) == 1 ? 1 : 0;
            }
            chalk += t == Terrain::Chalk ? 1 : 0;
            if (t == Terrain::Slag) {
                top_slag = std::max<int>(top_slag, map.elevation(x, y));
            } else {
                top_else = std::max<int>(top_else, map.elevation(x, y));
            }
            if (t != Terrain::Road) continue;
            ++road;
            bool trees = false;
            for (int dx = -3; dx <= 3; ++dx) trees = trees || (map.contains_tile(x + dx, y) && map.terrain(x + dx, y) == Terrain::Forest);
            lined += trees ? 1 : 0;
            // The left lane, where the tree line beyond its verge is open ground: a gap to cross.
            const bool left_lane = x >= 2 && map.terrain(x - 1, y) != Terrain::Road;
            const Terrain beyond = x >= 2 ? map.terrain(x - 2, y) : Terrain::Water;
            gaps += left_lane && (beyond == Terrain::Grass || beyond == Terrain::Plowed || beyond == Terrain::Crops) ? 1 : 0;
        }
    }
    CHECK(slag > 40 && chalk > 20);
    CHECK(top_slag > top_else);  // nothing stands higher
    CHECK(top_slag == TileMap::kMaxElevation);
    CHECK(steep > 8 && scree > 8);
    CHECK(lined * 2 > road);     // mostly lined...
    CHECK(gaps > 0);             // ...with gaps to cross
    const TerrainDef& rock = terrain_def(Terrain::Slag);
    CHECK(rock.speed_percent[static_cast<size_t>(MoveClass::Wheeled)] == 0);
    CHECK(rock.speed_percent[static_cast<size_t>(MoveClass::Vehicle)] < rock.speed_percent[static_cast<size_t>(MoveClass::Foot)]);
    CHECK(terrain_def(Terrain::Chalk).speed_percent[static_cast<size_t>(MoveClass::Wheeled)] < 100);
}

// Men dig in anywhere in the fields, among the apple trees and in the
// kitchen gardens; not into the loose rock of a spoil tip.
void test_dig_in_the_fields() {
    auto diggable = [](Terrain t) {
        TileMap map(10, 10);
        map.set_terrain(5, 5, t);
        const World world(1, map);
        return world.diggable({5, 5});
    };
    for (const Terrain t : {Terrain::Crops, Terrain::Wheat, Terrain::Orchard, Terrain::Garden}) CHECK(diggable(t));
    CHECK(!diggable(Terrain::Slag));
}

// Wheat, apple orchards and kitchen gardens about the villages. A dairy farm
// a side, its two long cowsheds spacious enough to be made into depots, a coop
// by them; a coop and a cow shed at each of the villagers' small holdings.
void test_farmland() {
    for (const MapSizePreset& preset : kMapSizes) {
        Simulation sim(1, make_demo_map(preset.tiles));
        setup_demo_scenario(sim.world_for_setup());
        const World& world = sim.world();
        int wheat = 0;
        int orchards = 0;
        int gardens = 0;
        for (int y = 0; y < world.map().height(); ++y) {
            for (int x = 0; x < world.map().width(); ++x) {
                const Terrain t = world.map().terrain(x, y);
                wheat += t == Terrain::Wheat ? 1 : 0;
                orchards += t == Terrain::Orchard ? 1 : 0;
                gardens += t == Terrain::Garden ? 1 : 0;
            }
        }
        CHECK(wheat > 40 && orchards > 20 && gardens > 8);
        int long_sheds = 0;
        int small_sheds = 0;
        int coops = 0;
        int factories = 0;  // the industrial zone's shops, both sides
        for (const Structure& s : world.structures()) {
            if (s.look == HouseLook::House) continue;
            CHECK(s.type == StructureType::House);
            if (s.look == HouseLook::Factory) {
                ++factories;
                CHECK(s.tiles.size() >= kSpaciousTiles);
            } else if (s.look == HouseLook::Coop) {
                ++coops;
                CHECK(s.tiles.size() == 2);
            } else if (s.tiles.size() >= kSpaciousTiles) {
                ++long_sheds;
                CHECK(s.tiles.size() == 14);
            } else {
                ++small_sheds;
                CHECK(s.tiles.size() == 2);
            }
        }
        CHECK(long_sheds == 4 && small_sheds == 4 && coops == 6 && factories == 4);
    }
}

// Every upgrade is researched in its own building, as offered on its card.
void test_upgrade_buildings() {
    const std::pair<UpgradeId, StructureType> where[] = {
        {UpgradeId::ReactiveArmor, StructureType::ArmorBarracks},
        {UpgradeId::FireControl, StructureType::ArmorBarracks},
        {UpgradeId::Atgm, StructureType::ArmorBarracks},
        {UpgradeId::FiringTables, StructureType::ArtilleryBarracks},
        {UpgradeId::DrilledCrews, StructureType::ArtilleryBarracks},
        {UpgradeId::LongRangeCharges, StructureType::ArtilleryBarracks},
        {UpgradeId::BodyArmor, StructureType::InfantryBarracks},
        {UpgradeId::LoadVests, StructureType::InfantryBarracks},
        {UpgradeId::GhillieSuits, StructureType::ReconBarracks},
        {UpgradeId::HeavyCharges, StructureType::EngineerBarracks},
        {UpgradeId::PrefabPillbox, StructureType::EngineerBarracks},
        {UpgradeId::SecureComms, StructureType::SignalsBarracks},
        {UpgradeId::MastAntennas, StructureType::SignalsBarracks},
        {UpgradeId::RadarTracking, StructureType::AirDefenseBarracks},
        {UpgradeId::CockpitArmor, StructureType::Airfield},
        {UpgradeId::TankEngine, StructureType::ArmorBarracks},
        {UpgradeId::AddOnArmor, StructureType::ArmorBarracks},
        {UpgradeId::FastReload, StructureType::ArmorBarracks},
        {UpgradeId::ClusterMunitions, StructureType::ArtilleryBarracks},
        {UpgradeId::IncendiaryShells, StructureType::ArtilleryBarracks},
        {UpgradeId::PhosphorusShells, StructureType::ArtilleryBarracks},
        {UpgradeId::Kontakt5, StructureType::ArmorBarracks},
        {UpgradeId::Relikt, StructureType::ArmorBarracks},
    };
    for (const auto& [id, building] : where) CHECK(upgrade_def(id).building == building);
    CHECK(ability_def(AbilityId::Atgm).needs == UpgradeId::Atgm);
    // No building offers more than its bottom and middle rows hold.
    for (size_t b = 0; b < kStructureTypeCount; ++b) {
        int offered = 0;
        for (size_t i = 0; i < kUpgradeCount; ++i) {
            offered += upgrade_def(static_cast<UpgradeId>(i)).building == static_cast<StructureType>(b) ? 1 : 0;
        }
        CHECK(offered <= 10);
    }
}

int main() {
    test_fixed_math();
    test_rng();
    test_determinism();
    test_command_arrival_order_does_not_matter();
    test_foreign_units_are_ignored();
    test_unit_reaches_target();
    test_high_ground();
    test_projectiles_can_be_dodged();
    test_armor_lets_through_at_least_one();
    test_tanks_fire_on_the_move();
    test_tanks_blow_up();
    test_aim_by_the_range();
    test_attack_order_kills_target();
    test_cannot_attack_own_units();
    test_armies_fight();
    test_demo_map_is_fair();
    test_holds_fire_through_own_troops();
    test_moves_up_when_a_hill_is_in_the_way();
    test_bodies_in_the_path_take_the_hit();
    test_shells_through_a_tree_line_hit_hidden_friends();
    test_fire_from_high_ground_passes_over_own_troops();
    test_forest_stops_some_bullets();
    test_fog_of_war();
    test_sight_lines();
    test_cover_hides_until_spotted();
    test_firing_gives_away_a_hidden_shooter();
    test_no_shooting_into_the_fog();
    test_attack_orders_need_the_target_in_sight();
    test_observation_post_watches_its_sector();
    test_crossed_sectors_find_men_in_cover();
    test_tank_switches_rounds();
    test_area_shot();
    test_mg_sweep_hits_along_the_front();
    test_grenade_goes_over_cover();
    test_skills_need_the_unit_and_the_time();
    test_riflemen_dig_a_trench();
    test_foxholes_and_trenches_give_cover();
    test_parapet_faces_one_way();
    test_foxholes_spoil_the_aim();
    test_firing_while_walking_in_a_trench();
    test_riflemen_build_foxholes_and_parapets();
    test_trench_fight_ignores_cover();
    test_trench_fight();
    test_foxhole_becomes_a_dugout();
    test_grenades_fall_into_foxholes();
    test_out_of_fuel_but_still_shooting();
    test_tanker_refuels_vehicles_nearby();
    test_out_of_rounds_until_rearmed();
    test_service_vehicles_refill_at_depots();
    test_tanker_goes_up_in_flames();
    test_artillery_brackets_its_target();
    test_ranging_follows_the_target();
    test_guns_deploy_and_pack_up();
    test_guns_lay_before_firing();
    test_guns_hold_fire_unless_ordered();
    test_firing_guns_give_themselves_away();
    test_gun_pits();
    test_camouflaged_guns();
    test_ags_rapid_fire();
    test_ags_reaches_into_trenches();
    test_mlrs_salvo();
    test_tank_indirect_fire();
    test_self_propelled_howitzer();
    test_research();
    test_smoke_screen();
    test_burning_wreck_smoke();
    test_bombs_and_rubble();
    test_burst_dust();
    test_train_upgrades();
    test_upgrade_effects();
    test_mines();
    test_sappers_find_and_clear_mines();
    test_wire_and_hedgehogs();
    test_pillbox();
    test_demolition_charges();
    test_couriers_reach_silent_units();
    test_direction_finding();
    test_signals_barracks();
    test_aircraft_fly_missions();
    test_strike_follows_the_target();
    test_run_lines_up();
    test_bingo_fuel();
    test_only_air_defence_reaches_aircraft();
    test_manpads();
    test_radar_sees_aircraft();
    test_shilka();
    test_airfield_losses();
    test_aviation_buildings();
    test_structures_come_from_the_map();
    test_infantry_garrisons_a_house();
    test_garrison_is_safe_from_bullets();
    test_house_collapse_kills_the_garrison();
    test_bridge_can_be_blown();
    test_garrison_leaves_on_move();
    test_rear_troops_cut_timber();
    test_cut_down_forest_becomes_field();
    test_rear_troops_quarry_stone();
    test_trucks_collect_timber();
    test_rally_points();
    test_ifv_carries_squad();
    test_ifv_bail_out();
    test_quarters_house_the_men();
    test_tank_reaches_far();
    test_rpg_catches_tanks();
    test_armor_upgrades();
    test_atgm();
    test_artillery_upgrades();
    test_infantry_upgrades();
    test_recon_and_engineer_upgrades();
    test_signals_upgrades();
    test_air_upgrades();
    test_upgrade_buildings();
    test_reactive_armor_line();
    test_axes_hire_their_own_tanks();
    test_real_tanks();
    test_axes_hire_their_own_apcs();
    test_real_apcs();
    test_axes_hire_their_own_guns();
    test_real_guns();
    test_donbas_landmarks();
    test_farmland();
    test_dig_in_the_fields();
    test_armor_upgrades_more();
    test_artillery_shells();
    test_workshop();
    test_field_hospital();
    test_headquarters_trains_rear_troops();
    test_rear_troops_retrain_as_riflemen();
    test_trains_bring_men_and_freight();
    test_trucks_haul_freight_to_depots();
    test_trucks_haul_what_they_are_told();
    test_trucks_assigned_to_a_depot();
    test_idle_hands();
    test_service_vehicles_on_the_rail_run();
    test_apartment_blocks();
    test_cell_towers();
    test_gas_stations();
    test_grain_elevators();
    test_demo_map_has_barns();
    test_demo_map_relief();
    test_take_over_a_village_building();
    test_taking_over_rules();
    test_attached_supply();
    test_rations();
    test_infantry_runs_out_of_rounds();
    test_positions_hold_ammunition();
    test_stocking_rules();
    test_hunger_weakens_the_army();
    test_radio_call_for_supply();
    test_rear_troops_unload_faster();
    test_no_trains_without_the_station();
    test_fuel_depot_burns();
    test_trucks_are_unarmed();
    test_rear_troops_build_barracks();
    test_more_builders_build_faster();
    test_building_placement_rules();
    test_warehouse_takes_in_materials();
    test_vehicle_drives_around_forest();
    test_roads_fields_and_swamps();
    test_crops_swamps_and_craters();
    test_bogs();
    test_shelling_leaves_craters();
    test_crater_kinds();
    test_shelling_shreds_trees();
    test_highway_and_bridges();
    test_infantry_walks_through_forest_slower();
    test_vehicle_stops_at_the_shore();
    test_group_moves_at_slowest_speed();

    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("All engine tests passed\n");
    return 0;
}
