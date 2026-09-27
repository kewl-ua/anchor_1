#include "render/camera.h"

#include <algorithm>
#include <cmath>

namespace render {

namespace {

constexpr float kPanSpeed = 900.0f;  // screen pixels per second
constexpr float kEdgeScrollMargin = 8.0f;
constexpr float kMinZoom = 0.3f;
constexpr float kMaxZoom = 2.5f;
constexpr float kZoomStep = 1.15f;

}  // namespace

void RtsCamera::set_bounds(Rectangle bounds) {
    bounds_ = bounds;
    clamp_to_world();
}

void RtsCamera::set_zoom(float zoom) {
    camera_.zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    clamp_to_world();
}

void RtsCamera::center_on(Vector2 world_pos) {
    fit_to_screen();
    camera_.target = world_pos;
    clamp_to_world();
}

void RtsCamera::update(float dt) {
    fit_to_screen();

    // Pan, in screen space so the speed feels the same at any zoom.
    Vector2 pan{0, 0};
    if (IsKeyDown(KEY_LEFT)) pan.x -= 1;
    if (IsKeyDown(KEY_RIGHT)) pan.x += 1;
    if (IsKeyDown(KEY_UP)) pan.y -= 1;
    if (IsKeyDown(KEY_DOWN)) pan.y += 1;

    if (IsWindowFocused() && IsCursorOnScreen()) {
        const Vector2 mouse = GetMousePosition();
        const auto w = static_cast<float>(GetScreenWidth());
        const auto h = static_cast<float>(GetScreenHeight());
        if (mouse.x <= kEdgeScrollMargin) pan.x -= 1;
        if (mouse.x >= w - kEdgeScrollMargin) pan.x += 1;
        if (mouse.y <= kEdgeScrollMargin) pan.y -= 1;
        if (mouse.y >= h - kEdgeScrollMargin) pan.y += 1;
    }

    const float step = kPanSpeed * dt / camera_.zoom;
    camera_.target.x += std::clamp(pan.x, -1.0f, 1.0f) * step;
    camera_.target.y += std::clamp(pan.y, -1.0f, 1.0f) * step;

    if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
        const Vector2 delta = GetMouseDelta();
        camera_.target.x -= delta.x / camera_.zoom;
        camera_.target.y -= delta.y / camera_.zoom;
    }

    // Zoom keeping the point under the cursor in place.
    if (const float wheel = GetMouseWheelMove(); wheel != 0.0f) {
        const Vector2 mouse = GetMousePosition();
        const Vector2 before = screen_to_world(mouse);
        camera_.zoom = std::clamp(camera_.zoom * std::pow(kZoomStep, wheel), kMinZoom, kMaxZoom);
        const Vector2 after = screen_to_world(mouse);
        camera_.target.x += before.x - after.x;
        camera_.target.y += before.y - after.y;
    }

    clamp_to_world();
}

Vector2 RtsCamera::screen_to_world(Vector2 screen) const { return GetScreenToWorld2D(screen, camera_); }

Vector2 RtsCamera::world_to_screen(Vector2 world) const { return GetWorldToScreen2D(world, camera_); }

Rectangle RtsCamera::visible_world_rect() const {
    const Vector2 top_left = screen_to_world({0, 0});
    const Vector2 bottom_right = screen_to_world(
        {static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())});
    return {top_left.x, top_left.y, bottom_right.x - top_left.x, bottom_right.y - top_left.y};
}

void RtsCamera::fit_to_screen() {
    camera_.offset = {GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f};
}

void RtsCamera::clamp_to_world() {
    // Allow the view to go a bit past the map edge, like most RTS games.
    // If the whole map fits on screen, just center it.
    constexpr float kSlack = 200.0f;
    auto clamp_axis = [](float target, float half_view, float start, float size) {
        const float lo = start + half_view - kSlack;
        const float hi = start + size - half_view + kSlack;
        return lo > hi ? start + size * 0.5f : std::clamp(target, lo, hi);
    };
    camera_.target.x = clamp_axis(camera_.target.x, camera_.offset.x / camera_.zoom, bounds_.x, bounds_.width);
    camera_.target.y = clamp_axis(camera_.target.y, camera_.offset.y / camera_.zoom, bounds_.y, bounds_.height);
}

}  // namespace render
