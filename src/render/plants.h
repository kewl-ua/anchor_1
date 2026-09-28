#pragma once

#include <cstdint>

#include <raylib.h>

// The crops in the fields in pixel art: sunflowers, maize, wheat, a
// kitchen garden's beds; each plant standing (by its stage, swaying) and
// flattened (where vehicles went through). Baked once into sheets of
// frames, a frame a cell, the plant's foot at the cell's origin.
namespace render::plants {

enum class Plant : uint8_t { Sunflower, Maize, Wheat, Garden, Count };

// A sheet's layout: its cells, where the foot is in one, how many frames.
struct Layout {
    int w;
    int h;
    float origin_x;
    float origin_y;
    int frames;
};
Layout layout(Plant p);

// Frames by what they show.
// Sunflower: stage 0 in bloom, 1 ripening (the head hanging, the petals
// going), 2 dry (black); `height` 0..2; `lean` 0..2 (left, upright, right);
// `back`: the head turned away (in bloom). Flattened: by stage, which way.
int sunflower(int stage, int height, int lean, bool back);
int sunflower_down(int stage, int way);
// Maize: stage 0 green, 1 dry; `height` 0..1; `lean` 0..2. Flattened.
int maize(int stage, int height, int lean);
int maize_down(int stage, int way);
// Wheat, a tuft of it: stage 0 unripe, 1 ripe; `variant` 0..1; `lean` 0..3 (the wind). Lodged, flattened.
int wheat(int stage, int variant, int lean);
int wheat_down(int stage, int variant);
// A kitchen garden: 0 potatoes, 1 a cabbage, 2 tomatoes on a stake; `variant` 0..2. Trodden in.
int garden(int kind, int variant);
int garden_down(int variant);

Image bake(Plant p);

}  // namespace render::plants
