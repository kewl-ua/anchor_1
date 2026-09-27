#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <raylib.h>

#include "engine/world.h"

namespace hud {

// Network status to display. Plain data, so the HUD doesn't depend on net.
struct NetStatus {
    bool online = false;
    int ping_ms = -1;
    engine::Tick input_delay = 0;
    bool waiting = false;  // stalled on another player's input
    std::optional<engine::Tick> desync_tick;
    std::string_view message;  // e.g. "Opponent disconnected"
};

// One cell of the command grid. The app fills the grid for whatever is
// selected; the HUD only draws it.
struct CommandButton {
    const char* label = nullptr;    // nullptr: an empty cell
    const char* tooltip = nullptr;  // the full name, shown on hover
    engine::Stock cost{};           // shown on the button; red if we can't afford it
    bool enabled = true;            // dimmed when it can't be used now
    bool active = false;            // lit: being aimed, or switched on
    float cooldown = 0.0f;          // share of the cooldown still to wait, 0..1
};

// The AoE II grid: a hotkey belongs to a cell, whatever the cell holds.
inline constexpr size_t kGridColumns = 5;
inline constexpr size_t kGridRows = 3;
inline constexpr size_t kGridSlots = kGridColumns * kGridRows;
inline constexpr std::array<char, kGridSlots> kGridKeys = {'Q', 'W', 'E', 'R', 'T', 'A', 'S', 'D',
                                                           'F', 'G', 'Z', 'X', 'C', 'V', 'B'};

// Client-side state the HUD displays that is not part of the game state.
struct HudState {
    engine::PlayerId local_player = 0;
    std::span<const engine::EntityId> selection;  // sorted
    engine::EntityId selected_structure = 0;      // one of our buildings, instead of units
    std::span<const CommandButton> commands;      // kGridSlots cells, or empty
    bool dragging = false;
    Rectangle drag_rect{};  // screen space
    std::string_view targeting;  // label of the order being aimed ("Attack-move"...), empty if none
    std::string_view cursor_hint;  // what a click would do on what's under the cursor, empty if nothing
    std::string_view placing;    // name of the building being placed, empty if none
    // Ground points under the screen corners (top-left, top-right,
    // bottom-right, bottom-left), for the camera frame on the minimap.
    std::array<Vector2, 4> view_ground{};
    NetStatus net;
    bool reveal = false;  // fog of war lifted (development)
};

// Screen-space UI. Reads the game state, never changes it: whatever a HUD
// button should do goes back through the app as a command.
class Hud {
public:
    Hud() = default;
    ~Hud();
    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    // Rebuilds cached images (the minimap under the viewer's fog of war)
    // when the map or the fog changes. Call once per frame before draw().
    void update(const engine::World& world, engine::PlayerId viewer, bool reveal);
    void draw(const engine::World& world, const HudState& state) const;

    // True if the point is over a HUD panel, so the click must not reach the world.
    bool captures_point(Vector2 screen_pos) const;
    // Ground point under a screen point, if that point is on the minimap.
    std::optional<Vector2> minimap_to_ground(Vector2 screen_pos) const;
    // Which cell of the command grid is under the point.
    std::optional<size_t> button_at(Vector2 screen_pos) const;
    // Whether the point is on the top bar's idle rear troops and trucks button.
    bool idle_button_at(Vector2 screen_pos) const {
        return idle_rect_.width > 0 && CheckCollisionPointRec(screen_pos, idle_rect_);
    }

private:
    struct Layout {
        Rectangle top_bar;
        Rectangle bottom_panel;
        Rectangle grid;  // the command grid, left in the bottom panel
        Rectangle info;  // what is selected, right of the grid
        Rectangle minimap_panel;
        Rectangle minimap;  // the diamond's bounding box inside the panel
    };
    static Layout layout();

    void draw_top_bar(const engine::World& world, const HudState& state, Rectangle area) const;
    void draw_banner(const NetStatus& net) const;
    void draw_bottom_panel(const engine::World& world, const HudState& state, const Layout& l) const;
    void draw_help(Rectangle area) const;
    void draw_selection(const engine::World& world, const HudState& state, Rectangle area) const;
    void draw_unit_card(const engine::World& world, const engine::Unit& u, Rectangle area) const;
    void draw_structure_card(const engine::World& world, const engine::Structure& s, Rectangle area) const;
    void draw_grid(const HudState& state, const engine::Stock& stock) const;
    void draw_tooltip(const CommandButton& b, const engine::Stock& stock) const;
    static Rectangle button_rect(size_t slot);
    void draw_hp_bar(const engine::Unit& u, Rectangle area) const;
    void draw_minimap(const engine::World& world, const HudState& state, const Layout& l) const;

    // Ground (tiles) <-> minimap-local pixels, for the cached map size.
    Vector2 ground_to_minimap(Vector2 ground, Rectangle minimap) const;

    mutable Rectangle idle_rect_{};  // where draw_top_bar put the idle button
    Texture2D minimap_texture_{};
    int32_t map_width_ = 0;
    int32_t map_height_ = 0;
    uint32_t map_revision_ = 0;
    uint32_t vision_revision_ = 0;
    bool painted_reveal_ = false;
    std::vector<engine::TilePos> pixel_tile_;   // tile under each minimap pixel
    std::vector<float> pixel_light_;            // height shading and contour lines
    std::vector<engine::Terrain> seen_terrain_;  // the ground as last seen
};

// Full-screen message shown before a network game starts.
void draw_lobby_screen(std::string_view status);

}  // namespace hud
