#include "render/terrain_art.h"

#include <vector>

namespace render {

namespace {

constexpr const char* kFiles[] = {"grass",  "forest",   "trail",  "water", "urban",  "rock",     "concrete",
                                  "dirtroad", "plowed", "crops",  "swamp", "crater", "riverbed", "earth"};
static_assert(std::size(kFiles) == kMaterialCount);

// Atlas cells: the texture with a 1-texel border wrapped around from the
// opposite side, so filtering at a piece's edge still sees the same ground.
constexpr int kCell = TerrainArt::kTexture + 2;
constexpr int kColumns = 4;
constexpr int kRows = (static_cast<int>(kMaterialCount) + kColumns - 1) / kColumns;

}  // namespace

TerrainArt::~TerrainArt() {
    if (atlas_.id != 0) UnloadTexture(atlas_);
}

void TerrainArt::load(const std::string& dir) {
    if (tried_) return;
    tried_ = true;
    std::vector<Color> pixels(static_cast<size_t>(kCell * kColumns * kCell * kRows), BLANK);
    const int width = kCell * kColumns;
    bool any = false;
    for (size_t m = 0; m < kMaterialCount; ++m) {
        const std::string path = dir + kFiles[m] + ".png";
        if (!FileExists(path.c_str())) continue;
        Image img = LoadImage(path.c_str());
        if (img.data == nullptr) continue;
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        if (img.width != kTexture || img.height != kTexture) ImageResize(&img, kTexture, kTexture);
        const auto* src = static_cast<const Color*>(img.data);
        const int cx = static_cast<int>(m % kColumns) * kCell;
        const int cy = static_cast<int>(m / kColumns) * kCell;
        for (int y = 0; y < kCell; ++y) {
            const int sy = (y - 1 + kTexture) % kTexture;
            for (int x = 0; x < kCell; ++x) {
                const int sx = (x - 1 + kTexture) % kTexture;
                pixels[static_cast<size_t>((cy + y) * width + cx + x)] = src[sy * kTexture + sx];
            }
        }
        UnloadImage(img);
        present_[m] = true;
        any = true;
    }
    if (!any) return;
    const Image atlas{pixels.data(), width, kCell * kRows, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    atlas_ = LoadTextureFromImage(atlas);
    SetTextureFilter(atlas_, TEXTURE_FILTER_POINT);  // crisp pixels
}

std::optional<Material> TerrainArt::material_of(engine::Terrain t) {
    using engine::Terrain;
    switch (t) {
        case Terrain::Grass:
        case Terrain::Wire:
        case Terrain::Hedgehogs: return Material::Grass;
        case Terrain::Forest: return Material::Forest;
        case Terrain::Trail: return Material::Trail;
        case Terrain::Water: return Material::Water;
        case Terrain::Urban:
        case Terrain::House:
        case Terrain::Ruins:
        case Terrain::Rail: return Material::Urban;
        case Terrain::Rock: return Material::Rock;
        case Terrain::Building:
        case Terrain::Airstrip:
        case Terrain::Road:
        case Terrain::Apartment:
        case Terrain::Tower:
        case Terrain::GasStation:
        case Terrain::Elevator: return Material::Concrete;
        case Terrain::DirtRoad: return Material::DirtRoad;
        case Terrain::Plowed: return Material::Plowed;
        case Terrain::Crops: return Material::Crops;
        case Terrain::Swamp: return Material::Swamp;
        case Terrain::Crater: return Material::Crater;
        case Terrain::Riverbed: return Material::Riverbed;
        case Terrain::Trench:
        case Terrain::Foxhole:
        case Terrain::Dugout:
        case Terrain::GunPit:
        case Terrain::Pillbox: return Material::Earth;
        case Terrain::Bridge:  // the deck is drawn as it is
        case Terrain::Count: break;
    }
    return std::nullopt;
}

int TerrainArt::rank(Material m) {
    // Land spills over the shore; grass over tracks, fields and dug earth;
    // the forest floor over the grass at the forest's edge.
    // Concrete is low, so a road's staircase edges soften under the verges.
    switch (m) {
        case Material::Water: return 0;
        case Material::Concrete: return 1;
        case Material::Riverbed: return 2;
        case Material::Swamp: return 3;
        case Material::Earth: return 4;
        case Material::Crater: return 5;
        case Material::Plowed: return 6;
        case Material::Crops: return 7;
        case Material::DirtRoad: return 8;
        case Material::Urban: return 9;
        case Material::Trail: return 10;
        case Material::Grass: return 11;
        case Material::Forest: return 12;
        case Material::Rock: return 13;
        case Material::Count: break;
    }
    return -1;
}

Rectangle TerrainArt::piece(Material m, int tx, int ty) const {
    const auto i = static_cast<int>(m);
    const int wx = ((tx % kTilesPerTexture) + kTilesPerTexture) % kTilesPerTexture;
    const int wy = ((ty % kTilesPerTexture) + kTilesPerTexture) % kTilesPerTexture;
    return {static_cast<float>((i % kColumns) * kCell + 1 + wx * kPerTile),
            static_cast<float>((i / kColumns) * kCell + 1 + wy * kPerTile), static_cast<float>(kPerTile),
            static_cast<float>(kPerTile)};
}

}  // namespace render
