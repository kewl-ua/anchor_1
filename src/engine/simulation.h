#pragma once

#include <map>
#include <vector>

#include "engine/command.h"
#include "engine/world.h"

namespace engine {

// Advances the World in fixed ticks and applies commands scheduled for them.
//
// Deciding WHEN a command runs (input delay, waiting for peers) is the job of
// net::Lockstep; the simulation only guarantees that commands scheduled for
// the same tick are applied in the same order on every machine.
class Simulation {
public:
    static constexpr int kTicksPerSecond = engine::kTicksPerSecond;

    Simulation(uint64_t seed, TileMap map);

    // Schedules a command for an exact future tick.
    void schedule(Tick tick, Command cmd);

    void step();
    // The commands scheduled for a tick (before it runs).
    const std::vector<Command>& scheduled(Tick tick) const;

    const World& world() const { return world_; }
    // Mutable access is for initial setup only; during the game every change
    // must go through commands, otherwise peers desync.
    World& world_for_setup() { return world_; }

private:
    World world_;
    std::map<Tick, std::vector<Command>> pending_;
};

}  // namespace engine
