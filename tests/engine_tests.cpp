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
bool seen_in_cover(UnitTypeId watcher, Fixed distance, UnitTypeId hider, bool hider_moves) {
    TileMap map(40, 20);
    for (int y = 3; y <= 17; ++y) {
        for (int x = 12; x <= 15; ++x) map.set_terrain(x, y, Terrain::Forest);
    }
    Simulation sim(1, map);
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
    const EntityId scout = w.spawn_unit(0, UnitTypeId::Scout, at(14, 20));
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

// The demo map has barns near the middle, two a side, well away from both bases.
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
        CHECK(barns == 4);
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
// an empty one just stops.
void test_tanker_goes_up_in_flames() {
    auto neighbour_hurt = [](int32_t fuel_aboard) {
        Simulation sim(1, TileMap(40, 20));
        World& w = sim.world_for_setup();
        const EntityId tanker = w.spawn_unit(0, UnitTypeId::FuelTanker, at(10, 10));
        w.unit_for_setup(tanker)->hp = 1;
        w.unit_for_setup(tanker)->carrying = fuel_aboard;
        const EntityId neighbour = w.spawn_unit(0, UnitTypeId::Truck, at(11, 11));
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
    for (int i = 0; i < 10; ++i) sim.step();
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
    CHECK(widest <= kSalvoSpread && widest > Fixed::from_int(1));
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
    const EntityId enemy = w.spawn_unit(1, UnitTypeId::Truck, at(15, 10));
    w.spawn_unit(0, UnitTypeId::Truck, at(10, 6));  // another pair of eyes, beside the screen
    for (Tick i = 0; i < 2 * kVisionInterval; ++i) sim.step();
    CHECK(seen(sim, 0, enemy) && seen(sim, 1, tank));
    issue(sim, {.type = CommandType::Stop, .player = 0, .units = {tank}});
    w.unit_for_setup(tank)->facing = {Fixed::from_int(1), Fixed{}};  // towards the enemy
    issue(sim, use_ability(0, {tank}, AbilityId::Smoke, 0, 0));
    for (Tick i = 0; i < 2 * kVisionInterval + 2; ++i) sim.step();
    CHECK(sim.world().smokes().size() == 1);
    CHECK(!seen(sim, 1, tank));
    for (Tick i = 0; i < kSmokeTicks; ++i) sim.step();
    CHECK(sim.world().smokes().empty());
    CHECK(seen(sim, 1, tank));
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

    Simulation rockets(1, TileMap(40, 30));
    rockets.world_for_setup().upgrade_for_setup(0, UpgradeId::ClusterRockets);
    const EntityId mlrs = rockets.world_for_setup().spawn_unit(0, UnitTypeId::Mlrs, at(5, 15));
    rockets.schedule(0, use_ability(0, {mlrs}, AbilityId::Salvo, 25, 15));
    Fixed burst{};
    for (int i = 0; i < 200 && burst.raw == 0; ++i) {
        rockets.step();
        for (const Projectile& p : rockets.world().projectiles()) burst = p.weapon.splash_radius;
    }
    CHECK(burst == unit_type(UnitTypeId::Mlrs).weapon.splash_radius * kClusterPercent / 100);
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
    const EntityId lone_tank = cw.spawn_unit(1, UnitTypeId::Tank, at(20, 22));
    const EntityId gunner = cw.spawn_unit(1, UnitTypeId::MachineGunner, at_half(72, 37));
    const EntityId target = cw.spawn_unit(0, UnitTypeId::Tank, at_half(62, 37));
    cw.unit_for_setup(target)->rounds = 0;  // takes it, can't answer
    issue(c.sim, fire_at(0, {c.plane}, 60, 18));
    bool overhead = false;
    for (int i = 0; i < 200 && !overhead; ++i) {
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
    test_headquarters_trains_rear_troops();
    test_rear_troops_retrain_as_riflemen();
    test_trains_bring_men_and_freight();
    test_trucks_haul_freight_to_depots();
    test_trucks_haul_what_they_are_told();
    test_trucks_assigned_to_a_depot();
    test_idle_hands();
    test_service_vehicles_on_the_rail_run();
    test_demo_map_has_barns();
    test_take_over_a_village_building();
    test_taking_over_rules();
    test_attached_supply();
    test_rations();
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
