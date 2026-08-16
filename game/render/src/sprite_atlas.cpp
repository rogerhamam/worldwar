#include "render/sprite_atlas.h"

#include <SDL_image.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <stdexcept>

SpriteAtlas::SpriteAtlas(SDL_Renderer* renderer, std::string asset_dir)
    : renderer_(renderer), asset_dir_(std::move(asset_dir)) {
    std::ifstream f(asset_dir_ + "/manifest.json");
    if (!f) {
        throw std::runtime_error("failed to open manifest.json in " + asset_dir_);
    }
    nlohmann::json j;
    f >> j;
    for (auto& [name, m] : j.at("sprites").items()) {
        Meta meta;
        meta.ox = m.value("ox", 0);
        meta.oy = m.value("oy", 0);
        meta.fw = m.value("fw", 0);
        meta.fh = m.value("fh", 0);
        meta.frames = m.value("frames", 1);
        manifest_.emplace(name, meta);
    }
}

SpriteAtlas::~SpriteAtlas() {
    for (auto& [name, tex] : sheets_) {
        if (tex) SDL_DestroyTexture(tex);
    }
}

const SpriteAtlas::Meta* SpriteAtlas::meta(const std::string& name) const {
    auto it = manifest_.find(name);
    return it == manifest_.end() ? nullptr : &it->second;
}

void SpriteAtlas::register_sprite(const std::string& name, const Meta& m) {
    manifest_[name] = m;
}

void SpriteAtlas::register_sprite(const std::string& name, const Meta& m, const std::string& file_path) {
    manifest_[name] = m;
    sprite_files_[name] = file_path;
}

SDL_Texture* SpriteAtlas::sheet(const std::string& name) {
    auto it = sheets_.find(name);
    if (it != sheets_.end()) return it->second;

    SDL_Texture* tex = nullptr;
    // A sprite registered with an explicit file path (campaign custom units)
    // loads straight from there; fall back to the asset-dir lookup if that
    // fails (e.g. the campaign's sprite file wasn't deployed alongside it).
    if (auto fit = sprite_files_.find(name); fit != sprite_files_.end()) {
        tex = IMG_LoadTexture(renderer_, fit->second.c_str());
    }
    for (const char* sub : {"sprites", "backgrounds"}) {
        if (tex) break;
        std::string path = asset_dir_ + "/" + sub + "/" + name + ".png";
        tex = IMG_LoadTexture(renderer_, path.c_str());
        if (tex) break;
    }
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    sheets_.emplace(name, tex);
    return tex;
}

bool SpriteAtlas::draw(const std::string& name, int sx, int sy, int frame,
                        double scale, double angle, bool flip, Uint8 alpha, SDL_Color tint) {
    const Meta* m = meta(name);
    if (!m) return false;
    SDL_Texture* tex = sheet(name);
    if (!tex) return false;

    frame = std::max(0, std::min(frame, m->frames - 1));
    SDL_Rect src{frame * m->fw, 0, m->fw, m->fh};

    int w = std::max(1, static_cast<int>(std::round(m->fw * scale)));
    int h = std::max(1, static_cast<int>(std::round(m->fh * scale)));
    double ox = m->ox * scale;
    double oy = m->oy * scale;

    SDL_RendererFlip flip_mode = SDL_FLIP_NONE;
    double angle_deg = 0.0;
    double pivot_x = ox;

    // flip and rotation compose (SDL mirrors the texture, then rotates the dst
    // rect around `center` in screen space) -- they used to be mutually
    // exclusive here, but the ballistic missile needs BOTH: mirrored when
    // firing left AND rotated along its arc. Existing callers never pass both,
    // so their behaviour is unchanged.
    if (flip) {
        flip_mode = SDL_FLIP_HORIZONTAL;
        pivot_x = w - ox; // mirrored content -> pivot measured from the right
    }
    if (angle != 0.0) {
        // Python: deg = -degrees(angle) (CCW-positive radians -> screen
        // CW-positive degrees, since our y axis points down).
        angle_deg = -angle * 180.0 / M_PI;
    }

    SDL_Rect dst{static_cast<int>(sx - pivot_x), static_cast<int>(sy - oy), w, h};
    SDL_Point center{static_cast<int>(pivot_x), static_cast<int>(oy)};

    bool tinted = tint.r != 255 || tint.g != 255 || tint.b != 255;
    if (alpha < 255) SDL_SetTextureAlphaMod(tex, alpha);
    if (tinted) SDL_SetTextureColorMod(tex, tint.r, tint.g, tint.b);
    SDL_RenderCopyEx(renderer_, tex, &src, &dst, angle_deg, &center, flip_mode);
    if (alpha < 255) SDL_SetTextureAlphaMod(tex, 255);
    if (tinted) SDL_SetTextureColorMod(tex, 255, 255, 255);
    return true;
}

bool SpriteAtlas::draw_in_rect(const SDL_Rect& rect, const std::string& name,
                                int frame, int pad) {
    const Meta* m = meta(name);
    if (!m) return false;
    SDL_Texture* tex = sheet(name);
    if (!tex) return false;

    frame = std::max(0, std::min(frame, m->frames - 1));
    SDL_Rect src{frame * m->fw, 0, m->fw, m->fh};

    int aw = rect.w - pad * 2, ah = rect.h - pad * 2;
    double sc = std::min(static_cast<double>(aw) / m->fw, static_cast<double>(ah) / m->fh);
    int w = std::max(1, static_cast<int>(m->fw * sc));
    int h = std::max(1, static_cast<int>(m->fh * sc));

    SDL_Rect dst{rect.x + rect.w / 2 - w / 2, rect.y + rect.h / 2 - h / 2, w, h};
    SDL_RenderCopy(renderer_, tex, &src, &dst);
    return true;
}

bool SpriteAtlas::draw_stretched(const std::string& name, const SDL_Rect& dst,
                                  int frame, bool flip, Uint8 alpha) {
    const Meta* m = meta(name);
    if (!m) return false;
    SDL_Texture* tex = sheet(name);
    if (!tex) return false;

    frame = std::max(0, std::min(frame, m->frames - 1));
    SDL_Rect src{frame * m->fw, 0, m->fw, m->fh};
    if (alpha != 255) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(tex, alpha);
    }
    SDL_RenderCopyEx(renderer_, tex, &src, &dst, 0.0, nullptr,
                      flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    if (alpha != 255) SDL_SetTextureAlphaMod(tex, 255); // restore for other users
    return true;
}
