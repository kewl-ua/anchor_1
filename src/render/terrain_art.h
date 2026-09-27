#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "engine/terrain.h"
#include "raylib.h"

namespace render {

// What the ground of a tile is drawn with (several terrains share one: the
// concrete of a road, a runway and a building's yard).
enum class Material : uint8_t {
    Grass,
    Forest,
    Trail,
    Water,
    Urban,
    Rock,
    Concrete,
    DirtRoad,
    Plowed,
    Crops,
    Swamp,
    Crater,
    Riverbed,
    Earth,  // dug ground: trenches, foxholes, dugouts, gun pits
    Count,
};
inline constexpr size_t kMaterialCount = static_cast<size_t>(Material::Count);

// The ground textures (assets/terrain/<material>.png, 256x256, tileable)
// packed into one atlas. A tile draws its own 32x32 piece of its material,
// the piece its place on the map calls for, so a texture flows on across 8x8
// tiles without a seam. A material without a file keeps the flat colour.
class TerrainArt {
public:
    static constexpr int kTexture = 256;  // side of a source texture, texels
    static constexpr int kPerTile = 32;   // texels across one tile
    static constexpr int kTilesPerTexture = kTexture / kPerTile;

    TerrainArt() = default;
    ~TerrainArt();
    TerrainArt(const TerrainArt&) = delete;
    TerrainArt& operator=(const TerrainArt&) = delete;

    // Loads what there is in `dir`; once (later calls do nothing).
    void load(const std::string& dir);
    bool loaded() const { return tried_; }

    static std::optional<Material> material_of(engine::Terrain t);
    // Soft edges: a material spills over onto a neighbour of lower rank.
    static int rank(Material m);

    bool has(Material m) const { return present_[static_cast<size_t>(m)]; }
    const Texture2D& atlas() const { return atlas_; }
    // Texel rectangle of tile (tx, ty)'s piece of the material.
    Rectangle piece(Material m, int tx, int ty) const;

private:
    Texture2D atlas_{};
    std::array<bool, kMaterialCount> present_{};
    bool tried_ = false;
};

}  // namespace render
