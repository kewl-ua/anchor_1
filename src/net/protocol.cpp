#include "net/protocol.h"

#include <utility>

#include "engine/scenario.h"

namespace net {

namespace {

// Little-endian, byte by byte: identical on every platform.
class Writer {
public:
    void u8(uint8_t v) { out_.push_back(v); }
    void u16(uint16_t v) { put(v, 2); }
    void u32(uint32_t v) { put(v, 4); }
    void u64(uint64_t v) { put(v, 8); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }

    std::vector<uint8_t> take() { return std::move(out_); }

private:
    void put(uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    std::vector<uint8_t> out_;
};

// Reading past the end doesn't throw: it returns zeros and marks the reader
// as failed, so decoders just check ok() once at the end.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    uint8_t u8() { return static_cast<uint8_t>(get(1)); }
    uint16_t u16() { return static_cast<uint16_t>(get(2)); }
    uint32_t u32() { return static_cast<uint32_t>(get(4)); }
    uint64_t u64() { return get(8); }
    int32_t i32() { return static_cast<int32_t>(u32()); }

    bool ok() const { return ok_; }
    bool finished() const { return ok_ && pos_ == data_.size(); }
    void fail() { ok_ = false; }

private:
    uint64_t get(size_t bytes) {
        if (!ok_ || data_.size() - pos_ < bytes) {
            ok_ = false;
            return 0;
        }
        uint64_t v = 0;
        for (size_t i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
        pos_ += bytes;
        return v;
    }

    std::span<const uint8_t> data_;
    size_t pos_ = 0;
    bool ok_ = true;
};

bool has_target_point(engine::CommandType type) {
    return type == engine::CommandType::Move || type == engine::CommandType::AttackMove ||
           type == engine::CommandType::AttackGround || type == engine::CommandType::Gather ||
           type == engine::CommandType::Build || type == engine::CommandType::Observe ||
           type == engine::CommandType::Ability || type == engine::CommandType::Collect ||
           type == engine::CommandType::Rally;
}

bool has_target_unit(engine::CommandType type) {
    return type == engine::CommandType::Attack || type == engine::CommandType::Garrison ||
           type == engine::CommandType::Train || type == engine::CommandType::Build ||
           type == engine::CommandType::Upgrade || type == engine::CommandType::Unload ||
           type == engine::CommandType::Research || type == engine::CommandType::Haul ||
           type == engine::CommandType::Supply || type == engine::CommandType::Rally;
}

void write_command(Writer& w, const engine::Command& cmd) {
    w.u8(static_cast<uint8_t>(cmd.type));
    w.u16(static_cast<uint16_t>(cmd.units.size()));
    for (engine::EntityId id : cmd.units) w.u32(id);
    if (has_target_point(cmd.type)) {
        w.i32(cmd.target.x.raw);
        w.i32(cmd.target.y.raw);
    }
    if (has_target_unit(cmd.type)) w.u32(cmd.target_unit);
    if (cmd.type == engine::CommandType::Train) w.u8(cmd.unit_type);
    if (cmd.type == engine::CommandType::Build) w.u8(cmd.structure_type);
    if (cmd.type == engine::CommandType::Research) w.u8(cmd.upgrade);
    if (cmd.type == engine::CommandType::Haul) w.u8(cmd.cargo);
    if (cmd.type == engine::CommandType::Ability) {
        w.u8(cmd.ability);
        w.i32(cmd.target_end.x.raw);
        w.i32(cmd.target_end.y.raw);
    }
}

std::optional<engine::Command> read_command(Reader& r) {
    engine::Command cmd;
    const uint8_t type = r.u8();
    if (type > static_cast<uint8_t>(engine::CommandType::Rally)) return std::nullopt;
    cmd.type = static_cast<engine::CommandType>(type);

    const uint16_t count = r.u16();
    if (!r.ok() || count > kMaxUnitsPerCommand) return std::nullopt;
    cmd.units.resize(count);
    for (engine::EntityId& id : cmd.units) id = r.u32();

    if (has_target_point(cmd.type)) {
        cmd.target.x = engine::Fixed::from_raw(r.i32());
        cmd.target.y = engine::Fixed::from_raw(r.i32());
    }
    if (has_target_unit(cmd.type)) cmd.target_unit = r.u32();
    if (cmd.type == engine::CommandType::Train) cmd.unit_type = r.u8();
    if (cmd.type == engine::CommandType::Build) cmd.structure_type = r.u8();
    if (cmd.type == engine::CommandType::Research) cmd.upgrade = r.u8();
    if (cmd.type == engine::CommandType::Haul) cmd.cargo = r.u8();
    if (cmd.type == engine::CommandType::Ability) {
        cmd.ability = r.u8();
        cmd.target_end.x = engine::Fixed::from_raw(r.i32());
        cmd.target_end.y = engine::Fixed::from_raw(r.i32());
    }
    if (!r.ok()) return std::nullopt;
    return cmd;
}

}  // namespace

std::vector<uint8_t> encode(const StartMessage& msg) {
    Writer w;
    w.u8(static_cast<uint8_t>(MessageType::Start));
    w.u16(msg.protocol_version);
    w.u64(msg.seed);
    w.u16(msg.map_size);
    w.u8(msg.player_count);
    w.u8(msg.your_player);
    return w.take();
}

std::vector<uint8_t> encode(const TickInput& msg) {
    Writer w;
    w.u8(static_cast<uint8_t>(MessageType::TickInput));
    w.u32(msg.tick);
    w.u32(msg.checksum_tick);
    w.u64(msg.checksum);
    w.u16(static_cast<uint16_t>(msg.commands.size()));
    for (const engine::Command& cmd : msg.commands) write_command(w, cmd);
    return w.take();
}

std::optional<MessageType> peek_type(std::span<const uint8_t> data) {
    if (data.empty()) return std::nullopt;
    switch (data[0]) {
        case static_cast<uint8_t>(MessageType::Start): return MessageType::Start;
        case static_cast<uint8_t>(MessageType::TickInput): return MessageType::TickInput;
        default: return std::nullopt;
    }
}

std::optional<StartMessage> decode_start(std::span<const uint8_t> data) {
    Reader r(data);
    if (r.u8() != static_cast<uint8_t>(MessageType::Start)) return std::nullopt;
    StartMessage msg;
    msg.protocol_version = r.u16();
    msg.seed = r.u64();
    msg.map_size = r.u16();
    msg.player_count = r.u8();
    msg.your_player = r.u8();
    if (!r.finished()) return std::nullopt;
    if (msg.map_size < engine::kMinMapSize || msg.map_size > engine::kMaxMapSize) return std::nullopt;
    return msg;
}

std::optional<TickInput> decode_tick_input(std::span<const uint8_t> data) {
    Reader r(data);
    if (r.u8() != static_cast<uint8_t>(MessageType::TickInput)) return std::nullopt;
    TickInput msg;
    msg.tick = r.u32();
    msg.checksum_tick = r.u32();
    msg.checksum = r.u64();

    const uint16_t count = r.u16();
    if (!r.ok() || count > kMaxCommandsPerTick) return std::nullopt;
    msg.commands.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        std::optional<engine::Command> cmd = read_command(r);
        if (!cmd) return std::nullopt;
        msg.commands.push_back(std::move(*cmd));
    }
    if (!r.finished()) return std::nullopt;
    return msg;
}

}  // namespace net
