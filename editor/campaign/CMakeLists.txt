find_package(nlohmann_json CONFIG REQUIRED)

# Campaign data model + JSON load/save. Kept as its own library (same
# split as the main game engine repo, which has its own copy of
# this exact library shared between its client and this editor's source)
# purely so the JSON schema stays documented in one clearly-scoped place.
# If you change the schema here, the main engine repo's copy needs the
# matching change or it won't be able to read campaigns saved by this tool.
add_library(ww_campaign STATIC
    src/campaign_data.cpp
)
target_include_directories(ww_campaign PUBLIC include)
target_link_libraries(ww_campaign PUBLIC
    nlohmann_json::nlohmann_json
)
