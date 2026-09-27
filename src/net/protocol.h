#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "engine/command.h"

namespace net {

// Bump whenever the wire format OR the simulation rules change: peers running
// different rules would desync.
inline constexpr uint16_t kProtocolVersion = 24;
inline constexpr uint16_t kDefaultPort = 7777;

// Limits that keep a malicious peer from making us allocate unbounded memory.
inline constexpr size_t kMaxCommandsPerTick = 64;
inline constexpr size_t kMaxUnitsPerCommand = 1024;

enum class MessageType : uint8_t {
    Start = 1,      // host -> client: game parameters
    TickInput = 2,  // each player -> everyone: commands for one tick
};

struct StartMessage {
    uint16_t protocol_version = kProtocolVersion;
    uint64_t seed = 0;
    uint16_t map_size = 0;  // tiles per side
    uint8_t player_count = 0;
    engine::PlayerId your_player = 0;

    bool operator==(const StartMessage&) const = default;
};

// One player's complete input for one tick. Sent every tick, even when empty:
// an empty TickInput is how a peer says "I did nothing, you may go on".
struct TickInput {
    engine::Tick tick = 0;
    // Command::player is not sent: the receiver knows who the sender is and
    // must never trust a player id claimed inside a packet.
    std::vector<engine::Command> commands;
    // The sender's World::checksum() at checksum_tick, for desync detection.
    engine::Tick checksum_tick = 0;
    uint64_t checksum = 0;
};

std::vector<uint8_t> encode(const StartMessage& msg);
std::vector<uint8_t> encode(const TickInput& msg);

std::optional<MessageType> peek_type(std::span<const uint8_t> data);

// Decoders never trust their input: truncated, oversized, trailing or
// otherwise malformed data yields nullopt.
std::optional<StartMessage> decode_start(std::span<const uint8_t> data);
std::optional<TickInput> decode_tick_input(std::span<const uint8_t> data);

}  // namespace net
