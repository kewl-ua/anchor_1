// Mines in our pixel art: the axes' real ones, drawn by hand in code as the
// works are. A pressure mine dug into its tile, the loose earth round it; a
// directional one up on its legs, facing the way it was laid.
#pragma once

#include <cstdint>

#include "engine/unit_types.h"
#include "engine/world.h"
#include "raylib.h"

namespace render::mines {

// Which real mine it is: the axis's own of its kind.
enum class Model : uint8_t {
    Tm62m,     // anti-tank, both axes: a steel disc, the MVCh-62 fuze in the middle
    Pmn2,      // anti-personnel, Authoritarian: green plastic, a black rubber cross on top
    M14,       // anti-personnel, Democratic: a little olive plastic one, its pressure plate
    Mon50,     // directional, Authoritarian: a curved green box up on its legs, the fuze on top
    Claymore,  // directional, Democratic: M18A1, lower and wider, on two pairs of scissor legs
    Count,
};
Model model_of(engine::MineKind kind, engine::Axis axis);
const char* name_of(Model model);

// A directional one's images: the way it faces, by the ground angle (0: along
// +x, a step 45 degrees towards +y).
inline constexpr int kDirs = 8;
int dir_of(Vector2 facing);

struct Sprite {
    Image img{};
    Vector2 origin{};  // where its point on the ground is in the image
    Vector2 fuze{};    // a directional one's fuze, where its tripwire starts (from the origin, pixels)
};
// The mine as it lies: `dir` a directional one's, `seed` how it lies and the earth round it.
Sprite bake(Model model, int dir, uint32_t seed);

}  // namespace render::mines
