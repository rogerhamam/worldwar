#include "render/text_renderer.h"

#include <cstdlib>

namespace {
bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
}  // namespace

TextRenderer::TextRenderer(SDL_Renderer* renderer, std::string font_path, std::string fallback_font_path,
                           int face_index, bool bold)
    : renderer_(renderer), font_path_(std::move(font_path)),
      fallback_font_path_(std::move(fallback_font_path)), face_index_(face_index), bold_(bold) {
}

TextRenderer::~TextRenderer() {
    for (auto& [size, f] : fonts_) {
        if (f) TTF_CloseFont(f);
    }
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
    // Solid (not Blended): hard 1-bit edges, no anti-aliasing, matching
    // GameMaker's default pixel-font look.
    SDL_Surface* surf = TTF_RenderText_Solid(f, text.c_str(), color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    SDL_Rect dst{x, y, surf->w, surf->h};
    SDL_FreeSurface(surf);
    if (tex) {
        SDL_RenderCopy(renderer_, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
}

void TextRenderer::measure(const std::string& text, int size, int& w, int& h) {
    w = h = 0;
    if (text.empty()) return;
    TTF_Font* f = font(size);
    if (!f) return;
    TTF_SizeText(f, text.c_str(), &w, &h);
}
