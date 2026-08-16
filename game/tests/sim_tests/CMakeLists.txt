find_package(Catch2 CONFIG REQUIRED)

add_executable(ww_sim_tests
    test_rng.cpp
    test_spatial_grid.cpp
    test_pathfind.cpp
    test_entities.cpp
    test_bonuses.cpp
    test_control.cpp
    test_world_integration.cpp
    test_scenario.cpp
    test_command.cpp
    test_aircraft.cpp
    test_stress_scenario.cpp
)
target_link_libraries(ww_sim_tests PRIVATE ww_sim Catch2::Catch2WithMain)

include(CTest)
add_test(NAME ww_sim_tests COMMAND ww_sim_tests)
