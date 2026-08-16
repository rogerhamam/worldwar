#include "render/text_renderer.h"

#include <algorithm>
#include <cstdlib>

namespace {
bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Per-frame instrumentation totals (see TextCacheStats). `g_live` is a running
// count rather than a per-frame total, so it is adjusted on both insert and
// erase instead of being reset.
TextCacheStats g_stats;
size_t g_live = 0;
}  // namespace

TextCacheStats ww_text_cache_stats() {
    TextCacheStats s = g_stats;
    s.live = g_live;
    return s;
}

void ww_text_cache_reset_frame() {
    g_stats = TextCacheStats{};
}

TextRenderer::TextRenderer(SDL_Renderer* renderer, std::string font_path, std::string fallback_font_path,
                           int face_index, bool bold)
    : renderer_(renderer), font_path_(std::move(font_path)),
      fallback_font_path_(std::move(fallback_font_path)), face_index_(face_index), bold_(bold) {
}

TextRenderer::~TextRenderer() {
    for (auto& [key, c] : cache_) {
        if (c.tex) SDL_DestroyTexture(c.tex);
    }
    g_live -= std::min(g_live, cache_.size());
    for (auto& [size, f] : fonts_) {
        if (f) TTF_CloseFont(f);
    }
}

void TextRenderer::evict_old() {
    // Drop stale entries (not touched in ~a few hundred draws) so churning
    // numbers don't grow the cache forever. Bound the work per call: destroying
    // a texture forces a GPU sync (~tens of us each), so sweeping thousands at
    // once stalled a whole frame ~230ms (seen in a perf log).
    //
    // kMaxEvictPerCall was 128, and that was still far too bursty. The batch is
    // only shed once the cache is back OVER the cap, so the work arrives in
    // lumps: 128 destroys land on one frame, then nothing until ~128 more misses
    // have accumulated. At a handful of fresh strings per frame that is a spike
    // every ~18 frames -- exactly the period in the player's perf log, where the
    // frame cost was ~145ms and sat entirely in SDL_RenderPresent (a GPU sync is
    // precisely what would show up there rather than in the draw phase). A GPU
    // sync that costs "tens of us" in the best case costs far more when the
    // driver is already busy, so the per-destroy estimate the 128 was sized
    // against is the optimistic one.
    //
    // 12 per call keeps the same steady-state throughput -- eviction is driven
    // by the miss rate either way (each over-cap call sheds 12, and it takes 12
    // fresh misses to go back over the cap), so nothing accumulates; it just
    // spreads the same destroys across ~10x more frames. The scan itself is
    // deliberately left unbounded: walking a 4096-entry hash map is a few
    // microseconds, i.e. nothing next to even one texture destroy, and capping
    // it would risk never reaching stale entries in the map's tail.
    constexpr uint64_t kEvictAge = 512;
    constexpr int kMaxEvictPerCall = 12;
    Uint64 t0 = SDL_GetPerformanceCounter();
    int removed = 0;
    for (auto it = cache_.begin(); it != cache_.end() && removed < kMaxEvictPerCall;) {
        if (tick_ - it->second.used > kEvictAge) {
            if (it->second.tex) SDL_DestroyTexture(it->second.tex);
            it = cache_.erase(it);
            ++removed;
            --g_live;
        } else {
            ++it;
        }
    }
    g_stats.evicted += static_cast<uint64_t>(removed);
    ++g_stats.evict_calls;
    g_stats.evict_ms +=
        (SDL_GetPerformanceCounter() - t0) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
}

TTF_Font* TextRenderer::font(int size) {
    auto it = fonts_.find(size);
    if (it != fonts_.end()) return it->second;
    TTF_Font* f = nullptr;
    if (ends_with(font_path_, ".fon")) {
        // Legacy .FON bitmap fonts bundle a handful of discrete point sizes
        // as SEPARATE FACES rather than being freely scalable -- opening
        // via TTF_OpenFont (always face index 0) silently returns the
        // SMALLEST embedded strike (8pt/11px) no matter what pixel size is
        // requested, which is why bumping the requested size alone never
        // made the text look any bigger. serife.fon's 6 faces, probed via
        // freetype-py: {face 0: 11px, 1: 13px, 2: 16px, 3: 19px, 4: 24px,
        // 5: 32px} -- pick whichever face's bitmap height is closest to the
        // size actually wanted.
        static constexpr struct { long face_index; int px; } kStrikes[] = {
            {0, 11}, {1, 13}, {2, 16}, {3, 19}, {4, 24}, {5, 32},
        };
        long best_index = kStrikes[0].face_index;
        int best_px = kStrikes[0].px;
        int best_diff = std::abs(size - best_px);
        for (auto& s : kStrikes) {
            int diff = std::abs(size - s.px);
            if (diff < best_diff) { best_diff = diff; best_index = s.face_index; best_px = s.px; }
        }
        f = TTF_OpenFontIndex(font_path_.c_str(), best_px, best_index);
    } else {
        f = TTF_OpenFontIndex(font_path_.c_str(), size, face_index_);
    }
    if (!f) {
        SDL_Log("TTF_OpenFont failed for %s @ %d: %s", font_path_.c_str(), size, TTF_GetError());
        if (!fallback_font_path_.empty()) {
            f = TTF_OpenFont(fallback_font_path_.c_str(), size);
            if (!f) {
                SDL_Log("TTF_OpenFont fallback failed for %s @ %d: %s", fallback_font_path_.c_str(),
                        size, TTF_GetError());
            }
        }
    }
    if (f && bold_) TTF_SetFontStyle(f, TTF_STYLE_BOLD);
    fonts_[size] = f;
    return f;
}

void TextRenderer::draw(const std::string& text, int x, int y, SDL_Color color, int size) {
    if (text.empty()) return;
    TTF_Font* f = font(size);
    if (!f) return;
    ++tick_;
    uint32_t col = (static_cast<uint32_t>(color.r) << 24) | (static_cast<uint32_t>(color.g) << 16) |
                   (static_cast<uint32_t>(color.b) << 8) | static_cast<uint32_t>(color.a);
    // Cache hit: just blit the already-uploaded texture (same pixels).
    auto it = cache_.find(Key{text, size, col});
    if (it != cache_.end()) {
        it->second.used = tick_;
        if (it->second.tex) {
            SDL_Rect dst{x, y, it->second.w, it->second.h};
            SDL_RenderCopy(renderer_, it->second.tex, nullptr, &dst);
        }
        return;
    }
    // Miss: rasterise + upload once, draw, and cache the texture.
    // Solid (not Blended): hard 1-bit edges, no anti-aliasing, matching
    // GameMaker's default pixel-font look.
    SDL_Surface* surf = TTF_RenderText_Solid(f, text.c_str(), color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    int w = surf->w, h = surf->h;
    SDL_FreeSurface(surf);
    if (tex) {
        SDL_Rect dst{x, y, w, h};
        SDL_RenderCopy(renderer_, tex, nullptr, &dst);
    }
    cache_.emplace(Key{text, size, col}, Cached{tex, w, h, tick_});
    ++g_live;
    ++g_stats.created;
    constexpr size_t kMaxCache = 4096;
    if (cache_.size() > kMaxCache) evict_old();
}

void TextRenderer::measure(const std::string& text, int size, int& w, int& h) {
    w = h = 0;
    if (text.empty()) return;
    TTF_Font* f = font(size);
    if (!f) return;
    TTF_SizeText(f, text.c_str(), &w, &h);
}
