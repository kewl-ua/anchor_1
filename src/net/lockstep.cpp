#include "net/lockstep.h"

#include <utility>

namespace net {

namespace {

// How far ahead of us a peer's input may legitimately be. An honest peer is
// at most about two input delays ahead (it can't pass a tick without our
// input); the margin only bounds memory against a misbehaving one.
constexpr engine::Tick kMaxTicksAhead = 256;

uint32_t player_bit(engine::PlayerId p) { return uint32_t{1} << p; }

}  // namespace

Lockstep::Lockstep(engine::Simulation& sim, engine::PlayerId local_player, int player_count,
                   Transport* transport)
    : sim_(sim),
      local_(local_player),
      player_count_(player_count),
      transport_(transport),
      input_delay_(transport ? kOnlineInputDelay : kOfflineInputDelay),
      next_seal_tick_(input_delay_) {}

void Lockstep::submit(engine::Command cmd) {
    if (replay_) return;  // (watching it played over)
    cmd.player = local_;
    if (cmd.units.size() > kMaxUnitsPerCommand) cmd.units.resize(kMaxUnitsPerCommand);
    local_buffer_.push_back(std::move(cmd));
}

bool Lockstep::try_step() {
    receive_packets();
    seal_local_input();

    // Nobody can have issued anything for the first input_delay_ ticks.
    const engine::Tick tick = sim_.world().tick();
    if (tick >= input_delay_ && !has_all_inputs(tick)) return false;

    arrived_.erase(tick);
    if (observer_) observer_(tick, sim_.scheduled(tick));
    sim_.step();

    // Every peer's checksum for tick T came with its input for T + delay,
    // which we needed to get here, so older entries are no longer needed.
    const engine::Tick now = sim_.world().tick();
    while (!local_checksums_.empty() && local_checksums_.begin()->first + input_delay_ < now) {
        local_checksums_.erase(local_checksums_.begin());
    }
    return true;
}

void Lockstep::seal_local_input() {
    // Close the local input for (current tick + delay): from now on, new
    // commands go to the next tick. Runs once per tick.
    const engine::World& world = sim_.world();
    while (next_seal_tick_ <= world.tick() + input_delay_) {
        TickInput input;
        input.tick = next_seal_tick_++;
        if (local_buffer_.size() > kMaxCommandsPerTick) local_buffer_.resize(kMaxCommandsPerTick);
        input.commands = std::exchange(local_buffer_, {});

        if (replay_) {  // the commands as they were then, whoever gave them
            if (const auto it = replay_->find(input.tick); it != replay_->end()) {
                for (const engine::Command& cmd : it->second) sim_.schedule(input.tick, cmd);
            }
            arrived_[input.tick] |= player_bit(local_);
            continue;
        }
        for (const engine::Command& cmd : input.commands) sim_.schedule(input.tick, cmd);
        arrived_[input.tick] |= player_bit(local_);

        if (transport_) {
            input.checksum_tick = world.tick();
            input.checksum = world.checksum();
            local_checksums_[input.checksum_tick] = input.checksum;
            auto [first, last] = early_remote_checksums_.equal_range(input.checksum_tick);
            for (auto it = first; it != last; ++it) compare_checksum(it->first, it->second);
            early_remote_checksums_.erase(first, last);

            transport_->broadcast(encode(input));
        }
    }
}

void Lockstep::receive_packets() {
    if (!transport_) return;

    std::vector<Packet> packets;
    transport_->receive(packets);
    for (Packet& packet : packets) {
        std::optional<TickInput> input = decode_tick_input(packet.data);
        if (!input) {
            ++rejected_packets_;
            continue;
        }
        on_tick_input(packet.from, std::move(*input));
    }
}

void Lockstep::on_tick_input(engine::PlayerId from, TickInput input) {
    const engine::Tick now = sim_.world().tick();
    const bool valid_sender = from != local_ && from < player_count_;
    // An honest peer seals tick T while standing on tick T - delay and sends
    // its checksum of that tick, so checksum_tick is fully determined.
    const bool valid_tick = input.tick >= input_delay_ && input.tick >= now &&
                            input.tick <= now + kMaxTicksAhead &&
                            input.checksum_tick == input.tick - input_delay_;
    const bool duplicate = valid_tick && (arrived_[input.tick] & player_bit(from)) != 0;
    if (!valid_sender || !valid_tick || duplicate) {
        ++rejected_packets_;
        return;
    }

    for (engine::Command& cmd : input.commands) {
        cmd.player = from;  // a peer can only ever command its own units
        sim_.schedule(input.tick, std::move(cmd));
    }
    arrived_[input.tick] |= player_bit(from);

    if (local_checksums_.contains(input.checksum_tick)) {
        compare_checksum(input.checksum_tick, input.checksum);
    } else {
        // The peer is ahead of us; compare once we get there.
        early_remote_checksums_.emplace(input.checksum_tick, input.checksum);
    }
}

void Lockstep::compare_checksum(engine::Tick tick, uint64_t remote) {
    if (local_checksums_.at(tick) != remote) report_desync(tick);
}

void Lockstep::report_desync(engine::Tick tick) {
    if (!desync_tick_ || tick < *desync_tick_) desync_tick_ = tick;
}

bool Lockstep::has_all_inputs(engine::Tick tick) const {
    if (replay_) return arrived_.contains(tick);
    const uint32_t everyone = (uint32_t{1} << player_count_) - 1;
    const auto it = arrived_.find(tick);
    return it != arrived_.end() && it->second == everyone;
}

}  // namespace net
