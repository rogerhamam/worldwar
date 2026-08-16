find_package(nlohmann_json CONFIG REQUIRED)

add_library(ww_sim STATIC
    src/catalog.cpp
    src/rng.cpp
    src/spatial_grid.cpp
    src/pathfind.cpp
    src/bonuses.cpp
    src/control.cpp
    src/control_ai.cpp
    src/world.cpp
    src/unit_behavior.cpp
    src/aircraft_behavior.cpp
    src/building_behavior.cpp
    src/deer_behavior.cpp
    src/projectile_behavior.cpp
    src/scenario.cpp
    src/match.cpp
    src/command.cpp
    src/command_codec.cpp
    src/stress_scenario.cpp
)
target_include_directories(ww_sim PUBLIC include)
target_link_libraries(ww_sim PUBLIC nlohmann_json::nlohmann_json ww_campaign)
target_compile_definitions(ww_sim PUBLIC
    WW_DATA_DIR="${WW_DATA_DIR}"
    WW_ASSET_DIR="${WW_ASSET_DIR}"
)
