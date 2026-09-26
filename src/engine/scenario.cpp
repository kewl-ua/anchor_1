#include "engine/scenario.h"

#include <algorithm>
#include <cstdlib>

#include "engine/economy.h"

namespace engine {

namespace {

// All positions and sizes are in percent of the map size, so every preset
// gets the same layout. Player 1 starts at (15, 85), player 2 at (85, 15):
// the river runs between them along the (0,0)-(100,100) diagonal.

struct Hill {
    int32_t x_pct;
    int32_t y_pct;
    int32_t height;       // elevation levels
    int32_t plateau_pct;  // flat top radius
};
constexpr Hill kHills[] = {
    {44, 54, 2, 2},  // bridgehead height at the central bridge
    {30, 42, 2, 2},
    {42, 70, 2, 2},
    {22, 60, 1, 3},
    {56, 80, 3, 1},
    {26, 76, 1, 2},  // a knoll in front of the first player's base
};
constexpr int32_t kTilesPerLevel = 3;  // slope length per elevation level

struct Blob {
    int32_t x_pct;
    int32_t y_pct;
    int32_t radius_pct;
};
constexpr Blob kForests[] = {
    {32, 14, 7}, {10, 45, 5}, {62, 38, 4}, {18, 32, 3},
    {6, 90, 3},  // the woodline behind the first player's base
};
constexpr Blob kPonds[] = {{22, 44, 2}};
// Stone outcrops: one next to each base, a few to fight over.
constexpr Blob kRocks[] = {{20, 91, 1}, {30, 52, 1}, {12, 30, 1}};

struct Segment {
    int32_t x0_pct;
    int32_t y0_pct;
    int32_t x1_pct;
    int32_t y1_pct;
};
// Trails cut through the forests above (they only replace forest tiles).
constexpr Segment kTrails[] = {
    {23, 14, 41, 14}, {32, 5, 32, 23}, {4, 45, 16, 45}, {62, 33, 62, 43},
};
// Tree lines (posadki) between fields, with gaps for crossing.
constexpr Segment kTreeLines[] = {
    {18, 40, 32, 40}, {25, 50, 25, 72}, {30, 60, 48, 60}, {40, 62, 40, 80}, {45, 72, 60, 72},
};
constexpr int32_t kTreeLineGapEvery = 10;  // tiles
constexpr int32_t kTreeLineGapWidth = 2;

struct Rect {
    int32_t x_pct;
    int32_t y_pct;
    int32_t w_pct;
    int32_t h_pct;
};
constexpr Rect kVillages[] = {{36, 50, 4, 3}, {10, 62, 4, 3}};

constexpr int32_t kRiverAmplitudePct = 5;
constexpr int64_t kRiverHalfWidth = 2;   // in diagonal steps: ~3.5 tiles wide
constexpr int64_t kBridgeHalfWidth = 2;  // along the river: wide enough to span it
constexpr int32_t kBridgeOffsetPct = 25;  // side bridges, measured along the river from the center
constexpr int32_t kBaseInsetPct = 15;     // bases sit this far from the corners
constexpr int32_t kBaseClearingPct = 6;
constexpr int32_t kArmyForwardTiles = 10;  // the army stands this far in front of the headquarters
constexpr int32_t kHeadquartersSize = 3;
constexpr int kStartingWorkers = 5;
// Personnel, Food, Materials, Ammo, Fuel: enough for a barracks and a warehouse.
constexpr Stock kStartingStock = {10, 300, 300, 150, 150};

// Paints terrain on a tile and on its mirror image through the map center,
// so the map is fair by construction.
class Painter {
public:
    explicit Painter(TileMap& map) : map_(map), size_(map.width()) {}

    int32_t at(int32_t pct) const { return size_ * pct / 100; }

    void paint(int32_t x, int32_t y, Terrain t) {
        if (!map_.contains_tile(x, y)) return;
        map_.set_terrain(x, y, t);
        map_.set_terrain(map_.width() - 1 - x, map_.height() - 1 - y, t);
    }

    // Calls fn(x, y, dist) for tiles whose center is within `r` tiles of the
    // tile corner (cx, cy); `dist` is in tiles, rounded down.
    template <typename Fn>
    void disc(int32_t cx, int32_t cy, int32_t r, Fn fn) {
        for (int32_t y = cy - r - 1; y <= cy + r; ++y) {
            for (int32_t x = cx - r - 1; x <= cx + r; ++x) {
                if (!map_.contains_tile(x, y)) continue;
                // Measured in half tiles to stay in integers.
                const int64_t dx = 2 * x + 1 - 2 * cx;
                const int64_t dy = 2 * y + 1 - 2 * cy;
                fn(x, y, static_cast<int32_t>(isqrt(static_cast<uint64_t>(dx * dx + dy * dy)) / 2));
            }
        }
    }

    // Axis-aligned or not, the line is 4-connected so vehicles can follow it.
    template <typename Fn>
    void line(const Segment& s, Fn fn) {
        int32_t x = at(s.x0_pct);
        int32_t y = at(s.y0_pct);
        const int32_t x1 = at(s.x1_pct);
        const int32_t y1 = at(s.y1_pct);
        int32_t i = 0;
        fn(x, y, i++);
        while (x != x1 || y != y1) {
            if (std::abs(x1 - x) >= std::abs(y1 - y)) {
                x += x1 > x ? 1 : -1;
            } else {
                y += y1 > y ? 1 : -1;
            }
            fn(x, y, i++);
        }
    }

    TileMap& map() { return map_; }

private:
    TileMap& map_;
    int32_t size_;
};

// A cheap deterministic hash for ragged forest edges.
uint32_t tile_noise(int32_t x, int32_t y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// A meandering river along the main diagonal, symmetric through the center.
void paint_river(Painter& p, int32_t size) {
    // Coordinates along (a) and across (b) the diagonal, from tile centers in
    // doubled units so everything stays integer.
    const int64_t wavelength = std::max<int64_t>(8, (size * 4 / 3) & ~int64_t{1});  // even
    const int64_t half = wavelength / 2;
    const int64_t amplitude = size * kRiverAmplitudePct / 100;
    const int64_t bridges[] = {0, size * kBridgeOffsetPct / 100, -size * kBridgeOffsetPct / 100};

    for (int32_t y = 0; y < size; ++y) {
        for (int32_t x = 0; x < size; ++x) {
            const int64_t a = (2 * x + 1 + 2 * y + 1) / 2 - size;
            const int64_t b = y - x;
            // Parabolic arcs make an odd, sine-like wave, with no floating point.
            const int64_t phase = ((a % wavelength) + wavelength) % wavelength;
            const int64_t q = phase < half ? phase : phase - half;
            const int64_t bend = amplitude * 16 * q * (half - q) / (wavelength * wavelength);
            const int64_t center = phase < half ? bend : -bend;
            if (std::abs(b - center) > kRiverHalfWidth) continue;

            bool bridge = false;
            for (int64_t at : bridges) bridge = bridge || std::abs(a - at) <= kBridgeHalfWidth;
            p.paint(x, y, bridge ? Terrain::Bridge : Terrain::Water);
        }
    }
}

void raise_hill(TileMap& map, Painter& p, const Hill& hill) {
    const int32_t size = map.width();
    const int32_t plateau = std::max(1, size * hill.plateau_pct / 100);
    const int32_t reach = plateau + hill.height * kTilesPerLevel;
    auto raise = [&](int32_t cx, int32_t cy) {
        p.disc(cx, cy, reach, [&](int32_t x, int32_t y, int32_t dist) {
            // Rivers stay at the bottom of their valley.
            const Terrain t = map.terrain(x, y);
            if (t == Terrain::Water || t == Terrain::Bridge) return;
            const int32_t slope = std::max(0, dist - plateau);
            const int32_t level = hill.height - (slope + kTilesPerLevel - 1) / kTilesPerLevel;
            if (level > map.elevation(x, y)) map.set_elevation(x, y, static_cast<uint8_t>(level));
        });
    };
    raise(p.at(hill.x_pct), p.at(hill.y_pct));
    raise(size - p.at(hill.x_pct), size - p.at(hill.y_pct));
}

void spawn_squad(World& world, PlayerId owner, int32_t cx, int32_t cy, UnitTypeId type, int count) {
    for (int i = 0; i < count; ++i) {
        // Scatter within +-3 tiles, in hundredths of a tile.
        const int32_t dx = world.rng().next_range(-300, 300);
        const int32_t dy = world.rng().next_range(-300, 300);
        world.spawn_unit(owner, type,
                         {Fixed::from_int(cx) + Fixed::from_ratio(dx, 100), Fixed::from_int(cy) + Fixed::from_ratio(dy, 100)});
    }
}

void spawn_army(World& world, PlayerId owner, int32_t cx, int32_t cy) {
    spawn_squad(world, owner, cx, cy, UnitTypeId::Rifleman, 6);
    spawn_squad(world, owner, cx, cy, UnitTypeId::MachineGunner, 2);
    spawn_squad(world, owner, cx, cy, UnitTypeId::Grenadier, 2);
    spawn_squad(world, owner, cx, cy, UnitTypeId::Tank, 2);
    spawn_squad(world, owner, cx, cy, UnitTypeId::Ifv, 1);
}

}  // namespace

TileMap make_demo_map(int32_t size) {
    TileMap map(size, size);
    Painter p(map);

    for (const Blob& f : kForests) {
        p.disc(p.at(f.x_pct), p.at(f.y_pct), std::max(2, p.at(f.radius_pct)), [&](int32_t x, int32_t y, int32_t dist) {
            const auto ragged = static_cast<int32_t>(tile_noise(x, y) % 3) - 1;  // -1..1 tiles
            if (dist <= std::max(2, p.at(f.radius_pct)) + ragged) p.paint(x, y, Terrain::Forest);
        });
    }
    for (const Segment& s : kTrails) {
        p.line(s, [&](int32_t x, int32_t y, int32_t) {
            if (map.contains_tile(x, y) && map.terrain(x, y) == Terrain::Forest) p.paint(x, y, Terrain::Trail);
        });
    }
    for (const Segment& s : kTreeLines) {
        p.line(s, [&](int32_t x, int32_t y, int32_t i) {
            if (i % kTreeLineGapEvery >= kTreeLineGapWidth) p.paint(x, y, Terrain::Forest);
        });
    }
    for (const Rect& v : kVillages) {
        const int32_t x0 = p.at(v.x_pct);
        const int32_t y0 = p.at(v.y_pct);
        for (int32_t dy = 0; dy < std::max(3, p.at(v.h_pct)); ++dy) {
            for (int32_t dx = 0; dx < std::max(3, p.at(v.w_pct)); ++dx) {
                // 2x2 blocks of houses separated by one-tile streets.
                const bool house = dx % 3 != 0 && dy % 3 != 0;
                p.paint(x0 + dx, y0 + dy, house ? Terrain::House : Terrain::Urban);
            }
        }
    }
    for (const Blob& pond : kPonds) {
        p.disc(p.at(pond.x_pct), p.at(pond.y_pct), std::max(1, p.at(pond.radius_pct)),
               [&](int32_t x, int32_t y, int32_t dist) {
                   if (dist <= std::max(1, p.at(pond.radius_pct))) p.paint(x, y, Terrain::Water);
               });
    }
    for (const Blob& rock : kRocks) {
        p.disc(p.at(rock.x_pct), p.at(rock.y_pct), std::max(1, p.at(rock.radius_pct)),
               [&](int32_t x, int32_t y, int32_t dist) {
                   if (dist <= std::max(1, p.at(rock.radius_pct))) p.paint(x, y, Terrain::Rock);
               });
    }
    paint_river(p, size);

    // Keep the start areas open.
    const FixedVec2 base = demo_base_position(size, 0);
    p.disc(base.x.to_int(), base.y.to_int(), std::max(6, p.at(kBaseClearingPct)),
           [&](int32_t x, int32_t y, int32_t dist) {
               if (dist <= std::max(6, p.at(kBaseClearingPct))) p.paint(x, y, Terrain::Grass);
           });

    for (const Hill& hill : kHills) raise_hill(map, p, hill);
    return map;
}

void setup_demo_scenario(World& world) {
    for (PlayerId player = 0; player < 2; ++player) {
        const FixedVec2 base = demo_base_position(world.map().width(), player);
        const int32_t bx = base.x.to_int();
        const int32_t by = base.y.to_int();
        // Towards the map center: player 1 sits bottom-left, player 2 top-right.
        const int32_t forward = player == 0 ? 1 : -1;

        world.set_stock(player, kStartingStock);
        world.place_structure(StructureType::Headquarters, player,
                              {bx - kHeadquartersSize / 2, by - kHeadquartersSize / 2}, kHeadquartersSize,
                              kHeadquartersSize);
        // Rear troops behind the headquarters, the army in front of it.
        for (int i = 0; i < kStartingWorkers; ++i) {
            world.spawn_unit(player, UnitTypeId::Worker,
                             {Fixed::from_int(bx - 2 + i) - Fixed::from_int(2 * forward),
                              Fixed::from_int(by + 3 * forward)});
        }
        spawn_army(world, player, bx + kArmyForwardTiles * forward, by - kArmyForwardTiles * forward);
    }
}

FixedVec2 demo_base_position(int32_t map_size, PlayerId player) {
    const int32_t inset = map_size * kBaseInsetPct / 100;
    const int32_t x = player == 0 ? inset : map_size - inset;
    const int32_t y = player == 0 ? map_size - inset : inset;
    return {Fixed::from_int(x), Fixed::from_int(y)};
}

}  // namespace engine
