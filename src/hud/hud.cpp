#include "theme/input.h"
#include "hud/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "engine/simulation.h"
#include "theme/palette.h"

namespace hud {

namespace {

constexpr float kTopBarHeight = 28.0f;
constexpr float kPanelWidth = 840.0f;
constexpr float kPanelHeight = 140.0f;
constexpr float kPadding = 12.0f;
constexpr int kFontSize = 18;
constexpr int kSmallFontSize = 16;
constexpr int kCardFontSize = 14;  // the lines of a unit or building card
// The command grid on the left of the bottom panel.
constexpr float kGridWidth = 376.0f;
constexpr float kGridGap = 4.0f;

// The minimap is a 2:1 diamond, like the map itself in isometric view.
constexpr int kMinimapWidth = 260;
constexpr int kMinimapHeight = 130;
constexpr float kMinimapPadding = 8.0f;

void draw_panel(Rectangle r) {
    DrawRectangleRec(r, theme::kPanel);
    DrawRectangleLinesEx(r, 1.0f, theme::kPanelBorder);
}

void draw_text(const char* text, float x, float y, int size, Color color) {
    DrawText(text, static_cast<int>(x), static_cast<int>(y), size, color);
}

float to_float(engine::Fixed f) { return static_cast<float>(f.raw) / engine::Fixed::kOneRaw; }

Color shade(Color c, float k) {
    auto channel = [k](unsigned char v) {
        return static_cast<unsigned char>(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f));
    };
    return {channel(c.r), channel(c.g), channel(c.b), c.a};
}

// Minimap-local pixel <-> ground tile, for a map of w x h tiles.
Vector2 minimap_pixel_to_ground(Vector2 px, float w, float h) {
    const float diff = px.x / kMinimapWidth * (w + h) - h;  // x - y
    const float sum = px.y / kMinimapHeight * (w + h);      // x + y
    return {(sum + diff) * 0.5f, (sum - diff) * 0.5f};
}

// Whole seconds until a future tick, rounded up.
int seconds_until(const engine::World& world, engine::Tick when) {
    const engine::Tick left = when > world.tick() ? when - world.tick() : 0;
    return static_cast<int>((left + engine::kTicksPerSecond - 1) / engine::kTicksPerSecond);
}

// The largest font (down to 10) at which the text fits the width.
int fitting_font(const char* text, int size, float width) {
    while (size > 10 && static_cast<float>(MeasureText(text, size)) > width) --size;
    return size;
}

// "150 mat. 50 fuel", or empty if it's free.
void format_price(char (&out)[96], const engine::Stock& cost, bool long_names) {
    static constexpr const char* kShortNames[] = {"men", "food", "mat.", "ammo", "fuel"};
    out[0] = 0;
    size_t used = 0;
    for (size_t i = 0; i < engine::kResourceCount && used < sizeof(out); ++i) {
        if (cost[i] == 0) continue;
        const char* name = long_names ? engine::resource_name(static_cast<engine::Resource>(i)) : kShortNames[i];
        used += static_cast<size_t>(std::snprintf(out + used, sizeof(out) - used, "%d %s  ", cost[i], name));
    }
}

}  // namespace

Hud::~Hud() {
    if (minimap_texture_.id != 0) UnloadTexture(minimap_texture_);
}

Hud::Layout Hud::layout() {
    const auto w = static_cast<float>(GetScreenWidth());
    const auto h = static_cast<float>(GetScreenHeight());

    const Rectangle minimap_panel{w - kMinimapWidth - 2 * kMinimapPadding - kPadding,
                                  h - kMinimapHeight - 2 * kMinimapPadding - kPadding,
                                  kMinimapWidth + 2 * kMinimapPadding, kMinimapHeight + 2 * kMinimapPadding};
    const Rectangle minimap{minimap_panel.x + kMinimapPadding, minimap_panel.y + kMinimapPadding, kMinimapWidth,
                            kMinimapHeight};

    // The command panel is centered in the space left of the minimap: the
    // command grid on its left, the card of what is selected on its right.
    const float free_w = minimap_panel.x - 2 * kPadding;
    const float panel_w = std::max(0.0f, std::min(kPanelWidth, free_w));
    const Rectangle bottom_panel{kPadding + (free_w - panel_w) * 0.5f, h - kPanelHeight - kPadding, panel_w,
                                 kPanelHeight};
    const Rectangle inner{bottom_panel.x + kPadding, bottom_panel.y + kPadding, bottom_panel.width - 2 * kPadding,
                          bottom_panel.height - 2 * kPadding};
    const float grid_w = std::min(kGridWidth, inner.width * 0.5f);
    const Rectangle grid{inner.x, inner.y, grid_w, inner.height};
    const Rectangle info{inner.x + grid_w + kPadding, inner.y, std::max(0.0f, inner.width - grid_w - kPadding),
                         inner.height};

    return {{0, 0, w, kTopBarHeight}, bottom_panel, grid, info, minimap_panel, minimap};
}

bool Hud::captures_point(Vector2 p) const {
    const Layout l = layout();
    return CheckCollisionPointRec(p, l.top_bar) || CheckCollisionPointRec(p, l.bottom_panel) ||
           CheckCollisionPointRec(p, l.minimap_panel);
}

void Hud::update(const engine::World& world, engine::PlayerId viewer, bool reveal) {
    const engine::TileMap& map = world.map();
    const size_t pixels = static_cast<size_t>(kMinimapWidth * kMinimapHeight);
    if (minimap_texture_.id == 0 || map.width() != map_width_ || map.height() != map_height_) {
        map_width_ = map.width();
        map_height_ = map.height();
        if (minimap_texture_.id != 0) UnloadTexture(minimap_texture_);
        Image image = GenImageColor(kMinimapWidth, kMinimapHeight, BLANK);
        minimap_texture_ = LoadTextureFromImage(image);
        UnloadImage(image);

        // Tile under every minimap pixel, (-1, -1) outside the map.
        pixel_tile_.assign(pixels, {-1, -1});
        for (int py = 0; py < kMinimapHeight; ++py) {
            for (int px = 0; px < kMinimapWidth; ++px) {
                const Vector2 g = minimap_pixel_to_ground({px + 0.5f, py + 0.5f}, static_cast<float>(map_width_),
                                                          static_cast<float>(map_height_));
                const engine::TilePos t{static_cast<int32_t>(std::floor(g.x)), static_cast<int32_t>(std::floor(g.y))};
                if (map.contains(t)) pixel_tile_[static_cast<size_t>(py * kMinimapWidth + px)] = t;
            }
        }
        // Higher ground lighter; borders between elevation levels are drawn
        // as contour lines, so the heights worth fighting over stand out.
        auto level = [&](int px, int py) {
            const engine::TilePos t = pixel_tile_[static_cast<size_t>(py * kMinimapWidth + px)];
            return map.contains(t) ? map.elevation(t.x, t.y) : -1;
        };
        pixel_light_.assign(pixels, 1.0f);
        for (int py = 0; py < kMinimapHeight; ++py) {
            for (int px = 0; px < kMinimapWidth; ++px) {
                const int l = level(px, py);
                const bool contour = (px > 0 && level(px - 1, py) >= 0 && level(px - 1, py) != l) ||
                                     (py > 0 && level(px, py - 1) >= 0 && level(px, py - 1) != l);
                pixel_light_[static_cast<size_t>(py * kMinimapWidth + px)] =
                    contour ? 0.6f : 0.85f + 0.12f * static_cast<float>(l);
            }
        }
        seen_terrain_.assign(static_cast<size_t>(map_width_ * map_height_), engine::Terrain::Grass);
        map_revision_ = map.revision() + 1;  // force the first paint
    }
    if (map.revision() == map_revision_ && world.vision_revision() == vision_revision_ && reveal == painted_reveal_) {
        return;
    }
    map_revision_ = map.revision();
    vision_revision_ = world.vision_revision();
    painted_reveal_ = reveal;

    // The ground as we last saw it: the fog hides what changed since.
    for (int ty = 0; ty < map_height_; ++ty) {
        for (int tx = 0; tx < map_width_; ++tx) {
            if (reveal || world.visible(viewer, {tx, ty})) {
                seen_terrain_[static_cast<size_t>(ty * map_width_ + tx)] = map.terrain(tx, ty);
            }
        }
    }
    std::vector<Color> colors(pixels, BLANK);
    for (size_t i = 0; i < pixels; ++i) {
        const engine::TilePos t = pixel_tile_[i];
        if (!map.contains(t)) continue;
        if (!reveal && !world.explored(viewer, t)) {
            colors[i] = {10, 11, 12, 255};  // never seen
            continue;
        }
        const float fog = reveal || world.visible(viewer, t) ? 1.0f : 0.5f;
        const Color base = theme::terrain_color(seen_terrain_[static_cast<size_t>(t.y * map_width_ + t.x)]);
        colors[i] = shade(base, pixel_light_[i] * fog);
    }
    UpdateTexture(minimap_texture_, colors.data());
}

Vector2 Hud::ground_to_minimap(Vector2 ground, Rectangle minimap) const {
    const auto w = static_cast<float>(map_width_);
    const auto h = static_cast<float>(map_height_);
    return {minimap.x + (ground.x - ground.y + h) / (w + h) * kMinimapWidth,
            minimap.y + (ground.x + ground.y) / (w + h) * kMinimapHeight};
}

std::optional<Vector2> Hud::minimap_to_ground(Vector2 p) const {
    const Layout l = layout();
    if (map_width_ == 0 || !CheckCollisionPointRec(p, l.minimap)) return std::nullopt;
    const Vector2 g = minimap_pixel_to_ground({p.x - l.minimap.x, p.y - l.minimap.y}, static_cast<float>(map_width_),
                                              static_cast<float>(map_height_));
    if (g.x < 0 || g.y < 0 || g.x > static_cast<float>(map_width_) || g.y > static_cast<float>(map_height_)) {
        return std::nullopt;
    }
    return g;
}

void Hud::draw_minimap(const engine::World& world, const HudState& state, const Layout& l) const {
    draw_panel(l.minimap_panel);
    if (minimap_texture_.id == 0) return;
    DrawTextureV(minimap_texture_, {l.minimap.x, l.minimap.y}, WHITE);

    const auto w = static_cast<float>(map_width_);
    const auto h = static_cast<float>(map_height_);
    const Vector2 corners[4] = {
        ground_to_minimap({0, 0}, l.minimap), ground_to_minimap({w, 0}, l.minimap),
        ground_to_minimap({w, h}, l.minimap), ground_to_minimap({0, h}, l.minimap)};
    for (int i = 0; i < 4; ++i) DrawLineV(corners[i], corners[(i + 1) % 4], theme::kPanelBorder);

    for (const engine::Unit& u : world.units()) {
        if (!state.reveal && !world.sees(state.local_player, u)) continue;
        const Vector2 p = ground_to_minimap({to_float(u.pos.x), to_float(u.pos.y)}, l.minimap);
        const float size = engine::unit_type(u.type).vehicle ? 3.0f : 2.0f;
        DrawRectangleV({p.x - size * 0.5f, p.y - size * 0.5f}, {size, size}, theme::player_color(u.owner));
    }

    // What the camera sees.
    BeginScissorMode(static_cast<int>(l.minimap.x), static_cast<int>(l.minimap.y), kMinimapWidth, kMinimapHeight);
    for (int i = 0; i < 4; ++i) {
        DrawLineV(ground_to_minimap(state.view_ground[static_cast<size_t>(i)], l.minimap),
                  ground_to_minimap(state.view_ground[static_cast<size_t>((i + 1) % 4)], l.minimap), WHITE);
    }
    EndScissorMode();
}

void Hud::draw(const engine::World& world, const HudState& state) const {
    if (state.dragging) {
        DrawRectangleRec(state.drag_rect, ColorAlpha(theme::kSelection, 0.12f));
        DrawRectangleLinesEx(state.drag_rect, 1.0f, theme::kSelection);
    }

    if (!state.targeting.empty()) {  // (the cursor's crosshairs show where)
        const Vector2 m = theme::mouse_position();
        draw_text(TextFormat("%.*s", static_cast<int>(state.targeting.size()), state.targeting.data()), m.x + 26,
                  m.y + 16, kSmallFontSize, theme::kDanger);
    }
    if (!state.cursor_hint.empty()) {
        // By the cursor, like AoE's cursor changing over a tree or a mine.
        const Vector2 m = theme::mouse_position();
        const char* hint = TextFormat("%.*s", static_cast<int>(state.cursor_hint.size()), state.cursor_hint.data());
        const float w = static_cast<float>(MeasureText(hint, kCardFontSize));
        const float x = std::min(m.x + 30, static_cast<float>(GetScreenWidth()) - w - 12);  // clear of the cursor
        const float y = m.y + (state.targeting.empty() ? 30.0f : 42.0f);
        DrawRectangleRec({x - 5, y - 3, w + 10, kCardFontSize + 6.0f}, {16, 18, 20, 210});
        draw_text(hint, x, y, kCardFontSize, theme::kText);
    }
    if (!state.placing.empty()) {
        const Vector2 m = theme::mouse_position();
        draw_text(TextFormat("%.*s: LMB to place, Shift for more, RMB cancels", static_cast<int>(state.placing.size()),
                             state.placing.data()),
                  m.x + 28, m.y + 22, kSmallFontSize, theme::kText);
    }

    const Layout l = layout();
    draw_top_bar(world, state, l.top_bar);
    draw_banner(state.net);
    draw_bottom_panel(world, state, l);
    draw_minimap(world, state, l);
    // Over everything: the cursor, its shape by what a click would do (over the panels, the arrow).
    if (state.show_cursor && (theme::g_fake_mouse || IsCursorOnScreen())) draw_cursor(captures_point(theme::mouse_position()) ? Cursor::Arrow : state.cursor, theme::mouse_position());
}

namespace {

// Who works on what, for the top bar: like the villager counts under the
// resources in AoE II.
struct Logistics {
    std::array<int, engine::kResourceCount> trucks{};  // trucks assigned to that freight
    int auto_trucks = 0;  // trucks hauling whatever piles up
    int gatherers = 0;    // rear troops cutting timber and quarrying stone
    int idle = 0;         // rear troops and trucks with nothing to do
    std::array<bool, engine::kResourceCount> depot{};  // a finished depot of ours takes it from trucks
};

Logistics logistics_of(const engine::World& world, engine::PlayerId player) {
    Logistics l;
    for (const engine::Unit& u : world.units()) {
        if (u.owner != player) continue;
        if (engine::idle_hand(u)) ++l.idle;
        if (u.order == engine::Order::Gather) ++l.gatherers;
        if (u.order != engine::Order::Haul) continue;
        if (u.haul_cargo == engine::Resource::Count) {
            ++l.auto_trucks;
        } else {
            ++l.trucks[static_cast<size_t>(u.haul_cargo)];
        }
    }
    for (const engine::Structure& s : world.structures()) {
        if (s.owner != player || !s.built) continue;
        if (const auto cargo = engine::depot_cargo(engine::role_of(s))) l.depot[static_cast<size_t>(*cargo)] = true;
    }
    return l;
}

// Tiny icons drawn with lines, AoE-style: a truck, a railway wagon, a man.
void draw_truck_icon(float x, float y, Color c) {
    DrawRectangleRec({x, y + 3, 9, 5}, c);
    DrawRectangleRec({x + 9, y + 4, 4, 4}, c);
    DrawCircleV({x + 3, y + 9}, 1.5f, c);
    DrawCircleV({x + 10, y + 9}, 1.5f, c);
}

void draw_wagon_icon(float x, float y, Color c) {
    DrawRectangleLinesEx({x, y + 2, 12, 6}, 1.0f, c);
    DrawCircleV({x + 3, y + 9}, 1.5f, c);
    DrawCircleV({x + 9, y + 9}, 1.5f, c);
}

void draw_bunk_icon(float x, float y, Color c) {
    DrawTriangle({x + 6, y}, {x, y + 5}, {x + 12, y + 5}, c);
    DrawRectangleRec({x + 1, y + 5, 10, 6}, c);
}

void draw_man_icon(float x, float y, Color c) {
    DrawCircleV({x + 4, y + 2}, 2.0f, c);
    DrawRectangleRec({x + 2, y + 5, 4, 6}, c);
}

// A few lines of explanation in a box under the top bar.
void draw_note(std::span<const char* const> lines, float x, float y) {
    float w = 0;
    for (const char* line : lines) w = std::max(w, static_cast<float>(MeasureText(line, kCardFontSize)));
    const float h = 10.0f + 20.0f * static_cast<float>(lines.size());
    x = std::clamp(x, 4.0f, static_cast<float>(GetScreenWidth()) - w - 20);
    draw_panel({x, y, w + 16, h});
    for (size_t i = 0; i < lines.size(); ++i) {
        draw_text(lines[i], x + 8, y + 7 + 20.0f * static_cast<float>(i), kCardFontSize,
                  i == 0 ? theme::kText : theme::kTextDim);
    }
}

}  // namespace

void Hud::draw_top_bar(const engine::World& world, const HudState& state, Rectangle area) const {
    DrawRectangleRec(area, theme::kPanel);
    DrawLineV({area.x, area.y + area.height}, {area.x + area.width, area.y + area.height}, theme::kPanelBorder);

    const float y = area.y + (area.height - kSmallFontSize) * 0.5f;
    float x = area.x + kPadding;
    const Vector2 mouse = theme::mouse_position();

    // Our stockpile first: it's what the player looks at most. Next to each
    // resource: the freight waiting for it at the station and the trucks on
    // it, or the rear troops gathering materials. Hover for the details.
    const engine::Stock& stock = world.stock(state.local_player);
    const Logistics logistics = logistics_of(world, state.local_player);
    const engine::Structure* station = world.station_of(state.local_player);
    // Copied as it's made: TextFormat's few buffers are reused by the next resource.
    std::array<std::string, 4> note{};
    size_t note_lines = 0;
    for (size_t r = 0; r < engine::kResourceCount; ++r) {
        const auto resource = static_cast<engine::Resource>(r);
        const float start = x;
        const char* name = engine::resource_name(resource);
        draw_text(name, x, y, kSmallFontSize, theme::kTextDim);
        x += static_cast<float>(MeasureText(name, kSmallFontSize)) + 6;
        const char* amount = TextFormat("%d", stock[r]);
        const bool starving = resource == engine::Resource::Food && world.hungry(state.local_player);
        draw_text(amount, x, y, kSmallFontSize, starving ? theme::kDanger : theme::kText);
        x += static_cast<float>(MeasureText(amount, kSmallFontSize)) + 8;
        if (starving) {
            draw_text("HUNGRY", x, y, kSmallFontSize, theme::kDanger);
            x += static_cast<float>(MeasureText("HUNGRY", kSmallFontSize)) + 8;
        }
        const int32_t housed = world.population(state.local_player);
        const int32_t bunks = world.bunks(state.local_player);
        if (resource == engine::Resource::Personnel) {
            // Housing, AoE-style: in service / bunks, red when full.
            const Color c = housed >= bunks ? theme::kDanger : theme::kTextDim;
            draw_bunk_icon(x, y + 1, c);
            x += 15;
            const char* room = TextFormat("%d/%d", housed, bunks);
            draw_text(room, x, y, kSmallFontSize, c);
            x += static_cast<float>(MeasureText(room, kSmallFontSize));
        }

        const bool by_truck = engine::depot_for(resource).has_value();
        const int waiting = station && by_truck ? station->cargo[r] : 0;
        const int trucks = logistics.trucks[r];
        if (by_truck) {
            // Piling up with nobody to haul it: amber; with no depot to take it: red.
            Color c = theme::kTextDim;
            if (waiting > 0 && !logistics.depot[r]) {
                c = theme::kDanger;
            } else if (waiting >= 3 * engine::kTruckCapacity && trucks == 0 && logistics.auto_trucks == 0) {
                c = theme::kWarning;
            }
            draw_wagon_icon(x, y + 3, c);
            x += 15;
            const char* at_station = TextFormat("%d", waiting);
            draw_text(at_station, x, y, kSmallFontSize, c);
            x += static_cast<float>(MeasureText(at_station, kSmallFontSize)) + 6;
            draw_truck_icon(x, y + 3, theme::kTextDim);
            x += 16;
            const char* on_it = TextFormat("%d", trucks);
            draw_text(on_it, x, y, kSmallFontSize, theme::kTextDim);
            x += static_cast<float>(MeasureText(on_it, kSmallFontSize));
        } else if (resource == engine::Resource::Materials) {
            draw_man_icon(x, y + 2, theme::kTextDim);
            x += 11;
            const char* gathering = TextFormat("%d", logistics.gatherers);
            draw_text(gathering, x, y, kSmallFontSize, theme::kTextDim);
            x += static_cast<float>(MeasureText(gathering, kSmallFontSize));
        }
        if (CheckCollisionPointRec(mouse, {start, area.y, x - start, area.height})) {
            note[0] = TextFormat("%s: %d in stock", name, stock[r]);
            note_lines = 1;
            if (resource == engine::Resource::Personnel) {
                note[note_lines++] = TextFormat("+%d with every train: look after the men",
                                                engine::kTrainCargo[static_cast<size_t>(engine::Resource::Personnel)]);
                note[note_lines++] = TextFormat("In service %d, bunks %d (headquarters %d, living quarters %d each, up to %d)",
                                                housed, bunks, engine::kHeadquartersBunks, engine::kQuartersBunks,
                                                engine::kMaxPopulation);
                note[note_lines++] = housed >= bunks && bunks < engine::kMaxPopulation
                                         ? "No room: hiring waits. Rear troops build living quarters (Q, then V)"
                                         : "Hiring waits when the bunks are full";
            } else if (resource == engine::Resource::Materials) {
                note[note_lines++] = TextFormat("Rear troops cutting timber or quarrying stone: %d", logistics.gatherers);
                note[note_lines++] = "They carry it to the headquarters or a warehouse";
            } else {
                if (resource == engine::Resource::Food) {
                    const int32_t men = world.mouths(state.local_player);
                    note[0] = TextFormat("Food: %d in stock. Rations: %d men eat %d a minute (next in %ds)%s", stock[r], men,
                                         men * engine::kRationPerMan * 60 * engine::kTicksPerSecond /
                                             static_cast<int32_t>(engine::kRationInterval),
                                         static_cast<int>((engine::kRationInterval - world.tick() % engine::kRationInterval) /
                                                          engine::kTicksPerSecond),
                                         world.hungry(state.local_player) ? ". HUNGRY: they shoot and move worse" : "");
                }
                const char* depot = engine::structure_type(*engine::depot_for(resource)).name;
                note[note_lines++] = TextFormat("Waiting at the station: %d. Trucks on it: %d (on auto: %d)", waiting,
                                                trucks, logistics.auto_trucks);
                note[note_lines++] = logistics.depot[r]
                                         ? TextFormat("Trucks take it to the %s", depot)
                                         : TextFormat("No %s: trucks can't take it anywhere!", depot);
            }
        }
        x += 18;
    }

    // Trucks left to haul whatever piles up; the idle ones, a click away.
    if (logistics.auto_trucks > 0) {
        draw_truck_icon(x, y + 3, theme::kTextDim);
        x += 16;
        const char* auto_trucks = TextFormat("auto %d", logistics.auto_trucks);
        draw_text(auto_trucks, x, y, kSmallFontSize, theme::kTextDim);
        x += static_cast<float>(MeasureText(auto_trucks, kSmallFontSize)) + 18;
    }
    const char* idle = TextFormat("Idle %d", logistics.idle);
    idle_rect_ = {x - 4, area.y + 3, static_cast<float>(MeasureText(idle, kSmallFontSize)) + 8, area.height - 6};
    if (logistics.idle > 0) DrawRectangleRec(idle_rect_, ColorAlpha(theme::kWarning, 0.25f));
    draw_text(idle, x, y, kSmallFontSize, logistics.idle > 0 ? theme::kWarning : theme::kTextDim);
    if (CheckCollisionPointRec(mouse, idle_rect_)) {
        note[0] = TextFormat("Rear troops and trucks with nothing to do: %d", logistics.idle);
        note[1] = "Click, or press '.', to pick them one by one";
        note_lines = 2;
    }
    x += idle_rect_.width + 18;
    // Men and freight only come by rail.
    const char* train = "No station: no trains";
    Color train_color = theme::kDanger;
    if (station) {
        train = TextFormat("Train in %ds (+%d men)", seconds_until(world, station->next_train),
                           engine::kTrainCargo[static_cast<size_t>(engine::Resource::Personnel)]);
        train_color = theme::kTextDim;
    }
    draw_text(train, x, y, kSmallFontSize, train_color);
    x += static_cast<float>(MeasureText(train, kSmallFontSize)) + 30;

    // Debug and network details, smaller, as far as they fit before the checksum.
    constexpr int kTiny = 14;
    const float ty = area.y + (area.height - kTiny) * 0.5f;
    const char* checksum = TextFormat("%016llX", static_cast<unsigned long long>(world.checksum()));
    const float w = static_cast<float>(MeasureText(checksum, kTiny));
    draw_text(checksum, area.x + area.width - w - kPadding, ty, kTiny, theme::kTextDim);
    const float right = area.x + area.width - w - kPadding - 12;
    auto put = [&](const char* text, Color color) {
        const float tw = static_cast<float>(MeasureText(text, kTiny));
        if (x + tw > right) return;
        draw_text(text, x, ty, kTiny, color);
        x += tw;
    };
    if (state.net.online) {
        put(TextFormat("You: Player %d  ", state.local_player + 1), theme::player_color(state.local_player));
        put(TextFormat("Ping %d ms  ", state.net.ping_ms), theme::kTextDim);
    }
    put(TextFormat("Tick %u  FPS %d  Delay %u", world.tick(), GetFPS(), state.net.input_delay), theme::kTextDim);
    if (note_lines > 0) {
        std::array<const char*, 4> lines{};
        for (size_t i = 0; i < note_lines; ++i) lines[i] = note[i].c_str();
        draw_note({lines.data(), note_lines}, mouse.x - 40, area.y + area.height + 4);
    }
}

void Hud::draw_banner(const NetStatus& net) const {
    const char* text = nullptr;
    Color color = theme::kText;
    if (net.desync_tick) {
        text = TextFormat("DESYNC at tick %u: the game states differ", *net.desync_tick);
        color = theme::kDanger;
    } else if (!net.message.empty()) {
        text = TextFormat("%.*s", static_cast<int>(net.message.size()), net.message.data());
        color = theme::kWarning;
    } else if (net.waiting) {
        text = "Waiting for the opponent...";
        color = theme::kWarning;
    }
    if (!text) return;

    const float w = static_cast<float>(MeasureText(text, kFontSize)) + 2 * kPadding;
    const Rectangle area{(static_cast<float>(GetScreenWidth()) - w) * 0.5f, kTopBarHeight + kPadding, w,
                         kFontSize + 2 * kPadding};
    draw_panel(area);
    draw_text(text, area.x + kPadding, area.y + kPadding, kFontSize, color);
}

void draw_lobby_screen(std::string_view status) {
    const auto w = static_cast<float>(GetScreenWidth());
    const auto h = static_cast<float>(GetScreenHeight());
    auto centered = [w](const char* text, float y, int size, Color color) {
        draw_text(text, (w - static_cast<float>(MeasureText(text, size))) * 0.5f, y, size, color);
    };
    centered("ANCHOR RTS", h * 0.5f - 60.0f, 40, theme::kText);
    centered(TextFormat("%.*s", static_cast<int>(status.size()), status.data()), h * 0.5f, kFontSize,
             theme::kWarning);
    centered("F10: quit", h * 0.5f + 40.0f, kSmallFontSize, theme::kTextDim);
}

void Hud::draw_bottom_panel(const engine::World& world, const HudState& state, const Layout& l) const {
    draw_panel(l.bottom_panel);
    const engine::Structure* s = world.find_structure(state.selected_structure);
    if (!s && state.selection.empty()) {
        // Nothing selected: the controls, across the whole panel.
        draw_help({l.bottom_panel.x + kPadding, l.bottom_panel.y + kPadding, l.bottom_panel.width - 2 * kPadding,
                   l.bottom_panel.height - 2 * kPadding});
        return;
    }
    if (s) {
        draw_structure_card(world, *s, l.info);
    } else {
        draw_selection(world, state, l.info);
    }
    draw_grid(state, world.stock(state.local_player));
}

Rectangle Hud::button_rect(size_t slot) {
    const Rectangle grid = layout().grid;
    const float w = (grid.width - kGridGap * static_cast<float>(kGridColumns - 1)) / static_cast<float>(kGridColumns);
    const float h = (grid.height - kGridGap * static_cast<float>(kGridRows - 1)) / static_cast<float>(kGridRows);
    const auto row = static_cast<float>(slot / kGridColumns);
    const auto column = static_cast<float>(slot % kGridColumns);
    return {grid.x + column * (w + kGridGap), grid.y + row * (h + kGridGap), w, h};
}

std::optional<size_t> Hud::button_at(Vector2 p) const {
    for (size_t slot = 0; slot < kGridSlots; ++slot) {
        if (CheckCollisionPointRec(p, button_rect(slot))) return slot;
    }
    return std::nullopt;
}

// The grid: every cell with its hotkey in the corner; a cell cooling down
// darkens from the top and clears as it gets ready.
void Hud::draw_grid(const HudState& state, const engine::Stock& stock) const {
    const Vector2 mouse = theme::mouse_position();
    const CommandButton* hovered = nullptr;
    for (size_t slot = 0; slot < kGridSlots; ++slot) {
        const Rectangle r = button_rect(slot);
        const CommandButton* b = slot < state.commands.size() && state.commands[slot].label ? &state.commands[slot]
                                                                                             : nullptr;
        if (!b) {
            DrawRectangleLinesEx(r, 1.0f, ColorAlpha(theme::kPanelBorder, 0.35f));
            continue;
        }
        const bool affordable = engine::can_afford(stock, b->cost);
        const bool usable = b->enabled && affordable && b->cooldown <= 0.0f;
        const bool hover = CheckCollisionPointRec(mouse, r);
        if (hover) hovered = b;
        DrawRectangleRec(r, hover ? Color{52, 60, 54, 255} : Color{32, 36, 34, 255});
        if (b->cooldown > 0.0f) DrawRectangleRec({r.x, r.y, r.width, r.height * b->cooldown}, {0, 0, 0, 150});
        DrawRectangleLinesEx(r, b->active ? 2.0f : 1.0f, b->active ? theme::kSelection : theme::kPanelBorder);

        const char key[2] = {kGridKeys[slot], 0};
        const auto key_w = static_cast<float>(MeasureText(key, 10));
        draw_text(key, r.x + r.width - key_w - 4, r.y + 3, 10, theme::kWarning);
        draw_text(b->label, r.x + 4, r.y + 4, fitting_font(b->label, 12, r.width - key_w - 12),
                  usable ? theme::kText : theme::kTextDim);
        char price[96];
        format_price(price, b->cost, false);
        if (price[0]) {
            draw_text(price, r.x + 4, r.y + r.height - 13, fitting_font(price, 10, r.width - 6),
                      affordable ? theme::kTextDim : theme::kDanger);
        }
    }
    if (hovered) draw_tooltip(*hovered, stock);
}

void Hud::draw_tooltip(const CommandButton& b, const engine::Stock& stock) const {
    const char* title = b.tooltip ? b.tooltip : b.label;
    char price[96];
    format_price(price, b.cost, true);
    const float title_w = static_cast<float>(MeasureText(title, kCardFontSize));
    const float price_w = price[0] ? static_cast<float>(MeasureText(price, 12)) : 0.0f;
    const float w = std::max(title_w, price_w) + 2 * 8;
    const float h = price[0] ? 44.0f : 28.0f;
    const Layout l = layout();
    const float x = std::clamp(theme::mouse_position().x - w * 0.5f, 4.0f, static_cast<float>(GetScreenWidth()) - w - 4);
    const Rectangle r{x, l.bottom_panel.y - h - 6, w, h};
    draw_panel(r);
    draw_text(title, r.x + 8, r.y + 7, kCardFontSize, theme::kText);
    if (price[0]) {
        draw_text(price, r.x + 8, r.y + 26, 12, engine::can_afford(stock, b.cost) ? theme::kTextDim : theme::kDanger);
    }
}

namespace {

// A depot's card: the trucks bringing its freight here.
void draw_depot_trucks(const engine::World& world, const engine::Structure& s, float y, Rectangle area) {
    const std::optional<engine::Resource> cargo = engine::depot_cargo(engine::role_of(s));
    if (!cargo || !s.built) return;
    int here = 0;
    for (const engine::Unit& u : world.units()) {
        if (u.owner != s.owner || u.order != engine::Order::Haul) continue;
        if (u.haul_cargo == *cargo && world.haul_destination(u, *cargo) == &s) ++here;
    }
    draw_text(TextFormat("Trucks bringing %s here: %d. RMB with trucks: assign them to this depot.",
                         engine::resource_name(*cargo), here),
              area.x, y, kCardFontSize, here > 0 ? theme::kText : theme::kWarning);
}

}  // namespace

void Hud::draw_structure_card(const engine::World& world, const engine::Structure& s, Rectangle area) const {
    const engine::StructureDef& def = engine::structure_type(s.type);
    if (s.research != engine::UpgradeId::Count) {
        const engine::UpgradeDef& up = engine::upgrade_def(s.research);
        const float done = static_cast<float>(s.research_progress) / static_cast<float>(up.time);
        const float y = area.y + area.height - 12;
        draw_text(TextFormat("Researching %s: %d%%", up.name, static_cast<int>(done * 100)), area.x, y - 16,
                  kCardFontSize, theme::kWarning);
        DrawRectangleRec({area.x, y, area.width, 5}, {0, 0, 0, 170});
        DrawRectangleRec({area.x, y, area.width * done, 5}, {230, 200, 60, 255});
    }
    const engine::StructureType role = engine::role_of(s);
    const char* hp = TextFormat("HP %d / %d", s.hp, def.max_hp);
    const float hp_x = area.x + area.width - 90 - 10 - static_cast<float>(MeasureText(hp, kCardFontSize));
    draw_text(hp, hp_x, area.y + 3, kCardFontSize, theme::kText);
    const char* name = role == s.type ? def.name : TextFormat("%s (village building)", engine::structure_type(role).name);
    if (role == s.type && s.type == engine::StructureType::House && s.look == engine::HouseLook::Cowshed) name = "Cowshed";
    if (role == s.type && s.type == engine::StructureType::House && s.look == engine::HouseLook::Coop) name = "Chicken coop";
    if (role == s.type && s.type == engine::StructureType::House && s.look == engine::HouseLook::Factory) name = "Factory shop";
    if (def.cache_capacity > 0) {
        name = TextFormat("%s   ammunition %d / %d", name, s.cache_owner == s.owner || s.cache == 0 ? s.cache : 0,
                          def.cache_capacity);
    }
    draw_text(name, area.x, area.y, fitting_font(name, kFontSize, hp_x - area.x - 12), theme::player_color(s.owner));
    const float frac = static_cast<float>(s.hp) / static_cast<float>(def.max_hp);
    DrawRectangleRec({area.x + area.width - 90, area.y + 7, 90, 6}, {0, 0, 0, 170});
    DrawRectangleRec({area.x + area.width - 90, area.y + 7, 90 * frac, 6}, {200, 200, 190, 255});

    const float line2 = area.y + kFontSize + 10;
    const float line3 = line2 + kCardFontSize + 6;
    const float line4 = line3 + kCardFontSize + 6;
    if (!s.built) {
        const bool converting = role != s.type;
        const engine::Tick work = converting ? engine::kConversionWork : def.build_time;
        const float done = static_cast<float>(s.build_progress) / static_cast<float>(std::max<engine::Tick>(1, work));
        draw_text(TextFormat("%s: %d%%", converting ? "Being turned into a depot" : "Under construction",
                             static_cast<int>(done * 100)),
                  area.x, line2, kCardFontSize, theme::kWarning);
        draw_text("RMB with rear troops to help", area.x, line3, kCardFontSize, theme::kTextDim);
        DrawRectangleRec({area.x, line4 + 4, area.width, 6}, {0, 0, 0, 170});
        DrawRectangleRec({area.x, line4 + 4, area.width * done, 6}, {230, 200, 60, 255});
        return;
    }

    auto freight = [](const engine::Stock& cargo) {
        return TextFormat("%d food, %d ammo, %d fuel", cargo[static_cast<size_t>(engine::Resource::Food)],
                          cargo[static_cast<size_t>(engine::Resource::Ammo)],
                          cargo[static_cast<size_t>(engine::Resource::Fuel)]);
    };
    switch (role) {
        case engine::StructureType::Station:
            draw_text(TextFormat("Next train in %ds: +%d men, %s", seconds_until(world, s.next_train),
                                 engine::kTrainCargo[static_cast<size_t>(engine::Resource::Personnel)],
                                 freight(engine::kTrainCargo)),
                      area.x, line2, kCardFontSize, theme::kTextDim);
            draw_text(TextFormat("Waiting for trucks: %s", freight(s.cargo)), area.x, line3, kCardFontSize,
                      theme::kText);
            draw_text("Trucks (hired at the headquarters) take it to the depots.", area.x, line4, kCardFontSize,
                      theme::kTextDim);
            return;
        case engine::StructureType::Warehouse:
            draw_text("Takes food from trucks, materials from rear troops.", area.x, line2, kCardFontSize,
                      theme::kTextDim);
            draw_text("Rear troops standing by unload trucks faster.", area.x, line3, kCardFontSize, theme::kTextDim);
            draw_depot_trucks(world, s, line4, area);
            return;
        case engine::StructureType::Workshop:
            draw_text(TextFormat("Repairs our vehicles parked by it (%d tiles), armor and wheels.", engine::kRepairReach.to_int()),
                      area.x, line2, kCardFontSize, theme::kTextDim);
            draw_text(TextFormat("%d at a time, the worst first: %d HP a second each,", static_cast<int>(engine::kRepairBays),
                                 engine::kRepairPerInterval),
                      area.x, line3, kCardFontSize, theme::kTextDim);
            draw_text("for a material a second each in spare parts.", area.x, line4, kCardFontSize, theme::kTextDim);
            return;
        case engine::StructureType::Hospital:
            draw_text(TextFormat("Beds: %d / %d. RMB with the wounded on it: in to be healed.",
                                 static_cast<int>(s.garrison.size()), engine::structure_type(s.type).capacity),
                      area.x, line2, kCardFontSize, theme::kText);
            draw_text(TextFormat("%d HP a second each; the well come out by themselves.",
                                 static_cast<int>(engine::kTicksPerSecond / engine::kHealTicks)),
                      area.x, line3, kCardFontSize, theme::kTextDim);
            return;
        case engine::StructureType::Quarters:
            draw_text(TextFormat("Bunks for %d men.", engine::kQuartersBunks), area.x, line2, kCardFontSize,
                      theme::kTextDim);
            draw_text(TextFormat("In service: %d, bunks: %d (up to %d).", world.population(s.owner),
                                 world.bunks(s.owner), engine::kMaxPopulation),
                      area.x, line3, kCardFontSize, theme::kTextDim);
            return;
        case engine::StructureType::AmmoDepot:
            draw_text("Takes ammunition from supply trucks.", area.x, line2, kCardFontSize, theme::kTextDim);
            draw_text("Rear troops standing by unload trucks faster.", area.x, line3, kCardFontSize, theme::kTextDim);
            draw_depot_trucks(world, s, line4, area);
            return;
        case engine::StructureType::Trench:
            draw_text(TextFormat("At a position (standing %d s): %d%% of hits taken by the walls.",
                                 static_cast<int>(engine::kSettleTicks / engine::kTicksPerSecond), engine::kTrenchCover),
                      area.x, line2, kCardFontSize, theme::kTextDim);
            draw_text(TextFormat("Walking along it: %d%%, and own fire outward -%d%%.", engine::kTrenchWalkingCover,
                                 100 - engine::kTrenchWalkingFirePercent),
                      area.x, line3, kCardFontSize, theme::kTextDim);
            draw_text(s.parapet ? "Parapet: +25% cover from the front." : "Tracks cross it slowly, wheels can't.",
                      area.x, line4, kCardFontSize, theme::kTextDim);
            return;
        case engine::StructureType::Foxhole:
            draw_text(TextFormat("%d%% of hits taken by the walls; own accuracy -%d%%.", engine::kFoxholeCover,
                                 100 - engine::kFoxholeAccuracyPercent),
                      area.x, line2, kCardFontSize, theme::kTextDim);
            if (s.upgrading) {
                const float done = static_cast<float>(s.upgrade_work) / static_cast<float>(engine::kDugoutWork);
                draw_text(TextFormat("Digging out a dugout: %d%% (the men in it dig)", static_cast<int>(done * 100)),
                          area.x, line3, kCardFontSize, theme::kWarning);
                DrawRectangleRec({area.x, line4 + 4, area.width, 6}, {0, 0, 0, 170});
                DrawRectangleRec({area.x, line4 + 4, area.width * done, 6}, {230, 200, 60, 255});
            } else if (s.parapet) {
                draw_text("Parapet: +25% cover from the front.", area.x, line3, kCardFontSize, theme::kTextDim);
            }
            return;
        case engine::StructureType::Wire:
            draw_text("Barbed wire: infantry crawls through, tracks roll it flat,", area.x, line2, kCardFontSize,
                      theme::kTextDim);
            draw_text("wheels can't get past.", area.x, line3, kCardFontSize, theme::kTextDim);
            return;
        case engine::StructureType::Hedgehogs:
            draw_text("Anti-tank hedgehogs: no vehicle gets through; infantry slowed.", area.x, line2, kCardFontSize,
                      theme::kTextDim);
            return;
        case engine::StructureType::Pillbox:
            draw_text(TextFormat("Firing point: %d / %d inside, firing only through the slit (%d degrees).",
                                 static_cast<int>(s.garrison.size()), def.capacity, engine::kPillboxSectorDegrees),
                      area.x, line2, fitting_font("Firing point: 3 / 3 inside, firing only through the slit (120 degrees).",
                                                  kCardFontSize, area.width),
                      theme::kText);
            draw_text("Bullets and fragments don't get in; an RPG through the slit does.", area.x, line3,
                      kCardFontSize, theme::kTextDim);
            return;
        case engine::StructureType::GunPit:
            draw_text(TextFormat("A gun's dug-in position: %d%% of hits taken by the walls,", engine::kGunPitCover),
                      area.x, line2, kCardFontSize, theme::kTextDim);
            draw_text("and the gun in it is hidden until it fires or is spotted up close.", area.x, line3,
                      kCardFontSize, theme::kTextDim);
            return;
        case engine::StructureType::Parapet:
            draw_text(TextFormat("+%d%% cover against fire from the front.", engine::kParapetCover), area.x, line2,
                      kCardFontSize, theme::kTextDim);
            return;
        case engine::StructureType::ObservationPost: {
            static constexpr const char* kKinds[] = {"An artificial stump out in the open", "A hide of branches",
                                                     "A platform up a tree"};
            draw_text(TextFormat("%s: a scout observing from it stays hidden.", kKinds[static_cast<int>(s.post)]), area.x, line2,
                      kCardFontSize, theme::kText);
            if (s.post == engine::PostKind::Tree) {
                draw_text(TextFormat("Up there he sees %d tiles farther over the woods.", engine::kTreePostRange), area.x, line3,
                          kCardFontSize, theme::kTextDim);
            }
            draw_text("Firing gives him away.", area.x, line4, kCardFontSize, theme::kTextDim);
            return;
        }
        case engine::StructureType::Dugout:
            draw_text(TextFormat("Shelter: %d / %d inside. Bullets and fragments don't reach them.",
                                 static_cast<int>(s.garrison.size()), def.capacity),
                      area.x, line2, kCardFontSize, theme::kText);
            draw_text("No firing from inside, and no seeing out.", area.x, line3, kCardFontSize, theme::kTextDim);
            draw_text("A grenade through the entrance hurts those inside.", area.x, line4, kCardFontSize,
                      theme::kWarning);
            return;
        case engine::StructureType::Apartment:
            draw_text(TextFormat("Garrison %d / %d, on the upper floors.", static_cast<int>(s.garrison.size()),
                                 def.capacity),
                      area.x, line2, kCardFontSize, theme::kText);
            draw_text(TextFormat("They see %d tiles farther and fire down as from high ground.", engine::kApartmentSightBonus),
                      area.x, line3, kCardFontSize, theme::kTextDim);
            draw_text("Nothing behind it is seen past it.", area.x, line4, kCardFontSize, theme::kTextDim);
            return;
        case engine::StructureType::GasStation:
            draw_text(TextFormat("Fuel in the tanks: %d. Held by our men: ours.",
                                 s.cargo[static_cast<size_t>(engine::Resource::Fuel)]),
                      area.x, line2, kCardFontSize, theme::kText);
            draw_text(TextFormat("Our vehicles within %d tiles of the pumps fill up; trucks and tankers haul it off.",
                                 engine::kPumpReach.to_int()),
                      area.x, line3, kCardFontSize, theme::kTextDim);
            draw_text("With fuel in it, it goes up in flames when destroyed.", area.x, line4, kCardFontSize,
                      theme::kWarning);
            return;
        case engine::StructureType::Elevator:
            draw_text(TextFormat("Grain: %d food. Held by our men, trucks haul it to the warehouse.",
                                 s.cargo[static_cast<size_t>(engine::Resource::Food)]),
                      area.x, line2, kCardFontSize, theme::kText);
            draw_text(TextFormat("From the top the garrison (%d / %d) sees %d tiles farther and fires down.",
                                 static_cast<int>(s.garrison.size()), def.capacity, engine::kElevatorSightBonus),
                      area.x, line3, kCardFontSize, theme::kTextDim);
            draw_text("Nothing behind it is seen past it.", area.x, line4, kCardFontSize, theme::kTextDim);
            return;
        case engine::StructureType::CellTower:
            draw_text(TextFormat("A scout up the mast sees %d%% as far; anyone else %d tiles farther.", engine::kTowerScoutSightPercent,
                                 engine::kTowerSightBonus),
                      area.x, line2, kCardFontSize, theme::kText);
            if (s.antenna != engine::kNoOwner) {
                draw_text(TextFormat("A DF aerial up it: bearings on enemy radios within %d tiles.", engine::kTowerDfRange.to_int()),
                          area.x, line3, kCardFontSize, theme::kText);
                draw_text("The enemy's man going up takes it down.", area.x, line4, kCardFontSize, theme::kWarning);
                return;
            }
            draw_text(TextFormat("Held by our men, it relays our radio within %d tiles.", engine::kTowerRelay.to_int()),
                      area.x, line3, kCardFontSize, theme::kTextDim);
            draw_text("A few shells bring it down, and him with it.", area.x, line4, kCardFontSize, theme::kWarning);
            return;
        case engine::StructureType::FuelDepot:
            draw_text("Takes fuel from supply trucks.", area.x, line2, kCardFontSize, theme::kTextDim);
            draw_text(TextFormat("Burns if destroyed: %d%% of the fuel is lost.", engine::kFuelDepotLossPercent),
                      area.x, line3, kCardFontSize, theme::kWarning);
            draw_depot_trucks(world, s, line4, area);
            return;
        default: break;
    }

    if (def.roster_size > 0 && s.research == engine::UpgradeId::Count) {
        draw_text(s.rally_set ? "Rally point set: RMB on the ground moves it" : "RMB on the ground: rally point for the new units",
                  area.x, area.y + area.height - 16, 12, theme::kTextDim);
    }
    // The hiring queue: the front one with its progress.
    constexpr float kIconW = 34.0f;
    constexpr float kIconH = 28.0f;
    for (size_t i = 0; i < s.queue.size(); ++i) {
        const engine::UnitTypeDef& def_i = engine::unit_type(s.queue[i]);
        const float x = area.x + static_cast<float>(i) * (kIconW + 4);
        DrawRectangleRec({x, line2, kIconW, kIconH}, ColorAlpha(theme::player_color(s.owner), 0.25f));
        DrawRectangleLinesEx({x, line2, kIconW, kIconH}, 1.0f, theme::player_color(s.owner));
        const float text_w = static_cast<float>(MeasureText(def_i.short_name, 10));
        draw_text(def_i.short_name, x + (kIconW - text_w) * 0.5f, line2 + 5, 10, theme::kText);
        if (i == 0) {
            const float done = static_cast<float>(s.progress) / static_cast<float>(def_i.train_time);
            DrawRectangleRec({x + 3, line2 + kIconH - 7, (kIconW - 6) * done, 3}, {90, 210, 90, 255});
        }
    }
    if (world.waits_for_bunks(s)) {
        draw_text("No bunks free: build living quarters", area.x, line2 + kIconH + 10, kCardFontSize, theme::kDanger);
    }
    if (def.roster_size > 0) {
        draw_text(TextFormat("queue %d/%d", static_cast<int>(s.queue.size()), static_cast<int>(engine::kMaxQueue)),
                  area.x + 5 * (kIconW + 4) + 8, line2 + 7, kCardFontSize, theme::kTextDim);
    }
    if (!s.garrison.empty()) {
        draw_text(TextFormat("%d at drill", static_cast<int>(s.garrison.size())), area.x, line2 + kIconH + 10,
                  kCardFontSize, theme::kTextDim);
    }
}

void Hud::draw_help(Rectangle area) const {
    const char* lines[] = {
        "LMB: select / drag box    Shift+LMB: add    F2: army    Space: jump to selection    F10: quit",
        "RMB: move / attack / infantry into a house / trucks to a depot / rear troops to work",
        "Command grid: the hotkey is the cell, as in AoE II:   Q W E R T  /  A S D F G  /  Z X C V B",
        "A: attack-move    S: stop    G: fire at ground    Esc or RMB: cancel aiming",
        "Buildings: click one to hire    Minimap: LMB moves the camera, RMB sends the selection",
    };
    float y = area.y;
    for (const char* line : lines) {
        draw_text(line, area.x, y, kCardFontSize, theme::kTextDim);
        y += kCardFontSize + 7;
    }
}

void Hud::draw_selection(const engine::World& world, const HudState& state, Rectangle area) const {
    const int count = static_cast<int>(state.selection.size());
    if (count == 1) {
        if (const engine::Unit* u = world.find_unit(state.selection.front())) draw_unit_card(world, *u, area);
        return;
    }

    draw_text(TextFormat("Selected: %d", count), area.x, area.y, kFontSize, theme::kText);

    // One icon per unit, as many as fit.
    constexpr float kIconW = 34.0f;
    constexpr float kIconH = 28.0f;
    constexpr float kGap = 4.0f;
    const int per_row = std::max(1, static_cast<int>((area.width + kGap) / (kIconW + kGap)));
    const float top = area.y + kFontSize + 10;
    const int max_rows = std::max(1, static_cast<int>((area.y + area.height - top + kGap) / (kIconH + kGap)));
    const int shown = std::min(count, per_row * max_rows);

    for (int i = 0; i < shown; ++i) {
        const engine::Unit* u = world.find_unit(state.selection[static_cast<size_t>(i)]);
        if (!u) continue;
        const engine::UnitTypeDef& def = engine::unit_type(u->type);
        const Color color = theme::player_color(u->owner);
        const float x = area.x + static_cast<float>(i % per_row) * (kIconW + kGap);
        const float y = top + static_cast<float>(i / per_row) * (kIconH + kGap);
        DrawRectangleRec({x, y, kIconW, kIconH}, ColorAlpha(color, 0.25f));
        DrawRectangleLinesEx({x, y, kIconW, kIconH}, 1.0f, color);
        const float text_w = static_cast<float>(MeasureText(def.short_name, 10));
        draw_text(def.short_name, x + (kIconW - text_w) * 0.5f, y + 5, 10, theme::kText);
        draw_hp_bar(*u, {x + 3, y + kIconH - 7, kIconW - 6, 3});
    }
}

namespace {

// A supply truck's assignment, and what holds it up if anything.
std::pair<const char*, Color> truck_status(const engine::World& world, const engine::Unit& u) {
    if (u.order == engine::Order::Collect) {
        return {TextFormat("By the wood: %d / %d materials aboard, %s", u.carrying, engine::kTruckCapacity,
                           u.delivering ? "taking them in" : "rear troops hand their loads in"),
                theme::kTextDim};
    }
    if (u.order != engine::Order::Haul) return {"Off the supply run: Q-R put it back on", theme::kWarning};
    const engine::Structure* station = world.station_of(u.owner);
    if (!station) return {"No station: no freight comes", theme::kDanger};
    if (u.haul_cargo == engine::Resource::Count) {
        bool any = false;
        for (engine::Resource r : {engine::Resource::Food, engine::Resource::Ammo, engine::Resource::Fuel}) {
            any = any || (station->cargo[static_cast<size_t>(r)] > 0 && world.haul_destination(u, r));
        }
        return {u.carrying > 0 || any ? "Hauls whatever piles up at the station"
                                      : "Hauls whatever piles up: nothing to take now, waiting for a train",
                theme::kTextDim};
    }
    const engine::Resource r = u.haul_cargo;
    const char* depot_name = engine::structure_type(*engine::depot_for(r)).name;
    const engine::Structure* depot = world.haul_destination(u, r);
    if (!depot) return {TextFormat("Hauls %s: no %s to take it to!", engine::resource_name(r), depot_name), theme::kDanger};
    const char* where = depot->id == u.haul_depot ? "its" : "the nearest";
    if (u.carrying == 0 && station->cargo[static_cast<size_t>(r)] <= 0) {
        return {TextFormat("Hauls %s to %s %s: none at the station, waiting for a train", engine::resource_name(r), where,
                           depot_name),
                theme::kTextDim};
    }
    return {TextFormat("Hauls %s to %s %s", engine::resource_name(r), where, depot_name), theme::kTextDim};
}

}  // namespace

void Hud::draw_unit_card(const engine::World& world, const engine::Unit& u, Rectangle area) const {
    const engine::UnitTypeDef& def = engine::unit_type(u.type);
    const engine::WeaponDef& weapon = engine::weapon_of(u);

    draw_text(def.name, area.x, area.y, kFontSize, theme::player_color(u.owner));
    const char* hp = TextFormat("HP %d / %d", u.hp, def.max_hp);
    draw_text(hp, area.x + area.width - 90 - 10 - static_cast<float>(MeasureText(hp, kCardFontSize)), area.y + 3,
              kCardFontSize, theme::kText);
    draw_hp_bar(u, {area.x + area.width - 90, area.y + 7, 90, 6});

    static constexpr const char* kDamageNames[] = {"bullet", "explosive", "anti-tank"};
    const float line2 = area.y + kFontSize + 8;
    const float step = kCardFontSize + 6;
    const float range = static_cast<float>(weapon.range.raw) / engine::Fixed::kOneRaw;
    const char* armament = "Unarmed";
    if (engine::is_armed(def)) {
        armament = TextFormat("%s%s: %d %s, range %.0f, every %.1f s", def.alt_weapon.damage > 0 ? "Loaded: " : "",
                              weapon.name, weapon.damage, kDamageNames[static_cast<int>(weapon.damage_type)], range,
                              static_cast<float>(weapon.reload) / engine::kTicksPerSecond);
    } else if (u.type == engine::UnitTypeId::Truck) {
        armament = TextFormat("Unarmed. Carries %d from the station to the depots.", engine::kTruckCapacity);
    }
    if (def.aircraft) {
        armament = TextFormat("%s: %d explosive each, all of them in one run along the target", weapon.name,
                              weapon.damage);
    }
    draw_text(armament, area.x, line2, fitting_font(armament, kCardFontSize, area.width), theme::kTextDim);
    draw_text(TextFormat("Armor: bullet %d, explosive %d, anti-tank %d", def.armor[0], def.armor[1], def.armor[2]),
              area.x, line2 + step, kCardFontSize, theme::kTextDim);

    static constexpr const char* kOrderNames[] = {"Idle",          "Moving",           "Attacking",
                                                  "Attack-moving", "Firing at ground", "Moving into a house",
                                                  "Gathering",     "Retraining",       "Building",
                                                  "Supply run",    "Observing, holding fire", "Using a skill",
                                                  "Looking after a unit", "Collecting by the wood"};
    static_assert(std::size(kOrderNames) == static_cast<size_t>(engine::Order::Collect) + 1);
    const char* state = kOrderNames[static_cast<int>(u.order)];
    if (u.order == engine::Order::Ability) state = engine::ability_def(u.order_ability).label;
    if (u.order == engine::Order::Idle && world.find_unit(u.engaged)) state = "Engaging";
    if (u.order == engine::Order::Garrison && world.find_unit(u.order_target)) state = "Mounting up";
    if (def.troop_capacity > 0) {
        state = TextFormat("%s, squad aboard %d/%d", state, static_cast<int>(u.passengers.size()), def.troop_capacity);
        if (!u.riders.empty()) state = TextFormat("%s, on the armor %d/%d", state, static_cast<int>(u.riders.size()), engine::kRidersOnArmor);
    }
    if (const engine::Structure* s = world.find_structure(u.inside)) {
        if (s->type == engine::StructureType::House) {
            state = TextFormat("In a house (%d/%d)", static_cast<int>(s->garrison.size()),
                               engine::structure_type(s->type).capacity);
        } else {
            state = TextFormat("At drill, out in %ds",
                               static_cast<int>((engine::kRetrainTicks - u.work) / engine::kTicksPerSecond + 1));
        }
    }
    if (def.worker && u.order == engine::Order::Gather) {
        const bool stone = world.map().terrain(u.gather_tile) == engine::Terrain::Rock;
        const engine::Structure* hq = world.nearest_owned(u.owner, engine::StructureType::Headquarters, u.pos);
        const engine::Structure* store = world.nearest_owned(u.owner, engine::StructureType::Warehouse, u.pos);
        if (!hq || (store && (store->center - u.pos).length_sq_raw() < (hq->center - u.pos).length_sq_raw())) hq = store;
        state = TextFormat("%s %d/%d, carries it to the %s", stone ? "Quarrying stone" : "Cutting timber", u.carrying,
                           engine::kCarryCapacity, hq ? engine::structure_type(hq->type).name : "... nowhere!");
    } else if (def.worker && u.carrying > 0) {
        state = TextFormat("%s, carrying %d materials", state, u.carrying);
    }
    if (def.aircraft) {
        const bool full = u.rounds >= def.rounds_capacity && u.fuel >= def.fuel_capacity;
        if (!u.airborne) {
            state = u.order == engine::Order::Idle ? (full ? "On the runway, ready" : "On the runway, rearming")
                                                   : "Taking off";
        } else if (u.shots_left > 0) {
            state = def.weapon.aerial_bomb ? "Diving, bombs" : "Diving, rockets";
        } else if (u.shots_left < 0) {
            state = "Going round for another run";
        } else if (u.order == engine::Order::Attack || u.order == engine::Order::AttackGround) {
            state = "On a mission";
        } else {
            state = "Back to the airfield";
        }
        if (u.airborne) {  // how high (a cruising aircraft is above most air defence)
            state = TextFormat("%s, height %d", state, static_cast<int>((u.altitude.raw + engine::Fixed::kOneRaw / 2) / engine::Fixed::kOneRaw));
        }
    }
    if (u.type == engine::UnitTypeId::Truck) {
        if (u.carrying > 0) {
            state = TextFormat("%s, %d %s aboard", state, u.carrying, engine::resource_name(u.carrying_type));
        } else if (u.order == engine::Order::Haul) {
            state = "Supply run, empty";
        }
    }
    const char* where = TextFormat("%s   %s, elevation %d", state,
                                   engine::terrain_def(world.map().terrain_at(u.pos)).name,
                                   world.map().elevation_at(u.pos));
    const int where_font = fitting_font(where, kCardFontSize, area.width);
    draw_text(where, area.x, line2 + 2 * step, where_font, theme::kTextDim);
    if (def.emitter) {
        // On the air, or silent: then do orders get through at once?
        const char* radio = def.relay_range.raw > 0 ? TextFormat("Radio on, relay %d", world.relay_reach(u).to_int())
                                                    : "Radio on";
        Color radio_color = theme::kTextDim;
        if (u.silent) {
            radio = world.in_touch(u) ? "Radio off, relayed"
                                      : TextFormat("Radio off: courier %d s",
                                                   static_cast<int>(engine::kCourierTicks / engine::kTicksPerSecond));
            radio_color = theme::kWarning;
        }
        for (const engine::Courier& c : world.couriers()) {
            if (std::find(c.cmd.units.begin(), c.cmd.units.end(), u.id) == c.cmd.units.end()) continue;
            radio = TextFormat("%s, courier in %d s", u.silent ? "Radio off" : "Radio on",
                               static_cast<int>((c.arrives - world.tick()) / engine::kTicksPerSecond + 1));
            break;
        }
        const float x = area.x + static_cast<float>(MeasureText(where, where_font)) + 24;
        draw_text(radio, x, line2 + 2 * step, fitting_font(radio, kCardFontSize, area.x + area.width - x), radio_color);
    }

    // Fuel and rounds, or the cargo of a service vehicle.
    const char* supply = nullptr;
    Color supply_color = theme::kTextDim;
    if (def.df_range.raw > 0) {
        // A direction finder: set up or not, and how many radios it hears.
        const char* stance = u.deployed ? "Deployed" : "Packed up: deploy to take bearings";
        if (u.deploy_work > 0) {
            stance = TextFormat("%s %d%%", u.deployed ? "Packing up" : "Setting up",
                                static_cast<int>(u.deploy_work * 100 / def.deploy_time));
        }
        int heard = 0;
        for (const engine::Bearing& b : world.bearings()) heard += b.station == u.id ? 1 : 0;
        supply = u.deployed ? TextFormat("%s    Bearings on %d radios within %d tiles", stance, heard, def.df_range.to_int())
                            : stance;
        if (!u.deployed) supply_color = theme::kWarning;
    } else if (def.deploy_time > 0) {
        // A gun: set up or not, what it's loaded with, and how far along the bracketing is.
        static constexpr const char* kShellNames[] = {"HE", "cluster", "incendiary", "white phosphorus"};
        static_assert(std::size(kShellNames) == engine::kShellCount);
        const char* stance = u.deployed ? "Deployed" : "Packed up";
        if (u.deploy_work > 0) {
            stance = TextFormat("%s %d%%", u.deployed ? "Packing up" : "Setting up",
                                static_cast<int>(u.deploy_work * 100 / std::max<engine::Tick>(1, world.deploy_ticks(u))));
        }
        if (def.weapon.indirect) stance = TextFormat("%s, %s", stance, kShellNames[static_cast<size_t>(u.shell)]);
        const char* ranging = "not ranged in";
        if (u.ranging_shots > 0) {
            ranging = TextFormat("ranging shot %d (%d%% on target)", u.ranging_shots,
                                 world.ranging_chance(u.owner, u.ranging_shots));
        }
        supply = TextFormat("%s%s    Rounds %d / %d    %s", stance, u.camouflaged ? ", camouflaged" : "", u.rounds,
                            world.rack(u), ranging);
        if (u.rounds <= 0) supply_color = theme::kDanger;
    } else if (def.fuel_capacity.raw > 0 || def.rounds_capacity > 0) {
        // Only what this one carries: a command vehicle has no rounds, a crew-served weapon no fuel.
        const int fuel = static_cast<int>(to_float(u.fuel));
        const bool thirsty = def.fuel_capacity.raw > 0;
        const bool armed = def.rounds_capacity > 0;
        supply = TextFormat("%s%s%s", thirsty ? TextFormat("Fuel %d / %d tiles", fuel, def.fuel_capacity.to_int()) : "",
                            thirsty && armed ? "    " : "",
                            armed ? TextFormat("Rounds %d / %d", u.rounds, world.rack(u)) : "");
        if ((thirsty && u.fuel.raw <= 0) || (armed && u.rounds <= 0)) supply_color = theme::kDanger;
        if (def.missile_capacity > 0 && world.has_upgrade(u.owner, engine::UpgradeId::Atgm)) {
            supply = TextFormat("%s    ATGM %d / %d", supply, u.missiles, def.missile_capacity);
        }
        if (u.mired > 0) {  // get it out while there's time
            supply = TextFormat("%s    Sinking in the bog: %d%%", supply, static_cast<int>(u.mired * 100 / engine::kBogLimit));
            supply_color = u.mired * 2 > engine::kBogLimit ? theme::kDanger : theme::kWarning;
        }
    } else if (u.type == engine::UnitTypeId::Truck ||
               (def.supplies != engine::Resource::Count && u.order == engine::Order::Haul)) {
        std::tie(supply, supply_color) = truck_status(world, u);
    } else if (def.supplies != engine::Resource::Count) {
        // Standing by it sees to whoever is near; attached or called, to one unit.
        const engine::Unit* v = u.order == engine::Order::Supply ? world.find_unit(u.serves) : nullptr;
        const engine::Structure* post = u.order == engine::Order::Supply ? world.find_structure(u.serves) : nullptr;
        const char* job = v ? TextFormat("%s %s.", u.on_call ? "Answering the radio call of:" : "Attached to:",
                                         engine::unit_type(v->type).name)
                            : TextFormat("Serves our vehicles within %d tiles; RMB on one: attach to it.",
                                         engine::kServiceRadius.to_int());
        if (post) {
            job = TextFormat("Stocking a %s: %d / %d.", engine::structure_type(post->type).name, post->cache,
                             engine::structure_type(post->type).cache_capacity);
        }
        supply = TextFormat("Aboard: %d / %d %s. %s", u.carrying, def.cargo_capacity,
                            engine::resource_name(def.supplies), job);
        if (u.carrying <= 0) supply_color = theme::kWarning;
    }
    // The tankers and ammunition trucks looking after this one.
    int tankers = 0;
    int ammo_trucks = 0;
    for (const engine::Unit& o : world.units()) {
        if (o.order != engine::Order::Supply || o.serves != u.id) continue;
        (engine::unit_type(o.type).supplies == engine::Resource::Fuel ? tankers : ammo_trucks) += 1;
    }
    if (supply && tankers + ammo_trucks > 0) {
        supply = TextFormat("%s    With it: %d tanker(s), %d ammo truck(s)", supply, tankers, ammo_trucks);
    }
    if (supply) {
        draw_text(supply, area.x, line2 + 3 * step, fitting_font(supply, kCardFontSize, area.width), supply_color);
    }
}

void Hud::draw_hp_bar(const engine::Unit& u, Rectangle area) const {
    const float frac = std::clamp(static_cast<float>(u.hp) / static_cast<float>(engine::unit_type(u.type).max_hp),
                                  0.0f, 1.0f);
    const Color fill = frac > 0.6f ? Color{90, 210, 90, 255} : frac > 0.3f ? Color{230, 200, 60, 255}
                                                                             : Color{230, 70, 60, 255};
    DrawRectangleRec(area, {0, 0, 0, 170});
    DrawRectangleRec({area.x, area.y, area.width * frac, area.height}, fill);
}

}  // namespace hud
