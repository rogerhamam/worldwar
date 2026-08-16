cmake_minimum_required(VERSION 3.20)
project(WorldWarCampaignEditor CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# This project is fully standalone -- no dependency on the main game
# engine repo. assets/ and data/ here are trimmed copies (sprites/icons/
# civ data only, no audio, no sim code) of what the editor's palette needs;
# see README.md for what to do with finished campaign JSON files.
set(WW_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
set(WW_ASSET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets)

add_subdirectory(render)
add_subdirectory(campaign)

find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_image CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(campaign_editor
    src/main.cpp
    src/editor.cpp
    src/game_data.cpp
    src/catalog.cpp
)
target_include_directories(campaign_editor PRIVATE src)
target_link_libraries(campaign_editor PRIVATE
    ww_render
    ww_campaign
    SDL2::SDL2main
    SDL2::SDL2
    nlohmann_json::nlohmann_json
)
target_compile_definitions(campaign_editor PRIVATE
    WW_DATA_DIR="${WW_DATA_DIR}"
    WW_ASSET_DIR="${WW_ASSET_DIR}"
)
