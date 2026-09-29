// Battles of the war played over as demonstrations: a director's script
// for a scenario (engine/scenario.h). Its orders are real commands, put into
// the simulation on their ticks, so a --debug replay of it plays the same;
// its camera and captions are only for watching.
#pragma once

#include <utility>
#include <vector>

#include <raylib.h>

#include "engine/command.h"
#include "engine/scenario.h"

namespace app::battles {

struct CameraKey {
    float at;       // seconds in
    Vector2 look;   // the ground point in the middle of the screen (tiles)
    float zoom;
};

struct Caption {
    float from;  // seconds
    float to;
    const char* title;  // UTF-8
    const char* text;
};

struct Script {
    std::vector<std::pair<engine::Tick, engine::Command>> orders;  // every side's
    std::vector<CameraKey> camera;
    std::vector<Caption> captions;
    engine::Tick end = 0;  // when it's over
};

// The Siverskyi Donets crossing at Bilohorivka, May 2022.
Script donets(const engine::ScenarioSetup& setup);

// Where the camera is at `seconds`: eased from key to key.
CameraKey camera_at(const Script& script, float seconds);

}  // namespace app::battles
