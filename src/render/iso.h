#pragma once

#include <cmath>
#include <vector>

#include <raylib.h>

#include "engine/terrain.h"

// Isometric projection. The engine works on a flat grid of tiles ("ground"
// coordinates, 1.0 = one tile); only the renderer knows it is drawn as
// diamonds with hills.
//
//   ground (0,0) is the top corner of the map, +x runs down-right, +y down-left.
namespace render::iso {

inline constexpr float kTileWidth = 64.0f;       // pixels, diamond width
inline constexpr float kTileHeight = 32.0f;      // pixels, diamond height
inline constexpr float kElevationStep = 16.0f;   // pixels per elevation level

// Ground point at a given terrain height -> iso pixel.
inline Vector2 project(Vector2 ground, float height) {
    return {(ground.x - ground.y) * kTileWidth * 0.5f,
            (ground.x + ground.y) * kTileHeight * 0.5f - height * kElevationStep};
}

// Inverse of project() for a known height.
inline Vector2 unproject(Vector2 iso_px, float height) {
    const float diff = iso_px.x / (kTileWidth * 0.5f);                                // x - y
    const float sum = (iso_px.y + height * kElevationStep) / (kTileHeight * 0.5f);   // x + y
    return {(sum + diff) * 0.5f, (sum - diff) * 0.5f};
}

// Tiles have whole elevation levels; for drawing, tile corners average their
// neighbours so hills get slopes instead of steps.
float corner_height(const engine::TileMap& map, int cx, int cy);
// Smooth terrain height anywhere on the map, in elevation levels.
float surface_height(const engine::TileMap& map, Vector2 ground);
// Ground point under an iso pixel, taking hills into account.
Vector2 pick_ground(const engine::TileMap& map, Vector2 iso_px);
// Iso-space rectangle that contains the whole map.
Rectangle map_bounds(const engine::TileMap& map);

// A bridge's deck: flat at its banks' level over the river, from v0 to v1
// along it, its middle running from u0 (at v0) to u1 (at v1), `half` either
// side of that (u = x + y, v = y - x: on screen rows and columns). A point on
// it stands on the deck, not in the water.
struct Deck {
    float v0;
    float v1;
    float u0;
    float u1;
    float half;
    float height;  // elevation levels
    float middle(float v) const { return u0 + (u1 - u0) * (v - v0) / (v1 - v0); }
    bool contains(float u, float v) const { return v >= v0 && v <= v1 && std::fabs(u - middle(v)) <= half; }
};
void set_decks(std::vector<Deck> decks);

}  // namespace render::iso
