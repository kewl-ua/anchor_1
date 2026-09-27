#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/fixed.h"

namespace engine {

enum class Terrain : uint8_t {
    Grass,   // open fields
    Forest,  // dense forest and tree lines: infantry only
    Trail,   // path through a forest: vehicles can use it
    Water,
    Urban,   // village streets and yards
    House,   // part of a House structure (see structures.h)
    Bridge,  // part of a Bridge structure
    Ruins,     // what's left of a collapsed house
    Rock,      // stone outcrop: impassable, quarried for materials
    Building,  // under a player's building (headquarters...)
    Rail,      // railway track: trains bring supplies along it to the station
    Trench,    // dug by infantry; tracks cross it slowly, wheels not at all
    Foxhole,   // a dug-in firing position
    Dugout,    // a shelter dug into the ground, roofed with logs (a Dugout structure)
    GunPit,    // a gun's dug-in position: a capunier, a mortar's closed position
    Wire,      // barbed wire: infantry crawls through, tracks crush it, wheels stop
    Hedgehogs, // anti-tank obstacles: no vehicle gets through
    Pillbox,   // a log-and-earth firing point (a Pillbox structure)
    Airstrip,  // an airfield's runway: flat concrete anyone can cross
    Road,      // a concrete or asphalt road: fast for everyone, no mine goes into it
    DirtRoad,  // a dirt road: faster than the fields
    Plowed,    // a plowed field: heavy going for wheels
    Crops,     // sunflowers, maize: tall enough to hide a man, not to stop a bullet
    Swamp,     // reeds and bog: men wade, tracks sink, wheels don't get in
    Crater,    // a shell crater: cover for a man lying in it, a hole for a vehicle
    Riverbed,  // the sand and pebbles of a dry riverbed, down in its gully
    Count,
};
inline constexpr size_t kTerrainCount = static_cast<size_t>(Terrain::Count);

enum class MoveClass : uint8_t {
    Foot,
    Vehicle,  // tracked: tanks, IFVs
    Wheeled,  // trucks: roads and fields, no ditches
    Count,
};
inline constexpr size_t kMoveClassCount = static_cast<size_t>(MoveClass::Count);

struct TerrainDef {
    const char* name;
    // Movement speed in percent, per MoveClass; 0 means impassable.
    std::array<int32_t, kMoveClassCount> speed_percent;
};

// Speeds: {foot, tracked, wheeled}.
inline constexpr TerrainDef kTerrainDefs[] = {
    {"Field", {100, 100, 100}},
    {"Forest", {55, 0, 0}},
    {"Forest trail", {100, 70, 70}},
    {"Water", {0, 0, 0}},
    {"Village", {90, 60, 60}},
    {"House", {0, 0, 0}},
    {"Bridge", {100, 100, 100}},
    {"Ruins", {70, 40, 30}},
    {"Rock", {0, 0, 0}},
    {"Building", {0, 0, 0}},
    {"Railway", {90, 80, 60}},
    {"Trench", {75, 30, 0}},
    {"Foxhole", {80, 50, 0}},
    {"Dugout", {0, 0, 0}},
    {"Gun pit", {70, 40, 40}},
    {"Barbed wire", {20, 80, 0}},
    {"Hedgehogs", {60, 0, 0}},
    {"Pillbox", {0, 0, 0}},
    {"Airstrip", {100, 100, 100}},
    {"Road", {110, 140, 160}},
    {"Dirt road", {100, 120, 130}},
    {"Plowed field", {90, 80, 60}},
    {"Sunflowers", {80, 90, 80}},
    {"Swamp", {40, 15, 0}},
    {"Crater", {80, 60, 30}},
    {"Dry riverbed", {90, 80, 70}},
};
static_assert(std::size(kTerrainDefs) == kTerrainCount);

inline const TerrainDef& terrain_def(Terrain t) { return kTerrainDefs[static_cast<size_t>(t)]; }

struct TilePos {
    int32_t x = 0;
    int32_t y = 0;

    bool operator==(const TilePos&) const = default;
};

// The tile a point lies on.
inline TilePos tile_of(FixedVec2 p) { return {p.x.to_int(), p.y.to_int()}; }

// Squared distance from a point to the nearest edge of a tile (0 inside it), raw units.
inline uint64_t distance_sq_to_tile(TilePos t, FixedVec2 p) {
    const Fixed x0 = Fixed::from_int(t.x);
    const Fixed y0 = Fixed::from_int(t.y);
    const Fixed one = Fixed::from_int(1);
    const Fixed dx = max(Fixed{}, max(x0 - p.x, p.x - (x0 + one)));
    const Fixed dy = max(Fixed{}, max(y0 - p.y, p.y - (y0 + one)));
    return FixedVec2{dx, dy}.length_sq_raw();
}
// The center of a tile, in continuous coordinates.
inline FixedVec2 tile_center(TilePos t) {
    return {Fixed::from_int(t.x) + Fixed::from_ratio(1, 2), Fixed::from_int(t.y) + Fixed::from_ratio(1, 2)};
}

// The battlefield: a grid of square tiles, each with a terrain type and an
// elevation level. Units move freely in continuous tile coordinates, where
// 1.0 is one tile. How it looks (isometric or not) is the renderer's business.
class TileMap {
public:
    static constexpr uint8_t kMaxElevation = 7;

    TileMap(int32_t width, int32_t height)
        : width_(width),
          height_(height),
          elevation_(static_cast<size_t>(width * height), 0),
          terrain_(static_cast<size_t>(width * height), Terrain::Grass),
          resource_(static_cast<size_t>(width * height), 0) {}

    int32_t width() const { return width_; }
    int32_t height() const { return height_; }
    FixedVec2 size() const { return {Fixed::from_int(width_), Fixed::from_int(height_)}; }

    bool contains(TilePos t) const { return t.x >= 0 && t.y >= 0 && t.x < width_ && t.y < height_; }
    bool contains_tile(int32_t tx, int32_t ty) const { return contains({tx, ty}); }

    // The tile must be inside the map.
    uint8_t elevation(int32_t tx, int32_t ty) const { return elevation_[index(tx, ty)]; }
    void set_elevation(int32_t tx, int32_t ty, uint8_t level) {
        elevation_[index(tx, ty)] = std::min(level, kMaxElevation);
        ++revision_;
    }
    Terrain terrain(int32_t tx, int32_t ty) const { return terrain_[index(tx, ty)]; }
    Terrain terrain(TilePos t) const { return terrain(t.x, t.y); }
    void set_terrain(int32_t tx, int32_t ty, Terrain t) {
        const Terrain old = terrain_[index(tx, ty)];
        terrain_[index(tx, ty)] = t;
        ++revision_;
        for (size_t cls = 0; cls < kMoveClassCount; ++cls) {
            if (terrain_def(old).speed_percent[cls] > 0 && terrain_def(t).speed_percent[cls] == 0) {
                ++blocking_revision_;
                break;
            }
        }
    }

    // Changes whenever a tile changes (a bridge blown up, a forest cut down),
    // so cached images know to rebuild.
    uint32_t revision() const { return revision_; }
    // Changes only when some tile became impassable (a bridge blown up):
    // only then can an existing route lead into a dead end.
    uint32_t blocking_revision() const { return blocking_revision_; }

    // Materials left on a tile (forest timber, rock stone).
    int32_t resource(TilePos t) const { return contains(t) ? resource_[index(t.x, t.y)] : 0; }
    void set_resource(TilePos t, int32_t amount) { resource_[index(t.x, t.y)] = amount; }

    // Elevation of the tile under a point; points outside use the nearest edge tile.
    uint8_t elevation_at(FixedVec2 p) const {
        const TilePos t = clamp_tile(tile_of(p));
        return elevation(t.x, t.y);
    }
    Terrain terrain_at(FixedVec2 p) const { return terrain(clamp_tile(tile_of(p))); }

    // Movement speed on a tile in percent; 0 = impassable. Outside the map is impassable.
    int32_t speed_percent(TilePos t, MoveClass cls) const {
        if (!contains(t)) return 0;
        return terrain_def(terrain(t)).speed_percent[static_cast<size_t>(cls)];
    }
    bool passable(TilePos t, MoveClass cls) const { return speed_percent(t, cls) > 0; }

    TilePos clamp_tile(TilePos t) const {
        return {std::clamp(t.x, 0, width_ - 1), std::clamp(t.y, 0, height_ - 1)};
    }

    // Ground height at a tile corner: the average of the tiles around it, so
    // hills have slopes. This is exactly the surface the renderer draws.
    Fixed corner_height(int32_t cx, int32_t cy) const {
        int32_t sum = 0;
        int32_t count = 0;
        for (int32_t ty = cy - 1; ty <= cy; ++ty) {
            for (int32_t tx = cx - 1; tx <= cx; ++tx) {
                if (!contains_tile(tx, ty)) continue;
                sum += elevation(tx, ty);
                ++count;
            }
        }
        return count ? Fixed::from_ratio(sum, count) : Fixed{};
    }

    // Smooth ground height anywhere, in elevation levels: the four corners of
    // the tile blended. Used for lines of fire.
    Fixed surface_height(FixedVec2 p) const {
        const TilePos t = clamp_tile(tile_of(p));
        const Fixed fx = clamp(p.x - Fixed::from_int(t.x), Fixed{}, Fixed::from_int(1));
        const Fixed fy = clamp(p.y - Fixed::from_int(t.y), Fixed{}, Fixed::from_int(1));
        const Fixed c00 = corner_height(t.x, t.y);
        const Fixed c10 = corner_height(t.x + 1, t.y);
        const Fixed c01 = corner_height(t.x, t.y + 1);
        const Fixed c11 = corner_height(t.x + 1, t.y + 1);
        const Fixed top = c00 + (c10 - c00) * fx;
        const Fixed bottom = c01 + (c11 - c01) * fx;
        return top + (bottom - top) * fy;
    }

private:
    size_t index(int32_t tx, int32_t ty) const { return static_cast<size_t>(ty * width_ + tx); }

    int32_t width_;
    int32_t height_;
    std::vector<uint8_t> elevation_;
    std::vector<Terrain> terrain_;
    std::vector<int32_t> resource_;
    uint32_t revision_ = 0;
    uint32_t blocking_revision_ = 0;
};

}  // namespace engine
