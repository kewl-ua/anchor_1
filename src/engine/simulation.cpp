#include "engine/simulation.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace engine {

Simulation::Simulation(uint64_t seed, TileMap map) : world_(seed, std::move(map)) {}

void Simulation::schedule(Tick tick, Command cmd) {
    // A command for a tick that already ran means a peer fell out of sync.
    assert(tick >= world_.tick());
    pending_[tick].push_back(std::move(cmd));
}

void Simulation::step() {
    if (auto it = pending_.find(world_.tick()); it != pending_.end()) {
        std::vector<Command>& commands = it->second;
        // Peers may receive commands in different orders; sorting by player
        // makes the apply order identical everywhere. stable_sort keeps each
        // player's own commands in the order they were issued.
        std::stable_sort(commands.begin(), commands.end(),
                         [](const Command& a, const Command& b) { return a.player < b.player; });
        for (const Command& cmd : commands) world_.apply(cmd);
        pending_.erase(it);
    }
    world_.step();
}

}  // namespace engine
