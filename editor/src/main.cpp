// Standalone campaign editor: a separate program from worldwar_client so
// campaign authoring can happen independently of (and be released
// alongside) the main game -- see campaign_data.h for the on-disk format
// this reads/writes, and editor.h for the actual screens/flow.
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

#include "editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {
// Same "prefer a folder next to the exe, fall back to the compile-time
// dev path" resolution worldwar_client's app.cpp uses, so a released copy
// of this tool finds assets|data when placed next to it
// in the same dist folder, but still works from the build tree too.
std::string resolve_dir(const char* subdir, const char* dev_fallback) {
    if (char* base = SDL_GetBasePath()) {
        std::filesystem::path candidate = std::filesystem::path(base) / subdir;
        SDL_free(base);
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate.string();
    }
    return dev_fallback;
}
} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) {
        SDL_Log("IMG_Init failed: %s", IMG_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        SDL_Log("TTF_Init failed: %s", TTF_GetError());
        return 1;
    }

    const std::string asset_dir = resolve_dir("assets", WW_ASSET_DIR);
    const std::string data_dir = resolve_dir("data", WW_DATA_DIR);

    // WIDTH/HEIGHT is the fixed VIRTUAL canvas everything is laid out against
    // (kPanel etc. in editor.cpp); it's letterbox-scaled into whatever the
    // actual window size is (see recompute_letterbox below). The window itself
    // must fit the DISPLAY, or a 1024-tall window on a 1080p screen (minus the
    // taskbar and title bar) has its top and bottom clipped off. Shrink the
    // initial window to the usable desktop area, keeping the 1600x1024 aspect.
    const int WIDTH = 1600, HEIGHT = 1024;
    int init_w = WIDTH, init_h = HEIGHT;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
        // Leave headroom for the window's own title bar + borders (usable
        // bounds already exclude the taskbar, but not the window chrome).
        const int chrome = 64;
        double fit = std::min({1.0, static_cast<double>(usable.w) / WIDTH,
                               static_cast<double>(usable.h - chrome) / HEIGHT});
        init_w = static_cast<int>(std::lround(WIDTH * fit));
        init_h = static_cast<int>(std::lround(HEIGHT * fit));
    }
    SDL_Window* window = SDL_CreateWindow(
        "World War Campaign Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, init_w, init_h,
        SDL_WINDOW_RESIZABLE);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Texture* render_target =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, WIDTH, HEIGHT);
    if (!render_target) {
        SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
        return 1;
    }

    double letterbox_scale = 1.0;
    SDL_Rect letterbox_dst{0, 0, WIDTH, HEIGHT};
    auto recompute_letterbox = [&]() {
        int win_w, win_h;
        SDL_GetWindowSize(window, &win_w, &win_h);
        if (win_w <= 0 || win_h <= 0) return;
        letterbox_scale = std::min(static_cast<double>(win_w) / WIDTH, static_cast<double>(win_h) / HEIGHT);
        int dst_w = static_cast<int>(std::lround(WIDTH * letterbox_scale));
        int dst_h = static_cast<int>(std::lround(HEIGHT * letterbox_scale));
        letterbox_dst = SDL_Rect{(win_w - dst_w) / 2, (win_h - dst_h) / 2, dst_w, dst_h};
    };
    recompute_letterbox();
    auto to_virtual = [&](int& x, int& y) {
        x = static_cast<int>(std::lround((x - letterbox_dst.x) / letterbox_scale));
        y = static_cast<int>(std::lround((y - letterbox_dst.y) / letterbox_scale));
    };

    // Replaced by the custom spr_mouse (or, while erasing, spr_erase)
    // cursor Editor::draw() draws itself every frame -- same convention as
    // the main game's own GameClient::post_construct.
    SDL_ShowCursor(SDL_DISABLE);

    {
        Editor editor(renderer, asset_dir, data_dir, WIDTH, HEIGHT);
        bool running = true;

        // Verification-only synthetic-input hooks, same convention as
        // worldwar_client's app.cpp (WW_TEST_CLICK*/WW_SHOT): lets an
        // automated/headless run drive a click sequence and capture a
        // screenshot, without a human at the mouse.
        auto synth_click = [&](int x, int y) {
            SDL_Event down{}, up{};
            down.type = SDL_MOUSEBUTTONDOWN;
            down.button.button = SDL_BUTTON_LEFT;
            down.button.x = x;
            down.button.y = y;
            up.type = SDL_MOUSEBUTTONUP;
            up.button.button = SDL_BUTTON_LEFT;
            up.button.x = x;
            up.button.y = y;
            editor.handle_event(down);
            editor.handle_event(up);
        };
        // Moves the (synthetic) mouse without clicking -- for verifying
        // hover-only UI (EditMap's placement ghost preview/hover
        // highlight), which a plain click can't exercise since it never
        // fires a real SDL_MOUSEMOTION.
        int move_x = -1, move_y = -1;
        if (const char* c = SDL_getenv("WW_TEST_MOVE")) sscanf(c, "%d,%d", &move_x, &move_y);
        int click_x[7], click_y[7];
        for (int i = 0; i < 7; ++i) click_x[i] = click_y[i] = -1;
        const char* click_env_names[7] = {"WW_TEST_CLICK",  "WW_TEST_CLICK2", "WW_TEST_CLICK3",
                                          "WW_TEST_CLICK4", "WW_TEST_CLICK5", "WW_TEST_CLICK6",
                                          "WW_TEST_CLICK7"};
        for (int i = 0; i < 7; ++i) {
            if (const char* c = SDL_getenv(click_env_names[i])) sscanf(c, "%d,%d", &click_x[i], &click_y[i]);
        }
        // Copied into std::strings immediately -- SDL_getenv's returned
        // pointer isn't guaranteed to stay valid once other SDL calls
        // touch the process environment (same fix worldwar_client's
        // app.cpp already needed for its own WW_SHOT).
        const char* text_env_raw = SDL_getenv("WW_TEST_TEXT"); // injected as one SDL_TEXTINPUT at frame 20
        std::string text_env_str = text_env_raw ? text_env_raw : "";
        const char* text_env = text_env_str.empty() ? nullptr : text_env_str.c_str();
        const char* shot_path_raw = SDL_getenv("WW_SHOT");
        std::string shot_path_str = shot_path_raw ? shot_path_raw : "";
        const char* shot_path = shot_path_str.empty() ? nullptr : shot_path_str.c_str();
        int shot_after_frames = 30;
        if (const char* n = SDL_getenv("WW_SHOT_FRAMES")) shot_after_frames = std::atoi(n);
        // Signed notch count for EditMap's zoom (see Editor::handle_event's
        // SDL_MOUSEWHEEL case) -- positive zooms in, negative zooms out;
        // fired as that many individual wheel events (not one event with a
        // large wheel.y) since the handler treats any nonzero wheel.y as
        // exactly one 1.1x notch, matching real scroll-wheel input.
        int wheel_notches = 0;
        if (const char* n = SDL_getenv("WW_TEST_WHEEL")) wheel_notches = std::atoi(n);
        // "x0,y0,x1,y1" -- a real press-hold-release drag (button down at
        // (x0,y0), a motion to (x1,y1) while still held, then button up
        // there), for verifying press-and-hold interactions (Terrain's
        // paint brush, Objectives' click-drag-release kill-area rectangle)
        // that WW_TEST_CLICK* can't exercise since synth_click always
        // pairs its down/up at the same point in the same instant.
        int drag_x0 = -1, drag_y0 = -1, drag_x1 = -1, drag_y1 = -1;
        if (const char* c = SDL_getenv("WW_TEST_DRAG")) {
            sscanf(c, "%d,%d,%d,%d", &drag_x0, &drag_y0, &drag_x1, &drag_y1);
        }
        int frame_count = 0;

        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) {
                    running = false;
                } else if (ev.type == SDL_WINDOWEVENT && (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                                                          ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
                    recompute_letterbox();
                } else {
                    SDL_Event tev = ev;
                    if (tev.type == SDL_MOUSEBUTTONDOWN || tev.type == SDL_MOUSEBUTTONUP) {
                        to_virtual(tev.button.x, tev.button.y);
                    } else if (tev.type == SDL_MOUSEMOTION) {
                        to_virtual(tev.motion.x, tev.motion.y);
                    }
                    editor.handle_event(tev);
                }
            }
            if (editor.wants_quit()) running = false;

            for (int i = 0; i < 7; ++i) {
                if (click_x[i] >= 0 && frame_count == (i + 1) * 5) synth_click(click_x[i], click_y[i]);
            }
            if (move_x >= 0 && frame_count == 45) {
                SDL_Event mev{};
                mev.type = SDL_MOUSEMOTION;
                mev.motion.x = move_x;
                mev.motion.y = move_y;
                editor.handle_event(mev);
            }
            if (text_env && frame_count == 20) {
                SDL_Event tev{};
                tev.type = SDL_TEXTINPUT;
                std::snprintf(tev.text.text, sizeof(tev.text.text), "%s", text_env);
                editor.handle_event(tev);
            }
            if (wheel_notches != 0 && frame_count == 40) {
                SDL_Event wev{};
                wev.type = SDL_MOUSEWHEEL;
                wev.wheel.y = (wheel_notches > 0) ? 1 : -1;
                for (int i = 0; i < std::abs(wheel_notches); ++i) editor.handle_event(wev);
            }
            if (drag_x0 >= 0 && frame_count == 42) {
                SDL_Event down{};
                down.type = SDL_MOUSEBUTTONDOWN;
                down.button.button = SDL_BUTTON_LEFT;
                down.button.x = drag_x0;
                down.button.y = drag_y0;
                editor.handle_event(down);
            }
            if (drag_x0 >= 0 && frame_count == 43) {
                SDL_Event mev{};
                mev.type = SDL_MOUSEMOTION;
                mev.motion.x = drag_x1;
                mev.motion.y = drag_y1;
                editor.handle_event(mev);
            }
            if (drag_x0 >= 0 && frame_count == 44) {
                SDL_Event up{};
                up.type = SDL_MOUSEBUTTONUP;
                up.button.button = SDL_BUTTON_LEFT;
                up.button.x = drag_x1;
                up.button.y = drag_y1;
                editor.handle_event(up);
            }

            SDL_SetRenderTarget(renderer, render_target);
            editor.draw(renderer);
            SDL_SetRenderTarget(renderer, nullptr);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, render_target, nullptr, &letterbox_dst);
            SDL_RenderPresent(renderer);

            ++frame_count;
            if (shot_path && frame_count >= shot_after_frames) {
                int w, h;
                SDL_GetRendererOutputSize(renderer, &w, &h);
                SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
                SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                SDL_SaveBMP(surf, shot_path);
                SDL_FreeSurface(surf);
                running = false;
            }
        }
    }

    SDL_DestroyTexture(render_target);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 0;
}
