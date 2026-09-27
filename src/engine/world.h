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
    Observe,       // scout: an observation post watching the sector towards order_point, holding fire
    Ability,       // using the skill order_ability at order_point (to order_point2)
    Supply,        // a service vehicle looking after the unit `serves`
    Collect,       // a supply truck parked at order_point by the wood, taking loads in
};

inline constexpr Tick kNeverFired = std::numeric_limits<Tick>::max();

// Fog of war is worked out this often, not every tick.
inline constexpr Tick kVisionInterval = kTicksPerSecond / 4;
// A shot gives away a hidden shooter for this long.
inline constexpr Tick kRevealTicks = 3 * kTicksPerSecond;
// A target on the move is spotted from this much farther away (percent)...
inline constexpr int32_t kSpotMovingPercent = 200;
// ...and a stealthy one (a scout) only from this much closer.
inline constexpr int32_t kSpotStealthyPercent = 50;
// Observation posts watch a 90 degree sector. Every post whose sector covers
// a man in cover makes him out from this many tiles farther: cross
// observation from several posts finds what one alone would miss.
inline constexpr Fixed kSectorDetection = Fixed::from_int(4);

// Field works. Cover is the share of hits (percent) the walls take instead
// of the man: bullets and fragments from the level or below, not from above
// and not lobbed ones.
inline constexpr int32_t kFoxholeCover = 50;
inline constexpr int32_t kCraterCover = 40;  // a man lying in a shell crater
inline constexpr int32_t kTrenchCover = 50;         // at a position in a trench...
inline constexpr int32_t kTrenchWalkingCover = 10;  // ...walking along it
inline constexpr int32_t kParapetCover = 25;        // on top, against fire from the front
inline constexpr Tick kSettleTicks = 2 * kTicksPerSecond;  // standing this long in a trench = at a position
// Small arms fired from a trench before settling at a position hit this hard (percent).
inline constexpr int32_t kTrenchWalkingFirePercent = 25;
inline constexpr int32_t kFoxholeAccuracyPercent = 80;
// A trench fight: this close, walls protect nobody.
inline constexpr Fixed kCloseQuarters = Fixed::from_int(2);
inline constexpr int32_t kAssaultCloseQuartersPercent = 150;  // assault troopers' damage up close
// Digging, for one rifleman; several at one trench tile dig it faster.
inline constexpr Tick kTrenchWork = 6 * kTicksPerSecond;
inline constexpr Tick kFoxholeWork = 8 * kTicksPerSecond;
inline constexpr Tick kParapetWork = 5 * kTicksPerSecond;
inline constexpr int32_t kMaxTrenchLength = 16;  // tiles per order
// A foxhole dug out into a dugout by the men standing in it (up to 4).
inline constexpr Stock kDugoutCost = {0, 0, 60, 0, 0};
inline constexpr Tick kDugoutWork = 20 * kTicksPerSecond;
inline constexpr int32_t kDugoutDiggers = 4;
inline constexpr int32_t kGrenadeVictims = 3;  // a grenade into a room hurts this many inside

// Direct fire far out: beyond a gun's effective range its accuracy falls,
// down to this share at the full range (a tank firing 50 tiles out).
inline constexpr int32_t kFarAccuracyPercent = 35;
// An anti-tank round into a vehicle's side or rear (more than 60 degrees off
// the way it faces) does this much of its damage: an ambush from the flank.
inline constexpr int32_t kFlankHitPercent = 200;

// An IFV carries its squad: foot soldiers get in this close to it, ride
// unseen and unhurt, and get out at the back. Knocked out with them aboard,
// they bail out, each losing this share of his health.
inline constexpr Fixed kBoardDistance = Fixed::from_ratio(3, 2);
inline constexpr int32_t kBailOutHurtPercent = 50;

// Vehicle supply. A unit of fuel from the stock drives a vehicle this many tiles.
inline constexpr int32_t kTilesPerFuel = 2;
// Service vehicles look after their own vehicles this close, handing over a
// unit of cargo every so many ticks.
inline constexpr Fixed kServiceRadius = Fixed::from_int(4);
inline constexpr Tick kRefuelInterval = 4;
inline constexpr Tick kRearmInterval = 8;
inline constexpr Tick kRefillInterval = 2;  // loading at the depot
// A service vehicle attached to a unit keeps this close to it.
inline constexpr Fixed kEscortDistance = Fixed::from_int(3);
// Ammunition stocked at a combat position: men this close to it (or inside
// it) draw on it, a unit of it every kRearmInterval; the truck stocking it
// unloads from this close.
inline constexpr Fixed kCacheReach = Fixed::from_int(3);
inline constexpr Fixed kCacheUnload = Fixed::from_ratio(3, 2);

// Artillery. Ranging (bracketing): the chance a shell lands on the aim point
// on the 1st, 2nd and 3rd and later shots at the same target, and how far
// off the others land. A new aim point this close to the last one is still
// the same target. A target our scouts see is bracketed one step faster.
inline constexpr std::array<int32_t, 3> kRangingChance = {17, 50, 95};
inline constexpr std::array<Fixed, 3> kRangingSpread = {Fixed::from_int(4), Fixed::from_int(2), Fixed::from_int(1)};
inline constexpr Fixed kOnTargetSpread = Fixed::from_ratio(3, 10);
inline constexpr Fixed kSameTarget = Fixed::from_ratio(3, 2);
// A gun's flash gives it away this long to whoever looks its way; an
// observation post facing it sees the flash from twice its reach. Firing
// from a village, the locals report it to the enemy wherever he is.
inline constexpr Tick kGunRevealTicks = 6 * kTicksPerSecond;
inline constexpr int32_t kFlashSectorPercent = 200;
inline constexpr Tick kReportedTicks = 15 * kTicksPerSecond;
// Gun pits (a capunier, a mortar's closed position) and camouflage.
inline constexpr int32_t kGunPitCover = 50;
inline constexpr Tick kGunPitWork = 10 * kTicksPerSecond;
inline constexpr Tick kCamouflageWork = 10 * kTicksPerSecond;
// AGS rapid fire: five grenades this far apart along the front, each off by
// up to kBurstJitter; one burst in twenty lays them in a perfect row.
inline constexpr int32_t kBurstGrenades = 5;
inline constexpr Fixed kBurstSpacing = Fixed::from_ratio(4, 5);
inline constexpr Fixed kBurstJitter = Fixed::from_ratio(3, 5);
inline constexpr int32_t kPerfectBurstPercent = 5;
// MLRS salvo: a rocket every so often, anywhere within this of the point.
inline constexpr Fixed kSalvoSpread = Fixed::from_int(3);
inline constexpr Tick kSalvoInterval = 2;
// A tank firing from a covered position wears its barrel: HP per shot.
inline constexpr int32_t kBarrelWearPercent = 1;

// Craters: a heavy shell or rocket bursting on open ground or a road leaves
// one this often (percent), a medium one (a mortar bomb) less often.
inline constexpr Fixed kHeavyBurst = Fixed::from_ratio(6, 5);
inline constexpr Fixed kMediumBurst = Fixed::from_int(1);
inline constexpr int32_t kHeavyCraterPercent = 60;
inline constexpr int32_t kMediumCraterPercent = 25;

// Engineering. A mine covers its tile: the first enemy of the right kind to
// come onto it (infantry for an AP mine, vehicles for an AT one) sets it
// off. The enemy doesn't see it until one of his sappers comes this close.
inline constexpr Fixed kMineDetection = Fixed::from_ratio(5, 2);
inline constexpr Stock kApMineCost = {0, 0, 0, 5, 0};
inline constexpr Stock kAtMineCost = {0, 0, 0, 10, 0};
inline constexpr Tick kMineWork = 4 * kTicksPerSecond;
inline constexpr Tick kClearWork = 5 * kTicksPerSecond;
inline constexpr Fixed kClearRadius = Fixed::from_ratio(3, 2);
inline constexpr Tick kWireWork = 3 * kTicksPerSecond;       // a tile
inline constexpr Tick kHedgehogWork = 5 * kTicksPerSecond;   // a tile
inline constexpr Stock kWireCost = {0, 0, 5, 0, 0};          // a tile
inline constexpr Stock kHedgehogCost = {0, 0, 10, 0, 0};     // a tile
// A pillbox's garrison fires only through its slit: this wide a sector.
inline constexpr int32_t kPillboxSectorDegrees = 120;
inline constexpr Tick kPlantWork = 8 * kTicksPerSecond;
inline constexpr Tick kFuseTicks = 5 * kTicksPerSecond;
inline constexpr Fixed kSapperRetreat = Fixed::from_int(3);  // runs this far from the charge

// Upgrades' effects.
inline constexpr Tick kTrainIntervalUpgraded = 45 * kTicksPerSecond;
inline constexpr int32_t kTrainCapacityPercent = 150;
inline constexpr int32_t kSabotPercent = 130;
inline constexpr int32_t kClusterPercent = 150;
inline constexpr Fixed kOpticsDetection = Fixed::from_int(2);
inline constexpr int32_t kOpticsSectorTiles = 3;
inline constexpr int32_t kShovelWorkPercent = 67;  // of the digging time
// A smoke screen: this wide around a point this far ahead of the tank, this long.
inline constexpr Fixed kSmokeRadius = Fixed::from_int(2);
inline constexpr Fixed kSmokeAhead = Fixed::from_int(2);
inline constexpr Tick kSmokeTicks = 20 * kTicksPerSecond;

// Electronic warfare. Orders to a unit keeping radio silence go by courier,
// this long, unless a relay is close: the headquarters, a command vehicle
// or a signaller on the air.
inline constexpr Tick kCourierTicks = 3 * kTicksPerSecond;
inline constexpr Fixed kHeadquartersRelay = Fixed::from_int(12);
// A cell tower held by our men relays radio this far.
inline constexpr Fixed kTowerRelay = Fixed::from_int(15);
// Upper floors and masts: the garrison sees this much farther (tiles), and
// fires down as from this much higher ground (elevation levels).
inline constexpr int32_t kApartmentSightBonus = 3;
inline constexpr int32_t kTowerSightBonus = 8;
inline constexpr uint8_t kUpperFloorLevels = 2;
inline constexpr int32_t kElevatorSightBonus = 6;
// Spoils: fuel in a gas station's tanks, grain in an elevator. Whoever holds
// them (men inside) hauls it off to his depots; his vehicles this close to
// the pumps fill up there.
inline constexpr int32_t kGasStationFuel = 300;
inline constexpr int32_t kElevatorFood = 600;
inline constexpr Fixed kPumpReach = Fixed::from_int(2);
// Two bearings on a radio fix it only if they cross at 20 degrees or more
// (sin^2 20 = 0.117): from nearly the same spot they only give a direction.
inline constexpr int64_t kFixSinSqPermille = 117;

// Aviation. Aircraft fly this high over the ground (elevation levels), see
// and are seen from up there, and only air defence reaches them.
inline constexpr Fixed kFlightHeight = Fixed::from_int(3);
// A unit of fuel from the stock takes an aircraft this many tiles.
inline constexpr int32_t kAircraftTilesPerFuel = 4;
// On the airfield: a rocket and a unit of fuel every so often, from the stock.
inline constexpr Tick kAirRearmInterval = 10;
// The rocket run: it begins this far short of the target, lined up on it
// within 15 degrees; a rocket every kRocketInterval, each at the ground this
// far ahead of the aircraft and up to kRocketScatter off.
inline constexpr Fixed kRunStart = Fixed::from_int(9);
inline constexpr Fixed kRunAlignedCos = Fixed::from_ratio(966, 1000);
inline constexpr Fixed kRocketAhead = Fixed::from_int(5);
inline constexpr Tick kRocketInterval = 2;
inline constexpr Fixed kRocketScatter = Fixed::from_int(1);
// Turning: the heading swings this much sideways a tick (8.5 degrees); this
// close to the heading wanted, it's on it. Closer than kTurnClearance to the
// target an aircraft flies straight on and comes round again.
inline constexpr Fixed kAircraftTurn = Fixed::from_ratio(3, 20);
inline constexpr Fixed kAlignedCos = Fixed::from_ratio(989, 1000);
inline constexpr Fixed kTurnClearance = Fixed::from_ratio(7, 2);
// Turns home with this much fuel (tiles) to spare over the way back.
inline constexpr Fixed kBingoReserve = Fixed::from_int(10);
// Air defence guns with their radar switched off aim by eye: this share of the hits (percent).
inline constexpr int32_t kOpticalSightPercent = 50;

// A DF station's bearing on an enemy radio: from the station towards it.
struct Bearing {
    PlayerId owner = 0;
    EntityId station = 0;
    EntityId target = 0;
    FixedVec2 from{};
    FixedVec2 dir{};  // not normalized; only the direction is the player's to know
};

// An order on its way to a unit keeping radio silence.
struct Courier {
    Tick arrives = 0;
    Command cmd;
};

// A smoke screen: nothing is seen into or through it.
struct Smoke {
    FixedVec2 center{};
    Fixed radius{};
    Tick clears = 0;
};

// A mine on a tile.
struct Mine {
    uint32_t id = 0;
    PlayerId owner = 0;
    TilePos tile{};
    bool anti_tank = false;
    uint8_t found_by = 0;  // bit per player that knows where it is
};

// A demolition charge ticking against a structure.
struct Charge {
    PlayerId owner = 0;
    EntityId target = 0;
    FixedVec2 pos{};
    Tick goes_off = 0;
};

// The tiles of a trench dug from a to b: a 4-connected line, so men can walk
// along it, at most kMaxTrenchLength long.
std::vector<TilePos> trench_line(TilePos a, TilePos b);

struct Unit {
    EntityId id = 0;
    PlayerId owner = 0;
    UnitTypeId type = UnitTypeId::Rifleman;
    int32_t hp = 0;

    FixedVec2 pos{};
    FixedVec2 prev_pos{};                            // before the last tick, for render interpolation
    FixedVec2 facing{Fixed::from_int(1), Fixed{}};  // direction, not normalized
    // The way the hull points: where it last drove. A turret (`facing`) turns
    // to its target, the hull doesn't; the flanks are the hull's.
    FixedVec2 hull{Fixed::from_int(1), Fixed{}};
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

    // The structure this unit is garrisoned in, or the IFV it rides in; 0 if
    // outside. In a structure the unit stands at its center, can't be hit and
    // fires from the windows; in an IFV it rides along and doesn't fire.
    EntityId inside = 0;
    // An IFV: the men aboard, in the order they got in.
    std::vector<EntityId> passengers;

    // Rear troops and trucks at work.
    TilePos gather_tile{};  // the forest or rock being worked
    int32_t carrying = 0;   // materials in hand, or a truck's load
    Resource carrying_type = Resource::Materials;
    Tick work = 0;          // progress on the current bit of work (chopping, retraining, unloading)

    // Bit per player: who currently sees this unit (updated with the fog of war).
    uint8_t seen_by = 0;

    // Skills.
    uint8_t round_type = 0;                            // loaded: 0 the main round, 1 the other one (a tank's AP)
    std::array<Tick, kMaxAbilities> ability_ready{};  // tick from which each skill can be used again
    AbilityId order_ability = AbilityId::AreaShot;     // Order::Ability
    FixedVec2 order_point2{};                          // the other end of a line
    int32_t shots_left = 0;                            // a burst in progress
    Tick still = 0;                                    // ticks since it last moved

    // Vehicles run out: fuel in tiles of driving, rounds for the main gun.
    Fixed fuel{};
    int32_t rounds = 0;

    // Guns: set up or packed, and how far that is along; the point being
    // bracketed and how many shots in.
    bool deployed = false;
    Tick deploy_work = 0;
    FixedVec2 ranging_point{};
    uint8_t ranging_shots = 0;
    bool camouflaged = false;  // under nets: hidden like in a forest until it moves
    bool perfect_burst = false;  // this AGS burst lands in a perfect row

    // A supply truck's assignment: the freight it hauls (Count: whatever
    // piles up most at the station) and the depot it takes it to (0: the
    // nearest one for it).
    Resource haul_cargo = Resource::Count;
    EntityId haul_depot = 0;

    // A tanker or an ammunition truck on Order::Supply: the unit it looks
    // after, attached to it for good or, `on_call`, answering its radio
    // call once.
    EntityId serves = 0;
    bool on_call = false;
    // A collecting truck on its way to unload what the rear troops gave it.
    bool delivering = false;

    bool silent = false;  // radio silence: no bearings on it, orders by courier
    bool airborne = false;  // an aircraft in the air: only air defence reaches it
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
    WeaponDef weapon{};     // what was fired
    bool lobbed = false;    // arcs over everything and only comes down at the target
    bool enters = false;    // a hand grenade: goes in through a window or a dugout's entrance
    // Fired at an aircraft: bursts in the air, never touches the ground. A
    // missile that is going to hit flies after `homing`; one that misses, 0.
    bool at_air = false;
    EntityId homing = 0;
};

// Where a projectile went off, kept for a few seconds so the renderer (and
// later sound, detection of firing guns...) can react to it.
struct Impact {
    Tick tick = 0;
    FixedVec2 pos{};
    UnitTypeId shooter_type = UnitTypeId::Rifleman;
    Fixed splash{};  // radius of the burst, tiles
    bool air = false;  // up in the sky: a missile bursting at an aircraft
};

// The round a unit's gun is loaded with: the main one, or the alternative.
inline const WeaponDef& weapon_of(const Unit& u) {
    const UnitTypeDef& def = unit_type(u.type);
    return u.round_type == 1 && def.alt_weapon.damage > 0 ? def.alt_weapon : def.weapon;
}

// How much a vehicle takes on at the station for the supply run.
int32_t haul_capacity(const Unit& u);

// Rear troops and supply trucks with nothing to do: AoE's idle villagers.
inline bool idle_hand(const Unit& u) {
    const UnitTypeDef& def = unit_type(u.type);
    return u.order == Order::Idle && !u.inside && (def.worker || u.type == UnitTypeId::Truck);
}

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
    // Setup and tests: direct access to a unit (to hand it a nearly empty tank...).
    Unit* unit_for_setup(EntityId id) { return find_unit_mut(id); }
    Structure* structure_for_setup(EntityId id) { return find_structure_mut(id); }

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
    // Whether `player` may stock ammunition at this structure: a combat
    // position not held by the enemy, with no one else's stock in it.
    bool can_stock(const Structure& s, PlayerId player) const;
    // Whether `player`'s rear troops may turn this building into a depot: a
    // spacious village building, not a depot already, not held by the enemy.
    bool can_convert(const Structure& s, PlayerId player) const;
    // A player's railway station (the first one), if it still stands.
    const Structure* station_of(PlayerId player) const;
    // The nearest finished building of this type of `owner`'s.
    const Structure* nearest_owned(PlayerId owner, StructureType type, FixedVec2 from) const;
    // Where a truck takes a load of `cargo`: its own depot if it's assigned
    // one for that freight and it stands, otherwise the nearest.
    const Structure* haul_destination(const Unit& truck, Resource cargo) const;
    // Open ground a trench, foxhole or parapet can go on.
    bool diggable(TilePos t) const;
    // Mines and charges. A player sees his own mines and the ones his sappers found.
    const std::vector<Mine>& mines() const { return mines_; }
    bool knows(PlayerId player, const Mine& m) const {
        return m.owner == player || (player < kMaxPlayers && ((m.found_by >> player) & 1) != 0);
    }
    const std::vector<Charge>& charges() const { return charges_; }
    const std::vector<Smoke>& smokes() const { return smokes_; }
    bool has_upgrade(PlayerId player, UpgradeId id) const {
        return player < kMaxPlayers && ((upgrades_[player] >> static_cast<uint32_t>(id)) & 1u) != 0;
    }
    // Electronic warfare. Bearings our DF stations took at the last fog
    // update; orders still on their way to silent units.
    const std::vector<Bearing>& bearings() const { return bearings_; }
    const std::vector<Courier>& couriers() const { return couriers_; }
    // Whether an order reaches the unit at once: its radio is on, or a
    // relay of ours is close enough.
    bool in_touch(const Unit& u) const;
    // Two of `player`'s bearings on this radio cross at a wide enough angle:
    // he knows where it is.
    bool fixed_by(PlayerId player, const Unit& u) const;
    // Rations: the men `player` has to feed, and whether his army went hungry.
    int32_t mouths(PlayerId player) const;
    // Housing: the men in service and in training, and the bunks there are for them.
    int32_t population(PlayerId player) const;
    int32_t bunks(PlayerId player) const;
    // A building's hiring waits for room in the quarters.
    bool waits_for_bunks(const Structure& s) const;
    bool hungry(PlayerId player) const { return player < kMaxPlayers && hungry_[player]; }
    // Setup and tests: an upgrade without the research.
    void upgrade_for_setup(PlayerId player, UpgradeId id) {
        upgrades_[player % kMaxPlayers] |= 1u << static_cast<uint32_t>(id);
    }

    // Fog of war, updated every kVisionInterval ticks. A tile is visible when
    // one of the player's units or buildings has a line of sight to it;
    // explored once it ever was.
    bool visible(PlayerId player, TilePos t) const { return fog_at(visible_, player, t); }
    bool explored(PlayerId player, TilePos t) const { return fog_at(explored_, player, t); }
    // Own units are always seen. Others when their tile is in view, unless
    // they hide in cover (a forest, a house) and nobody is close enough to
    // spot them and they haven't just given themselves away by firing.
    bool sees(PlayerId player, const Unit& u) const {
        return u.owner == player || (player < kMaxPlayers && ((u.seen_by >> player) & 1) != 0);
    }
    // A building is seen when any of its tiles is.
    bool sees(PlayerId player, const Structure& s) const;
    // Changes with every fog update, so cached images know to rebuild.
    uint32_t vision_revision() const { return vision_revision_; }
    // Called before each tick's commands: brings the fog of war up to date
    // when it's due.
    void begin_tick();

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
    // A command carried out now: apply() has already sent couriers off.
    void deliver(const Command& cmd);
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
    void apply_collect(const Command& cmd);
    void apply_rally(const Command& cmd);
    void update_collect(Unit& u);
    // Of our trucks collecting by the wood, the one whose spot is nearest.
    Unit* nearest_collector(PlayerId owner, FixedVec2 from);
    void apply_observe(const Command& cmd);

    // Skills and field works (world_skills.cpp).
    void apply_ability(const Command& cmd);
    void apply_upgrade(const Command& cmd);
    void apply_unload(const Command& cmd);
    void update_ability(Unit& u);
    void finish_ability(Unit& u);
    void update_upgrades();
    // A lobbed shot: arcs over everything, comes down at the aim point.
    void lob(Unit& shooter, FixedVec2 aim, const WeaponDef& weapon, bool enters);
    EntityId place_fieldwork(StructureType type, PlayerId owner, TilePos t, FixedVec2 facing);
    void update_gathering(Unit& u);
    void update_retrain(Unit& u);
    void update_building(Unit& u);
    void update_hauling(Unit& u);
    void update_production();
    void update_trains();
    void update_rations();
    // `stock_burns`: a depot burns part of its owner's fuel with it; a gas station only what's in it.
    void burn_fuel_depot(const Structure& depot, bool stock_burns = true);
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
    void apply_board(const Command& cmd, const Unit& carrier);
    void seek_carrier(Unit& u, Unit& carrier);
    bool board(Unit& u, Unit& carrier);
    void hurt_structure(const Structure& s, const WeaponDef& weapon);
    void collapse(const Structure& s);
    void on_map_changed();
    EntityId structure_id_at(TilePos tile) const;

    void update_unit(Unit& u);
    const Unit* find_enemy_in_sight(Unit& u);
    void engage(Unit& u, const Unit& target);
    void engage_ground(Unit& u);

    // Fog of war (world_vision.cpp).
    void update_vision();
    // Indices of the tiles an observer on `tile` sees, cached.
    const std::vector<int32_t>& sight_from(TilePos tile, int32_t radius, Fixed eye, EntityId own_structure);
    bool line_of_sight(FixedVec2 from, Fixed eye_height, TilePos target, EntityId own_structure) const;
    bool spotted(PlayerId player, const Unit& target) const;
    // An observation post's sector: who watches, from where, and the tiles
    // in it that are in plain view (sorted).
    struct Sector {
        PlayerId owner;
        FixedVec2 from;
        std::vector<int32_t> tiles;
        FixedVec2 dir{};     // where it looks
        int32_t radius = 0;  // tiles
    };
    // A gun that just fired and gives itself away to `player`: its flash seen
    // by an observation post, or the locals reporting it from a village.
    bool betrayed(PlayerId player, const Unit& gun) const;
    // A point one of `player`'s scouts is looking at.
    bool spotted_by_scouts(PlayerId player, FixedVec2 point) const;
    Fixed ground_at(FixedVec2 p) const;  // TileMap::surface_height, from cached corners
    bool fog_at(const std::array<std::vector<uint8_t>, kMaxPlayers>& grid, PlayerId player, TilePos t) const {
        if (player >= kMaxPlayers || grid[player].empty() || !map_.contains(t)) return false;
        return grid[player][static_cast<size_t>(t.y * map_.width() + t.x)] != 0;
    }

    // Line of fire. try_fire() returns false if a hill or house blocks the
    // line (the caller should move); it holds fire if own troops are in the way.
    FireLine fire_line(const Unit& shooter, FixedVec2 aim, Fixed aim_height) const;
    // How high above the ground a garrison looks and fires from.
    Fixed window_height(EntityId structure) const;
    // `own_structure` (the house a garrisoned shooter fires from) doesn't block.
    Obstruction trace_terrain(const FireLine& line, Fixed start, bool roll_foliage, int32_t foliage_percent,
                              EntityId own_structure, Fixed& stop);
    const Unit* first_unit_on(const FireLine& line, Fixed t0, Fixed t1, EntityId ignore, Fixed& hit) const;
    bool own_troops_in_line(const Unit& shooter, const FireLine& line, Fixed start) const;
    // `spends` = the shot uses up one of the unit's rounds.
    bool try_fire(Unit& shooter, FixedVec2 aim, const Unit* target, const WeaponDef& weapon, bool spends = true);
    void fire(Unit& shooter, FixedVec2 aim, Fixed aim_height, const WeaponDef& weapon, bool spends = true);
    bool out_of_rounds(const Unit& u) const;
    // Service vehicles: look after our vehicles nearby; load up at a depot.
    void serve(Unit& u);
    void refill(Unit& u);
    // Service vehicles and the units they look after.
    void apply_supply(const Command& cmd);
    void update_supply(Unit& u);
    void call_supply(const Unit& caller);
    // An ammunition truck keeping a combat position stocked.
    void stock_position(Unit& u, Structure& post);
    // Men at positions take ammunition from the stock there; vehicles by the
    // pumps of a gas station we hold fill up from its tanks.
    void draw_from_caches();
    // Where a truck loads next: the railway station, or a gas station or an
    // elevator our men hold, the nearest with freight on it for this truck.
    const Structure* freight_source(const Unit& u, std::optional<Resource>& pick) const;
    // Drives up to `v` and hands over a unit of cargo every so often.
    void hand_over(Unit& u, const Unit& v);
    // Loads up at the nearest depot for the cargo; false once full or when it can't.
    bool load_up(Unit& u);
    // A fireball: fuel or ammunition going up.
    void burst_into_flames(FixedVec2 at, PlayerId owner, const WeaponDef& fire);

    // Engineering (world_engineering.cpp).
    void lay_mine(Unit& u, bool anti_tank);
    void clear_mines(Unit& u);
    void put_up_obstacles(Unit& u, StructureType type);
    void start_pillbox(Unit& u);
    void plant_charge(Unit& u);
    void update_mines();
    void update_charges();
    void apply_research(const Command& cmd);
    void update_research();
    void update_smoke();
    bool in_smoke(FixedVec2 p) const;
    // Work a job takes a unit, shortened by the owner's upgrades (shovels).
    Tick work_needed(const Unit& u, Tick base) const;
    void find_mines();

    // Aviation and air defence (world_air.cpp).
    enum class Steer : uint8_t { Turn, Straight, Direct };
    void give_mission(Unit& plane, FixedVec2 point, EntityId target);
    void update_aircraft(Unit& u);
    void fly(Unit& u, FixedVec2 goal, Steer how);
    void fire_rocket(Unit& u);
    void rearm_aircraft(Unit& u);
    const Structure* home_airfield(const Unit& u) const;
    FixedVec2 parking_spot(const Structure& airfield, EntityId self) const;
    const Unit* find_air_target(Unit& u);
    void fire_at_air(Unit& u, const Unit& target);
    void move_missile(Projectile& p);
    bool sky_watch(PlayerId player, const Unit& plane) const;

    // Electronic warfare (world_signals.cpp).
    void update_couriers();
    void take_bearings();
    // The nearest visible enemy a pillbox's garrison can fire at through its slit.
    const Unit* find_enemy_in_slit(Unit& u, const Structure& pillbox);

    // Artillery (world_artillery.cpp). A step of setting up or packing up;
    // true once done.
    bool deploy_step(Unit& u);
    bool pack_step(Unit& u);
    void engage_indirect(Unit& u, FixedVec2 aim, std::shared_ptr<const FlowField>& path, TilePos goal,
                         const WeaponDef& weapon);
    // `wear`: HP the shot costs the gun (a tank's barrel, fired as artillery).
    void fire_indirect(Unit& u, FixedVec2 aim, const WeaponDef& weapon, int32_t wear = 0);

    // Movement. `formation` limits the speed to the group's slowest unit.
    std::shared_ptr<const FlowField> field_to(TilePos goal, MoveClass cls);
    Step navigate(Unit& u, FixedVec2 point, std::shared_ptr<const FlowField>& path, TilePos field_goal,
                  bool formation);
    Step step_towards(Unit& u, FixedVec2 point, bool formation);
    bool move_to(Unit& u, FixedVec2 next);
    bool can_stand(const Unit& u, FixedVec2 p) const;

    // A hit on the way to a unit: where it comes from decides cover and the
    // high ground bonus.
    struct Shot {
        FixedVec2 from;               // the shooter, or the burst
        uint8_t elevation = 0;        // the shooter's, for the high ground rule
        bool blast = false;           // fragments from a burst, not a bullet
        bool plunging = false;        // lobbed or a fireball: walls don't help
        int32_t damage_percent = 100;
    };
    // Percent of hits the victim's field works take for him.
    int32_t cover_percent(const Unit& victim, const Shot& shot) const;

    void move_projectiles();
    void explode(const Projectile& p, FixedVec2 at, const Unit* direct_hit);
    // A burst on open ground may leave a crater there.
    void maybe_crater(FixedVec2 at, const WeaponDef& weapon);
    void hurt(const Unit& victim, const WeaponDef& weapon, const Shot& shot);
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

    // Fog of war: a byte per tile per player (empty for players not in the game).
    std::array<std::vector<uint8_t>, kMaxPlayers> visible_;
    std::array<std::vector<uint8_t>, kMaxPlayers> explored_;
    uint32_t vision_revision_ = 0;
    bool vision_ready_ = false;
    std::vector<Fixed> corner_heights_;  // (width + 1) x (height + 1)
    // What an observer sees from a tile, by (tile, radius, eye, own building).
    // Only a cache of a pure function of the map, dropped when the map changes.
    std::map<uint64_t, std::vector<int32_t>> sight_cache_;
    uint32_t sight_cache_map_revision_ = 0;
    std::vector<Sector> sectors_;  // rebuilt with the fog
    std::map<int32_t, Tick> dig_work_;  // trench tiles being dug: tile index -> work done
    std::vector<Mine> mines_;
    uint32_t next_mine_id_ = 1;
    std::vector<Charge> charges_;
    std::array<uint32_t, kMaxPlayers> upgrades_{};  // bit per UpgradeId
    std::vector<Smoke> smokes_;
    std::vector<Bearing> bearings_;  // rebuilt with the fog
    std::vector<Courier> couriers_;  // in the order they were sent, so they arrive in it
    std::array<bool, kMaxPlayers> hungry_{};
};

}  // namespace engine
