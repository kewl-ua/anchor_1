#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "net/transport.h"

namespace net {

// A two-player ENet connection: one side hosts, the other joins. Runs the
// handshake (StartMessage) and then serves as the game's Transport.
class EnetSession final : public Transport {
public:
    enum class State {
        Connecting,    // host: waiting for an opponent; client: waiting for Start
        Ready,         // the game can start
        Failed,        // could not set up the game
        Disconnected,  // the opponent left mid-game
    };

    static std::unique_ptr<EnetSession> host(uint16_t port, uint64_t seed, int32_t map_size);
    static std::unique_ptr<EnetSession> join(const std::string& address, uint16_t port);
    ~EnetSession() override;

    EnetSession(const EnetSession&) = delete;
    EnetSession& operator=(const EnetSession&) = delete;

    // Pumps the network; call once per frame.
    void update();

    State state() const;
    // Human-readable status for the lobby screen and HUD.
    const std::string& status() const;
    // Valid once Ready; the host decides both.
    uint64_t seed() const;
    int32_t map_size() const;
    engine::PlayerId local_player() const;
    int player_count() const { return 2; }
    // Round-trip time to the opponent, or -1 if not connected.
    int ping_ms() const;

    void broadcast(std::span<const uint8_t> data) override;
    void receive(std::vector<Packet>& out) override;

private:
    // Keeps enet.h out of this header: it drags in windows.h, which clashes
    // with raylib (CloseWindow, DrawText, Rectangle...).
    struct Impl;

    EnetSession();

    std::unique_ptr<Impl> impl_;
};

}  // namespace net
