#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "engine/command.h"
#include "engine/economy.h"
#include "engine/fixed.h"
#include "engine/pathfinding.h"
#include "engine/rng.h"
#include "engine/structures.h"
#include "engine/terrain.h"
#include "engine/unit_types.h"

namespace engine {

enum class Order : uint8_t {
    Idle,          // stand, but fight enemies that come into sight
    Move,          // walk to order_point, ignoring enemies
    Attack,        // hunt order_target
    AttackMove,    // walk to order_point, fighting enemies on the way
    AttackGround,  // keep firing at order_point
    Garrison,      // walk to structure order_target and go in
    Gather,        // work gather_tile, carry the materials to the headquarters, repeat
    Retrain,       // walk into headquarters order_target, come out a rifleman
    Build,         // walk to structure order_target and build it until it's done
    Haul,          // supply truck: load at the station, unload at the depot, repeat
};

inline constexpr Tick kNeverFired = std::numeric_limits<Tick>::max();

struct Unit {
    EntityId id = 0;
    PlayerId owner = 0;
    UnitTypeId type = UnitTypeId::Rifleman;
    int32_t hp = 0;

    FixedVec2 pos{};
    FixedVec2 prev_pos{};                            // before the last tick, for render interpolation
    FixedVec2 facing{Fixed::from_int(1), Fixed{}};  // direction, not normalized
    bool moving = false;                             // moved during the last tick

    Order order = Order::Idle;
    FixedVec2 order_point{};    // Move / AttackMove destination (this unit's slot in the formation)
    TilePos order_goal{};       // the tile the whole group heads for
    Fixed speed_cap{};          // formation speed = the group's slowest unit; 0 = none
    EntityId order_target = 0;  // Attack
    EntityId engaged = 0;       // the enemy currently being shot at
    Tick cooldown = 0;          // ticks until the weapon is ready

    // Routes are pure functions of (map, goal, move class), shared between
    // units and cached, so they are not part of the checksum.
    std::shared_ptr<const FlowField> order_path;  // towards order_goal, shared by the group
    std::shared_ptr<const FlowField> chase_path;  // towards the enemy being chased

    Tick last_shot_tick = kNeverFired;  // for muzzle flashes and tracers
    FixedVec2 last_shot_at{};

    // The structure this unit is garrisoned in, 0 if outside. Inside, the
    // unit stands at the structure's center, can't be hit and fires from the
    // windows.
    EntityId inside = 0;

    // Rear troops and trucks at work.
    TilePos gather_tile{};  // the forest or rock being worked
    int32_t carrying = 0;   // materials in hand, or a truck's load
    Resource carrying_type = Resource::Materials;
    Tick work = 0;          // progress on the current bit of work (chopping, retraining, unloading)
};

// A shell or rocket in flight along a straight line of fire. It flies to a
// fixed point, so whoever is no longer standing there when it lands is not
// hit, but whoever steps into its path is.
struct Projectile {
    uint32_t id = 0;
    PlayerId owner = 0;
    EntityId shooter = 0;
    UnitTypeId shooter_type = UnitTypeId::Rifleman;  // whose weapon fired it
    uint8_t shooter_elevation = 0;                   // for the high ground bonus
    FixedVec2 origin{};
    FixedVec2 pos{};
    FixedVec2 prev_pos{};
    FixedVec2 target{};    // where it lands: the aim point, or the tree/house/hill in the way
    Fixed origin_height{};  // flight heights at both ends, in elevation levels
    Fixed target_height{};
};

// Where a projectile went off, kept for a few seconds so the renderer (and
// later sound, detection of firing guns...) can react to it.
struct Impact {
    Tick tick = 0;
    FixedVec2 pos{};
    UnitTypeId shooter_type = UnitTypeId::Rifleman;
};

// The complete game state.
//
// Everything in here must be deterministic: the same initial state plus the
// same commands must produce a bit-identical state on every machine. Only
// Simulation should call apply()/step(); other layers get a const World&.
class World {
public:
    World(uint64_t seed, TileMap map);

    EntityId spawn_unit(PlayerId owner, UnitTypeId type, FixedVec2 pos);

    void apply(const Command& cmd);
    void step();

    Tick tick() const { return tick_; }
    const TileMap& map() const { return map_; }
    FixedVec2 size() const { return map_.size(); }
    Rng& rng() { return rng_; }
    const std::vector<Unit>& units() const { return units_; }
    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    // Impacts of the last few seconds, oldest first.
    const std::deque<Impact>& recent_impacts() const { return recent_impacts_; }
    const Unit* find_unit(EntityId id) const;

    // Houses and bridges come from the map's House/Bridge tiles; player
    // buildings are placed with place_structure().
    const std::vector<Structure>& structures() const { return structures_; }
    const Structure* find_structure(EntityId id) const;
    const Structure* structure_at(TilePos tile) const;
    // Setup: a player's building on a w x h block of tiles starting at `origin`.
    EntityId place_structure(StructureType type, PlayerId owner, TilePos origin, int32_t w, int32_t h);
    // Whether a building of this type fits with its top-left tile at `origin`:
    // open ground only, nothing else there.
    bool can_place(StructureType type, TilePos origin) const;

    // Economy.
    const Stock& stock(PlayerId player) const { return stock_[player % kMaxPlayers]; }
    void set_stock(PlayerId player, const Stock& stock) { stock_[player % kMaxPlayers] = stock; }
    // A player's railway station (the first one), if it still stands.
    const Structure* station_of(PlayerId player) const;

    // Hash of the whole state. Peers exchange it to detect desyncs early.
    uint64_t checksum() const;

private:
    struct PendingDamage {
        EntityId victim;  // a unit or a structure
        int32_t amount;
    };

    enum class Step : uint8_t {
        Moved,
        Arrived,
        Blocked,  // terrain makes the point unreachable from here
    };

    // A straight line of fire with heights (in elevation levels) at both ends.
    struct FireLine {
        FixedVec2 from;
        Fixed from_height;
        FixedVec2 to;
        Fixed to_height;

        FixedVec2 point(Fixed t) const { return from + (to - from) * t; }
        Fixed height(Fixed t) const { return from_height + (to_height - from_height) * t; }
    };
    enum class Obstruction : uint8_t { None, Terrain, Foliage };

    Unit* find_unit_mut(EntityId id);
    Structure* find_structure_mut(EntityId id);
    void apply_group_move(const Command& cmd, Order order);
    void apply_attack(const Command& cmd);
    void apply_attack_ground(const Command& cmd);
    void apply_garrison(const Command& cmd);
    void apply_stop(const Command& cmd);

    // Economy (world_economy.cpp).
    void init_resources();
    void apply_gather(const Command& cmd);
    void apply_train(const Command& cmd);
    void apply_retrain(const Command& cmd);
    void apply_build(const Command& cmd);
    void apply_haul(const Command& cmd);
    void update_gathering(Unit& u);
    void update_retrain(Unit& u);
    void update_building(Unit& u);
    void update_hauling(Unit& u);
    void update_production();
    void update_trains();
    void burn_fuel_depot(const Structure& depot);
    const Structure* nearest_owned(PlayerId owner, StructureType type, FixedVec2 from) const;
    const Structure* nearest_headquarters(PlayerId owner, FixedVec2 from) const;
    // The nearest finished building of `owner` that takes in materials.
    const Structure* nearest_drop_off(PlayerId owner, FixedVec2 from) const;
    std::optional<TilePos> nearest_resource(TilePos around, int32_t radius) const;
    FixedVec2 door_of(const Structure& s, MoveClass cls) const;

    // Structures.
    void build_structures();
    void update_garrisoned(Unit& u);
    void seek_garrison(Unit& u);
    bool enter(Unit& u, Structure& s);
    void leave_structure(Unit& u);
    void hurt_structure(const Structure& s, const WeaponDef& weapon);
    void collapse(const Structure& s);
    void on_map_changed();
    EntityId structure_id_at(TilePos tile) const;

    void update_unit(Unit& u);
    const Unit* find_enemy_in_sight(Unit& u);
    void engage(Unit& u, const Unit& target);
    void engage_ground(Unit& u);

    // Line of fire. try_fire() returns false if a hill or house blocks the
    // line (the caller should move); it holds fire if own troops are in the way.
    FireLine fire_line(const Unit& shooter, FixedVec2 aim, Fixed aim_height) const;
    // `own_structure` (the house a garrisoned shooter fires from) doesn't block.
    Obstruction trace_terrain(const FireLine& line, Fixed start, bool roll_foliage, int32_t foliage_percent,
                              EntityId own_structure, Fixed& stop);
    const Unit* first_unit_on(const FireLine& line, Fixed t0, Fixed t1, EntityId ignore, Fixed& hit) const;
    bool own_troops_in_line(const Unit& shooter, const FireLine& line, Fixed start) const;
    bool try_fire(Unit& shooter, FixedVec2 aim, const Unit* target);
    void fire(Unit& shooter, FixedVec2 aim, Fixed aim_height);

    // Movement. `formation` limits the speed to the group's slowest unit.
    std::shared_ptr<const FlowField> field_to(TilePos goal, MoveClass cls);
    Step navigate(Unit& u, FixedVec2 point, std::shared_ptr<const FlowField>& path, TilePos field_goal,
                  bool formation);
    Step step_towards(Unit& u, FixedVec2 point, bool formation);
    bool move_to(Unit& u, FixedVec2 next);
    bool can_stand(const Unit& u, FixedVec2 p) const;

    void move_projectiles();
    void explode(const Projectile& p, FixedVec2 at, const Unit* direct_hit);
    void hurt(const Unit& victim, const WeaponDef& weapon, uint8_t attacker_elevation);
    void apply_damage_and_remove_dead();
    void separate_units();
    FixedVec2 clamp_to_map(FixedVec2 p, Fixed margin) const;

    Tick tick_ = 0;
    TileMap map_;
    Rng rng_;
    EntityId next_id_ = 1;
    uint32_t next_projectile_id_ = 1;
    std::vector<Unit> units_;  // always sorted by id
    std::vector<Projectile> projectiles_;
    std::deque<Impact> recent_impacts_;
    std::vector<Structure> structures_;       // sorted by id
    std::vector<EntityId> structure_tiles_;  // structure id per tile, 0 = none
    std::array<Stock, kMaxPlayers> stock_{};
    // Damage is applied after every unit has acted, so all units of a tick
    // shoot "at the same time" and the id order gives no one an advantage.
    std::vector<PendingDamage> pending_damage_;
    // Routes by (move class, goal tile). Only a cache: a route is rebuilt
    // identically whenever it's missing, so it can't affect the game.
    std::map<uint64_t, std::weak_ptr<const FlowField>> field_cache_;
};

}  // namespace engine
