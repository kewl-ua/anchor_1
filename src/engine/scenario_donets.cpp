// The Siverskyi Donets crossing at Bilohorivka, May 2022, played over as a
// demonstration: the river between pine woods on its low left bank and the
// chalk of its high right bank, the village on top; the Authoritarian
// axis's brigade on the left bank with its pontoon parks, its column on
// the one road through the woods, its guns behind; the Democratic axis's
// paratroopers in trenches along the crest and in the village, two tanks
// by the wood, the scouts watching from the chalk, the howitzers far back.
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "engine/scenario.h"

namespace engine {

namespace {

constexpr int32_t kRiverY = 62;        // the river's middle, at the map's middle
constexpr int32_t kRiverHalf = 2;      // tiles of water either side of it: five wide
constexpr int32_t kMeander = 3;        // tiles its bends swing either way
constexpr int32_t kMeanderLength = 64;  // tiles from bend to bend and back
constexpr int32_t kRoadX = 60;         // the road down through the woods to the crossing (two tiles wide)
constexpr int32_t kCrossingX = 60;     // the first bridge
constexpr int32_t kCrossing2X = 74;    // the second, downstream
constexpr int32_t kCrestUp = 3;        // the right bank's chalk face: a level a tile, up to this

uint32_t noise(int32_t x, int32_t y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + 0x5bd1e995u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// The river's middle at x: bends as parabolic arcs, as the demo map's river.
int32_t river_y(int32_t x) {
    const int32_t half = kMeanderLength / 2;
    const int32_t phase = ((x % kMeanderLength) + kMeanderLength) % kMeanderLength;
    const int32_t q = phase < half ? phase : phase - half;
    const int32_t bend = kMeander * 4 * q * (half - q) / (half * half);
    return kRiverY + (phase < half ? bend : -bend);
}

FixedVec2 at(int32_t x2, int32_t y2) { return {Fixed::from_ratio(x2, 2), Fixed::from_ratio(y2, 2)}; }
FixedVec2 tile_mid(int32_t x, int32_t y) { return tile_center({x, y}); }

}  // namespace

int32_t donets_river_y(int32_t x) { return river_y(x); }

TileMap make_donets_map(int32_t size) {
    TileMap map(size, size);
    for (int32_t y = 0; y < size; ++y) {
        for (int32_t x = 0; x < size; ++x) {
            const int32_t ry = river_y(x);
            const int32_t d = y - ry;  // across the river: - the left (north) bank, + the right
            const uint32_t n = noise(x, y);
            Terrain t = Terrain::Grass;
            uint8_t level = 0;
            if (std::abs(d) <= kRiverHalf) {
                t = Terrain::Water;
            } else if (d < 0) {
                // The left bank: a strip of meadow and reeds along the water, then pine woods
                // right up to the fields where the guns stand; the road down through them.
                const int32_t out = -d - kRiverHalf;  // tiles from the water
                const bool meadow = out <= 6 && x >= 30 && x <= 96;
                const bool clearing = out <= 11 && x >= kRoadX - 7 && x <= kRoadX + 8;  // where the column gathers
                if (out == 1 && (x < kRoadX - 9 || x > kCrossing2X + 4) && n % 3 == 0) {
                    t = Terrain::Swamp;
                } else if (meadow || clearing) {
                    t = (n % 23 == 0 && !clearing && out > 2) ? Terrain::Forest : Terrain::Grass;
                } else if (y <= 16) {
                    t = (y + x) % 5 == 0 ? Terrain::Plowed : Terrain::Grass;  // the fields behind the woods
                } else {
                    t = n % 29 == 0 ? Terrain::Grass : Terrain::Forest;
                }
                if ((x == kRoadX || x == kRoadX + 1) && out >= 1) t = Terrain::DirtRoad;
            } else {
                // The right bank: a beach, the chalk face up to the crest, the plateau.
                const int32_t out = d - kRiverHalf;
                level = static_cast<uint8_t>(std::clamp(out - 1, 0, kCrestUp));
                if (out <= 1) {
                    t = Terrain::Grass;
                } else if (out <= kCrestUp + 1) {
                    t = n % 4 == 0 ? Terrain::Grass : Terrain::Chalk;
                } else if (y >= size - 30) {
                    t = Terrain::Grass;  // the fields where the guns stand
                    if (y < size - 22) t = (x / 6) % 2 == 0 ? Terrain::Wheat : Terrain::Plowed;
                } else if ((x < 44 || x > 88) && out <= 30) {
                    t = n % 17 == 0 ? Terrain::Grass : Terrain::Forest;  // the pines on the chalk
                } else {
                    t = n % 11 == 0 ? Terrain::Chalk : Terrain::Grass;
                }
                // The way up from the crossings: a cut in the chalk.
                if ((x == kCrossingX || x == kCrossingX + 1 || x == kCrossing2X || x == kCrossing2X + 1) && out <= kCrestUp + 2) {
                    t = Terrain::DirtRoad;
                }
            }
            map.set_terrain(x, y, t);
            map.set_elevation(x, y, level);
        }
    }
    // Bilohorivka on the plateau: blocks of two by two houses along its
    // streets, kitchen gardens behind, an orchard or two; the street down
    // to the fields and the guns.
    for (int32_t x = 46; x <= 84; ++x) {
        for (int32_t y = 0; y < size; ++y) {
            const int32_t d = y - river_y(x);
            const int32_t in = d - (kRiverHalf + kCrestUp + 4);  // tiles into the village from its edge
            if (in < 0 || in > 12) continue;
            const int32_t bx = (x - 46) % 5;
            const int32_t by = in % 6;
            Terrain t = Terrain::Garden;
            if (by == 0) {
                t = Terrain::DirtRoad;  // a street along the river
            } else if (bx == 0) {
                t = Terrain::Grass;  // a lane between the plots
            } else if ((by == 1 || by == 2) && (bx == 1 || bx == 2) && noise(x / 5, in / 6) % 4 != 0) {
                t = Terrain::House;
            } else if (noise(x / 5, in / 6 + 7) % 5 == 0) {
                t = Terrain::Orchard;
            }
            map.set_terrain(x, y, t);
        }
    }
    for (int32_t y = 0; y < size; ++y) {  // the street from the crossing up through the village and on south
        if (y - river_y(kCrossingX) > kRiverHalf) {
            for (const int32_t x : {kCrossingX, kCrossingX + 1}) {
                if (map.terrain(x, y) != Terrain::House) map.set_terrain(x, y, Terrain::DirtRoad);
            }
        }
    }
    return map;
}

ScenarioSetup setup_donets(World& world) {
    ScenarioSetup setup;
    auto add = [&](const char* group, PlayerId owner, UnitTypeId type, FixedVec2 pos, FixedVec2 facing) {
        const EntityId id = world.spawn_unit(owner, type, pos);
        if (Unit* u = world.unit_for_setup(id)) {
            u->facing = facing;
            u->hull = facing;
        }
        setup.groups[group].push_back(id);
        return id;
    };
    const FixedVec2 south{Fixed{}, Fixed::from_int(1)};
    const FixedVec2 north{Fixed{}, Fixed::from_int(-1)};
    constexpr PlayerId kUa = 0;  // the Democratic axis
    constexpr PlayerId kRu = 1;  // the Authoritarian one
    for (const PlayerId p : {kUa, kRu}) world.set_stock(p, {50, 5000, 5000, 5000, 5000});
    world.upgrade_for_setup(kRu, UpgradeId::SmokeGrenades);

    // --- The left bank: the Authoritarian axis's brigade ---------------------
    const int32_t bank = river_y(kCrossingX) - kRiverHalf - 1;  // the left bank's water's edge at the crossing
    add("parks", kRu, UnitTypeId::PontoonPark, tile_mid(kCrossingX - 3, bank - 3), south);
    add("parks", kRu, UnitTypeId::PontoonPark, tile_mid(kCrossingX + 4, bank - 3), south);
    add("parks2", kRu, UnitTypeId::PontoonPark, tile_mid(kCrossing2X + 3, river_y(kCrossing2X + 3) - kRiverHalf - 5), south);
    for (const int32_t x : {kCrossingX - 6, kCrossingX + 7, kCrossingX + 12}) {
        add("smoke_tanks", kRu, UnitTypeId::Tank, tile_mid(x, river_y(x) - kRiverHalf - 2), south);
    }
    // The column down the road through the woods, nose to tail.
    static constexpr UnitTypeId kColumn[] = {
        UnitTypeId::Mtlb, UnitTypeId::Ifv, UnitTypeId::Ifv, UnitTypeId::Mtlb, UnitTypeId::Ifv, UnitTypeId::Tank,
        UnitTypeId::Ifv, UnitTypeId::Mtlb, UnitTypeId::Ifv, UnitTypeId::Ifv, UnitTypeId::Mtlb, UnitTypeId::Tank,
        UnitTypeId::Ifv, UnitTypeId::Mtlb, UnitTypeId::Truck, UnitTypeId::AmmoTruck, UnitTypeId::FuelTanker,
    };
    for (size_t i = 0; i < std::size(kColumn); ++i) {  // (a tile and three quarters apart)
        const FixedVec2 pos{Fixed::from_ratio(2 * kRoadX + 1 + 2 * static_cast<int32_t>(i % 2), 2),
                            Fixed::from_ratio(4 * (bank - 8) - 7 * static_cast<int32_t>(i), 4)};
        add(i < 6 ? "column_head" : "column_tail", kRu, kColumn[i], pos, south);
    }
    // The assault group waiting in the meadow to go over first.
    for (int i = 0; i < 8; ++i) {
        const UnitTypeId type = i == 3 ? UnitTypeId::MachineGunner : i == 6 ? UnitTypeId::Grenadier : UnitTypeId::Rifleman;
        add("assault", kRu, type, at(2 * (kCrossingX - 10) + (i % 4) * 2, 2 * (bank - 2) - (i / 4) * 2), south);
    }
    // The guns in the fields behind the woods.
    for (int i = 0; i < 4; ++i) {
        const EntityId id = add("ru_guns", kRu, i < 2 ? UnitTypeId::MstaS : UnitTypeId::Howitzer, tile_mid(50 + i * 7, 9), south);
        world.unit_for_setup(id)->deployed = true;
    }

    // --- The right bank: the Democratic axis --------------------------------
    // The paratroopers' trench along the crest over the crossing, its breastwork to the river.
    const int32_t crest = river_y(kCrossingX) + kRiverHalf + kCrestUp + 2;
    std::vector<TilePos> trench;
    for (int32_t x = kCrossingX - 9; x <= kCrossingX + 12; ++x) {
        if (x == kCrossingX || x == kCrossingX + 1) continue;  // (the way up stays open: it's covered from both sides)
        const TilePos t{x, river_y(x) + kRiverHalf + kCrestUp + 2};
        if (world.structure_at(t) || world.map().terrain(t) == Terrain::House) continue;
        if (Structure* s = world.structure_for_setup(world.place_structure(StructureType::Trench, kUa, t, 1, 1))) {
            s->parapet = true;
            s->facing = north;
        }
        trench.push_back(t);
    }
    static constexpr UnitTypeId kParas[] = {UnitTypeId::MachineGunner, UnitTypeId::Rifleman, UnitTypeId::Grenadier,
                                            UnitTypeId::Rifleman, UnitTypeId::Grenadier, UnitTypeId::MachineGunner,
                                            UnitTypeId::Rifleman, UnitTypeId::Grenadier, UnitTypeId::Rifleman,
                                            UnitTypeId::Grenadier, UnitTypeId::Rifleman, UnitTypeId::Rifleman};
    for (size_t i = 0; i < std::size(kParas); ++i) {
        const TilePos t = trench[(i * 2 + 1) % trench.size()];
        add("paras", kUa, kParas[i], tile_center(t), north);
    }
    // In the houses along the village's first street.
    for (int i = 0; i < 4; ++i) add("village", kUa, UnitTypeId::Rifleman, tile_mid(kCrossingX - 6 + i * 4, crest + 5), north);
    // The scouts on the chalk either side, watching the river.
    add("scouts", kUa, UnitTypeId::Scout, tile_mid(36, river_y(36) + kRiverHalf + kCrestUp + 2), north);
    add("scouts", kUa, UnitTypeId::Scout, tile_mid(94, river_y(94) + kRiverHalf + kCrestUp + 2), north);
    // Two tanks back from the crest either side of the way up, covering where it comes out.
    add("ua_tanks", kUa, UnitTypeId::T64BV, tile_mid(kCrossingX - 9, crest + 4), north);
    add("ua_tanks", kUa, UnitTypeId::T64BV, tile_mid(kCrossingX + 11, crest + 4), north);
    // Mortars behind the village; the howitzers far back in the fields.
    for (int i = 0; i < 2; ++i) add("mortars", kUa, UnitTypeId::Mortar, tile_mid(56 + i * 10, crest + 19), north);
    const int32_t size = world.map().height();
    for (int i = 0; i < 4; ++i) {
        const EntityId id = add("m777", kUa, UnitTypeId::M777, tile_mid(54 + i * 6, size - 32), north);
        world.unit_for_setup(id)->deployed = true;
    }
    for (int i = 0; i < 2; ++i) {
        const EntityId id = add("d30", kUa, UnitTypeId::Howitzer, tile_mid(44 + i * 40, size - 34), north);
        world.unit_for_setup(id)->deployed = true;
    }
    // Enough rounds for the whole of it (the guns' ammunition trucks are off the map).
    for (Unit& u : std::vector<Unit>(world.units())) {
        if (Unit* g = world.unit_for_setup(u.id)) g->rounds = std::max(g->rounds, 4 * unit_type(g->type).rounds_capacity);
    }

    // The houses nearest the crossing, for the riflemen in the village.
    std::vector<const Structure*> houses;
    for (const Structure& s : world.structures()) {
        if (s.type == StructureType::House) houses.push_back(&s);
    }
    const FixedVec2 near = tile_mid(kCrossingX, crest + 5);
    std::stable_sort(houses.begin(), houses.end(), [&](const Structure* a, const Structure* b) {
        return (a->center - near).length_sq_raw() < (b->center - near).length_sq_raw();
    });
    for (size_t i = 0; i < houses.size() && i < 3; ++i) setup.groups["houses"].push_back(houses[i]->id);

    // Where things are, for the director.
    setup.points["crossing"] = tile_mid(kCrossingX, river_y(kCrossingX));
    setup.points["crossing2"] = tile_mid(kCrossing2X, river_y(kCrossing2X));
    setup.points["bank"] = tile_mid(kCrossingX, bank - 4);
    setup.points["road"] = tile_mid(kRoadX, bank - 22);
    setup.points["crest"] = tile_mid(kCrossingX, crest);
    setup.points["village"] = tile_mid(kCrossingX + 4, crest + 7);
    setup.points["guns_ru"] = tile_mid(60, 9);
    setup.points["m777"] = tile_mid(63, size - 32);
    return setup;
}

}  // namespace engine
