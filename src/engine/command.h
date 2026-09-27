#pragma once

#include <cstdint>
#include <vector>

#include "engine/fixed.h"

namespace engine {

using EntityId = uint32_t;  // 0 is never a valid id
using PlayerId = uint8_t;
using Tick = uint32_t;

inline constexpr int kTicksPerSecond = 20;

enum class CommandType : uint8_t {
    Move,          // go there, ignore enemies
    Stop,
    Attack,        // attack one specific unit
    AttackMove,    // go there, fighting every enemy met on the way
    AttackGround,  // keep firing at a point, seen or not (suppress a tree line)
    Garrison,      // infantry: go into a house (target_unit = the structure)
    Gather,        // rear troops: cut timber / quarry stone at `target`
    Train,         // a building (target_unit) hires a unit (unit_type)
    Retrain,       // rear troops: go to the headquarters and come out as riflemen
    Build,         // rear troops: put up a building (structure_type at target) or help
                   // finish one (target_unit)
    Haul,          // supply trucks: run between the station and the depots; a depot
                   // (target_unit) or a `cargo` assigns them
    Observe,       // scouts: hold an observation post watching the sector towards `target`
    Ability,       // use a skill (`ability`) at `target` (a line: to `target_end`)
    Upgrade,       // a foxhole (target_unit) is dug out into a dugout
    Unload,        // everyone inside a house or dugout (target_unit) comes out
    Research,      // a building (target_unit) researches an upgrade (`upgrade`)
    Supply,        // tankers, ammunition trucks: look after one unit (target_unit) for good
};

// The only way anything outside the engine can change the game state.
// In multiplayer these are exactly what travels over the network.
struct Command {
    CommandType type = CommandType::Stop;
    PlayerId player = 0;
    std::vector<EntityId> units;
    FixedVec2 target{};          // Move, AttackMove, AttackGround, Gather; Build: the top-left tile
    EntityId target_unit = 0;    // Attack: a unit; Garrison, Train, Build: a structure
    uint8_t unit_type = 0;       // Train: a UnitTypeId
    uint8_t structure_type = 0;  // Build: a StructureType
    uint8_t ability = 0;         // Ability: an AbilityId
    FixedVec2 target_end{};      // Ability along a line: its other end
    uint8_t upgrade = 0;         // Research: an UpgradeId
    uint8_t cargo = 0;           // Haul: kHaulKeep, kHaulAuto or haul_code(Resource) (economy.h)
};

}  // namespace engine
