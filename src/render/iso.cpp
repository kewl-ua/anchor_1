#include "render/iso.h"

#include <algorithm>
#include <cmath>

namespace render::iso {

float corner_height(const engine::TileMap& map, int cx, int cy) {
    float sum = 0.0f;
    int count = 0;
    for (int ty = cy - 1; ty <= cy; ++ty) {
        for (int tx = cx - 1; tx <= cx; ++tx) {
            if (!map.contains_tile(tx, ty)) continue;
            sum += map.elevation(tx, ty);
            ++count;
        }
    }
    return count ? sum / static_cast<float>(count) : 0.0f;
}

float surface_height(const engine::TileMap& map, Vector2 ground) {
    const float x = std::clamp(ground.x, 0.0f, static_cast<float>(map.width()) - 0.001f);
    const float y = std::clamp(ground.y, 0.0f, static_cast<float>(map.height()) - 0.001f);
    const int tx = static_cast<int>(x);
    const int ty = static_cast<int>(y);
    const float fx = x - static_cast<float>(tx);
    const float fy = y - static_cast<float>(ty);

    const float top = corner_height(map, tx, ty) * (1 - fx) + corner_height(map, tx + 1, ty) * fx;
    const float bottom = corner_height(map, tx, ty + 1) * (1 - fx) + corner_height(map, tx + 1, ty + 1) * fx;
    return top * (1 - fy) + bottom * fy;
}

Vector2 pick_ground(const engine::TileMap& map, Vector2 iso_px) {
    // The height depends on the point and the point on the height, so iterate;
    // a few rounds converge for gentle slopes.
    float height = 0.0f;
    Vector2 ground = unproject(iso_px, height);
    for (int i = 0; i < 4; ++i) {
        height = surface_height(map, ground);
        ground = unproject(iso_px, height);
    }
    return ground;
}

Rectangle map_bounds(const engine::TileMap& map) {
    const auto w = static_cast<float>(map.width());
    const auto h = static_cast<float>(map.height());
    const float top = -engine::TileMap::kMaxElevation * kElevationStep;
    const float left = project({0, h}, 0).x;
    const float right = project({w, 0}, 0).x;
    const float bottom = project({w, h}, 0).y;
    return {left, top, right - left, bottom - top};
}

}  // namespace render::iso
