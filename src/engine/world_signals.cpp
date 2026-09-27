// The electronic warfare half of World: radios on the air or keeping
// silence, couriers carrying orders to the silent ones, relays, and
// direction finders taking bearings and fixing radios.

#include <algorithm>

#include "engine/world.h"

namespace engine {

bool World::in_touch(const Unit& u) const {
    if (!u.silent) return true;
    for (const Structure& s : structures_) {
        if (s.owner != u.owner || s.type != StructureType::Headquarters || !s.built) continue;
        if ((s.center - u.pos).length_sq_raw() <= square_raw(kHeadquartersRelay)) return true;
    }
    for (const Structure& s : structures_) {
        // Held: someone of ours up it (an empty tower belongs to nobody).
        if (s.type != StructureType::CellTower || s.owner != u.owner) continue;
        if ((s.center - u.pos).length_sq_raw() <= square_raw(kTowerRelay)) return true;
    }
    for (const Unit& r : units_) {
        const Fixed reach = unit_type(r.type).relay_range;
        if (r.owner != u.owner || r.id == u.id || r.silent || reach.raw == 0) continue;
        if ((r.pos - u.pos).length_sq_raw() <= square_raw(reach)) return true;
    }
    return false;
}

// Couriers that got there hand over their orders, oldest first.
void World::update_couriers() {
    for (size_t i = 0; i < couriers_.size();) {
        if (couriers_[i].arrives > tick_) {
            ++i;
            continue;
        }
        const Command cmd = std::move(couriers_[i].cmd);
        couriers_.erase(couriers_.begin() + static_cast<std::ptrdiff_t>(i));
        deliver(cmd);
    }
}

// Every set-up DF station takes a bearing on every enemy radio on the air in
// its reach. Radio waves go over hills and through forests: no line of sight.
void World::take_bearings() {
    bearings_.clear();
    for (const Unit& station : units_) {
        const Fixed reach = unit_type(station.type).df_range;
        if (reach.raw == 0 || !station.deployed || station.owner >= kMaxPlayers) continue;
        for (const Unit& radio : units_) {
            if (radio.owner == station.owner || radio.silent || !unit_type(radio.type).emitter) continue;
            const FixedVec2 dir = radio.pos - station.pos;
            if (dir.length_sq_raw() > square_raw(reach)) continue;
            bearings_.push_back({station.owner, station.id, radio.id, station.pos, dir});
        }
    }
}

bool World::fixed_by(PlayerId player, const Unit& u) const {
    for (size_t i = 0; i < bearings_.size(); ++i) {
        const Bearing& a = bearings_[i];
        if (a.owner != player || a.target != u.id) continue;
        for (size_t j = i + 1; j < bearings_.size(); ++j) {
            const Bearing& b = bearings_[j];
            if (b.owner != player || b.target != u.id) continue;
            // In 1/64 of a tile: within a DF station's reach the products fit in 64 bits.
            const int64_t ax = a.dir.x.raw >> 10;
            const int64_t ay = a.dir.y.raw >> 10;
            const int64_t bx = b.dir.x.raw >> 10;
            const int64_t by = b.dir.y.raw >> 10;
            const int64_t cross = ax * by - ay * bx;
            const int64_t a_sq = ax * ax + ay * ay;
            const int64_t b_sq = bx * bx + by * by;
            if (cross * cross * 1000 >= kFixSinSqPermille * a_sq * b_sq) return true;
        }
    }
    return false;
}

}  // namespace engine
