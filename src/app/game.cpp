#include "app/game.h"

#include <algorithm>
#include <optional>

#include "engine/scenario.h"
#include "render/convert.h"
#include "render/iso.h"

namespace app {

namespace {

constexpr float kTickSeconds = 1.0f / engine::Simulation::kTicksPerSecond;
// After a stall, catch up at most this many ticks at once instead of fast-forwarding.
constexpr float kMaxCatchUpTicks = 5.0f;
// Short hiccups are normal; only tell the player about longer waits.
constexpr float kStallNoticeSeconds = 0.3f;

template <typename Pred>
std::optional<Vector2> center_of(const engine::World& world, Pred pred) {
    Vector2 sum{0, 0};
    int count = 0;
    for (const engine::Unit& u : world.units()) {
        if (!pred(u)) continue;
        const Vector2 p = render::to_vector2(u.pos);
        sum.x += p.x;
        sum.y += p.y;
        ++count;
    }
    if (count == 0) return std::nullopt;
    return Vector2{sum.x / static_cast<float>(count), sum.y / static_cast<float>(count)};
}

}  // namespace

Game::Game(uint64_t seed, int32_t map_size, engine::PlayerId local_player, int player_count,
           net::Transport* transport)
    : sim_(seed, engine::make_demo_map(map_size)),
      lockstep_(sim_, local_player, player_count, transport),
      controller_(local_player) {
    engine::setup_demo_scenario(sim_.world_for_setup());
    renderer_.set_viewer(local_player, reveal_);

    camera_.set_bounds(render::iso::map_bounds(sim_.world().map()));
    if (auto army = center_of(sim_.world(), [&](const engine::Unit& u) { return u.owner == local_player; })) {
        center_camera_on(*army);
    }
}

void Game::center_camera_on(Vector2 ground) {
    camera_.center_on(render::iso::project(ground, render::iso::surface_height(sim_.world().map(), ground)));
}

void Game::center_camera_on_selection() {
    const auto selection = controller_.selection();
    auto selected = [&](const engine::Unit& u) { return std::binary_search(selection.begin(), selection.end(), u.id); };
    if (auto center = center_of(sim_.world(), selected)) center_camera_on(*center);
}

void Game::update(float dt) {
    // Wall-clock time only decides WHEN ticks run, never what they compute.
    const float scaled = dt * time_scale_;
    accumulator_ = std::min(accumulator_ + scaled, kTickSeconds * kMaxCatchUpTicks * time_scale_);
    bool stalled = false;
    while (accumulator_ >= kTickSeconds && sim_.world().tick() < tick_limit_) {
        if (!lockstep_.try_step()) {
            stalled = true;
            break;
        }
        accumulator_ -= kTickSeconds;
    }
    stall_time_ = stalled ? stall_time_ + dt : 0.0f;
    alpha_ = std::min(accumulator_ / kTickSeconds, 1.0f);

    camera_.update(dt);
    if (IsKeyPressed(KEY_SPACE)) center_camera_on_selection();

    // Holding the left button on the minimap drags the camera around.
    // (With attack-move armed the click is an order instead.)
    const Vector2 mouse = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        minimap_drag_ = !controller_.targeting() && hud_.minimap_to_ground(mouse).has_value();
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) minimap_drag_ = false;
    if (minimap_drag_) {
        if (auto ground = hud_.minimap_to_ground(mouse)) center_camera_on(*ground);
    }

    // The idle rear troops and trucks, one by one: the '.' key or the top bar button.
    const bool idle_click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && hud_.idle_button_at(mouse);
    if ((IsKeyPressed(KEY_PERIOD) || idle_click) && controller_.select_next_idle(sim_.world())) {
        center_camera_on_selection();
    }

    controller_.update(sim_.world(), lockstep_, camera_, hud_, renderer_, alpha_);
    renderer_.update(sim_.world(), scaled);
    hud_.update(sim_.world(), lockstep_.local_player(), reveal_);
}

void Game::draw(hud::NetStatus net) const {
    std::optional<render::BuildGhost> ghost;
    if (const auto& p = controller_.placement()) ghost = render::BuildGhost{p->type, p->origin, p->valid};
    renderer_.draw(sim_.world(), camera_, alpha_, controller_.selection(), controller_.selected_structure(),
                   ghost ? &*ghost : nullptr, controller_.trench_preview());

    net.input_delay = lockstep_.input_delay();
    net.waiting = stall_time_ >= kStallNoticeSeconds;
    net.desync_tick = lockstep_.desync_tick();

    const auto w = static_cast<float>(GetScreenWidth());
    const auto h = static_cast<float>(GetScreenHeight());
    auto ground_at = [&](Vector2 screen) {
        return render::iso::pick_ground(sim_.world().map(), camera_.screen_to_world(screen));
    };

    hud_.draw(sim_.world(), {
        .local_player = lockstep_.local_player(),
        .selection = controller_.selection(),
        .selected_structure = controller_.selected_structure(),
        .commands = controller_.command_buttons(),
        .dragging = controller_.dragging(),
        .drag_rect = controller_.drag_rect(),
        .targeting = controller_.targeting_label(),
        .cursor_hint = controller_.cursor_hint(),
        .placing = controller_.placement() ? engine::structure_type(controller_.placement()->type).name : "",
        .view_ground = {ground_at({0, 0}), ground_at({w, 0}), ground_at({w, h}), ground_at({0, h})},
        .net = net,
        .reveal = reveal_,
    });
}

void Game::set_reveal(bool reveal) {
    reveal_ = reveal;
    renderer_.set_viewer(lockstep_.local_player(), reveal_);
}

void Game::select_army_and_attack_move(Vector2 ground) {
    controller_.select_army(sim_.world());
    controller_.order_attack_move(lockstep_, renderer_, ground);
}

}  // namespace app
