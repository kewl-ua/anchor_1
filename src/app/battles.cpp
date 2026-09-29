#include "app/battles.h"

#include <algorithm>
#include <cmath>

#include "engine/unit_types.h"
#include "render/convert.h"

namespace app::battles {

namespace {

engine::Tick at_second(float s) { return static_cast<engine::Tick>(std::lround(s * engine::kTicksPerSecond)); }

class Director {
public:
    explicit Director(const engine::ScenarioSetup& setup) : setup_(setup) {}

    const std::vector<engine::EntityId>& group(const char* name) const {
        static const std::vector<engine::EntityId> kNone;
        const auto it = setup_.groups.find(name);
        return it == setup_.groups.end() ? kNone : it->second;
    }
    engine::FixedVec2 point(const char* name) const {
        const auto it = setup_.points.find(name);
        return it == setup_.points.end() ? engine::FixedVec2{} : it->second;
    }
    Vector2 look(const char* name, float dx = 0.0f, float dy = 0.0f) const {
        const Vector2 p = render::to_vector2(point(name));
        return {p.x + dx, p.y + dy};
    }
    static engine::FixedVec2 off(engine::FixedVec2 p, float dx, float dy) {
        return p + render::to_fixed_vec2({dx, dy});
    }

    void order(float s, engine::PlayerId player, engine::Command cmd, const std::vector<engine::EntityId>& units) {
        cmd.player = player;
        cmd.units = units;
        script_.orders.push_back({at_second(s), std::move(cmd)});
    }
    void move(float s, engine::PlayerId p, const std::vector<engine::EntityId>& units, engine::FixedVec2 to, bool fight = false) {
        order(s, p, {.type = fight ? engine::CommandType::AttackMove : engine::CommandType::Move, .target = to}, units);
    }
    void fire(float s, engine::PlayerId p, const std::vector<engine::EntityId>& units, engine::FixedVec2 at) {
        order(s, p, {.type = engine::CommandType::AttackGround, .target = at}, units);
    }
    // Each gun of them at its own point along a line (a sheaf), from `a` to `b`.
    void sheaf(float s, engine::PlayerId p, const std::vector<engine::EntityId>& guns, engine::FixedVec2 a, engine::FixedVec2 b) {
        for (size_t i = 0; i < guns.size(); ++i) {
            const engine::Fixed t = guns.size() > 1 ? engine::Fixed::from_ratio(static_cast<int32_t>(i), static_cast<int32_t>(guns.size() - 1))
                                                    : engine::Fixed::from_ratio(1, 2);
            fire(s, p, {guns[i]}, a + (b - a) * t);
        }
    }
    void stop(float s, engine::PlayerId p, const std::vector<engine::EntityId>& units) {
        order(s, p, {.type = engine::CommandType::Stop}, units);
    }
    void skill(float s, engine::PlayerId p, const std::vector<engine::EntityId>& units, engine::AbilityId a, engine::FixedVec2 at) {
        order(s, p, {.type = engine::CommandType::Ability, .target = at, .ability = static_cast<uint8_t>(a)}, units);
    }

    Script& script() { return script_; }

private:
    const engine::ScenarioSetup& setup_;
    Script script_;
};

float ease(float t) { return t * t * (3.0f - 2.0f * t); }

}  // namespace

Script donets(const engine::ScenarioSetup& setup) {
    Director d(setup);
    constexpr engine::PlayerId kUa = 0;
    constexpr engine::PlayerId kRu = 1;
    using engine::AbilityId;
    const engine::FixedVec2 crossing = d.point("crossing");
    const engine::FixedVec2 crossing2 = d.point("crossing2");
    const engine::FixedVec2 crest = d.point("crest");
    const engine::FixedVec2 bank = d.point("bank");
    const engine::FixedVec2 road = d.point("road");
    const engine::FixedVec2 village = d.point("village");
    const std::vector<engine::EntityId>& m777 = d.group("m777");
    const std::vector<engine::EntityId>& d30 = d.group("d30");
    const std::vector<engine::EntityId>& mortars = d.group("mortars");
    const std::vector<engine::EntityId>& ru_guns = d.group("ru_guns");
    const std::vector<engine::EntityId>& head = d.group("column_head");
    // The column's tail: the nearer half down to the water after the head, the rest waiting on the road in the woods.
    const std::vector<engine::EntityId>& tail = d.group("column_tail");
    const std::vector<engine::EntityId> near_tail(tail.begin(), tail.begin() + static_cast<long>(tail.size() / 2));
    const std::vector<engine::EntityId> far_tail(tail.begin() + static_cast<long>(tail.size() / 2), tail.end());

    // The right bank takes up its places: the scouts' posts over the river,
    // the paratroopers along their trench, riflemen into the houses.
    d.skill(0.1f, kUa, d.group("scouts"), AbilityId::BuildPost, Director::off(crossing, 0.0f, -12.0f));
    d.order(0.1f, kUa, {.type = engine::CommandType::ManWorks, .target = crest}, d.group("paras"));
    const std::vector<engine::EntityId>& houses = d.group("houses");
    const std::vector<engine::EntityId>& village_men = d.group("village");
    for (size_t i = 0; i < village_men.size() && !houses.empty(); ++i) {
        d.order(0.1f, kUa, {.type = engine::CommandType::Garrison, .target_unit = houses[i % houses.size()]}, {village_men[i]});
    }

    // The left bank: the guns on the chalk face under the crest and on the fields behind the
    // village (by the map, blind); smoke over the water; the parks at it.
    d.sheaf(2.0f, kRu, ru_guns, Director::off(crest, -8.0f, -5.5f), Director::off(crest, 9.0f, -5.0f));
    d.skill(6.0f, kRu, d.group("smoke_tanks"), AbilityId::Smoke, crossing);
    d.move(9.0f, kRu, d.group("smoke_tanks"), Director::off(bank, 0.5f, -9.0f));  // (back up the road, out of the way)
    d.skill(7.0f, kRu, d.group("parks"), AbilityId::LayPontoon, crossing);
    d.move(9.0f, kRu, d.group("assault"), Director::off(bank, -2.0f, 2.0f));
    d.sheaf(16.0f, kRu, ru_guns, Director::off(village, -6.0f, 19.0f), Director::off(village, 8.0f, 20.0f));
    // Over: the head of the column and the assault group for the crest; the
    // rest down the road to the water, bunching up behind.
    d.move(21.0f, kRu, head, Director::off(crest, 0.5f, 4.0f), true);
    d.move(22.0f, kRu, d.group("assault"), Director::off(crest, -2.0f, 0.0f), true);
    d.move(24.0f, kRu, near_tail, Director::off(bank, 0.0f, 1.0f));

    // The right bank's guns: the bridge; the crowd on the bank; the way up out of it.
    d.sheaf(21.5f, kUa, m777, Director::off(crossing, 0.0f, -1.0f), Director::off(crossing, 0.0f, 1.0f));
    d.sheaf(24.0f, kUa, d30, Director::off(bank, -3.0f, 0.0f), Director::off(bank, 3.0f, 0.0f));
    d.fire(31.0f, kUa, mortars, Director::off(crossing, 0.5f, -4.5f));  // (the far end: well off our own trench)
    // The bridge down: two guns walk their fire up the column in the woods,
    // two keep the bank under it.
    d.sheaf(54.0f, kUa, {m777[0], m777[1]}, Director::off(road, 0.5f, -2.0f), Director::off(road, 0.5f, -9.0f));
    d.sheaf(54.0f, kUa, {m777[2], m777[3]}, Director::off(bank, -4.0f, 0.0f), Director::off(bank, 5.0f, 1.0f));

    // The second try downstream: the last park, and the rest of the column along the bank to it.
    d.skill(66.0f, kRu, d.group("parks2"), AbilityId::LayPontoon, crossing2);
    d.move(70.0f, kRu, tail, Director::off(crossing2, -2.0f, -6.0f));
    d.move(84.0f, kRu, tail, Director::off(crossing2, 0.5f, 9.0f), true);
    d.sheaf(88.0f, kUa, m777, Director::off(crossing2, 0.0f, -1.5f), Director::off(crossing2, 0.0f, 1.5f));
    d.sheaf(90.0f, kUa, d30, Director::off(crossing2, -3.0f, -5.0f), Director::off(crossing2, 3.0f, -5.0f));
    d.sheaf(118.0f, kUa, m777, Director::off(crossing2, -5.0f, -5.0f), Director::off(bank, 3.0f, 0.0f));
    // Those who got over: the tanks come up to the crest to finish them.
    d.move(124.0f, kUa, d.group("ua_tanks"), Director::off(crest, 0.0f, 2.0f), true);
    d.fire(100.0f, kUa, mortars, Director::off(crossing2, 0.5f, -5.0f));
    // It's over: the guns fall silent.
    d.stop(170.0f, kUa, m777);
    d.stop(170.0f, kUa, d30);
    d.stop(170.0f, kUa, mortars);
    d.stop(170.0f, kRu, ru_guns);

    Script& s = d.script();
    std::stable_sort(s.orders.begin(), s.orders.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    s.end = at_second(190.0f);
    s.camera = {
        {0.0f, d.look("crossing", -2.0f, -2.0f), 1.25f},
        {5.0f, d.look("bank", 0.0f, 2.0f), 1.9f},
        {18.0f, d.look("crossing", 0.0f, 0.0f), 2.1f},
        {21.5f, d.look("m777", 0.0f, -2.0f), 1.9f},
        {25.5f, d.look("crossing", 0.0f, -1.0f), 1.9f},
        {52.0f, d.look("bank", 0.0f, -1.0f), 1.6f},
        {60.0f, d.look("road", 0.0f, -6.0f), 1.6f},
        {68.0f, d.look("crossing2", -1.0f, -4.0f), 1.8f},
        {86.0f, d.look("crossing2", 0.0f, -1.0f), 2.0f},
        {110.0f, d.look("crossing2", -4.0f, -3.0f), 1.5f},
        {122.0f, d.look("crest", 2.0f, 1.0f), 1.8f},
        {150.0f, d.look("crest", 4.0f, -2.0f), 1.5f},
        {170.0f, d.look("crossing", 6.0f, -3.0f), 1.0f},
        {190.0f, d.look("crossing", 6.0f, -3.0f), 1.0f},
    };
    s.captions = {
        {0.0f, 7.0f, "Северский Донец у Белогоровки, май 2022", "74-я омсбр ВС РФ готовит переправу на правый берег"},
        {7.0f, 18.0f, "Артподготовка и дымовая завеса", "Инженеры наводят понтонный мост"},
        {18.0f, 21.5f, "Мост наведён", "Колонна идёт на переправу, остальные скапливаются у берега"},
        {21.5f, 25.5f, "Разведка на меловых высотах наводит артиллерию", "Бьют M777 17-й отдельной танковой бригады"},
        {25.5f, 52.0f, "Удар по переправе", "Звенья моста рвутся, техника на нём уходит под воду"},
        {52.0f, 66.0f, "Огонь переносится на колонну", "Техника зажата на единственной дороге через лес"},
        {66.0f, 88.0f, "Вторая попытка", "Новый мост ниже по течению"},
        {88.0f, 120.0f, "Его постигает та же участь", ""},
        {120.0f, 160.0f, "Правый берег удержан", "Десантники 80-й одшбр и танки 30-й омбр на гребне над рекой"},
        {160.0f, 190.0f, "Итог боёв 5–13 мая 2022 года",
         "потеряно более 80 единиц техники и около 485 человек из 550 (оценка ISW)"},
    };
    return s;
}

CameraKey camera_at(const Script& script, float seconds) {
    const std::vector<CameraKey>& keys = script.camera;
    if (keys.empty()) return {};
    if (seconds <= keys.front().at) return keys.front();
    for (size_t i = 1; i < keys.size(); ++i) {
        if (seconds > keys[i].at) continue;
        const CameraKey& a = keys[i - 1];
        const CameraKey& b = keys[i];
        // Held still for all but the last few seconds before the next key, then over to it;
        // far off, a cut at the key.
        if (std::hypot(b.look.x - a.look.x, b.look.y - a.look.y) > 20.0f) return seconds < b.at ? a : b;
        const float span = std::min(3.0f, b.at - a.at);
        const float t = ease(std::clamp((seconds - (b.at - span)) / std::max(0.01f, span), 0.0f, 1.0f));
        return {seconds, {a.look.x + (b.look.x - a.look.x) * t, a.look.y + (b.look.y - a.look.y) * t}, a.zoom + (b.zoom - a.zoom) * t};
    }
    return keys.back();
}

}  // namespace app::battles
