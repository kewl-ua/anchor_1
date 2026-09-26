#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "engine/simulation.h"
#include "net/protocol.h"
#include "net/transport.h"

namespace net {

// Deterministic lockstep.
//
// Every player sends its input for tick T (a TickInput, possibly empty) and
// nobody runs tick T before it has everyone's input for it. Local commands
// are scheduled input_delay() ticks ahead, which gives them time to reach the
// other peers, so normally nobody has to wait.
//
// Offline play uses the same class with one player and no transport, so
// single-player and multiplayer run exactly the same code path.
class Lockstep {
public:
    static constexpr engine::Tick kOfflineInputDelay = 1;  // 50 ms
    static constexpr engine::Tick kOnlineInputDelay = 3;   // 150 ms, covers ~150 ms one-way latency

    // `transport` may be null for offline play; otherwise it must outlive this object.
    Lockstep(engine::Simulation& sim, engine::PlayerId local_player, int player_count, Transport* transport);

    // Queues a command from the local player; it runs input_delay() ticks later.
    void submit(engine::Command cmd);

    // Handles incoming packets, then runs one tick if every player's input for
    // it has arrived. Returns false while waiting for another player.
    bool try_step();

    engine::PlayerId local_player() const { return local_; }
    engine::Tick input_delay() const { return input_delay_; }
    // First tick at which another peer's state differed from ours.
    std::optional<engine::Tick> desync_tick() const { return desync_tick_; }
    // Packets dropped as malformed or impossible (bugs or cheating).
    int rejected_packets() const { return rejected_packets_; }

private:
    void receive_packets();
    void on_tick_input(engine::PlayerId from, TickInput input);
    void seal_local_input();
    void compare_checksum(engine::Tick tick, uint64_t remote);
    void report_desync(engine::Tick tick);
    bool has_all_inputs(engine::Tick tick) const;

    engine::Simulation& sim_;
    const engine::PlayerId local_;
    const int player_count_;
    Transport* const transport_;
    const engine::Tick input_delay_;

    std::vector<engine::Command> local_buffer_;
    engine::Tick next_seal_tick_;
    std::map<engine::Tick, uint32_t> arrived_;  // tick -> bitmask of players whose input is in

    // Our checksums, kept until every peer's checksum for that tick arrived.
    std::map<engine::Tick, uint64_t> local_checksums_;
    // Peer checksums that arrived before we reached that tick ourselves.
    std::multimap<engine::Tick, uint64_t> early_remote_checksums_;
    std::optional<engine::Tick> desync_tick_;
    int rejected_packets_ = 0;
};

}  // namespace net
