#pragma once
#include <SDL.h>
#include <SDL_ttf.h>

#include <string>
#include <unordered_map>

// Minimal text rendering: renders on demand (no glyph atlas/cache) since
// the client only draws a handful of short, frequently-changing strings
// per frame (resource counts, HUD labels, FPS) -- not worth the
// complexity of a cached glyph atlas at this scale.
class TextRenderer {
public:
    // Tries `font_path` first (e.g. the classic Windows "MS Serif" bitmap
    // font, C:\Windows\Fonts\serife.fon -- FreeType has a `winfnt` driver
    // that can actually read these legacy .FON files), falling back to
    // `fallback_font_path` if that font fails to open at all. `face_index`
    // selects a face within a multi-face file (e.g. a .ttc collection --
    // msgothic.ttc bundles MS Gothic/MS UI Gothic/MS PGothic as faces
    // 0/1/2); ignored for .fon files, which pick their face by nearest
    // embedded bitmap strike size instead (see font()). `bold` requests
    // synthesized emboldening (TTF_STYLE_BOLD) -- there's no separate bold
    // .fon for MS Serif on Windows (serife.fon/seriff.fon are just two DPI
    // resolutions of the same weight), so this is FreeType's algorithmic
    // embolden, which does work on bitmap glyphs too, not just outlines.
    TextRenderer(SDL_Renderer* renderer, std::string font_path, std::string fallback_font_path = "",
                int face_index = 0, bool bold = false);
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // Draws with (x, y) as the top-left corner. Solid (non-antialiased,
    // hard 1-bit edges) to match GameMaker's default pixel-font look.
    void draw(const std::string& text, int x, int y, SDL_Color color, int size = 16);
    // Returns the rendered pixel width/height without drawing (for centering).
    void measure(const std::string& text, int size, int& w, int& h);

private:
    TTF_Font* font(int size);

    SDL_Renderer* renderer_;
    std::string font_path_;
    std::string fallback_font_path_;
    int face_index_;
    bool bold_;
    std::unordered_map<int, TTF_Font*> fonts_;
};
