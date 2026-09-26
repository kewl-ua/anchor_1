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
constexpr float kPanelWidth = 620.0f;
constexpr float kPanelHeight = 140.0f;
constexpr float kPadding = 12.0f;
constexpr int kFontSize = 18;
constexpr int kSmallFontSize = 16;

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

    // The command panel is centered in the space left of the minimap.
    const float free_w = minimap_panel.x - 2 * kPadding;
    const float panel_w = std::max(0.0f, std::min(kPanelWidth, free_w));
    const Rectangle bottom_panel{kPadding + (free_w - panel_w) * 0.5f, h - kPanelHeight - kPadding, panel_w,
                                 kPanelHeight};

    return {{0, 0, w, kTopBarHeight}, bottom_panel, minimap_panel, minimap};
}

bool Hud::captures_point(Vector2 p) const {
    const Layout l = layout();
    return CheckCollisionPointRec(p, l.top_bar) || CheckCollisionPointRec(p, l.bottom_panel) ||
           CheckCollisionPointRec(p, l.minimap_panel);
}

void Hud::update(const engine::World& world) {
    const engine::TileMap& map = world.map();
    if (minimap_texture_.id != 0 && map.width() == map_width_ && map.height() == map_height_ &&
        map.revision() == map_revision_) {
        return;
    }
    map_width_ = map.width();
    map_height_ = map.height();
    map_revision_ = map.revision();
    if (minimap_texture_.id != 0) UnloadTexture(minimap_texture_);

    // Tile under every minimap pixel, (-1, -1) outside the map.
    std::vector<engine::TilePos> tile(static_cast<size_t>(kMinimapWidth * kMinimapHeight), {-1, -1});
    auto at = [&](int px, int py) -> engine::TilePos& { return tile[static_cast<size_t>(py * kMinimapWidth + px)]; };
    for (int py = 0; py < kMinimapHeight; ++py) {
        for (int px = 0; px < kMinimapWidth; ++px) {
            const Vector2 g = minimap_pixel_to_ground({px + 0.5f, py + 0.5f}, static_cast<float>(map_width_),
                                                      static_cast<float>(map_height_));
            const engine::TilePos t{static_cast<int32_t>(std::floor(g.x)), static_cast<int32_t>(std::floor(g.y))};
            if (map.contains(t)) at(px, py) = t;
        }
    }
    auto level = [&](int px, int py) {
        const engine::TilePos t = at(px, py);
        return map.contains(t) ? map.elevation(t.x, t.y) : -1;
    };

    // Terrain colors, higher ground lighter; borders between elevation levels
    // are drawn as contour lines, so the heights worth fighting over stand out.
    Image image = GenImageColor(kMinimapWidth, kMinimapHeight, BLANK);
    for (int py = 0; py < kMinimapHeight; ++py) {
        for (int px = 0; px < kMinimapWidth; ++px) {
            const engine::TilePos t = at(px, py);
            if (!map.contains(t)) continue;
            const int l = level(px, py);
            const bool contour = (px > 0 && level(px - 1, py) >= 0 && level(px - 1, py) != l) ||
                                 (py > 0 && level(px, py - 1) >= 0 && level(px, py - 1) != l);
            const Color base = theme::terrain_color(map.terrain(t));
            const Color c = contour ? shade(base, 0.6f) : shade(base, 0.85f + 0.12f * static_cast<float>(l));
            ImageDrawPixel(&image, px, py, c);
        }
    }
    minimap_texture_ = LoadTextureFromImage(image);
    UnloadImage(image);
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
    draw_bottom_panel(world, state, l.bottom_panel);
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
    const engine::Tick until = world.next_reinforcement() - world.tick();
    const char* reinforcements = TextFormat("+%d men in %ds", engine::kReinforcementSize,
                                            static_cast<int>((until + engine::kTicksPerSecond - 1) / engine::kTicksPerSecond));
    draw_text(reinforcements, x, y, kSmallFontSize, theme::kTextDim);
    x += static_cast<float>(MeasureText(reinforcements, kSmallFontSize)) + 30;

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

void Hud::draw_bottom_panel(const engine::World& world, const HudState& state, Rectangle area) const {
    draw_panel(area);
    const Rectangle inner{area.x + kPadding, area.y + kPadding, area.width - 2 * kPadding,
                          area.height - 2 * kPadding};
    if (const engine::Structure* s = world.find_structure(state.selected_structure)) {
        draw_structure_card(world, *s, inner);
    } else if (state.selection.empty()) {
        draw_help(inner);
    } else {
        draw_selection(world, state, inner);
    }
}

Rectangle Hud::button_rect(size_t slot) {
    const Layout l = layout();
    const Rectangle inner{l.bottom_panel.x + kPadding, l.bottom_panel.y + kPadding, l.bottom_panel.width - 2 * kPadding,
                          l.bottom_panel.height - 2 * kPadding};
    constexpr float kGap = 6.0f;
    constexpr float kHeight = 34.0f;
    const float width = (inner.width - kGap * static_cast<float>(kButtonSlots - 1)) / static_cast<float>(kButtonSlots);
    return {inner.x + static_cast<float>(slot) * (width + kGap), inner.y + inner.height - kHeight, width, kHeight};
}

std::optional<size_t> Hud::button_at(Vector2 p) const {
    for (size_t slot = 0; slot < kButtonSlots; ++slot) {
        if (CheckCollisionPointRec(p, button_rect(slot))) return slot;
    }
    return std::nullopt;
}

void Hud::draw_button(size_t slot, char hotkey, const char* name, const engine::Stock& cost,
                      const engine::Stock& stock) const {
    static constexpr const char* kShortNames[] = {"men", "food", "mat.", "ammo", "fuel"};
    const Rectangle r = button_rect(slot);
    const bool affordable = engine::can_afford(stock, cost);
    const bool hover = CheckCollisionPointRec(GetMousePosition(), r);
    DrawRectangleRec(r, hover ? Color{52, 60, 54, 255} : Color{32, 36, 34, 255});
    DrawRectangleLinesEx(r, 1.0f, theme::kPanelBorder);
    draw_text(TextFormat("%c  %s", hotkey, name), r.x + 6, r.y + 4, 14, affordable ? theme::kText : theme::kTextDim);

    char price[96] = {};
    size_t used = 0;
    for (size_t i = 0; i < engine::kResourceCount && used < sizeof(price); ++i) {
        if (cost[i] == 0) continue;
        used += static_cast<size_t>(std::snprintf(price + used, sizeof(price) - used, "%d %s  ", cost[i], kShortNames[i]));
    }
    draw_text(price, r.x + 6, r.y + 21, 10, affordable ? theme::kTextDim : theme::kDanger);
}

void Hud::draw_structure_card(const engine::World& world, const engine::Structure& s, Rectangle area) const {
    const engine::StructureDef& def = engine::structure_type(s.type);
    draw_text(def.name, area.x, area.y, kFontSize, theme::player_color(s.owner));
    draw_text(TextFormat("HP %d / %d", s.hp, def.max_hp), area.x + 220, area.y + 2, kSmallFontSize, theme::kText);
    const float frac = static_cast<float>(s.hp) / static_cast<float>(def.max_hp);
    DrawRectangleRec({area.x + 340, area.y + 6, 120, 6}, {0, 0, 0, 170});
    DrawRectangleRec({area.x + 340, area.y + 6, 120 * frac, 6}, {200, 200, 190, 255});

    const float line2 = area.y + kFontSize + 10;
    if (!s.built) {
        const float done = static_cast<float>(s.build_progress) / static_cast<float>(def.build_time);
        draw_text(TextFormat("Under construction: %d%%   (RMB with rear troops to help)", static_cast<int>(done * 100)),
                  area.x, line2, kSmallFontSize, theme::kWarning);
        DrawRectangleRec({area.x, line2 + 24, 300, 6}, {0, 0, 0, 170});
        DrawRectangleRec({area.x, line2 + 24, 300 * done, 6}, {230, 200, 60, 255});
        return;
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
                  area.x + 5 * (kIconW + 4) + 8, line2 + 6, kSmallFontSize, theme::kTextDim);
    }
    if (!s.garrison.empty()) {
        draw_text(TextFormat("%d at drill", static_cast<int>(s.garrison.size())), area.x + 360, line2 + 6,
                  kSmallFontSize, theme::kTextDim);
    }

    static constexpr char kHireKeys[] = {'Q', 'W', 'E'};
    const engine::Stock& stock = world.stock(s.owner);
    for (size_t i = 0; i < std::min<size_t>(def.roster_size, kButtonSlots); ++i) {
        const engine::UnitTypeDef& unit = engine::unit_type(def.roster[i]);
        draw_button(i, kHireKeys[i], unit.name, unit.cost, stock);
    }
}

void Hud::draw_help(Rectangle area) const {
    const char* lines[] = {
        "LMB: select / drag box    Shift+LMB: add to selection    F2: select army",
        "RMB: move / attack / infantry into a house    S: stop    F10: quit",
        "A + LMB: attack-move    G + LMB: fire at ground    Space: jump to selection",
        "Rear troops: RMB forest/rock to gather, 1-3 build, R retrain",
        "Buildings: click to select, Q/W/E hire    Minimap: LMB camera, RMB send",
    };
    constexpr int kHelpFontSize = 14;
    float y = area.y;
    for (const char* line : lines) {
        draw_text(line, area.x, y, kHelpFontSize, theme::kTextDim);
        y += kHelpFontSize + 7;
    }
}

void Hud::draw_selection(const engine::World& world, const HudState& state, Rectangle area) const {
    const int count = static_cast<int>(state.selection.size());

    // Rear troops in the selection get the building buttons.
    bool workers = false;
    for (engine::EntityId id : state.selection) {
        const engine::Unit* u = world.find_unit(id);
        workers = workers || (u && engine::unit_type(u->type).worker);
    }
    if (workers) {
        static constexpr char kBuildKeys[] = {'1', '2', '3'};
        const engine::Stock& stock = world.stock(state.local_player);
        for (size_t i = 0; i < std::min(std::size(engine::kBuildable), kButtonSlots); ++i) {
            const engine::StructureDef& def = engine::structure_type(engine::kBuildable[i]);
            draw_button(i, kBuildKeys[i], def.name, def.cost, stock);
        }
    }
    const float bottom = workers ? button_rect(0).y - 4 : area.y + area.height;

    if (count == 1) {
        if (const engine::Unit* u = world.find_unit(state.selection.front())) draw_unit_card(world, *u, area);
        return;
    }

    draw_text(TextFormat("Selected: %d", count), area.x, area.y, kFontSize, theme::kText);

    // One icon per unit, as many as fit above the buttons.
    constexpr float kIconW = 34.0f;
    constexpr float kIconH = 28.0f;
    constexpr float kGap = 4.0f;
    const int per_row = std::max(1, static_cast<int>((area.width + kGap) / (kIconW + kGap)));
    const float top = area.y + kFontSize + 10;
    const int max_rows = std::max(1, static_cast<int>((bottom - top + kGap) / (kIconH + kGap)));
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
    const engine::WeaponDef& weapon = def.weapon;

    draw_text(def.name, area.x, area.y, kFontSize, theme::player_color(u.owner));
    draw_text(TextFormat("HP %d / %d", u.hp, def.max_hp), area.x + 180, area.y + 2, kSmallFontSize, theme::kText);
    draw_hp_bar(u, {area.x + 300, area.y + 6, 120, 6});

    static constexpr const char* kDamageNames[] = {"bullet", "explosive", "anti-tank"};
    const float line2 = area.y + kFontSize + 6;
    const float range = static_cast<float>(weapon.range.raw) / engine::Fixed::kOneRaw;
    draw_text(TextFormat("%s: %d %s, range %.1f, every %.1f s", weapon.name, weapon.damage,
                         kDamageNames[static_cast<int>(weapon.damage_type)], range,
                         static_cast<float>(weapon.reload) / engine::kTicksPerSecond),
              area.x, line2, kSmallFontSize, theme::kTextDim);
    draw_text(TextFormat("Armor: bullet %d, explosive %d, anti-tank %d", def.armor[0], def.armor[1], def.armor[2]),
              area.x, line2 + kSmallFontSize + 4, kSmallFontSize, theme::kTextDim);

    static constexpr const char* kOrderNames[] = {"Idle",         "Moving",           "Attacking",
                                                  "Attack-moving", "Firing at ground", "Moving into a house",
                                                  "Gathering",     "Retraining"};
    static_assert(std::size(kOrderNames) == static_cast<size_t>(engine::Order::Retrain) + 1);
    const char* state = kOrderNames[static_cast<int>(u.order)];
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
    draw_text(TextFormat("%s   %s, elevation %d", state, engine::terrain_def(world.map().terrain_at(u.pos)).name,
                         world.map().elevation_at(u.pos)),
              area.x, line2 + 2 * (kSmallFontSize + 4), kSmallFontSize, theme::kTextDim);
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
