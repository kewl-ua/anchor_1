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

    std::vector<uint64_t> checksums;
    for (int t = 0; t < ticks; ++t) {
        if (t == 0) {
            issue(sim, make_order(CommandType::AttackMove, 0, units_of(sim.world(), 0), center, center));
            issue(sim, make_order(CommandType::AttackMove, 1, units_of(sim.world(), 1), center, center));
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
            if (target_moves) sim.schedule(0, make_move(1, {target}, 14, 38));  // move orders ignore enemies
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
    issue(sim, make_move(1, {tank}, 14, 38));  // drive past without shooting back

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

// An explicit attack order hunts the target down even from out of sight.
void test_attack_order_kills_target() {
    Simulation sim(2, TileMap(40, 40));
    const EntityId tank = sim.world_for_setup().spawn_unit(0, UnitTypeId::Tank, at(5, 5));
    const EntityId victim = sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, at(25, 25));
    Command attack;
    attack.type = CommandType::Attack;
    attack.player = 0;
    attack.units = {tank};
    attack.target_unit = victim;
    issue(sim, attack);

    for (int i = 0; i < 1000 && sim.world().find_unit(victim); ++i) sim.step();
    CHECK(sim.world().find_unit(victim) == nullptr);
    sim.step();  // the tank notices on its next update
    const Unit* t = sim.world().find_unit(tank);
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
            // Every kind of natural terrain is there; ruins and buildings come later.
            const auto terrain = static_cast<Terrain>(t);
            if (terrain != Terrain::Ruins && terrain != Terrain::Building) CHECK(counts[t] > 0);
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
        trip.touched_forest = trip.touched_forest || sim.world().map().terrain_at(u->pos) == Terrain::Forest;
        if (trip.ticks > 3 && u->order == Order::Idle) break;
    }
    const Unit* u = sim.world().find_unit(id);
    trip.arrived = (u->pos - at(x, y)).length() < Fixed::from_ratio(1, 2);
    return trip;
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
    test_structures_come_from_the_map();
    test_infantry_garrisons_a_house();
    test_garrison_is_safe_from_bullets();
    test_house_collapse_kills_the_garrison();
    test_bridge_can_be_blown();
    test_garrison_leaves_on_move();
    test_rear_troops_cut_timber();
    test_cut_down_forest_becomes_field();
    test_rear_troops_quarry_stone();
    test_headquarters_trains_rear_troops();
    test_rear_troops_retrain_as_riflemen();
    test_trains_bring_men_and_freight();
    test_trucks_haul_freight_to_depots();
    test_rear_troops_unload_faster();
    test_no_trains_without_the_station();
    test_fuel_depot_burns();
    test_trucks_are_unarmed();
    test_rear_troops_build_barracks();
    test_more_builders_build_faster();
    test_building_placement_rules();
    test_warehouse_takes_in_materials();
    test_vehicle_drives_around_forest();
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
