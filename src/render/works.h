#pragma once

#include <array>
#include <cstdint>

#include <raylib.h>

// Field works in pixel art: trenches, foxholes, dugouts, breastworks, the
// guns' pits, wire and hedgehogs. Each tile's is worked out pixel by pixel
// as ground dug out and thrown up (a ray down from the eye through each
// pixel finds the spoil, the ditch's far wall, its floor), then what's built
// into it (planks, sandbags, logs, crates, stakes) is put in where it shows.
namespace render::works {

enum class Kind : uint8_t {
    Trench,     // a ditch through the tile, joining its neighbours
    Foxhole,    // a man's round pit
    Dugout,     // a log roof under earth, its door down some steps
    Parapet,    // a breastwork on open ground, a scrape behind it
    MortarPit,  // a round pit, the bombs in niches, the crew's slit beside
    GunPit,     // a towed gun's emplacement: a shallow platform in a horseshoe of earth
    Caponier,   // a self-propelled gun's: a pit its length, a ramp down into it
    Wire,       // a coil of barbed wire on stakes
    Hedgehogs,  // two steel hedgehogs
    Pontoon,    // a pontoon bridge's section on the water: its deck, its floats, a ramp where it meets the bank
};

// What's built into a trench tile (as the engine's TrenchFit numbers them).
enum Fit : uint8_t { kNoFit, kCell, kNest, kAtPost, kMortarPost, kParapetFit, kDugoutFit };

struct Spec {
    Kind kind = Kind::Trench;
    uint8_t links = 0;       // where a trench goes on to: +x 1, -x 2, +y 4, -y 8; wire corner to corner too: +x+y 16, -x-y 32, +x-y 64, -x+y 128
    bool parapet = false;    // a breastwork on the side it faces
    Vector2 facing{1, 0};    // on the ground (a gun pit: the way the gun faced)
    bool upgrading = false;  // being made a dugout: logs over it, more by it
    uint8_t fit = kNoFit;      // a trench tile: what's built into it (a Fit), facing `facing`
    uint8_t fitting = kNoFit;  // what it's being fitted out with
    float progress = 1.0f;     // dug this far (the ditch deeper, the bank higher, bags on it, logs laid)
    int damage = 0;          // 0 whole .. 3 caved in
    uint32_t seed = 0;
};

// Both images start at `at` (world pixels). `back`: all of it, drawn with
// the ground. `front`: what rises in front of whoever stands in it (the
// near bank, sandbags, the mound), drawn over them.
struct Sprite {
    Image back{};
    Image front{};
    Vector2 at{};
};

// A tile's works and its eight neighbours' (the ditches running on from
// them, their banks spilling over), row by row from (-1, -1); `has` says
// which there are. The tile's own is in the middle, [4].
struct Area {
    std::array<Spec, 9> spec{};
    std::array<bool, 9> has{};
};

// `corner`: the ground's height (world pixels up) at the tile's corners
// (0,0), (1,0), (1,1), (0,1).
Sprite bake(const Area& area, int tx, int ty, const std::array<float, 4>& corner);

// Where a man (or a gun) standing at `local` on the tile (0..1) stands in
// it: in the ditch, in the pit; and how deep he's sunk there (pixels).
struct Place {
    Vector2 at;
    float sunk;
};
Place place(const Spec& spec, Vector2 local, bool vehicle);

}  // namespace render::works
