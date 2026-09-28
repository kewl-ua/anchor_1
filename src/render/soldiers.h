#pragma once

#include <cstdint>

#include <raylib.h>

// Foot soldiers in pixel art: our own pack. Each man is a figure of limbs
// posed in three dimensions, drawn into pixels from every direction the way
// the vehicles are: shaded round, his edges inked, his side's colours on him.
namespace render::soldiers {

// What a man is doing, as drawn: each a run of frames.
enum class Pose : uint8_t {
    Stand,   // at ease, breathing
    Walk,    // a step cycle
    Aim,     // standing, weapon shouldered; the second frame its kick
    Kneel,   // down on one knee, aiming; its kick
    Prone,   // lying, aiming (a machine gun on its bipod); its kick
    Reload,  // on a knee, weapon up, a fresh magazine, a rocket
    Throw,   // a hand grenade: back, over, through
    Hurt,    // hit: doubled over
    Die,     // hit badly: reeling, down on his knees, falling, lying
    Work,    // an axe up and down; a spade in, the earth up
    Sit,     // resting on the ground, his weapon across his knees
    Climb,   // up a ladder, a tower
    Crew,    // on a knee at the weapon in front of him (a mortar, an AGS)
    Quarry,     // a pickaxe up and down into the rock
    CarryLog,   // walking with a log on his shoulder
    CarrySack,  // walking with a sack of stone on his back
    Heave,      // throwing his load up into a truck's bed, down by a door: lifting it, away
    Count,
};

// What he carries: his weapon, his headgear, what's on his back.
enum class Kit : uint8_t {
    Rifle,       // an AK, a helmet, a plate carrier
    MachineGun,  // a PKM
    Rpg,         // an RPG-7, spare rockets on his back
    Sniper,      // a Dragunov, a floppy hat (a scout)
    Assault,     // a black AK, a heavier carrier and a pack
    Sapper,      // a short carbine, a big pack
    Radio,       // a carbine, the radio on his back, its whip aerial (a signaller)
    Igla,        // a MANPADS tube
    Rear,        // no carrier, a cap; an axe, a spade (a rear trooper)
    Mortar,      // the tube carried on his back
    Ags,         // the AGS carried on his back
    Count,
};

inline constexpr int kDirs = 8;     // columns: the way he faces, by the ground angle (as the vehicles' sheets)
inline constexpr int kCellW = 48;   // a frame, pixels
inline constexpr int kCellH = 36;
inline constexpr float kOriginX = 24.0f;  // where his feet are in a frame
inline constexpr float kOriginY = 29.0f;

int first_frame(Pose p);
int frame_count(Pose p);
int total_frames();

// Every direction (columns) and frame (rows) of a kit in his side's colours.
Image bake(Kit kit, Color team);

// A point on him in a pose's frame: ahead of his feet, to his left (tiles on
// the ground) and up (pixels). The muzzle of his weapon.
struct Offset {
    float ahead;
    float left;
    float up;
};
Offset muzzle(Kit kit, Pose p, int frame);

}  // namespace render::soldiers
