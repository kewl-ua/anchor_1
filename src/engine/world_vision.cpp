// The vision half of World: the fog of war (who sees which tiles), cover
// (who hides in forests and houses) and spotting.

#include <algorithm>

#include "engine/heights.h"
#include "engine/world.h"

namespace engine {

namespace {

// Looking at a tile means seeing a man standing in its middle.
constexpr Fixed kLookHeight = Fixed::from_ratio(3, 10);
// A look may graze this many quarter-tile steps of foliage and still get
// through: the corner of a tree line, not the tree line itself.
constexpr int32_t kFoliageGrace = 1;
// Every level of height above the plain adds this many tiles of view.
constexpr int32_t kSightPerLevel = 1;
// Players' buildings look around too; a site under construction barely.
constexpr int32_t kHeadquartersSight = 8;
constexpr int32_t kBuildingSight = 6;
constexpr int32_t kSiteSight = 3;
// Enough sight cache entries for every tile an army stands on; beyond that
// it is simply dropped and refilled.
constexpr size_t kSightCacheLimit = 4096;

// How an observer's eyes sit: on foot, in a turret, at a window.
Fixed eye_height(const UnitTypeDef& def) { return def.vehicle ? kVehicleTop : kInfantryTop; }

int32_t structure_sight(const Structure& s) {
    const StructureType role = role_of(s);
    if (role == StructureType::House || role == StructureType::Bridge || role == StructureType::Apartment ||
        role == StructureType::CellTower || role == StructureType::GasStation || role == StructureType::Elevator) {
        return 0;  // the garrison looks instead
    }
    if (is_fieldwork(s.type) || s.type == StructureType::Dugout) return 0;           // just holes in the ground
    if (!s.built) return kSiteSight;
    return s.type == StructureType::Headquarters ? kHeadquartersSight : kBuildingSight;
}

// Cover: forests, the trails under their canopy, trenches and foxholes, and
// the inside of a house or dugout.
bool in_cover(const TileMap& map, const Unit& u) {
    if (u.airborne) return false;  // up in the open sky
    if (u.inside || u.camouflaged) return true;
    const Terrain t = map.terrain_at(u.pos);
    if (t == Terrain::Crops || t == Terrain::Swamp || t == Terrain::Crater) return !unit_type(u.type).vehicle;
    return t == Terrain::Forest || t == Terrain::Trail || t == Terrain::Trench || t == Terrain::Foxhole ||
           t == Terrain::GunPit;
}

}  // namespace

// Scouts take up an observation post and watch the sector towards the point.
void World::apply_observe(const Command& cmd) {
    for (EntityId id : cmd.units) {
        Unit* u = find_unit_mut(id);
        if (!u || u->owner != cmd.player || unit_type(u->type).sector_range.raw == 0) continue;
        leave_structure(*u);
        u->order = Order::Observe;
        u->order_point = clamp_to_map(cmd.target, Fixed{});
        const FixedVec2 dir = u->order_point - u->pos;
        if (dir.x.raw != 0 || dir.y.raw != 0) u->facing = dir;
        u->order_target = 0;
        u->engaged = 0;
        u->order_path.reset();
        u->chase_path.reset();
        u->speed_cap = Fixed{};
    }
}

bool World::sees(PlayerId player, const Structure& s) const {
    if (s.owner == player) return true;
    return std::any_of(s.tiles.begin(), s.tiles.end(), [&](const TilePos& t) { return visible(player, t); });
}

void World::begin_tick() {
    if (!vision_ready_ || tick_ % kVisionInterval == 0) update_vision();
}

Fixed World::ground_at(FixedVec2 p) const {
    const TilePos t = map_.clamp_tile(tile_of(p));
    const Fixed fx = clamp(p.x - Fixed::from_int(t.x), Fixed{}, Fixed::from_int(1));
    const Fixed fy = clamp(p.y - Fixed::from_int(t.y), Fixed{}, Fixed::from_int(1));
    const size_t stride = static_cast<size_t>(map_.width() + 1);
    const size_t i = static_cast<size_t>(t.y) * stride + static_cast<size_t>(t.x);
    const Fixed c00 = corner_heights_[i];
    const Fixed c10 = corner_heights_[i + 1];
    const Fixed c01 = corner_heights_[i + stride];
    const Fixed c11 = corner_heights_[i + stride + 1];
    const Fixed top = c00 + (c10 - c00) * fx;
    const Fixed bottom = c01 + (c11 - c01) * fx;
    return top + (bottom - top) * fy;
}

// Straight from the eyes to a man standing on the target tile, in quarter-tile
// steps, like a line of fire: a hill, a house, a rock or more than a sliver
// of forest in between hides it. The observer's own tile (the edge of the
// forest he stands in, the house he looks out of) doesn't.
bool World::line_of_sight(FixedVec2 from, Fixed eye, TilePos target, EntityId own_structure) const {
    const FixedVec2 to = tile_center(target);
    const Fixed to_height = ground_at(to) + kLookHeight;
    const FixedVec2 d = to - from;
    const int32_t samples = (d.length() * 4).to_int();
    const TilePos from_tile = tile_of(from);
    int32_t foliage = 0;
    for (int32_t i = 1; i < samples; ++i) {
        const Fixed t = Fixed::from_ratio(i, samples);
        const FixedVec2 p = from + d * t;
        const TilePos tile = tile_of(p);
        if (tile == target) break;         // the rest of the way is on the tile itself
        if (tile == from_tile) continue;   // still on our own tile
        if (!smokes_.empty() && in_smoke(p)) return false;  // nothing is seen into smoke or through it
        const Fixed h = eye + (to_height - eye) * t;
        const Fixed ground = ground_at(p);
        if (h < ground) return false;
        switch (map_.terrain(tile)) {
            case Terrain::Forest:
                if (h < ground + kTreeHeight && ++foliage > kFoliageGrace) return false;
                break;
            case Terrain::House:
                if (h < ground + kHouseHeight && structure_id_at(tile) != own_structure) return false;
                break;
            case Terrain::Apartment:
                if (h < ground + kApartmentHeight && structure_id_at(tile) != own_structure) return false;
                break;
            case Terrain::Elevator:
                if (h < ground + kElevatorHeight && structure_id_at(tile) != own_structure) return false;
                break;
            case Terrain::GasStation:
                if (h < ground + kHouseHeight && structure_id_at(tile) != own_structure) return false;
                break;
            case Terrain::Building:
                if (h < ground + kBuildingHeight && structure_id_at(tile) != own_structure) return false;
                break;
            case Terrain::Rock:
                if (h < ground + kRockHeight) return false;
                break;
            case Terrain::Pillbox:
                if (h < ground + kRockHeight && structure_id_at(tile) != own_structure) return false;
                break;
            default:
                break;
        }
    }
    return true;
}

const std::vector<int32_t>& World::sight_from(TilePos tile, int32_t radius, Fixed eye, EntityId own_structure) {
    if (map_.revision() != sight_cache_map_revision_ || sight_cache_.size() > kSightCacheLimit) {
        sight_cache_.clear();
        sight_cache_map_revision_ = map_.revision();
    }
    // radius: 6 bits, tile: 18 bits (up to 480 x 480), eye: 18 bits, building: the rest.
    const uint64_t key = (static_cast<uint64_t>(own_structure) << 42) |
                         (static_cast<uint64_t>(static_cast<uint32_t>(eye.raw) & 0x3FFFF) << 24) |
                         (static_cast<uint64_t>(tile.y * map_.width() + tile.x) << 6) |
                         static_cast<uint64_t>(std::min(radius, 63));
    auto [it, inserted] = sight_cache_.try_emplace(key);
    if (!inserted) return it->second;

    // Every tile whose center is within `radius` of ours, if the line of sight is clear.
    const FixedVec2 from = tile_center(tile);
    const Fixed eye_abs = ground_at(from) + eye;
    const int64_t r_sq = static_cast<int64_t>(radius) * radius;
    for (int32_t y = tile.y - radius; y <= tile.y + radius; ++y) {
        for (int32_t x = tile.x - radius; x <= tile.x + radius; ++x) {
            const int64_t dx = x - tile.x;
            const int64_t dy = y - tile.y;
            if (dx * dx + dy * dy > r_sq || !map_.contains_tile(x, y)) continue;
            if (line_of_sight(from, eye_abs, {x, y}, own_structure)) it->second.push_back(y * map_.width() + x);
        }
    }
    return it->second;
}

// Someone of `player` close enough to make out a man in cover (a scout from
// farther, a moving man from farther still, a scout in cover only from up
// close), or observation posts watching him: the more sectors cover him,
// the farther out he is made out.
bool World::spotted(PlayerId player, const Unit& target) const {
    int32_t percent = 100;
    if (target.moving) percent = percent * kSpotMovingPercent / 100;
    if (unit_type(target.type).stealthy) percent = percent * kSpotStealthyPercent / 100;
    for (const Unit& o : units_) {
        if (o.owner != player) continue;
        Fixed detection = unit_type(o.type).detection;
        if (unit_type(o.type).sector_range.raw > 0 && has_upgrade(player, UpgradeId::Optics)) detection += kOpticsDetection;
        const Fixed range = detection * percent / 100;
        if ((o.pos - target.pos).length_sq_raw() <= square_raw(range)) return true;
    }

    const TilePos tile = map_.clamp_tile(tile_of(target.pos));
    const int32_t index = tile.y * map_.width() + tile.x;
    int32_t covering = 0;
    uint64_t nearest_sq = UINT64_MAX;
    for (const Sector& s : sectors_) {
        if (s.owner != player || !std::binary_search(s.tiles.begin(), s.tiles.end(), index)) continue;
        ++covering;
        nearest_sq = std::min(nearest_sq, (s.from - target.pos).length_sq_raw());
    }
    if (covering == 0) return false;
    const Fixed range = kSectorDetection * covering * percent / 100;
    return nearest_sq <= square_raw(range);
}

void World::update_vision() {
    vision_ready_ = true;
    ++vision_revision_;
    const size_t tiles = static_cast<size_t>(map_.width() * map_.height());

    // Only players with something on the map get a fog grid.
    std::array<bool, kMaxPlayers> present{};
    for (const Unit& u : units_) {
        if (u.owner < kMaxPlayers) present[u.owner] = true;
    }
    for (const Structure& s : structures_) {
        if (s.owner < kMaxPlayers) present[s.owner] = true;
    }
    for (size_t p = 0; p < kMaxPlayers; ++p) {
        if (!present[p]) continue;
        visible_[p].assign(tiles, 0);
        explored_[p].resize(tiles, 0);
    }

    auto look = [&](PlayerId player, FixedVec2 pos, Fixed sight, Fixed eye, EntityId own_structure) {
        const TilePos tile = map_.clamp_tile(tile_of(pos));
        const int32_t radius = sight.to_int() + kSightPerLevel * map_.elevation(tile.x, tile.y);
        if (radius <= 0) return;
        std::vector<uint8_t>& vis = visible_[player];
        std::vector<uint8_t>& seen = explored_[player];
        for (int32_t i : sight_from(tile, radius, eye, own_structure)) {
            vis[static_cast<size_t>(i)] = 1;
            seen[static_cast<size_t>(i)] = 1;
        }
    };
    for (const Unit& u : units_) {
        if (u.owner >= kMaxPlayers) continue;
        const UnitTypeDef& def = unit_type(u.type);
        if (const Structure* s = find_structure(u.inside)) {
            if (s->type == StructureType::Dugout) continue;  // underground: sees nothing
            Fixed sight = def.sight;
            if (s->type == StructureType::Apartment) sight += Fixed::from_int(kApartmentSightBonus);
            if (s->type == StructureType::CellTower) sight += Fixed::from_int(kTowerSightBonus);
            if (s->type == StructureType::Elevator) sight += Fixed::from_int(kElevatorSightBonus);
            look(u.owner, s->center, sight, window_height(s->id), s->id);
        } else {
            look(u.owner, u.pos, def.sight, u.airborne ? kFlightHeight : eye_height(def), 0);
        }
    }
    for (const Structure& s : structures_) {
        if (s.owner >= kMaxPlayers) continue;
        look(s.owner, s.center, Fixed::from_int(structure_sight(s)), kWindowHeight, s.id);
    }

    // Observation posts: far out, but only within their 90 degree sector.
    sectors_.clear();
    for (const Unit& u : units_) {
        const UnitTypeDef& def = unit_type(u.type);
        if (u.owner >= kMaxPlayers || u.order != Order::Observe || u.inside || def.sector_range.raw == 0) continue;
        const TilePos tile = map_.clamp_tile(tile_of(u.pos));
        const FixedVec2 from = tile_center(tile);
        // In 1/256 of a tile, so the products below stay well inside 64 bits.
        const int64_t dx = (u.order_point - from).x.raw >> 8;
        const int64_t dy = (u.order_point - from).y.raw >> 8;
        if (dx == 0 && dy == 0) continue;
        const int32_t optics = has_upgrade(u.owner, UpgradeId::Optics) ? kOpticsSectorTiles : 0;
        const int32_t radius = def.sector_range.to_int() + optics + kSightPerLevel * map_.elevation(tile.x, tile.y);
        Sector sector{u.owner, from, {}, u.order_point - from, radius};
        for (int32_t i : sight_from(tile, radius, eye_height(def), 0)) {
            const int64_t tx = i % map_.width() - tile.x;
            const int64_t ty = i / map_.width() - tile.y;
            // Within 45 degrees of the direction: cos^2 >= 1/2.
            const int64_t dot = dx * tx + dy * ty;
            if (dot <= 0 || 2 * dot * dot < (dx * dx + dy * dy) * (tx * tx + ty * ty)) continue;
            sector.tiles.push_back(i);
            visible_[u.owner][static_cast<size_t>(i)] = 1;
            explored_[u.owner][static_cast<size_t>(i)] = 1;
        }
        sectors_.push_back(std::move(sector));
    }

    // Who sees whom: a man in the open is seen when his tile is in view, one
    // in cover only when he has just fired. Up close, anyone is made out,
    // even through the trees.
    find_mines();
    take_bearings();
    for (Unit& u : units_) {
        u.seen_by = 0;
        const Structure* house = find_structure(u.inside);
        const TilePos tile = map_.clamp_tile(tile_of(u.pos));
        const Tick reveal = weapon_of(u).indirect ? kGunRevealTicks : kRevealTicks;
        const bool fired = u.last_shot_tick != kNeverFired && tick_ - u.last_shot_tick < reveal;
        const bool hidden = in_cover(map_, u) && !fired;
        for (size_t p = 0; p < kMaxPlayers; ++p) {
            const auto player = static_cast<PlayerId>(p);
            if (!present[p] || player == u.owner) continue;
            // A garrison is where its house is, and the house is seen if any wall is.
            const bool in_view = house ? sees(player, *house) : visible(player, tile);
            if ((in_view && !hidden) || spotted(player, u) || betrayed(player, u) || fixed_by(player, u) ||
                sky_watch(player, u)) {
                u.seen_by = static_cast<uint8_t>(u.seen_by | (1u << p));
            }
        }
    }
}

}  // namespace engine
