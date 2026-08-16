cmake_minimum_required(VERSION 3.20)
project(WorldWarCpp CXX)

# Release version -- single source of truth is the VERSION file at the repo
# root. publish.bat reads the SAME file to name the release zip, so the
# in-game "Version X" label always matches the downloadable build (see the
# menu's draw_title). Reconfigure/rebuild after bumping VERSION.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" WW_VERSION LIMIT_COUNT 1)
string(STRIP "${WW_VERSION}" WW_VERSION)
add_compile_definitions(WW_VERSION="${WW_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Compile-time FALLBACK data/asset dirs, used only when the exe can't find an
# "assets"/"data" folder next to itself (see client/src/app.cpp resolve_dir) --
# i.e. dev builds run straight out of build/, and the headless runner. Point
# them at the repo root's shared assets/data (see resolve_dir's comment for
# why the exe prefers a copy sitting next to itself first -- that's how the
# release zip built by publish.bat is laid out). Override with
# -DWW_DATA_DIR=... / -DWW_ASSET_DIR=... if needed.
set(WW_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. CACHE PATH "worldwar repo root")
set(WW_DATA_DIR "${WW_REPO_ROOT}/data" CACHE PATH "fallback data dir")
set(WW_ASSET_DIR "${WW_REPO_ROOT}/assets" CACHE PATH "fallback asset dir")

add_subdirectory(sim)
add_subdirectory(net)
add_subdirectory(render)
# ww_campaign stays here (worldwar_client's in-game campaign-browsing
# screen reads campaign JSON via it) -- only the standalone AUTHORING tool
# moved out, to ../editor, a fully separate project with its own vendored
# copy of this same data-model library, so it can be developed
# independently without touching this engine's code at all.
add_subdirectory(campaign)
add_subdirectory(client)
add_subdirectory(tools/headless_runner)

option(WW_BUILD_TESTS "Build sim unit tests" OFF)
if(WW_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/sim_tests)
endif()
