find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_mixer CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(worldwar_client
    src/app.cpp
    src/game_client.cpp
    src/match_stats.cpp
    src/settings.cpp
    src/item_hotkeys.cpp
    src/menu/menu_controller.cpp
    src/audio/audio.cpp
)
target_include_directories(worldwar_client PRIVATE src)
target_link_libraries(worldwar_client PRIVATE
    ww_sim
    ww_net
    ww_render
    ww_campaign
    SDL2::SDL2main
    SDL2::SDL2
    SDL2_mixer::SDL2_mixer
    nlohmann_json::nlohmann_json
)
