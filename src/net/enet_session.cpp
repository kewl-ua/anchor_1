#include "net/enet_session.h"

#include <iterator>
#include <utility>

#include <enet/enet.h>

#include "net/protocol.h"

namespace net {

namespace {

constexpr engine::PlayerId kHostPlayer = 0;
constexpr engine::PlayerId kClientPlayer = 1;
constexpr uint32_t kDisconnectWaitMs = 300;
// ENet's default gives up after 30 s of silence; far too long to stare at
// "Connecting..." or at a frozen game.
constexpr uint32_t kPeerTimeoutMinMs = 3000;
constexpr uint32_t kPeerTimeoutMaxMs = 10000;

void set_peer_timeout(ENetPeer* peer) { enet_peer_timeout(peer, 0, kPeerTimeoutMinMs, kPeerTimeoutMaxMs); }

// enet_initialize() is global; keep it alive while any session exists.
int g_enet_users = 0;

bool acquire_enet() {
    if (g_enet_users == 0 && enet_initialize() != 0) return false;
    ++g_enet_users;
    return true;
}

void release_enet() {
    if (--g_enet_users == 0) enet_deinitialize();
}

}  // namespace

struct EnetSession::Impl {
    bool enet_acquired = false;
    bool is_host = false;
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;

    State state = State::Connecting;
    std::string status;
    uint64_t seed = 0;
    int32_t map_size = 0;
    engine::PlayerId local_player = 0;
    engine::PlayerId remote_player = 0;
    std::vector<Packet> inbox;

    void fail(std::string why) {
        state = State::Failed;
        status = std::move(why);
    }

    void send(std::span<const uint8_t> data) {
        if (!peer) return;
        ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer, 0, packet);
        enet_host_flush(host);  // don't wait for the next update() to hit the wire
    }

    void on_connect(ENetPeer* p) {
        if (!is_host) {
            status = "Connected, waiting for the host to start...";
            return;
        }
        peer = p;
        set_peer_timeout(peer);
        StartMessage start;
        start.seed = seed;
        start.map_size = static_cast<uint16_t>(map_size);
        start.player_count = 2;
        start.your_player = remote_player;
        send(encode(start));
        state = State::Ready;
        status = "Opponent connected";
    }

    void on_receive(std::span<const uint8_t> data) {
        if (state == State::Ready) {
            inbox.push_back({remote_player, std::vector<uint8_t>(data.begin(), data.end())});
            return;
        }
        if (is_host || state != State::Connecting) return;

        const std::optional<StartMessage> start = decode_start(data);
        if (!start || start->player_count != 2 || start->your_player != kClientPlayer) {
            fail("Bad handshake from the host");
        } else if (start->protocol_version != kProtocolVersion) {
            fail("Version mismatch: host has protocol v" + std::to_string(start->protocol_version) +
                 ", you have v" + std::to_string(kProtocolVersion));
        } else {
            seed = start->seed;
            map_size = start->map_size;
            state = State::Ready;
            status = "Connected";
        }
        if (state == State::Failed) enet_peer_disconnect(peer, 0);
    }

    void on_disconnect(ENetPeer* p) {
        if (p != peer) return;
        peer = nullptr;
        if (state == State::Ready) {
            state = State::Disconnected;
            status = "Opponent disconnected";
        } else if (state == State::Connecting) {
            fail(is_host ? "Opponent left before the game started" : "Could not connect to the host");
        }
    }
};

EnetSession::EnetSession() : impl_(std::make_unique<Impl>()) {}

std::unique_ptr<EnetSession> EnetSession::host(uint16_t port, uint64_t seed, int32_t map_size) {
    std::unique_ptr<EnetSession> session(new EnetSession());
    Impl& m = *session->impl_;
    m.is_host = true;
    m.seed = seed;
    m.map_size = map_size;
    m.local_player = kHostPlayer;
    m.remote_player = kClientPlayer;

    m.enet_acquired = acquire_enet();
    if (!m.enet_acquired) {
        m.fail("Failed to initialize networking");
        return session;
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    m.host = enet_host_create(&address, 1, 1, 0, 0);
    if (!m.host) {
        m.fail("Can't listen on port " + std::to_string(port) + " (already in use?)");
        return session;
    }
    m.status = "Hosting on port " + std::to_string(port) + ", waiting for an opponent...";
    return session;
}

std::unique_ptr<EnetSession> EnetSession::join(const std::string& address, uint16_t port) {
    std::unique_ptr<EnetSession> session(new EnetSession());
    Impl& m = *session->impl_;
    m.local_player = kClientPlayer;
    m.remote_player = kHostPlayer;

    m.enet_acquired = acquire_enet();
    if (!m.enet_acquired) {
        m.fail("Failed to initialize networking");
        return session;
    }

    m.host = enet_host_create(nullptr, 1, 1, 0, 0);
    if (!m.host) {
        m.fail("Failed to create a network socket");
        return session;
    }

    ENetAddress target{};
    if (enet_address_set_host(&target, address.c_str()) != 0) {
        m.fail("Unknown host: " + address);
        return session;
    }
    target.port = port;
    m.peer = enet_host_connect(m.host, &target, 1, 0);
    if (!m.peer) {
        m.fail("Failed to start connecting");
        return session;
    }
    set_peer_timeout(m.peer);
    m.status = "Connecting to " + address + ":" + std::to_string(port) + "...";
    return session;
}

EnetSession::~EnetSession() {
    Impl& m = *impl_;
    if (m.host && m.peer) {
        // Polite disconnect, so the opponent learns about it right away
        // instead of after a timeout.
        enet_peer_disconnect(m.peer, 0);
        ENetEvent event;
        bool done = false;
        while (!done && enet_host_service(m.host, &event, kDisconnectWaitMs) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
            if (event.type == ENET_EVENT_TYPE_DISCONNECT) done = true;
        }
        if (!done) enet_peer_reset(m.peer);
    }
    if (m.host) enet_host_destroy(m.host);
    if (m.enet_acquired) release_enet();
}

void EnetSession::update() {
    Impl& m = *impl_;
    if (!m.host) return;

    ENetEvent event;
    while (enet_host_service(m.host, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                m.on_connect(event.peer);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                if (event.peer == m.peer) m.on_receive({event.packet->data, event.packet->dataLength});
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                m.on_disconnect(event.peer);
                break;
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }
}

EnetSession::State EnetSession::state() const { return impl_->state; }
const std::string& EnetSession::status() const { return impl_->status; }
uint64_t EnetSession::seed() const { return impl_->seed; }
int32_t EnetSession::map_size() const { return impl_->map_size; }
engine::PlayerId EnetSession::local_player() const { return impl_->local_player; }

int EnetSession::ping_ms() const {
    return impl_->peer && impl_->state == State::Ready ? static_cast<int>(impl_->peer->roundTripTime) : -1;
}

void EnetSession::broadcast(std::span<const uint8_t> data) { impl_->send(data); }

void EnetSession::receive(std::vector<Packet>& out) {
    out.insert(out.end(), std::make_move_iterator(impl_->inbox.begin()),
               std::make_move_iterator(impl_->inbox.end()));
    impl_->inbox.clear();
}

}  // namespace net
