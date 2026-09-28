#pragma once

#include <optional>

#include <raylib.h>

// The mouse as the game reads it. A smoke test can set it to a point of its
// own (--mouse) to show what hovering there does, without moving the real
// cursor on the desktop.
namespace theme {

inline std::optional<Vector2> g_fake_mouse;

inline Vector2 mouse_position() { return g_fake_mouse ? *g_fake_mouse : GetMousePosition(); }

}  // namespace theme
