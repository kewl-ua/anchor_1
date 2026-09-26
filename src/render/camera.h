#pragma once

#include <raylib.h>

namespace render {

// RTS camera: arrow keys / screen-edge / middle-mouse-drag panning and
// wheel zoom towards the cursor. Pure view state, never affects the game.
//
// "World" here means the renderer's iso pixel space (see iso.h), not the
// engine's ground coordinates.
class RtsCamera {
public:
    void set_bounds(Rectangle bounds);
    void center_on(Vector2 world_pos);

    // Reads input and updates the view; call once per frame.
    void update(float dt);

    Vector2 screen_to_world(Vector2 screen) const;
    Vector2 world_to_screen(Vector2 world) const;
    // Visible world area, for culling.
    Rectangle visible_world_rect() const;

    const Camera2D& camera2d() const { return camera_; }

private:
    void fit_to_screen();
    void clamp_to_world();

    Camera2D camera_{{0, 0}, {0, 0}, 0.0f, 1.0f};
    Rectangle bounds_{0, 0, 0, 0};
};

}  // namespace render
