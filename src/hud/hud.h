#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

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

// Client-side state the HUD displays that is not part of the game state.
struct HudState {
    engine::PlayerId local_player = 0;
    std::span<const engine::EntityId> selection;  // sorted
    engine::EntityId selected_structure = 0;      // one of our buildings, instead of units
    bool dragging = false;
    Rectangle drag_rect{};  // screen space
    std::string_view targeting;  // label of the order being aimed ("Attack-move"...), empty if none
    std::string_view placing;    // name of the building being placed, empty if none
    // Ground points under the screen corners (top-left, top-right,
    // bottom-right, bottom-left), for the camera frame on the minimap.
    std::array<Vector2, 4> view_ground{};
    NetStatus net;
};

// Screen-space UI. Reads the game state, never changes it: whatever a HUD
// button should do goes back through the app as a command.
class Hud {
public:
    Hud() = default;
    ~Hud();
    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    // Rebuilds cached images (the minimap terrain) when the map changes.
    // Call once per frame before draw().
    void update(const engine::World& world);
    void draw(const engine::World& world, const HudState& state) const;

    // True if the point is over a HUD panel, so the click must not reach the world.
    bool captures_point(Vector2 screen_pos) const;
    // Ground point under a screen point, if that point is on the minimap.
    std::optional<Vector2> minimap_to_ground(Vector2 screen_pos) const;
    // Which command-panel button slot is under the point: a building's
    // hiring roster, or the rear troops' building list.
    std::optional<size_t> button_at(Vector2 screen_pos) const;
    static constexpr size_t kButtonSlots = 5;

private:
    struct Layout {
        Rectangle top_bar;
        Rectangle bottom_panel;
        Rectangle minimap_panel;
        Rectangle minimap;  // the diamond's bounding box inside the panel
    };
    static Layout layout();

    void draw_top_bar(const engine::World& world, const HudState& state, Rectangle area) const;
    void draw_banner(const NetStatus& net) const;
    void draw_bottom_panel(const engine::World& world, const HudState& state, Rectangle area) const;
    void draw_help(Rectangle area) const;
    void draw_selection(const engine::World& world, const HudState& state, Rectangle area) const;
    void draw_unit_card(const engine::World& world, const engine::Unit& u, Rectangle area) const;
    void draw_structure_card(const engine::World& world, const engine::Structure& s, Rectangle area) const;
    // One command button: hotkey + name on top, price below; dim if unaffordable.
    void draw_button(size_t slot, char hotkey, const char* name, const engine::Stock& cost,
                     const engine::Stock& stock) const;
    static Rectangle button_rect(size_t slot);
    void draw_hp_bar(const engine::Unit& u, Rectangle area) const;
    void draw_minimap(const engine::World& world, const HudState& state, const Layout& l) const;

    // Ground (tiles) <-> minimap-local pixels, for the cached map size.
    Vector2 ground_to_minimap(Vector2 ground, Rectangle minimap) const;

    Texture2D minimap_texture_{};
    int32_t map_width_ = 0;
    int32_t map_height_ = 0;
    uint32_t map_revision_ = 0;
};

// Full-screen message shown before a network game starts.
void draw_lobby_screen(std::string_view status);

}  // namespace hud
