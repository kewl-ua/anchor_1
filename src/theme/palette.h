#pragma once

#include <raylib.h>

#include "engine/command.h"
#include "engine/terrain.h"

// Shared visual style, used by both render and hud so neither depends on the other.
namespace theme {

inline constexpr Color kBackground{18, 20, 22, 255};
inline constexpr Color kGround{38, 46, 40, 255};
inline constexpr Color kGridLine{48, 58, 50, 255};
inline constexpr Color kMapBorder{90, 100, 92, 255};

inline constexpr Color kSelection{80, 230, 110, 255};
inline constexpr Color kMovePing{80, 230, 110, 255};

inline constexpr Color kPanel{16, 18, 20, 230};
inline constexpr Color kPanelBorder{70, 78, 72, 255};
inline constexpr Color kText{220, 226, 222, 255};
inline constexpr Color kTextDim{140, 150, 144, 255};
inline constexpr Color kWarning{240, 200, 80, 255};
inline constexpr Color kDanger{240, 90, 80, 255};

inline Color terrain_color(engine::Terrain t) {
    switch (t) {
        case engine::Terrain::Grass: return {84, 116, 62, 255};
        case engine::Terrain::Forest: return {50, 76, 42, 255};
        case engine::Terrain::Trail: return {118, 102, 72, 255};
        case engine::Terrain::Water: return {56, 94, 136, 255};
        case engine::Terrain::Urban: return {122, 118, 108, 255};
        case engine::Terrain::House: return {100, 96, 88, 255};
        case engine::Terrain::Bridge: return {128, 100, 66, 255};
        case engine::Terrain::Ruins: return {108, 100, 90, 255};
        case engine::Terrain::Rock: return {104, 106, 102, 255};
        case engine::Terrain::Building: return {132, 128, 118, 255};
        case engine::Terrain::Rail: return {110, 100, 88, 255};
        case engine::Terrain::Trench: return {98, 84, 62, 255};
        case engine::Terrain::Foxhole: return {104, 90, 66, 255};
        case engine::Terrain::Dugout: return {92, 80, 60, 255};
        case engine::Terrain::GunPit: return {100, 86, 64, 255};
        case engine::Terrain::Wire: return {88, 112, 64, 255};
        case engine::Terrain::Hedgehogs: return {90, 108, 66, 255};
        case engine::Terrain::Pillbox: return {96, 86, 66, 255};
        case engine::Terrain::Airstrip: return {118, 120, 116, 255};
        case engine::Terrain::Road: return {98, 100, 102, 255};
        case engine::Terrain::DirtRoad: return {138, 114, 80, 255};
        case engine::Terrain::Plowed: return {110, 92, 66, 255};
        case engine::Terrain::Crops: return {138, 140, 64, 255};
        case engine::Terrain::Swamp: return {64, 86, 68, 255};
        case engine::Terrain::Crater: return {92, 82, 68, 255};
        case engine::Terrain::Riverbed: return {150, 136, 106, 255};
        case engine::Terrain::Apartment: return {118, 114, 106, 255};
        case engine::Terrain::Tower: return {104, 106, 102, 255};
        case engine::Terrain::GasStation: return {112, 112, 108, 255};
        case engine::Terrain::Elevator: return {120, 116, 108, 255};
        case engine::Terrain::Slag: return {62, 58, 56, 255};
        case engine::Terrain::Chalk: return {206, 202, 188, 255};
        case engine::Terrain::Wheat: return {190, 162, 88, 255};
        case engine::Terrain::Orchard: return {88, 118, 62, 255};
        case engine::Terrain::Garden: return {96, 80, 58, 255};
        case engine::Terrain::Pontoon: return {82, 90, 78, 255};  // (steel on the water)
        case engine::Terrain::Count: break;
    }
    return MAGENTA;
}

inline Color player_color(engine::PlayerId player) {
    static constexpr Color kColors[] = {
        {70, 140, 255, 255},  // blue
        {230, 80, 70, 255},   // red
        {90, 200, 110, 255},  // green
        {240, 200, 60, 255},  // yellow
    };
    return kColors[player % 4];
}

}  // namespace theme
