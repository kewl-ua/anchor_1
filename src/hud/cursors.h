#pragma once

#include <cstdint>

#include <raylib.h>

// The mouse cursor, as in AoE II: what a click would do shows in its shape
// (a sword over an enemy, an axe over a tree, a hammer over a building site...).
namespace hud {

enum class Cursor : uint8_t {
    Arrow,      // nothing special
    Attack,     // a sword: attack it
    Axe,        // cut timber
    Pick,       // quarry stone
    Build,      // a hammer: build it, help build, repair
    Enter,      // go in: a house, a dugout, an IFV, the hospital
    Supply,     // a crate: haul, supply, load up, stock
    Target,     // crosshairs: aiming a skill, a fire order
    Forbidden,  // can't be done there
    Count,
};

// Draws the cursor with its hotspot at `at` (screen pixels).
void draw_cursor(Cursor cursor, Vector2 at);

}  // namespace hud
