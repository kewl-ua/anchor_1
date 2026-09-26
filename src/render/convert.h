#pragma once

#include <cmath>

#include <raylib.h>

#include "engine/fixed.h"

// Bridges between engine fixed-point and raylib floats. Converting to float is
// always safe (display only). Converting from float is fine for building
// commands: the rounded fixed value is what gets sent to every peer.
namespace render {

inline float to_float(engine::Fixed f) {
    return static_cast<float>(f.raw) / static_cast<float>(engine::Fixed::kOneRaw);
}

inline Vector2 to_vector2(engine::FixedVec2 v) { return {to_float(v.x), to_float(v.y)}; }

inline engine::Fixed to_fixed(float f) {
    return engine::Fixed::from_raw(static_cast<int32_t>(std::lround(f * engine::Fixed::kOneRaw)));
}

inline engine::FixedVec2 to_fixed_vec2(Vector2 v) { return {to_fixed(v.x), to_fixed(v.y)}; }

}  // namespace render
