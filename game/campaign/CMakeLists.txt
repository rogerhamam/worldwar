find_package(nlohmann_json CONFIG REQUIRED)

# Campaign data model + JSON load/save, shared by campaign_editor (authoring)
# and worldwar_client (in-game campaign-select browsing) -- split out so
# neither program needs to duplicate it, same idea as ww_render.
add_library(ww_campaign STATIC
    src/campaign_data.cpp
)
target_include_directories(ww_campaign PUBLIC include)
target_link_libraries(ww_campaign PUBLIC
    nlohmann_json::nlohmann_json
)
