#pragma once

#include <cstdint>

#include "engine/terrain.h"
#include "engine/world.h"

namespace engine {

struct MapSizePreset {
    const char* name;
    int32_t tiles;
};

// Age of Empires II map sizes.
inline constexpr MapSizePreset kMapSizes[] = {
    {"tiny", 120}, {"small", 144}, {"medium", 168}, {"normal", 200}, {"large", 220}, {"giant", 240},
};
inline constexpr int32_t kDefaultMapSize = 200;
inline constexpr int32_t kMinMapSize = 32;  // anything smaller is only for tests
inline constexpr int32_t kMaxMapSize = 480;

// Square map with hills worth fighting over, point-symmetric through the
// center so neither side has a better start.
TileMap make_demo_map(int32_t size = kDefaultMapSize);

// Initial armies in opposite corners of the map. Must be identical on every
// peer, so it only uses the world's own seeded RNG.
void setup_demo_scenario(World& world);

// Where a player's army starts on a map of the given size.
FixedVec2 demo_base_position(int32_t map_size, PlayerId player);

}  // namespace engine
