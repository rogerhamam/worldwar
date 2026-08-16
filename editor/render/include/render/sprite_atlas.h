#pragma once
#include <SDL.h>
#include <string>
#include <unordered_map>

// Loads GMK-extracted spritesheets (assets/sprites|backgrounds/<name>.png)
// described by assets/manifest.json. Each sheet is a horizontal strip of
// `frames` cells of size fw x fh; for units/buildings the 8 frames are the
// 8 player colours (frame = player id). (ox, oy) is the sprite's pivot
// point within a cell. Direct port of game/assets.py, but simpler: SDL2
// renders rotation/flip/scale on the GPU, so unlike pygame there's no need
// to pre-bake rotated/scaled surface caches.
class SpriteAtlas {
public:
    struct Meta {
        int ox = 0, oy = 0, fw = 0, fh = 0, frames = 1;
    };

    SpriteAtlas(SDL_Renderer* renderer, std::string asset_dir);
    ~SpriteAtlas();
    SpriteAtlas(const SpriteAtlas&) = delete;
    SpriteAtlas& operator=(const SpriteAtlas&) = delete;

    const Meta* meta(const std::string& name) const;

    // Draws sprite `name` so its pivot point lands at screen (sx, sy).
    // `angle` is radians, matching game/entity.py's convention (used for
    // projectiles); `flip` mirrors horizontally (units facing left).
    // `tint` is an RGB color multiply (SDL_SetTextureColorMod) -- default
    // white leaves the sprite unmodified; e.g. a red tint on the building
    // placement ghost signals an invalid spot.
    bool draw(const std::string& name, int sx, int sy, int frame = 0,
              double scale = 1.0, double angle = 0.0, bool flip = false,
              Uint8 alpha = 255, SDL_Color tint = {255, 255, 255, 255});

    // Draws centred by bounding box inside `rect`, scaled to fit (ignores
    // the pivot) -- the correct anchor for UI icons.
    bool draw_in_rect(const SDL_Rect& rect, const std::string& name,
                       int frame = 0, int pad = 4);

    // Stretches frame `frame` to exactly fill `dst` (top-left anchored, no
    // pivot). Used for ground tiles, which pygame draws via a plain
    // smoothscale blit rather than the pivoted sprite.draw() path.
    bool draw_stretched(const std::string& name, const SDL_Rect& dst,
                         int frame = 0, bool flip = false);

private:
    SDL_Texture* sheet(const std::string& name);

    SDL_Renderer* renderer_;
    std::string asset_dir_;
    std::unordered_map<std::string, Meta> manifest_;
    std::unordered_map<std::string, SDL_Texture*> sheets_;
};
