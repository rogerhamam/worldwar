#pragma once
#include <SDL.h>
#include <SDL_ttf.h>

#include <cstdint>
#include <string>
#include <unordered_map>

// Text rendering with a per-(string,size,colour) TEXTURE cache. Rasterising
// + SDL_CreateTextureFromSurface + SDL_DestroyTexture on EVERY draw EVERY
// frame was the dominant in-game GPU cost (a worldwar_perf.log showed the
// draw phase at ~25ms while the sim ran at ~1ms): most on-screen text
// (scoreboard, resource tallies, HP numbers, labels) repeats frame to frame,
// so caching the texture turns it into a single SDL_RenderCopy. Same pixels,
// far fewer texture uploads. Stale entries are evicted so the cache stays
// bounded even as dynamic numbers churn.
// Process-wide text-cache instrumentation, summed across every TextRenderer
// instance and read once per frame by the watchdog in app.cpp. This exists to
// settle a specific question from a live perf log: the log showed the frame
// cost sitting almost entirely in SDL_RenderPresent (~145ms), spiking on
// roughly every 18th frame, with the sim at ~0ms -- i.e. a periodic GPU sync,
// not game logic. Bulk SDL_DestroyTexture in evict_old is the prime suspect
// (destroying a texture forces a sync, and the batch size divided by the miss
// rate lands right around that period), but "prime suspect" isn't a diagnosis.
// These counters put the answer directly in the next log the player sends.
struct TextCacheStats {
    uint64_t created = 0;   // cache misses -> SDL_CreateTextureFromSurface
    uint64_t evicted = 0;   // textures destroyed by evict_old
    uint64_t evict_calls = 0;
    double evict_ms = 0.0;  // wall time inside evict_old
    size_t live = 0;        // entries currently cached, all instances
};
// Totals since the last ww_text_cache_reset_frame() (live is instantaneous).
TextCacheStats ww_text_cache_stats();
void ww_text_cache_reset_frame();

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

    // ---- texture cache ----
    struct Key {
        std::string text;
        int size;
        uint32_t color; // r<<24 | g<<16 | b<<8 | a
        bool operator==(const Key& o) const {
            return size == o.size && color == o.color && text == o.text;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            size_t h = std::hash<std::string>{}(k.text);
            h = h * 1099511628211u ^ static_cast<size_t>(k.size);
            h = h * 1099511628211u ^ static_cast<size_t>(k.color);
            return h;
        }
    };
    struct Cached {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
        uint64_t used = 0; // tick_ of the last draw that hit this entry
    };
    void evict_old();

    SDL_Renderer* renderer_;
    std::string font_path_;
    std::string fallback_font_path_;
    int face_index_;
    bool bold_;
    std::unordered_map<int, TTF_Font*> fonts_;
    std::unordered_map<Key, Cached, KeyHash> cache_;
    uint64_t tick_ = 0;
};
