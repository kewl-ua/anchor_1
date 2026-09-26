cmake_minimum_required(VERSION 3.24)
project(anchor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Dependencies ------------------------------------------------------------

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        6.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(raylib)

# ENet's own CMakeLists predates CMake 3.5 (rejected by CMake 4), so only
# download it (SOURCE_SUBDIR points nowhere) and build it ourselves.
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-add
)
FetchContent_MakeAvailable(enet)

add_library(enet STATIC
    ${enet_SOURCE_DIR}/callbacks.c
    ${enet_SOURCE_DIR}/compress.c
    ${enet_SOURCE_DIR}/host.c
    ${enet_SOURCE_DIR}/list.c
    ${enet_SOURCE_DIR}/packet.c
    ${enet_SOURCE_DIR}/peer.c
    ${enet_SOURCE_DIR}/protocol.c
    ${enet_SOURCE_DIR}/unix.c
    ${enet_SOURCE_DIR}/win32.c
)
target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
    target_compile_definitions(enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
    target_compile_definitions(enet PRIVATE
        HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
        HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
endif()

# --- Shared compile settings for our own targets -----------------------------

function(anchor_target_defaults target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# --- Layers ------------------------------------------------------------------
#
#   engine    deterministic simulation, no graphics, no floating-point
#   net       lockstep + wire protocol over an abstract transport (no sockets)
#   net_enet  the real transport, ENet
#   render    draws the world (raylib), reads engine state read-only
#   hud       screen-space UI (raylib), reads engine state read-only
#   app       window, main loop, input -> commands; glues everything together
#
# engine deliberately does NOT link raylib: any attempt to include it there
# fails to compile, which keeps the simulation portable and headless.
# ENet stays private to net_enet: its windows.h clashes with raylib.

add_library(engine STATIC
    src/engine/world.cpp
    src/engine/world_economy.cpp
    src/engine/pathfinding.cpp
    src/engine/simulation.cpp
    src/engine/scenario.cpp
    src/engine/unit_types.cpp
)
target_include_directories(engine PUBLIC src)
anchor_target_defaults(engine)

add_library(net STATIC
    src/net/protocol.cpp
    src/net/lockstep.cpp
)
target_link_libraries(net PUBLIC engine)
anchor_target_defaults(net)

add_library(net_enet STATIC
    src/net/enet_session.cpp
)
target_link_libraries(net_enet PUBLIC net PRIVATE enet)
anchor_target_defaults(net_enet)

# Shared colors/style, so render and hud don't depend on each other.
add_library(theme INTERFACE)
target_link_libraries(theme INTERFACE engine raylib)

add_library(render STATIC
    src/render/camera.cpp
    src/render/iso.cpp
    src/render/world_renderer.cpp
)
target_link_libraries(render PUBLIC engine raylib PRIVATE theme)
anchor_target_defaults(render)

add_library(hud STATIC
    src/hud/hud.cpp
)
target_link_libraries(hud PUBLIC engine raylib PRIVATE theme)
anchor_target_defaults(hud)

add_executable(anchor
    src/app/main.cpp
    src/app/game.cpp
    src/app/player_controller.cpp
)
target_link_libraries(anchor PRIVATE engine net net_enet render hud theme raylib)
anchor_target_defaults(anchor)

# --- Tests -------------------------------------------------------------------

enable_testing()

add_executable(engine_tests tests/engine_tests.cpp)
target_link_libraries(engine_tests PRIVATE engine)
anchor_target_defaults(engine_tests)
add_test(NAME engine_tests COMMAND engine_tests)

add_executable(net_tests tests/net_tests.cpp)
target_link_libraries(net_tests PRIVATE net)
anchor_target_defaults(net_tests)
add_test(NAME net_tests COMMAND net_tests)

# Guards determinism: the engine must not use floating-point, the renderer or
# nondeterministic standard facilities.
add_test(NAME engine_purity
    COMMAND ${CMAKE_COMMAND}
        -DENGINE_DIR=${CMAKE_SOURCE_DIR}/src/engine
        -P ${CMAKE_SOURCE_DIR}/cmake/check_engine_purity.cmake
)
