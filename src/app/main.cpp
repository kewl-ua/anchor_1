#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <raylib.h>

#include "app/game.h"
#include "engine/scenario.h"
#include "net/enet_session.h"
#include "net/protocol.h"
#include "render/convert.h"
#include "theme/palette.h"

namespace {

struct Options {
    enum class Mode { Offline, Host, Join };

    Mode mode = Mode::Offline;
    std::string address;
    uint16_t port = net::kDefaultPort;
    int32_t map_size = engine::kDefaultMapSize;  // a joining client uses the host's
    // `--smoke-test <file.png>`: plays a short scripted scene, saves a
    // screenshot and exits. Lets you (or Claude) check rendering and netcode
    // without clicking around.
    std::string smoke_screenshot;
    // `--look x,y`: during the smoke test, keep the camera on this ground
    // point (in tiles) instead of following the army.
    std::optional<Vector2> look;
    // `--reveal`: no fog of war on screen (the game itself still plays by it).
    bool reveal = false;
    // `--ticks n`: take the smoke screenshot at this tick.
    std::optional<engine::Tick> smoke_ticks;
    // `--scene garrison`: instead of attacking, the infantry moves into the
    // nearest house and the tanks shell the next one until it collapses.
    // Also `economy`, `build`, `logistics` (depots by the station, supply
    // trucks, the first train) and `recon` (scouts' observation posts).
    std::string scene;
};

// The scripted part of the smoke test. Returns where the camera should look,
// if the scene has a spot of its own.
std::optional<Vector2> start_smoke_scene(app::Game& game, const Options& options) {
    const engine::World& world = game.world();
    const engine::PlayerId me = game.local_player();

    if (options.scene == "economy") {
        // Rear troops cut the nearest woodline; the headquarters hires two more.
        const engine::FixedVec2 base = engine::demo_base_position(world.map().width(), me);
        const engine::TilePos b = engine::tile_of(base);
        std::optional<engine::TilePos> wood;
        int32_t best = 0;
        for (int32_t dy = -40; dy <= 40; ++dy) {
            for (int32_t dx = -40; dx <= 40; ++dx) {
                const engine::TilePos t{b.x + dx, b.y + dy};
                if (!world.map().contains(t) || world.map().terrain(t) != engine::Terrain::Forest) continue;
                if (!wood || dx * dx + dy * dy < best) {
                    wood = t;
                    best = dx * dx + dy * dy;
                }
            }
        }
        engine::Command gather{.type = engine::CommandType::Gather};
        for (const engine::Unit& u : world.units()) {
            if (u.owner == me && engine::unit_type(u.type).worker) gather.units.push_back(u.id);
        }
        if (wood) gather.target = engine::tile_center(*wood);
        game.select_units(gather.units);  // shows the building buttons
        game.submit(gather);
        for (const engine::Structure& s : world.structures()) {
            if (s.type != engine::StructureType::Headquarters || s.owner != me) continue;
            for (int i = 0; i < 2; ++i) {
                game.submit({.type = engine::CommandType::Train, .target_unit = s.id,
                             .unit_type = static_cast<uint8_t>(engine::UnitTypeId::Worker)});
            }
        }
        const Vector2 b2 = render::to_vector2(base);
        const Vector2 w2 = wood ? render::to_vector2(engine::tile_center(*wood)) : b2;
        return Vector2{(b2.x + w2.x) * 0.5f, (b2.y + w2.y) * 0.5f};
    }

    if (options.scene == "logistics") {
        // Rear troops put up an ammunition depot and a warehouse by the
        // station; the headquarters hires two supply trucks for the first train.
        const int32_t size = world.map().width();
        const engine::TilePos station = engine::demo_station_origin(size, 0);
        engine::TilePos ammo{station.x + 6, station.y + 4};
        engine::TilePos food{station.x + 9, station.y + 4};
        if (me == 1) {  // the same spots, mirrored through the map center
            ammo = {size - 2 - ammo.x, size - 2 - ammo.y};
            food = {size - 2 - food.x, size - 2 - food.y};
        }
        std::vector<engine::EntityId> workers;
        for (const engine::Unit& u : world.units()) {
            if (u.owner == me && engine::unit_type(u.type).worker) workers.push_back(u.id);
        }
        if (workers.size() < 5) return std::nullopt;
        game.submit({.type = engine::CommandType::Build,
                     .units = {workers[0], workers[1], workers[2]},
                     .target = engine::tile_center(ammo),
                     .structure_type = static_cast<uint8_t>(engine::StructureType::AmmoDepot)});
        game.submit({.type = engine::CommandType::Build,
                     .units = {workers[3], workers[4]},
                     .target = engine::tile_center(food),
                     .structure_type = static_cast<uint8_t>(engine::StructureType::Warehouse)});
        for (const engine::Structure& s : world.structures()) {
            if (s.type != engine::StructureType::Headquarters || s.owner != me) continue;
            for (int i = 0; i < 2; ++i) {
                game.submit({.type = engine::CommandType::Train, .target_unit = s.id,
                             .unit_type = static_cast<uint8_t>(engine::UnitTypeId::Truck)});
            }
        }
        const Vector2 a = render::to_vector2(world.station_of(me)->center);
        const Vector2 b = render::to_vector2(engine::tile_center(ammo));
        return Vector2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    }

    if (options.scene == "recon") {
        // The two scouts take up observation posts watching towards the
        // enemy; the sectors show while they are selected.
        std::vector<engine::EntityId> scouts;
        Vector2 sum{0, 0};
        for (const engine::Unit& u : world.units()) {
            if (u.owner != me || u.type != engine::UnitTypeId::Scout) continue;
            scouts.push_back(u.id);
            sum.x += render::to_vector2(u.pos).x;
            sum.y += render::to_vector2(u.pos).y;
        }
        if (scouts.empty()) return std::nullopt;
        const engine::TilePos b = engine::tile_of(engine::demo_base_position(world.map().width(), me));
        const int32_t fwd = me == 0 ? 1 : -1;
        game.submit({.type = engine::CommandType::Observe,
                     .units = scouts,
                     .target = engine::tile_center({b.x + 30 * fwd, b.y - 30 * fwd})});
        game.select_units(scouts);
        const auto n = static_cast<float>(scouts.size());
        return Vector2{sum.x / n, sum.y / n};
    }

    if (options.scene == "build") {
        // Three rear troops put up an infantry barracks in front of the
        // headquarters, two a warehouse towards the woodline.
        const engine::FixedVec2 base = engine::demo_base_position(world.map().width(), me);
        const engine::TilePos b = engine::tile_of(base);
        const int32_t fwd = me == 0 ? 1 : -1;
        std::vector<engine::EntityId> workers;
        for (const engine::Unit& u : world.units()) {
            if (u.owner == me && engine::unit_type(u.type).worker) workers.push_back(u.id);
        }
        if (workers.size() < 5) return std::nullopt;
        const engine::TilePos barracks{b.x + 4 * fwd - 1, b.y - 6 * fwd - 1};
        const engine::TilePos warehouse{b.x - 10 * fwd, b.y + 6 * fwd};
        game.submit({.type = engine::CommandType::Build,
                     .units = {workers[0], workers[1], workers[2]},
                     .target = engine::tile_center(barracks),
                     .structure_type = static_cast<uint8_t>(engine::StructureType::InfantryBarracks)});
        game.submit({.type = engine::CommandType::Build,
                     .units = {workers[3], workers[4]},
                     .target = engine::tile_center(warehouse),
                     .structure_type = static_cast<uint8_t>(engine::StructureType::Warehouse)});
        return render::to_vector2(base);
    }

    if (options.scene != "garrison") {
        // Attack-move into the enemy base. Offline the enemy waits at home;
        // online both armies meet halfway.
        const engine::FixedVec2 enemy_base =
            engine::demo_base_position(world.map().width(), static_cast<engine::PlayerId>(1 - me));
        game.select_army_and_attack_move(
            {static_cast<float>(enemy_base.x.to_int()), static_cast<float>(enemy_base.y.to_int())});
        return std::nullopt;
    }

    const engine::FixedVec2 base = engine::demo_base_position(world.map().width(), me);
    std::vector<const engine::Structure*> houses;
    for (const engine::Structure& s : world.structures()) {
        if (s.type == engine::StructureType::House) houses.push_back(&s);
    }
    std::sort(houses.begin(), houses.end(), [&](const engine::Structure* a, const engine::Structure* b) {
        return (a->center - base).length_sq_raw() < (b->center - base).length_sq_raw();
    });
    if (houses.size() < 2) return std::nullopt;

    engine::Command move_in{.type = engine::CommandType::Garrison, .target_unit = houses[0]->id};
    engine::Command shell{.type = engine::CommandType::AttackGround, .target = houses[1]->center};
    for (const engine::Unit& u : world.units()) {
        if (u.owner != me) continue;
        if (u.type == engine::UnitTypeId::Tank) {
            shell.units.push_back(u.id);
        } else if (!engine::unit_type(u.type).vehicle && !engine::unit_type(u.type).worker) {
            move_in.units.push_back(u.id);
        }
    }
    game.submit(move_in);
    game.submit(shell);
    return render::to_vector2(houses[0]->center);
}

constexpr uint64_t kSmokeSeed = 0xA11C0DE;
// On the default 200-tile map an army marching at its slowest unit's pace
// needs ~4 minutes to reach the enemy base.
constexpr engine::Tick kSmokeTicks = 6000;
constexpr float kSmokeTimeScale = 50.0f;

bool parse_int(const char* text, long min, long max, long& out) {
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < min || value > max) return false;
    out = value;
    return true;
}

bool parse_port(const char* text, uint16_t& port) {
    long value = 0;
    if (!parse_int(text, 1, 65535, value)) return false;
    port = static_cast<uint16_t>(value);
    return true;
}

// Either a preset name ("tiny" ... "giant") or a number of tiles.
bool parse_map_size(const std::string& text, int32_t& size) {
    for (const engine::MapSizePreset& preset : engine::kMapSizes) {
        if (text == preset.name) {
            size = preset.tiles;
            return true;
        }
    }
    long value = 0;
    if (!parse_int(text.c_str(), engine::kMinMapSize, engine::kMaxMapSize, value)) return false;
    size = static_cast<int32_t>(value);
    return true;
}

std::optional<Options> parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_next = i + 1 < argc;
        if (arg == "--host") {
            opt.mode = Options::Mode::Host;
            if (has_next && parse_port(argv[i + 1], opt.port)) ++i;
        } else if (arg == "--join" && has_next) {
            opt.mode = Options::Mode::Join;
            opt.address = argv[++i];
            if (const size_t colon = opt.address.rfind(':'); colon != std::string::npos) {
                if (!parse_port(opt.address.c_str() + colon + 1, opt.port)) return std::nullopt;
                opt.address.resize(colon);
            }
        } else if (arg == "--map" && has_next) {
            if (!parse_map_size(argv[++i], opt.map_size)) return std::nullopt;
        } else if (arg == "--smoke-test" && has_next) {
            opt.smoke_screenshot = argv[++i];
        } else if (arg == "--look" && has_next) {
            const std::string xy = argv[++i];
            const size_t comma = xy.find(',');
            long x = 0;
            long y = 0;
            if (comma == std::string::npos || !parse_int(xy.substr(0, comma).c_str(), 0, engine::kMaxMapSize, x) ||
                !parse_int(xy.substr(comma + 1).c_str(), 0, engine::kMaxMapSize, y)) {
                return std::nullopt;
            }
            opt.look = Vector2{static_cast<float>(x), static_cast<float>(y)};
        } else if (arg == "--reveal") {
            opt.reveal = true;
        } else if (arg == "--scene" && has_next) {
            opt.scene = argv[++i];
        } else if (arg == "--ticks" && has_next) {
            long ticks = 0;
            if (!parse_int(argv[++i], 1, 1000000, ticks)) return std::nullopt;
            opt.smoke_ticks = static_cast<engine::Tick>(ticks);
        } else {
            return std::nullopt;
        }
    }
    return opt;
}

void print_usage() {
    std::printf(
        "Usage: anchor [--host [port]] [--join <address>[:port]] [--map <size>] [--smoke-test <file.png>]\n"
        "  (no arguments)   play offline\n"
        "  --host [port]    host a 1v1 game (default port %u)\n"
        "  --join address   join a hosted game, e.g. --join 192.168.1.5 or --join 1.2.3.4:7777\n"
        "  --map size       tiny 120, small 144, medium 168, normal 200 (default), large 220, giant 240,\n"
        "                   or a number of tiles %d..%d; when joining, the host's size is used\n"
        "  --reveal         no fog of war on screen (for development)\n",
        static_cast<unsigned>(net::kDefaultPort), engine::kMinMapSize, engine::kMaxMapSize);
}

uint64_t random_seed() {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) | rd();
}

}  // namespace

int main(int argc, char** argv) {
    const std::optional<Options> options = parse_args(argc, argv);
    if (!options) {
        print_usage();
        return 1;
    }
    const bool smoke = !options->smoke_screenshot.empty();
    const engine::Tick smoke_ticks = options->smoke_ticks.value_or(kSmokeTicks);
    const uint64_t seed = smoke ? kSmokeSeed : random_seed();

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "Anchor RTS");
    SetExitKey(KEY_F10);  // Esc is reserved for in-game cancel/menu

    // The session must outlive the game, which uses it as its transport.
    std::unique_ptr<net::EnetSession> session;
    std::optional<app::Game> game;
    switch (options->mode) {
        case Options::Mode::Offline:
            game.emplace(seed, options->map_size, engine::PlayerId{0}, 1, nullptr);
            game->set_reveal(options->reveal);
            break;
        case Options::Mode::Host: session = net::EnetSession::host(options->port, seed, options->map_size); break;
        case Options::Mode::Join: session = net::EnetSession::join(options->address, options->port); break;
    }

    bool smoke_ordered = false;
    std::optional<Vector2> smoke_look;
    while (!WindowShouldClose()) {
        if (session) {
            session->update();
            if (!game && session->state() == net::EnetSession::State::Ready) {
                game.emplace(session->seed(), session->map_size(), session->local_player(), session->player_count(),
                             session.get());
                game->set_reveal(options->reveal);
            }
        }

        if (game) {
            if (smoke && !smoke_ordered) {
                game->set_tick_limit(smoke_ticks);
                game->set_time_scale(kSmokeTimeScale);
                smoke_look = start_smoke_scene(*game, *options);
                if (options->look) smoke_look = options->look;
                smoke_ordered = true;
            }
            game->update(GetFrameTime());
            if (smoke && (options->scene == "build" || options->scene == "logistics")) {
                // Show the barracks' or the station's card on the command panel.
                const engine::StructureType shown = options->scene == "build" ? engine::StructureType::InfantryBarracks
                                                                              : engine::StructureType::Station;
                for (const engine::Structure& s : game->world().structures()) {
                    if (s.type == shown && s.owner == game->local_player()) game->select_structure(s.id);
                }
            }
            if (smoke && smoke_look) {
                game->center_camera_on(*smoke_look);
            } else if (smoke) {
                game->center_camera_on_selection();
            }
        }

        BeginDrawing();
        ClearBackground(theme::kBackground);
        if (game) {
            hud::NetStatus net;
            if (session) {
                net.online = true;
                net.ping_ms = session->ping_ms();
                if (session->state() != net::EnetSession::State::Ready) net.message = session->status();
            }
            game->draw(net);
        } else {
            hud::draw_lobby_screen(session ? session->status() : std::string());
        }
        EndDrawing();

        if (smoke && game && game->world().tick() >= smoke_ticks) {
            int alive[2] = {0, 0};
            for (const engine::Unit& u : game->world().units()) ++alive[u.owner % 2];
            std::printf("SMOKE player=%d tick=%u checksum=%016llX alive=%d/%d\n", game->local_player() + 1,
                        game->world().tick(), static_cast<unsigned long long>(game->world().checksum()), alive[0],
                        alive[1]);
            Image shot = LoadImageFromScreen();
            ExportImage(shot, options->smoke_screenshot.c_str());
            UnloadImage(shot);
            break;
        }
        if (smoke && session && session->state() == net::EnetSession::State::Failed) {
            std::printf("SMOKE failed: %s\n", session->status().c_str());
            break;
        }
    }

    game.reset();
    session.reset();
    CloseWindow();
    return 0;
}
