find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_image CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# SDL2/SDL2_image/SDL2_ttf rendering utilities (sprite atlas, bitmap-font
# text, camera) shared between worldwar_client and campaign_editor -- split
# out of client/ (which used to compile these directly) so a second SDL2
# program doesn't need to duplicate them.
add_library(ww_render STATIC
    src/sprite_atlas.cpp
    src/text_renderer.cpp
    src/camera.cpp
)
target_include_directories(ww_render PUBLIC include)
target_link_libraries(ww_render PUBLIC
    SDL2::SDL2
    SDL2_image::SDL2_image
    SDL2_ttf::SDL2_ttf
    nlohmann_json::nlohmann_json
)
