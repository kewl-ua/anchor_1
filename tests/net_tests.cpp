// Network tests without sockets: an in-memory loopback stands in for ENet.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <span>
#include <utility>
#include <vector>

#include "engine/rng.h"
#include "engine/scenario.h"
#include "engine/simulation.h"
#include "net/lockstep.h"
#include "net/protocol.h"
#include "net/transport.h"

using namespace engine;
using namespace net;

namespace {

int g_failures = 0;

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// --- Test doubles ------------------------------------------------------------

// In-memory network. Each packet is delivered after a random latency (in
// frames), but never overtakes an earlier packet from the same sender, just
// like a reliable ordered ENet channel.
class LoopbackNetwork {
public:
    LoopbackNetwork(int players, uint64_t seed, uint32_t max_latency)
        : players_(players), rng_(seed), max_latency_(max_latency),
          last_delivery_(static_cast<size_t>(players * players), 0), inboxes_(static_cast<size_t>(players)) {}

    void advance_frame() { ++now_; }

    void send(PlayerId from, std::span<const uint8_t> data) {
        for (PlayerId to = 0; to < players_; ++to) {
            if (to == from) continue;
            uint64_t& last = last_delivery_[static_cast<size_t>(from * players_ + to)];
            last = std::max(last, now_ + rng_.next_below(max_latency_ + 1));
            inboxes_[to].push_back({last, Packet{from, std::vector<uint8_t>(data.begin(), data.end())}});
        }
    }

    void receive(PlayerId to, std::vector<Packet>& out) {
        std::deque<InFlight>& inbox = inboxes_[to];
        // Stable partition keeps the per-sender order intact.
        auto ready = std::stable_partition(inbox.begin(), inbox.end(),
                                           [this](const InFlight& p) { return p.deliver_at <= now_; });
        for (auto it = inbox.begin(); it != ready; ++it) out.push_back(std::move(it->packet));
        inbox.erase(inbox.begin(), ready);
    }

private:
    struct InFlight {
        uint64_t deliver_at;
        Packet packet;
    };

    int players_;
    Rng rng_;
    uint32_t max_latency_;
    uint64_t now_ = 0;
    std::vector<uint64_t> last_delivery_;
    std::vector<std::deque<InFlight>> inboxes_;
};

class LoopbackTransport : public Transport {
public:
    LoopbackTransport(LoopbackNetwork& network, PlayerId self) : network_(network), self_(self) {}
    void broadcast(std::span<const uint8_t> data) override { network_.send(self_, data); }
    void receive(std::vector<Packet>& out) override { network_.receive(self_, out); }

private:
    LoopbackNetwork& network_;
    PlayerId self_;
};

// Lets a test play the remote side by hand.
class ManualTransport : public Transport {
public:
    void broadcast(std::span<const uint8_t> data) override { sent.emplace_back(data.begin(), data.end()); }
    void receive(std::vector<Packet>& out) override {
        for (Packet& p : incoming) out.push_back(std::move(p));
        incoming.clear();
    }

    std::vector<std::vector<uint8_t>> sent;
    std::vector<Packet> incoming;
};

// One player's machine: its own simulation driven by its own lockstep.
struct Peer {
    Peer(uint64_t seed, PlayerId id, Transport& transport)
        : sim(seed, make_demo_map()), lockstep(sim, id, 2, &transport) {
        setup_demo_scenario(sim.world_for_setup());
    }
    Peer(const Peer&) = delete;  // lockstep refers to sim

    Simulation sim;
    Lockstep lockstep;
    std::vector<uint64_t> checksums;  // after every tick
};

std::vector<EntityId> units_of(const World& world, PlayerId player) {
    std::vector<EntityId> ids;
    for (const Unit& u : world.units()) {
        if (u.owner == player) ids.push_back(u.id);
    }
    return ids;
}

Command make_move(std::vector<EntityId> units, int32_t x, int32_t y) {
    Command cmd;
    cmd.type = CommandType::Move;
    cmd.units = std::move(units);
    cmd.target = {Fixed::from_int(x), Fixed::from_int(y)};
    return cmd;
}

bool same_command(const Command& a, const Command& b) {
    return a.type == b.type && a.units == b.units && a.target == b.target && a.target_unit == b.target_unit &&
           a.unit_type == b.unit_type && a.structure_type == b.structure_type && a.ability == b.ability &&
           a.target_end == b.target_end && a.upgrade == b.upgrade && a.cargo == b.cargo;
}

// --- Protocol ----------------------------------------------------------------

TickInput sample_input() {
    TickInput input;
    input.tick = 1234;
    input.checksum_tick = 1231;
    input.checksum = 0xDEADBEEFCAFEF00DULL;
    input.commands.push_back(make_move({1, 2, 3}, -50, 700));
    Command stop;
    stop.type = CommandType::Stop;
    stop.units = {7};
    input.commands.push_back(stop);
    Command attack;
    attack.type = CommandType::Attack;
    attack.units = {4, 5};
    attack.target_unit = 99;
    input.commands.push_back(attack);
    Command attack_move = make_move({8}, 30, 31);
    attack_move.type = CommandType::AttackMove;
    input.commands.push_back(attack_move);
    Command fire = make_move({9, 10}, 12, 13);
    fire.type = CommandType::AttackGround;
    input.commands.push_back(fire);
    Command move_in;
    move_in.type = CommandType::Garrison;
    move_in.units = {11};
    move_in.target_unit = 77;
    input.commands.push_back(move_in);
    Command gather = make_move({12, 13}, 40, 41);
    gather.type = CommandType::Gather;
    input.commands.push_back(gather);
    Command train;
    train.type = CommandType::Train;
    train.target_unit = 88;
    train.unit_type = 5;
    input.commands.push_back(train);
    Command retrain;
    retrain.type = CommandType::Retrain;
    retrain.units = {14, 15};
    input.commands.push_back(retrain);
    Command build = make_move({16}, 50, 51);
    build.type = CommandType::Build;
    build.target_unit = 99;
    build.structure_type = 3;
    input.commands.push_back(build);
    Command haul;
    haul.type = CommandType::Haul;
    haul.units = {17, 18};
    haul.target_unit = 42;
    haul.cargo = 3;
    input.commands.push_back(haul);
    Command supply;
    supply.type = CommandType::Supply;
    supply.units = {22};
    supply.target_unit = 23;
    input.commands.push_back(supply);
    Command collect = make_move({24}, 70, 71);
    collect.type = CommandType::Collect;
    input.commands.push_back(collect);
    Command rally = make_move({}, 80, 81);
    rally.type = CommandType::Rally;
    rally.target_unit = 25;
    input.commands.push_back(rally);
    Command shell = make_move({26, 27}, 0, 0);
    shell.type = CommandType::LoadShell;
    shell.ability = 2;
    input.commands.push_back(shell);
    Command post = make_move({19}, 60, 61);
    post.type = CommandType::Observe;
    input.commands.push_back(post);
    Command skill = make_move({20, 21}, 62, 63);
    skill.type = CommandType::Ability;
    skill.ability = 2;
    skill.target_end = {engine::Fixed::from_int(70), engine::Fixed::from_int(-3)};
    input.commands.push_back(skill);
    return input;
}

void test_protocol_round_trip() {
    const TickInput input = sample_input();
    const std::optional<TickInput> decoded = decode_tick_input(encode(input));
    CHECK(decoded.has_value());
    if (decoded) {
        CHECK(decoded->tick == input.tick);
        CHECK(decoded->checksum_tick == input.checksum_tick);
        CHECK(decoded->checksum == input.checksum);
        CHECK(decoded->commands.size() == input.commands.size());
        if (decoded->commands.size() == input.commands.size()) {
            for (size_t i = 0; i < input.commands.size(); ++i) {
                CHECK(same_command(decoded->commands[i], input.commands[i]));
            }
        }
    }

    StartMessage start;
    start.seed = 0x0123456789ABCDEFULL;
    start.map_size = 168;
    start.player_count = 2;
    start.your_player = 1;
    CHECK(decode_start(encode(start)) == start);

    StartMessage tiny = start;
    tiny.map_size = 5;  // nonsense from a broken or hostile host
    CHECK(!decode_start(encode(tiny)));
    CHECK(peek_type(encode(start)) == MessageType::Start);
    CHECK(peek_type(encode(input)) == MessageType::TickInput);
}

void test_protocol_rejects_garbage() {
    const std::vector<uint8_t> good = encode(sample_input());

    bool all_prefixes_rejected = true;
    for (size_t len = 0; len < good.size(); ++len) {
        all_prefixes_rejected = all_prefixes_rejected &&
                                !decode_tick_input(std::span(good).first(len)).has_value();
    }
    CHECK(all_prefixes_rejected);

    std::vector<uint8_t> trailing = good;
    trailing.push_back(0);
    CHECK(!decode_tick_input(trailing));

    std::vector<uint8_t> bad_type = good;
    bad_type[19] = 99;  // first command's type byte (1 + 4 + 4 + 8 + 2 = 19)
    CHECK(!decode_tick_input(bad_type));

    std::vector<uint8_t> huge_count = good;
    huge_count[17] = 0xFF;  // command count, little-endian u16
    huge_count[18] = 0xFF;
    CHECK(!decode_tick_input(huge_count));

    CHECK(!decode_start(good));
    CHECK(!peek_type(std::vector<uint8_t>{42}));
}

// --- Lockstep ----------------------------------------------------------------

// Two peers over a laggy network, each playing its own scripted orders at its
// own pace. They must end up bit-identical on every single tick.
void test_lockstep_peers_stay_in_sync() {
    LoopbackNetwork network(2, 99, /*max_latency=*/4);
    LoopbackTransport t0(network, 0);
    LoopbackTransport t1(network, 1);
    Peer peers[2] = {Peer(555, 0, t0), Peer(555, 1, t1)};
    Rng frame_rng(7);

    constexpr Tick kTicks = 600;
    for (int frame = 0; frame < 50000; ++frame) {
        network.advance_frame();
        for (int p = 0; p < 2; ++p) {
            Peer& peer = peers[p];
            const auto player = static_cast<PlayerId>(p);
            // Uneven frame rates: 0..2 ticks per frame.
            const uint32_t steps = frame_rng.next_below(3);
            for (uint32_t s = 0; s < steps && peer.sim.world().tick() < kTicks; ++s) {
                if (!peer.lockstep.try_step()) break;
                const Tick tick = peer.sim.world().tick();
                peer.checksums.push_back(peer.sim.world().checksum());
                if (tick % 25 == static_cast<Tick>(p) * 10) {
                    peer.lockstep.submit(make_move(units_of(peer.sim.world(), player),
                                                   5 + static_cast<int32_t>((tick * 7) % 54),
                                                   5 + static_cast<int32_t>((tick * 11) % 54)));
                }
            }
        }
        if (peers[0].sim.world().tick() >= kTicks && peers[1].sim.world().tick() >= kTicks) break;
    }

    CHECK(peers[0].sim.world().tick() == kTicks);
    CHECK(peers[1].sim.world().tick() == kTicks);
    CHECK(peers[0].checksums == peers[1].checksums);
    CHECK(!peers[0].lockstep.desync_tick());
    CHECK(!peers[1].lockstep.desync_tick());
    CHECK(peers[0].lockstep.rejected_packets() == 0);
    CHECK(peers[1].lockstep.rejected_packets() == 0);

    // The orders must actually have done something.
    Simulation no_orders(555, make_demo_map());
    setup_demo_scenario(no_orders.world_for_setup());
    for (Tick t = 0; t < kTicks; ++t) no_orders.step();
    CHECK(peers[0].sim.world().checksum() != no_orders.world().checksum());
}

// Without the opponent's input, a peer must wait instead of guessing.
void test_lockstep_waits_for_missing_input() {
    ManualTransport transport;
    Peer peer(1, 0, transport);

    int steps = 0;
    while (steps < 100 && peer.lockstep.try_step()) ++steps;
    CHECK(steps == static_cast<int>(Lockstep::kOnlineInputDelay));
    CHECK(!peer.lockstep.try_step());
}

// Peers that started from different states must notice it.
void test_lockstep_detects_desync() {
    LoopbackNetwork network(2, 3, 2);
    LoopbackTransport t0(network, 0);
    LoopbackTransport t1(network, 1);
    Peer a(42, 0, t0);
    Peer b(42, 1, t1);
    b.sim.world_for_setup().spawn_unit(1, UnitTypeId::Rifleman, {Fixed::from_int(32), Fixed::from_int(32)});  // a "bug"

    for (int frame = 0; frame < 1000; ++frame) {
        network.advance_frame();
        a.lockstep.try_step();
        b.lockstep.try_step();
    }
    CHECK(a.lockstep.desync_tick() == Tick{0});
    CHECK(b.lockstep.desync_tick() == Tick{0});
}

// The remote side is played by hand to send forged and broken packets.
void test_lockstep_rejects_forged_input() {
    ManualTransport transport;
    Peer peer(8, 0, transport);

    Simulation reference(8, make_demo_map());
    setup_demo_scenario(reference.world_for_setup());

    const Tick delay = Lockstep::kOnlineInputDelay;
    for (Tick t = 0; t < 100; ++t) {
        // Honest-looking input for tick t + delay, with the right checksum...
        TickInput input;
        input.tick = t + delay;
        input.checksum_tick = t;
        input.checksum = peer.sim.world().checksum();
        // ...except that player 2 tries to order player 1's army around.
        if (t == 0) input.commands.push_back(make_move(units_of(peer.sim.world(), 0), 5, 5));
        transport.incoming.push_back({1, encode(input)});

        if (t == 10) {
            transport.incoming.push_back({1, {1, 2, 3}});             // garbage
            transport.incoming.push_back({1, encode(input)});         // duplicate
            TickInput past = input;
            past.tick = t - 1;                                        // already happened
            past.checksum_tick = t - 1 - delay;
            transport.incoming.push_back({1, encode(past)});
            TickInput lying = input;
            lying.tick = t + delay + 1;
            lying.checksum_tick = t;                                  // impossible checksum tick
            transport.incoming.push_back({1, encode(lying)});
            transport.incoming.push_back({0, encode(input)});         // pretends to be us
        }

        CHECK(peer.lockstep.try_step());
        reference.step();
    }

    // The forged order was ignored, so the game equals one without any orders.
    CHECK(peer.sim.world().checksum() == reference.world().checksum());
    CHECK(peer.lockstep.rejected_packets() == 5);
    CHECK(!peer.lockstep.desync_tick());
}

void test_offline_lockstep_runs_alone() {
    Simulation sim(5, make_demo_map());
    setup_demo_scenario(sim.world_for_setup());
    Lockstep lockstep(sim, 0, 1, nullptr);
    CHECK(lockstep.input_delay() == Lockstep::kOfflineInputDelay);

    lockstep.submit(make_move(units_of(sim.world(), 0), 32, 32));
    bool all_steps_ran = true;
    for (int i = 0; i < 50; ++i) all_steps_ran = all_steps_ran && lockstep.try_step();
    CHECK(all_steps_ran);

    bool someone_moving = false;
    for (const Unit& u : sim.world().units()) someone_moving = someone_moving || (u.owner == 0 && u.moving);
    CHECK(someone_moving);
}

}  // namespace

int main() {
    test_protocol_round_trip();
    test_protocol_rejects_garbage();
    test_lockstep_peers_stay_in_sync();
    test_lockstep_waits_for_missing_input();
    test_lockstep_detects_desync();
    test_lockstep_rejects_forged_input();
    test_offline_lockstep_runs_alone();

    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("All net tests passed\n");
    return 0;
}
