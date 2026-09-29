// `--debug`: a log of the session and a replay of it, to find out what went
// wrong. Both go to logs/ beside the game: anchor_<date>_<time>.log (the
// session, every command with its tick, the checksum now and then, what the
// frame was doing) and .replay (every command, to play the game over with
// `--replay <file>`). A crash, with or without --debug, leaves a report
// there: where it happened, the call stack with the source lines, and a
// minidump.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "engine/command.h"
#include "engine/world.h"

namespace app::debug {

// The crash report armed (always); with `log`, the log and the replay opened.
void start(int argc, char** argv, bool log);
bool logging();

// A line in the log (printf-style); nothing without --debug.
void logf(const char* fmt, ...);
// The game's start: what a replay needs to set it up again.
void session(uint64_t seed, int32_t map_size, engine::PlayerId local_player, int player_count, const char* scenario);
// A tick about to run and the commands it applies (every player's).
void step(const engine::World& world, const std::vector<engine::Command>& commands);
// What the frame is doing now, for the crash report (a string literal).
void phase(const char* what);
// What a command is, in words.
std::string describe(const engine::Command& cmd);

struct Replay {
    uint64_t seed = 0;
    int32_t map_size = 0;
    engine::PlayerId local_player = 0;
    int player_count = 1;
    std::string scenario = "demo";  // engine::scenario_name()
    std::map<engine::Tick, std::vector<engine::Command>> commands;
    std::map<engine::Tick, uint64_t> checksums;  // as it went then: to see it goes the same
};
std::optional<Replay> load_replay(const std::string& path);

}  // namespace app::debug
