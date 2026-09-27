#include "hud/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
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

    if (!state.targeting.empty()) {
        const Vector2 m = GetMousePosition();
        DrawCircleLinesV(m, 9.0f, theme::kDanger);
        DrawLineV({m.x - 13, m.y}, {m.x + 13, m.y}, theme::kDanger);
        DrawLineV({m.x, m.y - 13}, {m.x, m.y + 13}, theme::kDanger);
        draw_text(TextFormat("%.*s", static_cast<int>(state.targeting.size()), state.targeting.data()), m.x + 14,
                  m.y + 8, kSmallFontSize, theme::kDanger);
    }
    if (!state.placing.empty()) {
        const Vector2 m = GetMousePosition();
        draw_text(TextFormat("%.*s: LMB to place, Shift for more, RMB cancels", static_cast<int>(state.placing.size()),
                             state.placing.data()),
                  m.x + 16, m.y + 16, kSmallFontSize, theme::kText);
    }

    const Layout l = layout();
    draw_top_bar(world, state, l.top_bar);
    draw_banner(state.net);
    draw_bottom_panel(world, state, l);
    draw_minimap(world, state, l);
}

void Hud::draw_top_bar(const engine::World& world, const HudState& state, Rectangle area) const {
    DrawRectangleRec(area, theme::kPanel);
    DrawLineV({area.x, area.y + area.height}, {area.x + area.width, area.y + area.height}, theme::kPanelBorder);

    const float y = area.y + (area.height - kSmallFontSize) * 0.5f;
    float x = area.x + kPadding;

    // Our stockpile first: it's what the player looks at most.
    const engine::Stock& stock = world.stock(state.local_player);
    for (size_t r = 0; r < engine::kResourceCount; ++r) {
        const char* name = engine::resource_name(static_cast<engine::Resource>(r));
        draw_text(name, x, y, kSmallFontSize, theme::kTextDim);
        x += static_cast<float>(MeasureText(name, kSmallFontSize)) + 6;
        const char* amount = TextFormat("%d", stock[r]);
        draw_text(amount, x, y, kSmallFontSize, theme::kText);
        x += static_cast<float>(MeasureText(amount, kSmallFontSize)) + 18;
    }
    // Men and freight only come by rail.
    const char* train = "No station: no trains";
    Color train_color = theme::kDanger;
    if (const engine::Structure* station = world.station_of(state.local_player)) {
        train = TextFormat("Train in %ds (+%d men)", seconds_until(world, station->next_train),
                           engine::kTrainCargo[static_cast<size_t>(engine::Resource::Personnel)]);
        train_color = theme::kTextDim;
    }
    draw_text(train, x, y, kSmallFontSize, train_color);
    x += static_cast<float>(MeasureText(train, kSmallFontSize)) + 30;

    // Debug and network details, smaller.
    constexpr int kTiny = 14;
    const float ty = area.y + (area.height - kTiny) * 0.5f;
    const char* stats = TextFormat("Tick %u  FPS %d  Delay %u", world.tick(), GetFPS(), state.net.input_delay);
    draw_text(stats, x, ty, kTiny, theme::kTextDim);
    x += static_cast<float>(MeasureText(stats, kTiny));
    if (state.net.online) {
        const char* online = TextFormat("  Ping %d ms  ", state.net.ping_ms);
        draw_text(online, x, ty, kTiny, theme::kTextDim);
        x += static_cast<float>(MeasureText(online, kTiny));
        draw_text(TextFormat("You: Player %d", state.local_player + 1), x, ty, kTiny,
                  theme::player_color(state.local_player));
    }

    const char* checksum = TextFormat("%016llX", static_cast<unsigned long long>(world.checksum()));
    const float w = static_cast<float>(MeasureText(checksum, kTiny));
    draw_text(checksum, area.x + area.width - w - kPadding, ty, kTiny, theme::kTextDim);
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
    const Vector2 mouse = GetMousePosition();
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
    const float x = std::clamp(GetMousePosition().x - w * 0.5f, 4.0f, static_cast<float>(GetScreenWidth()) - w - 4);
    const Rectangle r{x, l.bottom_panel.y - h - 6, w, h};
    draw_panel(r);
    draw_text(title, r.x + 8, r.y + 7, kCardFontSize, theme::kText);
    if (price[0]) {
        draw_text(price, r.x + 8, r.y + 26, 12, engine::can_afford(stock, b.cost) ? theme::kTextDim : theme::kDanger);
    }
}

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
    draw_text(def.name, area.x, area.y, kFontSize, theme::player_color(s.owner));
    const char* hp = TextFormat("HP %d / %d", s.hp, def.max_hp);
    const float hp_x = area.x + area.width - 90 - 10 - static_cast<float>(MeasureText(hp, kCardFontSize));
    draw_text(hp, hp_x, area.y + 3, kCardFontSize, theme::kText);
    const float frac = static_cast<float>(s.hp) / static_cast<float>(def.max_hp);
    DrawRectangleRec({area.x + area.width - 90, area.y + 7, 90, 6}, {0, 0, 0, 170});
    DrawRectangleRec({area.x + area.width - 90, area.y + 7, 90 * frac, 6}, {200, 200, 190, 255});

    const float line2 = area.y + kFontSize + 10;
    const float line3 = line2 + kCardFontSize + 6;
    const float line4 = line3 + kCardFontSize + 6;
    if (!s.built) {
        const float done = static_cast<float>(s.build_progress) / static_cast<float>(def.build_time);
        draw_text(TextFormat("Under construction: %d%%", static_cast<int>(done * 100)), area.x, line2, kCardFontSize,
                  theme::kWarning);
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
    switch (s.type) {
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
            return;
        case engine::StructureType::AmmoDepot:
            draw_text("Takes ammunition from supply trucks.", area.x, line2, kCardFontSize, theme::kTextDim);
            draw_text("Rear troops standing by unload trucks faster.", area.x, line3, kCardFontSize, theme::kTextDim);
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
        case engine::StructureType::Dugout:
            draw_text(TextFormat("Shelter: %d / %d inside. Bullets and fragments don't reach them.",
                                 static_cast<int>(s.garrison.size()), def.capacity),
                      area.x, line2, kCardFontSize, theme::kText);
            draw_text("No firing from inside, and no seeing out.", area.x, line3, kCardFontSize, theme::kTextDim);
            draw_text("A grenade through the entrance hurts those inside.", area.x, line4, kCardFontSize,
                      theme::kWarning);
            return;
        case engine::StructureType::FuelDepot:
            draw_text("Takes fuel from supply trucks.", area.x, line2, kCardFontSize, theme::kTextDim);
            draw_text(TextFormat("Burns if destroyed: %d%% of the fuel is lost.", engine::kFuelDepotLossPercent),
                      area.x, line3, kCardFontSize, theme::kWarning);
            return;
        default: break;
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
    draw_text(armament, area.x, line2, fitting_font(armament, kCardFontSize, area.width), theme::kTextDim);
    draw_text(TextFormat("Armor: bullet %d, explosive %d, anti-tank %d", def.armor[0], def.armor[1], def.armor[2]),
              area.x, line2 + step, kCardFontSize, theme::kTextDim);

    static constexpr const char* kOrderNames[] = {"Idle",          "Moving",           "Attacking",
                                                  "Attack-moving", "Firing at ground", "Moving into a house",
                                                  "Gathering",     "Retraining",       "Building",
                                                  "Supply run",    "Observing, holding fire", "Using a skill"};
    static_assert(std::size(kOrderNames) == static_cast<size_t>(engine::Order::Ability) + 1);
    const char* state = kOrderNames[static_cast<int>(u.order)];
    if (u.order == engine::Order::Ability) state = engine::ability_def(u.order_ability).label;
    if (u.order == engine::Order::Idle && world.find_unit(u.engaged)) state = "Engaging";
    if (const engine::Structure* s = world.find_structure(u.inside)) {
        if (s->type == engine::StructureType::House) {
            state = TextFormat("In a house (%d/%d)", static_cast<int>(s->garrison.size()),
                               engine::structure_type(s->type).capacity);
        } else {
            state = TextFormat("At drill, out in %ds",
                               static_cast<int>((engine::kRetrainTicks - u.work) / engine::kTicksPerSecond + 1));
        }
    }
    if (def.worker && u.carrying > 0) state = TextFormat("%s, carrying %d materials", state, u.carrying);
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
        const char* radio = def.relay_range.raw > 0 ? TextFormat("Radio on, relay %d", def.relay_range.to_int()) : "Radio on";
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
        // A gun: set up or not, and how far along the bracketing is.
        const char* stance = u.deployed ? "Deployed" : "Packed up";
        if (u.deploy_work > 0) {
            stance = TextFormat("%s %d%%", u.deployed ? "Packing up" : "Setting up",
                                static_cast<int>(u.deploy_work * 100 / def.deploy_time));
        }
        const char* ranging = "not ranged in";
        if (u.ranging_shots > 0) {
            ranging = TextFormat("ranging shot %d (%d%% on target)", u.ranging_shots,
                                 engine::kRangingChance[static_cast<size_t>(u.ranging_shots) - 1]);
        }
        supply = TextFormat("%s%s    Rounds %d / %d    %s", stance, u.camouflaged ? ", camouflaged" : "", u.rounds,
                            def.rounds_capacity, ranging);
        if (u.rounds <= 0) supply_color = theme::kDanger;
    } else if (def.fuel_capacity.raw > 0 || def.rounds_capacity > 0) {
        // Only what this one carries: a command vehicle has no rounds, a crew-served weapon no fuel.
        const int fuel = static_cast<int>(to_float(u.fuel));
        const bool thirsty = def.fuel_capacity.raw > 0;
        const bool armed = def.rounds_capacity > 0;
        supply = TextFormat("%s%s%s", thirsty ? TextFormat("Fuel %d / %d tiles", fuel, def.fuel_capacity.to_int()) : "",
                            thirsty && armed ? "    " : "",
                            armed ? TextFormat("Rounds %d / %d", u.rounds, def.rounds_capacity) : "");
        if ((thirsty && u.fuel.raw <= 0) || (armed && u.rounds <= 0)) supply_color = theme::kDanger;
    } else if (def.supplies != engine::Resource::Count) {
        supply = TextFormat("Aboard: %d / %d %s. Serves our vehicles within %d tiles.", u.carrying,
                            def.cargo_capacity, engine::resource_name(def.supplies),
                            engine::kServiceRadius.to_int());
        if (u.carrying <= 0) supply_color = theme::kWarning;
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
