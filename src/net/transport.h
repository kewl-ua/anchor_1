#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/command.h"

namespace net {

struct Packet {
    engine::PlayerId from = 0;
    std::vector<uint8_t> data;
};

// Moves messages between players, reliably and in order per sender.
// EnetSession implements it for real games, tests use an in-memory loopback.
class Transport {
public:
    virtual ~Transport() = default;

    // Sends to every other player.
    virtual void broadcast(std::span<const uint8_t> data) = 0;
    // Appends everything received since the last call to `out`.
    virtual void receive(std::vector<Packet>& out) = 0;
};

}  // namespace net
