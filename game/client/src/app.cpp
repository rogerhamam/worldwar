// Phase C: a real, playable single-player-vs-AI skirmish. GameClient owns
// the Match (sim), camera, sprite atlas, selection, and input handling;
// this file is just the SDL window/event pump, mirroring main.py's role.
//
// Scope note (see GameClient/task-list for the full rundown): no
// menu/setup/campaign screens yet (a skirmish auto-starts), no control
// groups/formation-drag/minimap/tech-tree overlay, no civ voice lines or
// music playback, no interpolation between fixed sim ticks. All
// documented deliberate cuts, not oversights.
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

#include "game_client.h"
#include "net/session.h"
#include "net/socket.h"
#include "net/upnp.h"
#include "menu/menu_controller.h"
#include "render/text_renderer.h"
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// WW_ASSET_DIR/WW_DATA_DIR (set by CMakeLists.txt) are absolute paths into
// THIS machine's repo-root assets/data -- fine for local dev builds,
// but baked into the exe at compile time, so a copy of the exe handed to
// someone else (via the launcher/publish.bat pipeline) could never find
// its assets on their machine. Prefer an "assets"/"data" folder sitting
// right next to the exe itself (which is how publish.bat's dist folder
// and every zip built from it are laid out) and only fall back to the
// compile-time dev path if that's not there.
std::string resolve_dir(const char* subdir, const char* dev_fallback) {
    if (char* base = SDL_GetBasePath()) {
        std::filesystem::path candidate = std::filesystem::path(base) / subdir;
        SDL_free(base);
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate.string();
    }
    return dev_fallback;
}

// ---- frame-time watchdog + crash log ---------------------------------------
// Writes worldwar_perf.log next to the exe. Every frame slower than 30 FPS
// (>33ms) is recorded with a per-phase breakdown (update/draw/present) and the
// live entity/effect counts, and an unhandled-exception (crash) handler dumps
// the LAST frame's stats -- so a mid-game stall, and the exact moment of a
// crash, land in a file the player can hand back verbatim. Deliberately does
// NOT auto-close the game (a transient dip shouldn't kill a match); the player
// closes it and sends the log.
std::ofstream g_perf_log;
std::string g_last_frame_info = "(no frames rendered yet)";
long long g_frame_index = 0;

#ifdef _WIN32
LONG WINAPI ww_crash_handler(EXCEPTION_POINTERS* info) {
    if (g_perf_log.is_open()) {
        g_perf_log << "\n*** CRASH (unhandled exception) ***\n";
        if (info && info->ExceptionRecord) {
            char b[160];
            std::snprintf(b, sizeof(b), "  exception code = 0x%08lX   at address = %p\n",
                          static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                          info->ExceptionRecord->ExceptionAddress);
            g_perf_log << b;
        }
        g_perf_log << "  LAST FRAME BEFORE CRASH -> " << g_last_frame_info << "\n"
                   << "  (This line shows what the game was doing when it froze/crashed --\n"
                   << "   send this whole file so the fault can be located.)\n";
        g_perf_log.flush();
    }
    return EXCEPTION_CONTINUE_SEARCH; // let the OS still terminate/report as normal
}
#endif

void ww_open_perf_log() {
    static bool opened = false;
    if (opened) return;
    opened = true;
    std::string path = "worldwar_perf.log";
    if (char* base = SDL_GetBasePath()) {
        path = std::string(base) + "worldwar_perf.log";
        SDL_free(base);
    }
    g_perf_log.open(path, std::ios::out | std::ios::trunc);
    if (g_perf_log.is_open()) {
        g_perf_log << "World War performance / crash log.\n"
                      "A line is written for every frame slower than 30 FPS (>33ms). If the game\n"
                      "crashes, the final block names the last frame -- send this whole file.\n"
                      "Format: f<n> tot=<ms> upd=<ms> draw=<ms> pres=<ms> | live entity counts\n"
                      "        | txt=<cached glyph textures> +<made this frame> -<destroyed this\n"
                      "          frame> evict=<ms in the cache sweep>\n\n";
        g_perf_log.flush();
        SDL_Log("Perf/crash log: %s", path.c_str());
    }
#ifdef _WIN32
    SetUnhandledExceptionFilter(ww_crash_handler);
#endif
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

    // Resolved from settings.json (Options > Graphics) -- see Settings::
    // resolution_index's comment for why this is a startup-only read
    // rather than a live setting. A fresh Settings just for this lookup is
    // fine; MenuController/GameClient each load() their own copy later.
    Settings startup_settings;
    startup_settings.load();
    int ridx = std::clamp(startup_settings.resolution_index, 0,
                           static_cast<int>(std::size(kResolutions)) - 1);
    // Not const: the Graphics options screen can change resolution live
    // while still in the menu (see the WIDTH/HEIGHT-updating block in the
    // menu loop below) -- GameClient is only ever constructed AFTER that
    // loop exits, so it always picks up whatever WIDTH/HEIGHT is current at
    // that point, with no separate "apply to an in-progress match" path
    // needed (the in-match pause menu has no Options/Graphics access point
    // -- see GameClient::draw_pause_menu).
    int WIDTH = kResolutions[ridx].w, HEIGHT = kResolutions[ridx].h;
    SDL_Window* window = SDL_CreateWindow(
        "World War (C++ preview)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return 1;
    }
    SDL_RendererInfo rinfo;
    SDL_GetRendererInfo(renderer, &rinfo);
    SDL_Log("Renderer: %s (accelerated=%d vsync=%d)", rinfo.name,
            (rinfo.flags & SDL_RENDERER_ACCELERATED) != 0, (rinfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0);

    // The game always renders at a fixed WIDTHxHEIGHT virtual resolution
    // into this offscreen target, which is then scaled (preserving aspect
    // ratio, letterboxed/pillarboxed with black bars) into however big the
    // actual window is. This keeps the HUD/world sprites at a constant
    // pixel density regardless of window size, instead of the previous
    // approach (resizing the camera/HUD to match the real window), which
    // made everything shrink when the window was made smaller.
    SDL_Texture* render_target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                                   SDL_TEXTUREACCESS_TARGET, WIDTH, HEIGHT);
    if (!render_target) {
        SDL_Log("SDL_CreateTexture (render target) failed: %s", SDL_GetError());
        return 1;
    }

    // Scale factor and destination rect for blitting the fixed-resolution
    // render target into the actual (possibly resized/fullscreen) window.
    // Hoisted above the GameClient scope block below (moved up from where
    // it used to live, right after GameClient's construction) so the menu
    // loop -- which runs BEFORE GameClient exists -- can use the exact same
    // letterboxing/coordinate-mapping as the game loop.
    double letterbox_scale = 1.0;
    SDL_Rect letterbox_dst{0, 0, WIDTH, HEIGHT};
    auto recompute_letterbox = [&]() {
        int win_w, win_h;
        SDL_GetWindowSize(window, &win_w, &win_h);
        if (win_w <= 0 || win_h <= 0) return; // minimized -- keep last valid values
        letterbox_scale = std::min(static_cast<double>(win_w) / WIDTH, static_cast<double>(win_h) / HEIGHT);
        int dst_w = static_cast<int>(std::lround(WIDTH * letterbox_scale));
        int dst_h = static_cast<int>(std::lround(HEIGHT * letterbox_scale));
        letterbox_dst = SDL_Rect{(win_w - dst_w) / 2, (win_h - dst_h) / 2, dst_w, dst_h};
    };
    recompute_letterbox();
    // Maps a real window pixel coordinate (e.g. from a mouse event) to the
    // fixed virtual-canvas coordinate space the game logic actually uses.
    auto to_virtual = [&](int& x, int& y) {
        x = static_cast<int>(std::lround((x - letterbox_dst.x) / letterbox_scale));
        y = static_cast<int>(std::lround((y - letterbox_dst.y) / letterbox_scale));
    };

    // WW_SKIP_MENU bypasses the menu entirely and starts a default skirmish
    // immediately, exactly like every build before the menu existed -- a
    // deliberate, permanent testing affordance so the WW_TEST_*/WW_SHOT
    // synthetic-input hooks throughout this file keep working unmodified
    // (they all assume the game is already running from frame 0).
    bool skip_menu = SDL_getenv("WW_SKIP_MENU") != nullptr;

    // One long-lived Audio for the whole app. Music (the title theme and the
    // in-game playlist) has to survive the menu -> match -> menu transitions,
    // but GameClient is built/destroyed per match, so Audio can't live inside
    // it -- GameClient takes this by reference. A single Mix_OpenAudio also
    // backs all SFX. Constructed after the SDL/renderer setup so the mixer
    // opens once, up front.
    Audio audio(asset_dir + "/sounds");
    // Options > Audio sliders -- reuses the same startup_settings load
    // above (resolution_index's read) rather than loading Settings a
    // second time. Both menu clicks and any in-game sound/music go through
    // this one Audio instance, so this is the only place these need
    // seeding; MenuController::set_audio's slider handlers update it live
    // from here on.
    audio.set_sfx_volume(startup_settings.sfx_volume);
    audio.set_music_volume(startup_settings.music_volume);

    // Outer loop: normally runs its body exactly once (menu -> match ->
    // exit), but loops back to re-show the menu if the in-match pause
    // menu's "Quit Game" was clicked (GameClient::wants_quit_to_menu) --
    // skip_menu bypasses the menu entirely so there's nothing to loop back
    // to in that mode, matching every other WW_SKIP_MENU behavior of
    // acting like the menu doesn't exist at all.
    bool app_should_exit = false;
    // Set right before looping back to the menu after a "Start Stress
    // Test" preview ends (see the app_should_exit check at the bottom of
    // this loop) -- consumed the next time a MenuController is constructed
    // below, jumping it straight to Options > Graphics (where the button
    // was clicked) instead of the title screen. Declared outside the loop
    // since it needs to survive from the iteration that's ending into the
    // next one that's about to start.
    bool return_to_graphics_options = false;
    while (!app_should_exit) {
    app_should_exit = true; // this iteration's the last one unless quit-to-menu overrides it below

    std::optional<ww::sim::SkirmishSettings> menu_settings;
    // Set instead of menu_settings when the campaign screen's level popup
    // "Play" button was clicked (MenuController::wants_play_level) -- a
    // COPY of the chosen Level, not a reference, since `menu` (and its
    // campaigns_ vector chosen_level() points into) is about to go out of
    // scope below.
    std::optional<ww::campaign::Level> menu_level;
    // The chosen Level's PARENT campaign's name -- also a copy, same
    // reasoning as menu_level above. Only meaningful alongside menu_level
    // (both set together below); threaded into GameClient's campaign
    // constructor so a win can be recorded against the right
    // campaign_level_key() (see GameClient::update's game-over handling).
    std::string menu_campaign_name;
    bool aborted = false;
    // Debug/perf-testing: set from the Graphics screen's "Start Stress
    // Test" button before `menu` goes out of scope below -- checked after
    // GameClient is constructed to instantly fill out every team's army
    // (see GameClient::populate_stress_test) and lock the player out of
    // interacting with it (GameClient::set_spectator), and every frame of
    // the game loop below to drive the 30-second/Enter/Escape auto-return.
    bool stress_test_requested = false;

    // ---- the multiplayer session ------------------------------------------
    // Declared HERE, above the menu, because the lobby screen drives it: the
    // handshake has to finish before the match is built, since the joiner
    // learns the seed and settings from the host and both machines must
    // generate the identical world from them. It then outlives the menu and is
    // handed on to GameClient (set_session, below), so app.cpp owns it and the
    // menu only borrows a pointer. The env hook further down uses the same
    // object.
    ww::net::Session mp_session;
    bool mp_active = false;
    bool mp_from_lobby = false;

    if (!skip_menu) {
        audio.play_title(); // looping title theme while in the menus (main.py play_title)
        MenuController menu(renderer, asset_dir, data_dir, WIDTH, HEIGHT);
        menu.set_audio(&audio); // menu button clicks play the UI click sound
        menu.set_session(&mp_session); // enables the Multiplayer button + lobby
        if (return_to_graphics_options) {
            menu.open_graphics_options();
            return_to_graphics_options = false;
        }
        bool menu_running = true;

        // Same synthetic-input/screenshot pattern as the game loop's
        // WW_TEST_CLICK*/WW_SHOT below, scoped to the menu loop (separate
        // env vars since frame counting is independent of the game loop's).
        auto menu_synth_click = [&](int x, int y) {
            SDL_Event down{}, up{};
            down.type = SDL_MOUSEBUTTONDOWN;
            down.button.button = SDL_BUTTON_LEFT;
            down.button.x = x; down.button.y = y;
            up.type = SDL_MOUSEBUTTONUP;
            up.button.button = SDL_BUTTON_LEFT;
            up.button.x = x; up.button.y = y;
            menu.handle_event(down);
            menu.handle_event(up);
        };
        int mclick1_x = -1, mclick1_y = -1, mclick2_x = -1, mclick2_y = -1, mclick3_x = -1, mclick3_y = -1;
        int mclick4_x = -1, mclick4_y = -1, mclick5_x = -1, mclick5_y = -1, mclick6_x = -1, mclick6_y = -1;
        int mclick7_x = -1, mclick7_y = -1;
        if (const char* c = SDL_getenv("WW_TEST_MENU_CLICK7")) sscanf(c, "%d,%d", &mclick7_x, &mclick7_y);
        if (const char* c = SDL_getenv("WW_TEST_MENU_CLICK")) sscanf(c, "%d,%d", &mclick1_x, &mclick1_y);
        if (const char* c = SDL_getenv("WW_TEST_MENU_CLICK2")) sscanf(c, "%d,%d", &mclick2_x, &mclick2_y);
        if (const char* c = SDL_getenv("WW_TEST_MENU_CLICK3")) sscanf(c, "%d,%d", &mclick3_x, &mclick3_y);
        if (const char* c = SDL_getenv("WW_TEST_MENU_CLICK4")) sscanf(c, "%d,%d", &mclick4_x, &mclick4_y);
        if (const char* c = SDL_getenv("WW_TEST_MENU_CLICK5")) sscanf(c, "%d,%d", &mclick5_x, &mclick5_y);
        if (const char* c = SDL_getenv("WW_TEST_MENU_CLICK6")) sscanf(c, "%d,%d", &mclick6_x, &mclick6_y);
        // Synthetic mouse-wheel notch (e.g. for the Hotkeys screen's Units &
        // Research scroll) -- applied at the same frame slot as CLICK4,
        // after the CLICK1-3 navigation sequence has had time to land.
        int mwheel_y = 0;
        if (const char* c = SDL_getenv("WW_TEST_MENU_WHEEL")) mwheel_y = std::atoi(c);
        const char* menu_shot_env = SDL_getenv("WW_MENU_SHOT");
        std::string menu_shot_str = menu_shot_env ? menu_shot_env : "";
        const char* menu_shot_path = menu_shot_str.empty() ? nullptr : menu_shot_str.c_str();
        int menu_shot_after_frames = 20;
        if (const char* n = SDL_getenv("WW_MENU_SHOT_FRAMES")) menu_shot_after_frames = std::atoi(n);
        // Jump straight to the Update Notes popup at entry N (0 = newest) for a
        // WW_MENU_SHOT, same pattern as WW_TEST_MENU_TECHTREE/CIVCHOOSER.
        if (const char* n = SDL_getenv("WW_TEST_MENU_NOTES")) menu.test_open_update_notes(std::atoi(n));
        int menu_frame_count = 0;

        while (menu_running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) {
                    menu_running = false;
                    aborted = true;
                } else if (ev.type == SDL_WINDOWEVENT &&
                          (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                           ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
                    recompute_letterbox();
                } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F4 &&
                          (ev.key.keysym.mod & KMOD_ALT)) {
                    menu_running = false;
                    aborted = true; // Alt+F4 quits from the menu too
                } else if (ev.type == SDL_KEYDOWN &&
                          (ev.key.keysym.sym == SDLK_F11 ||
                           (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)))) {
                    // Same F11/Alt+Enter fullscreen toggle as the game loop
                    // below -- was missing here entirely, so it silently
                    // did nothing while any menu screen was showing.
                    bool is_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                    SDL_SetWindowFullscreen(window, is_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    recompute_letterbox();
                } else {
                    SDL_Event tev = ev;
                    if (tev.type == SDL_MOUSEBUTTONDOWN || tev.type == SDL_MOUSEBUTTONUP) {
                        to_virtual(tev.button.x, tev.button.y);
                    } else if (tev.type == SDL_MOUSEMOTION) {
                        to_virtual(tev.motion.x, tev.motion.y);
                    }
                    menu.handle_event(tev);
                }
            }
            if (mclick1_x >= 0 && menu_frame_count == 5) menu_synth_click(mclick1_x, mclick1_y);
            if (mclick2_x >= 0 && menu_frame_count == 10) menu_synth_click(mclick2_x, mclick2_y);
            if (mclick3_x >= 0 && menu_frame_count == 15) menu_synth_click(mclick3_x, mclick3_y);
            if (mclick4_x >= 0 && menu_frame_count == 18) menu_synth_click(mclick4_x, mclick4_y);
            {
                // Frame 18 by default (WW_TEST_MENU_WHEEL_FRAME overrides it,
                // e.g. to scroll AFTER a later click reaches the target
                // screen -- the Tech Tree screen only exists once its own
                // "open" click has already fired).
                int wheel_frame = 18;
                if (const char* wf = SDL_getenv("WW_TEST_MENU_WHEEL_FRAME")) wheel_frame = std::atoi(wf);
                if (mwheel_y != 0 && menu_frame_count == wheel_frame) {
                    SDL_Event wheel{};
                    wheel.type = SDL_MOUSEWHEEL;
                    wheel.wheel.y = mwheel_y;
                    menu.handle_event(wheel);
                }
            }
            if (mclick5_x >= 0 && menu_frame_count == 21) menu_synth_click(mclick5_x, mclick5_y);
            if (mclick6_x >= 0 && menu_frame_count == 24) menu_synth_click(mclick6_x, mclick6_y);
            if (mclick7_x >= 0 && menu_frame_count == 27) menu_synth_click(mclick7_x, mclick7_y);
            // One-shot synthetic "hold Ctrl, press <letter>, release <letter>"
            // for exercising the Hotkeys screen's press-and-release rebind
            // flow -- WW_TEST_MENU_CTRLKEY=<letter>, fired as a KEYDOWN (with
            // KMOD_CTRL set) then a KEYUP two frames later, both at/after
            // frame 24 so a prior CLICK5/6 has had time to open a rebind.
            if (const char* ck = SDL_getenv("WW_TEST_MENU_CTRLKEY")) {
                if (ck[0] && menu_frame_count == 24) {
                    SDL_Event down{};
                    down.type = SDL_KEYDOWN;
                    down.key.keysym.sym = SDL_GetKeyFromName(std::string(1, ck[0]).c_str());
                    down.key.keysym.mod = KMOD_LCTRL;
                    menu.handle_event(down);
                } else if (ck[0] && menu_frame_count == 26) {
                    SDL_Event up{};
                    up.type = SDL_KEYUP;
                    up.key.keysym.sym = SDL_GetKeyFromName(std::string(1, ck[0]).c_str());
                    up.key.keysym.mod = KMOD_LCTRL;
                    menu.handle_event(up);
                }
            }
            // Same idea, no Ctrl -- WW_TEST_MENU_PLAINKEY=<SDL key name, e.g.
            // "5" or "Z" or "Left Shift">, for exercising rebinds that
            // previously-reserved keys (digits, Z, Shift) should now accept.
            if (const char* pk = SDL_getenv("WW_TEST_MENU_PLAINKEY")) {
                if (pk[0] && menu_frame_count == 24) {
                    SDL_Event down{};
                    down.type = SDL_KEYDOWN;
                    down.key.keysym.sym = SDL_GetKeyFromName(pk);
                    menu.handle_event(down);
                } else if (pk[0] && menu_frame_count == 26) {
                    SDL_Event up{};
                    up.type = SDL_KEYUP;
                    up.key.keysym.sym = SDL_GetKeyFromName(pk);
                    menu.handle_event(up);
                }
            }
            // WW_TEST_MENU_TECHTREE=<civ>: jump straight to the Tech Tree
            // screen previewing that civ (frame 2), and WW_TEST_MENU_HOVER=
            // "x,y" injects a virtual-space mouse hover just before the shot
            // so a tooltip renders. Both are screenshot-only affordances.
            if (const char* tt = SDL_getenv("WW_TEST_MENU_TECHTREE")) {
                if (menu_frame_count == 2) {
                    int tciv = 0, tscroll = 0; // "civ" or "civ,scroll"
                    std::sscanf(tt, "%d,%d", &tciv, &tscroll);
                    menu.test_open_tech_tree(tciv, tscroll);
                }
            }
            if (const char* cc = SDL_getenv("WW_TEST_MENU_CIVCHOOSER")) {
                if (menu_frame_count == 2) {
                    int tciv = 0, tlead = 0; // "civ" or "civ,leader"
                    std::sscanf(cc, "%d,%d", &tciv, &tlead);
                    menu.test_open_civ_chooser(tciv, tlead);
                }
            }
            if (const char* hv = SDL_getenv("WW_TEST_MENU_HOVER")) {
                int hx = -1, hy = -1;
                // Re-inject every frame once the target screen is up, so a
                // stray real-cursor MOUSEMOTION in a later frame's poll can't
                // clobber the hover before the screenshot is taken.
                if (std::sscanf(hv, "%d,%d", &hx, &hy) == 2 && menu_frame_count >= 3) {
                    SDL_Event mm{};
                    mm.type = SDL_MOUSEMOTION;
                    mm.motion.x = hx; // already virtual-space coords
                    mm.motion.y = hy;
                    menu.handle_event(mm);
                }
            }
            if (SDL_getenv("WW_TEST_MENU_F11") && menu_frame_count == 5) {
                // Pushed onto SDL's real event queue (not handed directly
                // to menu.handle_event) so it's picked up by the SAME
                // SDL_PollEvent loop above and exercises the actual F11
                // handling path, not a synthetic bypass of it.
                SDL_Event f11{};
                f11.type = SDL_KEYDOWN;
                f11.key.keysym.sym = SDLK_F11;
                SDL_PushEvent(&f11);
            }

            // Graphics options screen's resolution buttons apply live --
            // recreate the virtual-canvas render target at the new size and
            // let both the menu's own layout and the letterbox scale catch
            // up, all before this frame draws, so the very same click's
            // frame already reflows to the new size (see wants_resize()'s
            // comment in menu_controller.h).
            if (menu.wants_resize()) {
                int nw, nh;
                menu.consume_resize(nw, nh);
                if (nw != WIDTH || nh != HEIGHT) {
                    SDL_Texture* new_target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                                                SDL_TEXTUREACCESS_TARGET, nw, nh);
                    if (new_target) {
                        SDL_DestroyTexture(render_target);
                        render_target = new_target;
                        WIDTH = nw;
                        HEIGHT = nh;
                        menu.resize(WIDTH, HEIGHT);
                        recompute_letterbox();
                        // The OS cursor hasn't physically moved, but the
                        // letterbox scale/offset it maps through just did --
                        // mouse_pos_ (drawn cursor position) would otherwise
                        // stay stale at its pre-resize virtual coordinate
                        // until the next real MOUSEMOTION event, which reads
                        // as the cursor "jumping" the moment the player next
                        // moves it. Re-derive it immediately from the real
                        // cursor position through the NEW mapping instead.
                        int mx, my;
                        SDL_GetMouseState(&mx, &my);
                        to_virtual(mx, my);
                        SDL_Event resync{};
                        resync.type = SDL_MOUSEMOTION;
                        resync.motion.x = mx;
                        resync.motion.y = my;
                        menu.handle_event(resync);
                    } else {
                        SDL_Log("SDL_CreateTexture (resize) failed: %s", SDL_GetError());
                    }
                }
            }

            SDL_SetRenderTarget(renderer, render_target);
            menu.draw(renderer);
            SDL_SetRenderTarget(renderer, nullptr);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer); // letterbox/pillarbox bars
            SDL_RenderCopy(renderer, render_target, nullptr, &letterbox_dst);
            SDL_RenderPresent(renderer);

            ++menu_frame_count;
            if (menu_shot_path && menu_frame_count >= menu_shot_after_frames) {
                int w, h;
                SDL_GetRendererOutputSize(renderer, &w, &h);
                SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
                SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                SDL_SaveBMP(surf, menu_shot_path);
                SDL_FreeSurface(surf);
                menu_running = false;
                aborted = true; // WW_MENU_SHOT is for capturing a menu screen only, not launching a match
            }

            if (menu.wants_quit()) {
                menu_running = false;
                aborted = true;
            } else if (menu.wants_start()) {
                menu_settings = menu.build_settings();
                stress_test_requested = menu.wants_stress_test();
                menu_running = false;
            } else if (menu.wants_start_mp()) {
                // The lobby has already hosted/joined AND reached Ready, so the
                // seed and settings are settled. Everything else the network
                // match needs is done below, next to the env-hook path.
                mp_from_lobby = true;
                menu_running = false;
            } else if (menu.wants_play_level()) {
                menu_level = menu.chosen_level();
                menu_campaign_name = menu.chosen_campaign_name();
                menu_running = false;
            }
        }
    }

    // Scoped so GameClient's SDL_Textures are destroyed before the
    // renderer/SDL_Quit below tear down the GPU context they belong to
    // (see the memory note on this exact SpriteAtlas-teardown-order bug).
    if (!aborted) {
    std::optional<GameClient> client_opt;
    // WW_SPECTATE + WW_SKIP_MENU: build the tournament's own settings rather
    // than the default skirmish ones, or the match on screen is not the match
    // that was measured (--tournament runs ostland / 200 pop / size 64 /
    // revealed, and the arena's default skirmish is none of those). Overridable
    // so the same hook can watch a match from a differently-configured run.
    // Civ pair follows plan_match: `--vary-civs` derives both from the match
    // index, so pass them explicitly when reproducing such a match.
    // ---- multiplayer entry points -----------------------------------------
    // The normal one is now the LOBBY: Main Menu > Multiplayer (see
    // MenuController::draw_multiplayer). It hosts or joins, shows the addresses
    // to read out, runs the UPnP probe off the render thread, and reports
    // progress -- all the things the console wait below cannot.
    //
    // The env hook stays, because it is still the fastest way to bring two
    // builds up for a sync test and it needs no clicking:
    //
    //   host:   WW_MP_HOST=1                     [WW_MP_PORT=27015]
    //   join:   WW_MP_JOIN=<address>             [WW_MP_PORT=27015]
    //
    // The host opens the port (UPnP first, and it still hosts if that fails --
    // a manually forwarded port or a flat LAN/VPN works just as well), then
    // waits. The joiner connects and receives the seed and settings, so both
    // sides build the identical match. Team 0 is the host, team 1 the joiner.
    // (mp_session / mp_active are declared above the menu -- the lobby needs
    // the session before this point.)
    uint16_t mp_port = ww::net::kDefaultPort;
    if (const char* p = SDL_getenv("WW_MP_PORT")) mp_port = static_cast<uint16_t>(std::atoi(p));
    const char* mp_host_env = SDL_getenv("WW_MP_HOST");
    const char* mp_join_env = SDL_getenv("WW_MP_JOIN");
    std::string mp_join_addr = mp_join_env ? mp_join_env : "";

    std::optional<ww::sim::SkirmishSettings> spectate_settings;
    if (skip_menu && SDL_getenv("WW_SPECTATE")) {
        ww::sim::SkirmishSettings s;
        s.n_players = 2;
        s.max_pop = 200;
        s.map_size = 64;
        s.map_type = "ostland";
        s.reveal_mode = 2;
        s.difficulty = 1;
        s.civs = {0, 2};
        if (const char* v = SDL_getenv("WW_SPECTATE_POP")) s.max_pop = std::atoi(v);
        if (const char* v = SDL_getenv("WW_SPECTATE_SIZE")) s.map_size = std::atoi(v);
        if (const char* v = SDL_getenv("WW_SPECTATE_MAP")) s.map_type = v;
        if (const char* v = SDL_getenv("WW_SPECTATE_DIFF")) s.difficulty = std::atoi(v);
        if (const char* v = SDL_getenv("WW_SPECTATE_CIVS")) {
            int a = 0, b = 2;
            if (std::sscanf(v, "%d,%d", &a, &b) == 2) s.civs = {a, b};
        }
        spectate_settings = s;
    }
    // Establish the network match BEFORE the client is built: the joiner has to
    // learn the seed and settings from the host, because the world is generated
    // from them and both machines must generate the identical one.
    std::optional<ww::sim::SkirmishSettings> mp_settings;
    if (mp_from_lobby) {
        // The lobby already did all of the above -- host()/join(), the poll
        // loop, and the wait for Ready -- while drawing, so there is nothing
        // left to do but adopt what it agreed on. Pinning WW_SEED is what makes
        // the joiner generate the HOST's world rather than one of its own
        // (match_seed() reads it), exactly as in the env path below.
        char seed_txt[32];
        std::snprintf(seed_txt, sizeof(seed_txt), "%llu",
                      static_cast<unsigned long long>(mp_session.seed()));
        SDL_setenv("WW_SEED", seed_txt, 1);
        mp_settings = mp_session.settings();
        mp_active = true;
        SDL_Log("[mp] lobby: seed=%s, you are team %d, ping %dms", seed_txt,
                mp_session.local_team(), mp_session.ping_ms());
    } else if (mp_host_env || mp_join_env) {
        ww::sim::SkirmishSettings s = menu_settings ? *menu_settings : ww::sim::SkirmishSettings{};
        s.n_players = 2;
        // Only team 0 is fog-restricted when placing buildings, which would
        // handicap the joiner specifically. Reveal removes the asymmetry.
        s.reveal_mode = 2;
        if (mp_host_env) {
            uint64_t seed = static_cast<uint64_t>(SDL_GetPerformanceCounter());
            if (const char* sv = SDL_getenv("WW_SEED")) seed = std::strtoull(sv, nullptr, 10);
            // Open the listening socket BEFORE talking to the router. UPnP
            // discovery is SSDP over multicast with per-target timeouts, so it
            // can take the better part of a minute on a network with no router
            // to find -- and a joiner who connects during that window gets
            // "connection refused" from a host that is, as far as the player is
            // concerned, already hosting. The mapping is only needed by the
            // time a remote player actually connects, so it can follow.
            if (!mp_session.host(mp_port, s, seed)) {
                SDL_Log("[mp] could not host: %s", mp_session.error().c_str());
                return 1;
            }
            SDL_Log("[mp] listening on port %u", mp_port);
            ww::net::PortMapResult pm = ww::net::map_port(mp_port);
            SDL_Log("[mp] UPnP: router=%s mapped=%s external=%s%s%s", pm.discovered ? "yes" : "no",
                    pm.mapped ? "yes" : "no",
                    pm.external_ip.empty() ? "(unknown)" : pm.external_ip.c_str(),
                    pm.error.empty() ? "" : " -- ", pm.error.c_str());
            for (const std::string& a : ww::net::local_addresses())
                SDL_Log("[mp] this machine is reachable at %s:%u", a.c_str(), mp_port);
            if (!pm.external_ip.empty())
                SDL_Log("[mp] over the internet: %s:%u%s", pm.external_ip.c_str(), mp_port,
                        pm.mapped ? "" : "  (port NOT opened -- forward it manually)");
            SDL_Log("[mp] waiting for the other player...");
        } else {
            SDL_Log("[mp] connecting to %s:%u ...", mp_join_addr.c_str(), mp_port);
            if (!mp_session.join(mp_join_addr, mp_port)) {
                SDL_Log("[mp] could not connect: %s", mp_session.error().c_str());
                return 1;
            }
        }
        // Block until the handshake finishes. A lobby screen would do this in
        // the background; here a console wait is honest and 40 lines shorter.
        for (int i = 0; i < 60000; ++i) { // ~10 minutes of patience
            mp_session.poll();
            if (mp_session.status() == ww::net::Status::Ready) break;
            if (mp_session.status() == ww::net::Status::Failed) {
                SDL_Log("[mp] failed: %s", mp_session.error().c_str());
                return 1;
            }
            SDL_Delay(10);
        }
        if (mp_session.status() != ww::net::Status::Ready) {
            SDL_Log("[mp] gave up waiting for the other player");
            return 1;
        }
        // ---- the env hook has no lobby, so it stands in for one ------------
        // Reaching Ready is no longer enough to start: there is a roster screen
        // now, and the match only begins when the HOST calls start_lobby_match
        // with both slots ready (see net/session.h). This path has no UI to
        // press either button in, so it asserts readiness on its own behalf and,
        // if it is the host, starts as soon as the other side is ready too.
        //
        // That keeps every combination working: two env peers behave exactly as
        // they always did, and an env peer mixed with a real lobby peer simply
        // shows up already ready.
        {
            ww::net::LobbySlot me = mp_session.lobby_slot(mp_session.local_team());
            me.ready = true;
            mp_session.set_local_slot(me);
        }
        for (int i = 0; i < 60000 && !mp_session.match_starting(); ++i) {
            mp_session.poll();
            if (mp_host_env) mp_session.start_lobby_match(); // no-op until both are ready
            if (mp_session.status() == ww::net::Status::Failed ||
                mp_session.status() == ww::net::Status::Closed) {
                SDL_Log("[mp] lobby ended: %s", mp_session.error().c_str());
                return 1;
            }
            if (mp_session.match_starting()) break;
            SDL_Delay(10);
        }
        if (!mp_session.match_starting()) {
            SDL_Log("[mp] gave up waiting in the lobby");
            return 1;
        }
        // Both sides now agree on the seed and settings. match_seed() reads
        // WW_SEED, so pin it -- that is what makes the joiner generate the
        // host's world rather than one of its own.
        char seed_txt[32];
        std::snprintf(seed_txt, sizeof(seed_txt), "%llu",
                      static_cast<unsigned long long>(mp_session.seed()));
        SDL_setenv("WW_SEED", seed_txt, 1);
        mp_settings = mp_session.settings();
        mp_active = true;
        SDL_Log("[mp] connected. seed=%s, you are team %d, ping %dms", seed_txt,
                mp_session.local_team(), mp_session.ping_ms());
    }

    if (mp_settings)
        client_opt.emplace(renderer, audio, asset_dir, data_dir, WIDTH, HEIGHT, *mp_settings);
    else if (spectate_settings)
        client_opt.emplace(renderer, audio, asset_dir, data_dir, WIDTH, HEIGHT, *spectate_settings);
    else if (skip_menu) client_opt.emplace(renderer, audio, asset_dir, data_dir, WIDTH, HEIGHT);
    else if (menu_level) {
        client_opt.emplace(renderer, audio, asset_dir, data_dir, WIDTH, HEIGHT, *menu_level, menu_campaign_name);
    }
    else client_opt.emplace(renderer, audio, asset_dir, data_dir, WIDTH, HEIGHT, *menu_settings);
    GameClient& client = *client_opt;

    if (mp_active) {
        // Both teams are human. Attaching the session hands the sim clock to
        // the lockstep scheduler: from here the match only advances when both
        // players' commands for the next turn have arrived.
        auto& teams = client.match().control().teams;
        teams[0].is_ai = false;
        teams[1].is_ai = false;
        client.set_session(&mp_session, mp_session.local_team());
        mp_session.start_match();
    }

    // ---- WW_SPECTATE: watch an AI-vs-AI arena match ------------------------
    // The A/B tournament (headless_runner --tournament) has no renderer at all,
    // so a measured match is a row in a CSV and nothing else. This reproduces
    // one on screen: same seed, same settings, same per-team ai_variant split,
    // with no human side and every order-issuing path locked out. Because the
    // sim is deterministic per seed, what plays out here IS the match that was
    // measured, not a re-roll of it.
    //
    //   WW_SPECTATE=<variant>   candidate ai_variant (0 = watch two baselines)
    //   WW_SPECTATE_TEAM=<0|1>  which side carries it (default 1; the
    //                           tournament alternates this per match --
    //                           `cand_team` in the run's CSV says which)
    //   WW_SEED=<n>             the match seed (see match_seed())
    //
    // Everything else mirrors run_one_match in tools/headless_runner/main.cpp,
    // including reveal_mode 2 -- only team 0 is fog-restricted when placing
    // buildings, which would otherwise handicap one specific side. Pair with
    // WW_SKIP_MENU to go straight in; without it the menu's own settings are
    // used and only the AI/spectator part of this applies.
    const char* spectate_env = SDL_getenv("WW_SPECTATE");
    if (spectate_env) {
        auto& teams = client.match().control().teams;
        int cand_variant = std::atoi(spectate_env);
        int cand_team = 1;
        if (const char* t = SDL_getenv("WW_SPECTATE_TEAM")) cand_team = std::atoi(t);
        for (int i = 0; i < 2; ++i) {
            teams[i].is_ai = true;
            // new_skirmish only opts teams i != 0 into the map-derived skirmish
            // plan (team 0 is the human in a real game). Both sides are AI here,
            // so team 0 has to be opted in explicitly or it silently runs the
            // fallback economy and the two sides aren't comparable.
            teams[i].ai_map_derive = true;
            teams[i].ai_variant = (i == cand_team) ? cand_variant : 0;
        }
        client.set_spectator(true);
        SDL_Log("WW_SPECTATE: team %d runs ai_variant %d, team %d runs baseline 0",
                cand_team, cand_variant, 1 - cand_team);
    }

    if (stress_test_requested || SDL_getenv("WW_STRESS_TEST")) client.populate_stress_test();
    // Spectator lockout is specific to the "Start Stress Test" button
    // (stress_test_requested) -- NOT the WW_STRESS_TEST env var, which is a
    // separate debug hook for populating an otherwise-normal, still-
    // player-controlled match (see its own WW_SKIP_MENU-paired usage).
    if (stress_test_requested) client.set_spectator(true);

    // Debug/verification-only: jump the camera to an arbitrary world point
    // and/or zoom out, so a WW_SHOT screenshot can inspect somewhere other
    // than wherever the camera starts (e.g. team 0's base) without needing
    // real mouse-wheel/drag input.
    if (const char* cc = SDL_getenv("WW_TEST_CAMCENTER")) {
        double cx = 0, cy = 0;
        sscanf(cc, "%lf,%lf", &cx, &cy);
        client.camera().center_on(cx, cy);
    }
    if (const char* z = SDL_getenv("WW_TEST_ZOOM")) client.camera().zoom = std::atof(z);

    if (SDL_getenv("WW_TEST_TEAMLOG")) {
        auto& teams = client.match().control().teams;
        int n = menu_settings ? menu_settings->n_players : 2;
        for (int i = 0; i < n; ++i) {
            SDL_Log("team %d civ=%d colour=%d is_ai=%d", i, teams[i].civ, teams[i].colour, teams[i].is_ai);
        }
    }

    // Debug/verification-only: a player rifleman and an enemy rifleman placed
    // 3.5 tiles apart -- inside rifleman RANGE (4) but beyond its SIGHT (3) --
    // to verify a unit engages an enemy within its weapon range.
    if (SDL_getenv("WW_TEST_SHOOT")) {
        ww::sim::World& w = client.match().world();
        double T = ww::sim::TILE;
        for (auto ref : w.active_buildings) {
            ww::sim::Building* b = w.get_building(ref);
            if (b && b->common.team == 0 && b->name == "base") {
                double ox = b->common.x, oy = b->common.y - 6.0 * T; // open ground north of the base
                ww::sim::EntityRef p = w.spawn_unit("rifleman", 0, ox, oy);
                ww::sim::EntityRef e = w.spawn_unit("rifleman", 1, ox + 3.5 * T, oy);
                if (auto* pu = w.get(p)) pu->hold = ww::sim::Vec2{pu->common.x, pu->common.y};
                if (auto* eu = w.get(e)) eu->hold = ww::sim::Vec2{eu->common.x, eu->common.y};
                break;
            }
        }
    }

    // Debug/verification-only: force an enemy unit to spawn next to the
    // player's own base, to test combat/effects without waiting for the
    // AI to march across the map.
    if (SDL_getenv("WW_TEST_COMBAT")) {
        ww::sim::World& w = client.match().world();
        for (auto ref : w.active_buildings) {
            ww::sim::Building* b = w.get_building(ref);
            if (b && b->common.team == 0 && b->name == "base") {
                w.spawn_unit("rifleman", 1, b->common.x + 60, b->common.y + 10);
                break;
            }
        }
    }

    // Debug/verification-only: Li Zongren's infantry (Unit::phase_trees) walk
    // straight through a wall of trees on a move order, while a normal rifleman
    // (phase_trees cleared) is stopped by it. Both get a move goal to the far
    // side; a WW_SHOT after ~250 frames shows the phased one across and the
    // control one stuck on the near side.
    if (SDL_getenv("WW_TEST_TREES")) {
        ww::sim::World& w = client.match().world();
        auto& teams = client.match().control().teams;
        teams[0].civ = 7;     // China
        teams[0].leader = 1;  // Li Zongren -> spawned infantry gain phase_trees
        for (auto ref : w.active_buildings) {
            ww::sim::Building* b = w.get_building(ref);
            if (!b || b->common.team != 0 || b->name != "base") continue;
            double x = b->common.x, y = b->common.y;
            // Scene well east of the base (clear of its footprint): a vertical
            // wall of trees between the start points (west) and goals (east).
            for (int i = -3; i <= 3; ++i) w.spawn_resource("tree", x + 224, y + i * 32);
            ww::sim::EntityRef phased = w.spawn_unit("rifleman", 0, x + 96, y - 24);
            ww::sim::EntityRef control = w.spawn_unit("rifleman", 0, x + 96, y + 24);
            if (auto* u = w.get(phased)) {
                u->move_goal = ww::sim::Vec2{x + 352, y - 24};
                u->need_path = true; // a real move order sets this so astar runs
            }
            if (auto* u = w.get(control)) {
                u->phase_trees = false;
                u->move_goal = ww::sim::Vec2{x + 352, y + 24};
                u->need_path = true;
            }
            break;
        }
    }

    // Debug/verification-only: prove the jet chain unlocks a full age early for
    // Germany + Hermann Goering. At War era (2) jet engine should be
    // researchable, then jet fighter upgrade, then jet fighter available.
    if (SDL_getenv("WW_TEST_JETS")) {
        auto& ctrl = client.match().control();
        auto& t0 = ctrl.teams[0];
        t0.civ = 2;      // Nazi Germany
        t0.leader = 1;   // Hermann Goering
        t0.era = 2;      // War era
        auto has = [](const std::vector<std::string>& v, const std::string& s) {
            return std::find(v.begin(), v.end(), s) != v.end();
        };
        auto uni = ctrl.available_techs("university", 0);
        SDL_Log("WW_TEST_JETS era2 jet_engine_available=%d", has(uni, "jet engine") ? 1 : 0);
        t0.tech.insert("jet engine");
        auto ab = ctrl.available_techs("airbase", 0);
        SDL_Log("WW_TEST_JETS era2 jet_fighter_upgrade_available=%d", has(ab, "jet fighter upgrade") ? 1 : 0);
        t0.tech.insert("jet fighter upgrade");
        auto units = ctrl.available_units("airbase", 0);
        SDL_Log("WW_TEST_JETS era2 jet_fighter_available=%d", has(units, "jet fighter") ? 1 : 0);
        auto sumcost = [&](const std::string& item) {
            int s = 0;
            for (auto& [k, v] : ctrl.cost_of(item, 0)) s += v;
            return s;
        };
        SDL_Log("WW_TEST_JETS cost jet_engine=%d jet_fighter_upgrade=%d jet_fighter_UNIT=%d",
                sumcost("jet engine"), sumcost("jet fighter upgrade"), sumcost("jet fighter"));
    }

    // Debug/verification-only: Soviet Heavy Artillery unique. At Scientific era
    // with both artillery upgrades, the barracks should offer Heavy Artillery;
    // also spawns one by the base so a WW_SHOT shows the team-coloured sprite.
    if (SDL_getenv("WW_TEST_HEAVYARTY")) {
        auto& ctrl = client.match().control();
        auto& t0 = ctrl.teams[0];
        { // Non-Soviet gating check: even with the techs, UK must NOT get it.
            t0.civ = 0; t0.era = 3;
            t0.tech.insert("artillery upgrade");
            t0.tech.insert("heavy artillery upgrade");
            auto u0 = ctrl.available_units("barracks", 0);
            auto t0t = ctrl.available_techs("barracks", 0);
            bool hu = std::find(u0.begin(), u0.end(), std::string("heavy artillery")) != u0.end();
            bool ht = std::find(t0t.begin(), t0t.end(), std::string("heavy artillery upgrade")) != t0t.end();
            SDL_Log("WW_TEST_HEAVYARTY nonSoviet(UK) unit=%d tech=%d (both should be 0)", hu, ht);
            t0.tech.clear();
        }
        t0.civ = 3; t0.era = 2; // War era (heavy artillery is now War-era)
        t0.tech.insert("artillery upgrade"); // prereq; leave heavy upgrade UNresearched so it shows on the card
        t0.tech.insert("420mm mortar");      // 5-Year Plan -> verify the -20% cost
        auto techs = ctrl.available_techs("barracks", 0);
        bool has = std::find(techs.begin(), techs.end(), std::string("heavy artillery upgrade")) != techs.end();
        int hc = 0, tk = 0;
        for (auto& [k, v] : ctrl.cost_of("heavy artillery", 0)) hc += v;
        for (auto& [k, v] : ctrl.cost_of("tank", 0)) tk += v;
        SDL_Log("WW_TEST_HEAVYARTY 5YP heavy_arty_cost=%d tank_cost=%d (base 220 / 190)", hc, tk);
        int cost = 0;
        for (auto& [k, v] : ctrl.cost_of("heavy artillery", 0)) cost += v;
        int upc = 0;
        for (auto& [k, v] : ctrl.cost_of("heavy artillery upgrade", 0)) upc += v;
        SDL_Log("WW_TEST_HEAVYARTY barracks_has_heavy_arty=%d unit_cost=%d upgrade_cost=%d", has ? 1 : 0,
                cost, upc);
        ww::sim::World& w = client.match().world();
        for (auto ref : w.active_buildings) {
            ww::sim::Building* b = w.get_building(ref);
            if (b && b->common.team == 0 && b->name == "base") {
                w.spawn_unit("heavy artillery", 0, b->common.x + 110, b->common.y);
                w.spawn_unit("tank", 0, b->common.x + 110, b->common.y - 90); // Soviet tank skin
                w.spawn_unit("heavy tank", 0, b->common.x - 150, b->common.y - 110); // Soviet heavy tank skin
                ww::sim::EntityRef dh = w.spawn_unit("heavy tank", 0, b->common.x - 20, b->common.y - 110);
                w.hurt(dh, 9999.0); // killed -> verify the Soviet heavy tank wreck
                ww::sim::EntityRef dt = w.spawn_unit("tank", 0, b->common.x + 260, b->common.y - 90);
                w.hurt(dt, 9999.0); // killed -> verify the Soviet tank wreck

                // A second one killed immediately, to verify the death wreck.
                ww::sim::EntityRef dead = w.spawn_unit("heavy artillery", 0, b->common.x + 210, b->common.y);
                w.hurt(dead, 9999.0);
                // A completed barracks, selected, so a WW_SHOT captures its
                // command card and confirms the Heavy Artillery Upgrade icon
                // doesn't overlap another icon.
                ww::sim::EntityRef bk = w.spawn_building("barracks", 0, b->common.x - 140, b->common.y);
                if (ww::sim::Building* bb = w.get_building(bk)) {
                    bb->complete = true;
                    bb->construction = 100.0;
                    client.camera().center_on(bb->common.x, bb->common.y);
                    client.test_select(bb->common.x, bb->common.y);
                }
                break;
            }
        }
    }

    // Debug/verification-only: spawn a friendly airbase and a fighter
    // right next to the player's base and send it off on a long move
    // order, to test takeoff/flight/shadows without playing through to
    // actually researching/building an airbase and training one.
    if (SDL_getenv("WW_TEST_PLANE")) {
        ww::sim::World& w = client.match().world();
        for (auto ref : w.active_buildings) {
            ww::sim::Building* b = w.get_building(ref);
            if (b && b->common.team == 0 && b->name == "base") {
                ww::sim::EntityRef airbase =
                    w.spawn_building("airbase", 0, b->common.x + 150, b->common.y);
                ww::sim::EntityRef fighter =
                    w.spawn_unit("fighter", 0, b->common.x + 160, b->common.y + 20);
                if (ww::sim::Unit* u = w.get(fighter)) {
                    u->move_goal = ww::sim::Vec2{b->common.x + 500, b->common.y - 150};
                }
                (void)airbase;
                break;
            }
        }
    }

    // Debug/verification-only: spawn a couple of artillery pieces (team 0 and
    // team 1) by the base and immediately kill them, to see the new
    // spr_artillery_rubble wreck render in each team's colour.
    if (SDL_getenv("WW_TEST_ARTY_WRECK")) {
        ww::sim::World& w = client.match().world();
        for (auto ref : w.active_buildings) {
            ww::sim::Building* b = w.get_building(ref);
            if (b && b->common.team == 0 && b->name == "base") {
                for (int t = 0; t < 2; ++t) {
                    ww::sim::EntityRef a =
                        w.spawn_unit("artillery", t, b->common.x - 50 + t * 90, b->common.y - 30);
                    if (ww::sim::Unit* u = w.get(a)) w.hurt(a, u->common.hp + 10.0);
                    ww::sim::EntityRef h =
                        w.spawn_unit("heavy tank", t, b->common.x - 50 + t * 90, b->common.y + 40);
                    if (ww::sim::Unit* u = w.get(h)) w.hurt(h, u->common.hp + 10.0);
                }
                break;
            }
        }
    }

    // Debug/verification-only: spawn a team-0 artillery by the base and set a
    // standing attack-ground target ~160px away, to see it shell a fixed point
    // consecutively (Unit::attack_ground).
    if (SDL_getenv("WW_TEST_ARTY_GROUND")) {
        ww::sim::World& w = client.match().world();
        for (auto ref : w.active_buildings) {
            ww::sim::Building* b = w.get_building(ref);
            if (b && b->common.team == 0 && b->name == "base") {
                ww::sim::EntityRef a = w.spawn_unit("artillery", 0, b->common.x - 40, b->common.y - 40);
                if (ww::sim::Unit* u = w.get(a)) u->attack_ground = ww::sim::Vec2{b->common.x - 40, b->common.y - 200};
                break;
            }
        }
    }

    // Debug/verification-only: spawn a bomber right next to an enemy
    // building and force it to attack, to test bomb-dropping and the
    // explosion animation without waiting on tech/training/travel time.
    if (SDL_getenv("WW_TEST_BOMB")) {
        ww::sim::World& w = client.match().world();
        for (auto ref : w.active_buildings) {
            ww::sim::Building* b = w.get_building(ref);
            if (b && b->common.team == 0 && b->name == "base") {
                ww::sim::EntityRef target =
                    w.spawn_building("house", 1, b->common.x + 200, b->common.y);
                ww::sim::EntityRef bomber =
                    w.spawn_unit("bomber", 0, b->common.x + 180, b->common.y + 20);
                if (ww::sim::Unit* u = w.get(bomber)) {
                    u->attack_target = target;
                    u->forced = true;
                }
                break;
            }
        }
    }

    // Debug/verification-only: force a team-0 civilian to be already
    // adjacent to a resource and gathering it, to test the working-sprite
    // animation without waiting on the randomized map layout.
    if (SDL_getenv("WW_TEST_HAMMER")) {
        ww::sim::World& w = client.match().world();
        for (auto ref : w.active_units) {
            ww::sim::Unit* u = w.get(ref);
            if (u && u->common.team == 0 && u->name == "civilian") {
                ww::sim::EntityRef tree = w.spawn_resource("tree", u->common.x + 15, u->common.y);
                u->gather_target = tree;
                break;
            }
        }
    }

    // Debug/verification-only: place a house foundation flush against the
    // TOP of the player's base and send a civilian to build it starting
    // from well below the base, reproducing the reported "builder gets
    // stuck routing around the base" scenario deterministically instead of
    // waiting for a randomized map to happen to produce it.
    if (SDL_getenv("WW_TEST_BUILDROUTE")) {
        ww::sim::World& w = client.match().world();
        for (auto ref : w.active_buildings) {
            ww::sim::Building* base = w.get_building(ref);
            if (!(base && base->common.team == 0 && base->name == "base")) continue;
            auto [hx, hy] = w.snap("house", base->common.x,
                                   base->common.y - base->foot_h / 2.0 - 32.0);
            ww::sim::EntityRef foundation =
                w.spawn_building("house", 0, hx, hy, /*constructing=*/true);
            for (auto uref : w.active_units) {
                ww::sim::Unit* u = w.get(uref);
                if (u && u->common.team == 0 && u->name == "civilian") {
                    u->common.x = base->common.x;
                    u->common.y = base->common.y + base->foot_h / 2.0 + 150.0;
                    u->build_target = foundation;
                    break;
                }
            }
            w.prime();
            break;
        }
    }

    // Debug/verification-only: selects (and centers the camera on) the
    // first team-0 civilian, for testing civilian-only command-card state
    // (build_eco/build_military, the Next Page auto-switch) without a
    // synthetic click needing to guess the map's randomized spawn point.
    if (SDL_getenv("WW_TEST_SELECT_CIVILIAN")) {
        ww::sim::World& w = client.match().world();
        for (auto ref : w.active_units) {
            ww::sim::Unit* u = w.get(ref);
            if (u && u->common.team == 0 && u->name == "civilian") {
                client.camera().center_on(u->common.x, u->common.y);
                client.test_select(u->common.x, u->common.y);
                break;
            }
        }
    }

    // Debug/verification-only: pushes one of each notification kind plus a
    // couple of teams' worth of score/era/leader state, to check the
    // notification stack and the score HUD without waiting on real
    // research/training/age-advance timers.
    if (SDL_getenv("WW_TEST_NOTIFY")) {
        ww::sim::World& w = client.match().world();
        auto& teams = client.match().control().teams;
        teams[0].era = 2;
        teams[0].score = 40;
        teams[0].leader = 1;
        if (teams.size() > 1) {
            teams[1].era = 1;
            teams[1].score = 15;
            teams[1].leader = 1;
            teams[1].colour = 1;
        }
        w.events.push({ww::sim::EventType::Notify, "age_advance", 0, 0, 0, ww::sim::kNullRef, ""});
        w.events.push(
            {ww::sim::EventType::Notify, "research_complete", 0, 0, 0, ww::sim::kNullRef, "rifleman upgrade"});
        w.events.push({ww::sim::EventType::Notify, "building_ready", 0, 0, 0, ww::sim::kNullRef, "barracks"});
        w.events.push({ww::sim::EventType::Notify, "unit_created", 0, 0, 0, ww::sim::kNullRef, "rifleman"});
        // Deliberately long, to check draw_notifications' word-wrap (see
        // World::message_triggers/editor's Events tab) --
        // every other notification kind above is a short one-liner that
        // never actually wraps.
        w.events.push({ww::sim::EventType::Notify, "map_message", 0, 0, 0, ww::sim::kNullRef,
                       "This is a deliberately long map-author message meant to test whether the "
                       "notification banner correctly wraps onto multiple lines instead of running "
                       "off the edge of the screen."});
        // Also drop a real, visible trigger near team 0's own base (same
        // spot post_construct centres the camera on) so a WW_SHOT capture
        // can confirm the in-world spr_message icon itself, not just the
        // notification banner above.
        for (auto ref : w.active_buildings) {
            if (auto* b = w.get_building(ref); b && b->common.team == 0 && b->name == "base") {
                w.message_triggers.push_back({b->common.x + 3 * ww::sim::TILE, b->common.y, "Hi!"});
                break;
            }
        }
    }

    // Debug/verification-only: era 3 + a huge stockpile for team 0, so the
    // command-card's fixed building/tech layouts can be screenshotted with
    // everything unlocked at once (WW_TEST_CLICK/CLICK2 then select the
    // base and open a build category to see it).
    if (SDL_getenv("WW_TEST_LAYOUT")) {
        auto& team0 = client.match().control().teams[0];
        team0.era = 3;
        for (auto& [k, v] : team0.res) v = 100000;
        if (SDL_getenv("WW_TEST_LAYOUT_UPGRADED")) {
            // Collapses the light/regular/heavy tank tiers down to one
            // slot (tiger2), so factory's training row doesn't reach the
            // rightmost column where diesel engine/blowback reload live --
            // a more typical late-era state than the bare-minimum one
            // above, where every tank tier is still separately available.
            team0.tech.insert("heavy tank upgrade");
            team0.tech.insert("tiger2 tank upgrade");
        }
        if (SDL_getenv("WW_TEST_LAYOUT_STEEL")) team0.tech.insert("steel plane armor");
        // Research every prereq so the otherwise-gated scientific-era upgrades
        // become available (elite waffen upgrade needs refined steel, atomic
        // bomb needs nuclear physics, jet fighter upgrade needs jet engine,
        // tiger2 needs heavy tank upgrade) -- lets a WW_SHOT verify the UI.
        for (const char* t : {"refined steel", "heavy tank", "heavy tank upgrade", "jet engine",
                              "nuclear physics", "atomic bomb", "bolt action rifle",
                              "semi automatic rifle", "rifleman upgrade"}) {
            team0.tech.insert(t);
        }
        // WW_TEST_LAYOUT_GERMAN: play Germany (for elite waffen / tiger2, which
        // are German-only). Default civ (UK) is used otherwise.
        if (SDL_getenv("WW_TEST_LAYOUT_GERMAN")) {
            team0.civ = 2;
            // The match is created with team 0 as UK (civ 0), so grant_free_techs
            // already handed team 0 UK's free Radar before this hook flips the civ
            // to Germany. Germany researches Radar normally, so drop the free copy
            // here -- otherwise it reads as already-researched and vanishes from the
            // university card, hiding it from this layout screenshot.
            team0.tech.erase("radar");
        }
        ww::sim::World& w = client.match().world();
        for (auto ref : w.active_buildings) {
            ww::sim::Building* base = w.get_building(ref);
            if (base && base->common.team == 0 && base->name == "base") {
                double bx = base->common.x, by = base->common.y;
                w.spawn_building("factory", 0, bx - 300, by);
                w.spawn_building("airbase", 0, bx + 300, by);
                w.spawn_building("university", 0, bx - 300, by + 200);
                w.spawn_building("fortress", 0, bx + 300, by + 200);
                w.spawn_building("barracks", 0, bx - 150, by - 200);
                w.spawn_building("academy", 0, bx + 150, by - 200);
                w.spawn_building("shipyard", 0, bx, by - 200);
                w.spawn_building("market", 0, bx, by + 200);
                // Tall/thin base-anchored-footprint buildings (see World::
                // footprint_dy) -- included so a WW_SHOT can verify their
                // selection outline/click hit-test hugs the sprite's base,
                // not its visual middle.
                w.spawn_building("tower", 0, bx - 450, by);
                w.spawn_building("outpost", 0, bx + 450, by);
                // A ballistic missile unit so a WW_SHOT can verify its sprite
                // states + Pack/Unpack command card. WW_TEST_BALLISTIC_DEPLOY
                // pre-deploys it (unpacked) to check the deployed/loaded sprite.
                {
                    ww::sim::EntityRef bmref = w.spawn_unit("ballistic missile", 0, bx + 120, by + 380);
                    // Extra launchers in other team colours to verify the recolour
                    // (well clear of the horizontal flight path to the test target).
                    w.spawn_unit("ballistic missile", 1, bx - 60, by + 560);
                    w.spawn_unit("ballistic missile", 2, bx + 120, by + 560);
                    if (SDL_getenv("WW_TEST_BALLISTIC_DEPLOY")) {
                        // An enemy building in range, and an attack-ground order
                        // on it (the launcher is non-aggressive, so it won't fire
                        // unbidden) -- lets a late-frame WW_SHOT catch the swivel +
                        // lobbing missile + rear flame + blast.
                        w.spawn_building("tower", 1, bx + 460, by + 380);
                        if (ww::sim::Unit* bu = w.get(bmref)) {
                            bu->packed = false;
                            bu->pack_t = 0.0;
                            bu->attack_ground = ww::sim::Vec2{bx + 460, by + 380};
                        }
                    }
                    if (SDL_getenv("WW_TEST_BALLISTIC_PACKING")) {
                        // Freeze it mid-unpack so a WW_SHOT catches the progress bar.
                        if (ww::sim::Unit* bu = w.get(bmref)) { bu->pack_target = false; bu->pack_t = 2.5; }
                    }
                }
                // WW_TEST_CARRIER: both aircraft-carrier tiers + a low-fuel
                // plane, to verify the sprites render and the mobile-airbase
                // landing works (the fighter descends and parks on the tier-1
                // carrier's deck to refuel).
                if (SDL_getenv("WW_TEST_CARRIER")) {
                    // Two carriers on the right (clear of the test marines), one
                    // facing LEFT and one facing RIGHT, each loaded with several
                    // low-fuel fighters so the deck-diagonal parking + heading
                    // flip are both visible.
                    struct { double cx; int face; const char* type; } cs[2] = {
                        {bx + 120, -1, "aircraft carrier"}, {bx + 380, +1, "aircraft carrier2"}};
                    for (auto& c : cs) {
                        ww::sim::EntityRef cref = w.spawn_unit(c.type, 0, c.cx, by + 170);
                        if (ww::sim::Unit* cu = w.get(cref)) cu->facing = c.face;
                        for (int k = 0; k < 5; ++k) {
                            ww::sim::EntityRef pl = w.spawn_unit("fighter", 0, c.cx, by + 60);
                            if (ww::sim::Unit* p = w.get(pl)) p->fuel = 5.0;
                        }
                    }
                }
                // WW_TEST_CARRIER_RETURN: a lone carrier + a low-fuel fighter
                // airborne well to the RIGHT of it, whose home_id is that carrier
                // -- it should fly back LEFT to the carrier (not off to an airbase).
                if (SDL_getenv("WW_TEST_CARRIER_RETURN")) {
                    ww::sim::EntityRef cref = w.spawn_unit("aircraft carrier", 0, bx - 150, by + 160);
                    uint32_t cid = 0;
                    if (ww::sim::Unit* cu = w.get(cref)) cid = cu->common.id;
                    ww::sim::EntityRef pl = w.spawn_unit("fighter", 0, bx + 300, by + 60);
                    if (ww::sim::Unit* p = w.get(pl)) {
                        p->fuel = 15.0;      // < 25 -> wants to land
                        p->height = 64.0;    // airborne
                        p->landed = false;
                        p->home_id = cid;    // "came from" this carrier
                    }
                }
                // Royal Marines (UK unique) near the base for WW_SHOT checks of
                // its sprite/command card (WW_TEST_LAYOUT_SELECT=marine). Three
                // different team colours in a row to verify the team recolour.
                w.spawn_unit("royal marine", 0, bx - 250, by + 120);
                w.spawn_unit("royal marine", 1, bx - 190, by + 120);
                w.spawn_unit("royal marine", 2, bx - 130, by + 120);
                // WW_TEST_SHIPSPAWN: drop a shipyard on the nearest shore, aim its
                // rally flag out into open water, blitz-build a transport + carrier,
                // and centre on it -- verifies the big hulls launch OUTWARD toward
                // the rally (not on top of the dock).
                if (SDL_getenv("WW_TEST_SHIPSPAWN")) {
                    double best = 1e18, shx = 0, shy = 0, wdirx = 0, wdiry = 1;
                    bool got = false;
                    for (int ty = 0; ty < w.rows; ++ty) {
                        for (int tx = 0; tx < w.cols; ++tx) {
                            double px = (tx + 0.5) * 32.0, py = (ty + 0.5) * 32.0;
                            if (!w.is_water(px, py)) continue;
                            bool landadj = false;
                            double wx = 0, wy = 0;
                            int wn = 0;
                            for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx) {
                                    if (!dx && !dy) continue;
                                    if (w.is_water(px + dx * 32.0, py + dy * 32.0)) { wx += dx; wy += dy; ++wn; }
                                    else landadj = true;
                                }
                            if (!landadj || wn == 0) continue;
                            double d = (px - bx) * (px - bx) + (py - by) * (py - by);
                            if (d < best) { best = d; shx = px; shy = py; wdirx = wx; wdiry = wy; got = true; }
                        }
                    }
                    if (got) {
                        auto ref = w.spawn_building("shipyard", 0, shx, shy);
                        if (ww::sim::Building* sy = w.get_building(ref)) {
                            double wl = std::hypot(wdirx, wdiry);
                            if (wl < 1e-6) { wdirx = 0; wdiry = 1; wl = 1; }
                            sy->gather_x = shx + wdirx / wl * 5.0 * 32.0; // rally flag in open water
                            sy->gather_y = shy + wdiry / wl * 5.0 * 32.0;
                            w.enqueue(ref, "transport ship");
                            w.enqueue(ref, "aircraft carrier");
                        }
                        client.match().control().teams[0].blitz = true;
                        client.camera().center_on(shx, shy);
                    }
                }
                // WW_TEST_LAYOUT_SELECT names a building to centre on + select,
                // so a WW_SHOT captures its command card.
                if (const char* sel = SDL_getenv("WW_TEST_LAYOUT_SELECT")) {
                    std::string s = sel;
                    double sx = bx, sy = by;
                    if (s == "factory") { sx = bx - 300; sy = by; }
                    else if (s == "airbase") { sx = bx + 300; sy = by; }
                    else if (s == "university") { sx = bx - 300; sy = by + 200; }
                    else if (s == "fortress") { sx = bx + 300; sy = by + 200; }
                    else if (s == "barracks") { sx = bx - 150; sy = by - 200; }
                    else if (s == "academy") { sx = bx + 150; sy = by - 200; }
                    else if (s == "shipyard") { sx = bx; sy = by - 200; }
                    else if (s == "market") { sx = bx; sy = by + 200; }
                    else if (s == "tower") { sx = bx - 450; sy = by; }
                    else if (s == "outpost") { sx = bx + 450; sy = by; }
                    else if (s == "ballistic") { sx = bx + 120; sy = by + 380; }
                    else if (s == "marine") { sx = bx - 250; sy = by + 120; }
                    client.camera().center_on(sx, sy);
                    client.test_select(sx, sy);
                }
                break;
            }
        }
    }

    // Copy immediately: SDL_getenv's returned pointer isn't guaranteed to
    // stay valid once other SDL calls touch the process environment (this
    // was actually corrupted by the time it was used further down).
    const char* shot_path_env = SDL_getenv("WW_SHOT");
    std::string shot_path_str = shot_path_env ? shot_path_env : "";
    const char* shot_path = shot_path_str.empty() ? nullptr : shot_path_str.c_str();
    int shot_after_frames = 3;
    if (const char* n = SDL_getenv("WW_SHOT_FRAMES")) shot_after_frames = std::atoi(n);
    int frame_count = 0;

    // ---- WW_SHOT_SEQ: capture a time-lapse of a whole match ----------------
    // WW_SHOT is a single frame, which is fine for verifying a sprite or a
    // command card but cannot show a match unfolding. This writes a numbered
    // BMP every WW_SHOT_SEQ_EVERY frames into an existing directory and ends
    // the run after WW_SHOT_SEQ_COUNT of them, so a spectated arena match comes
    // out as a frame sequence that assembles into a recording.
    //   WW_SHOT_SEQ=<dir>  WW_SHOT_SEQ_EVERY=<frames>  WW_SHOT_SEQ_COUNT=<n>
    const char* seq_dir_env = SDL_getenv("WW_SHOT_SEQ");
    std::string seq_dir = seq_dir_env ? seq_dir_env : ""; // copy: see shot_path above
    int seq_every = 30, seq_count = 240, seq_taken = 0;
    if (const char* n = SDL_getenv("WW_SHOT_SEQ_EVERY")) seq_every = std::max(1, std::atoi(n));
    if (const char* n = SDL_getenv("WW_SHOT_SEQ_COUNT")) seq_count = std::atoi(n);

    // ---- WW_SPEED: run the simulation faster than real time ----------------
    // A match takes ~20 real minutes to play out at the fixed 20Hz sim rate,
    // which makes watching (or recording) one impractical. Scaling the real
    // time handed to the fixed-timestep accumulator runs the same deterministic
    // steps, just more of them per frame -- the sim is unaffected, only how
    // fast it is fed. GameClient's own catch-up guard (kMaxStepsPerFrame) caps
    // the real ceiling at a few steps per frame, so very large values simply
    // saturate rather than misbehaving.
    double speed_mult = 1.0;
    if (const char* s = SDL_getenv("WW_SPEED")) speed_mult = std::max(0.1, std::atof(s));

    // A time-lapse wants a FIXED frame. WW_TEST_CAMCENTER/WW_TEST_ZOOM are
    // applied once at startup, which is enough for a single WW_SHOT but not for
    // a sequence: over a minute of capture the camera drifts (a stray wheel
    // event zooms it, and several in-game cues -- alerts, selection jumps --
    // re-centre it), and the resulting frames don't line up into anything
    // watchable. While WW_SHOT_SEQ is capturing, re-assert both every frame.
    bool lock_cam = !seq_dir.empty();
    double lock_cx = 0.0, lock_cy = 0.0, lock_zoom = 0.0;
    bool lock_centre = false;
    if (const char* cc = SDL_getenv("WW_TEST_CAMCENTER"))
        lock_centre = std::sscanf(cc, "%lf,%lf", &lock_cx, &lock_cy) == 2;
    if (const char* z = SDL_getenv("WW_TEST_ZOOM")) lock_zoom = std::atof(z);

    // Synthetic-click test hook, same spirit as WW_SHOT: WW_TEST_CLICK="x,y"
    // injects a left-button click at that screen position a few frames in,
    // so selection/input logic can be verified via a WW_SHOT capture
    // without a human at the mouse.
    const char* click_env = SDL_getenv("WW_TEST_CLICK");
    std::string click_str = click_env ? click_env : "";
    int click_x = -1, click_y = -1;
    if (!click_str.empty()) {
        sscanf(click_str.c_str(), "%d,%d", &click_x, &click_y);
    }
    const char* click2_env = SDL_getenv("WW_TEST_CLICK2");
    std::string click2_str = click2_env ? click2_env : "";
    int click2_x = -1, click2_y = -1;
    if (!click2_str.empty()) {
        sscanf(click2_str.c_str(), "%d,%d", &click2_x, &click2_y);
    }
    const char* click3_env = SDL_getenv("WW_TEST_CLICK3");
    std::string click3_str = click3_env ? click3_env : "";
    int click3_x = -1, click3_y = -1;
    if (!click3_str.empty()) {
        sscanf(click3_str.c_str(), "%d,%d", &click3_x, &click3_y);
    }
    const char* rclick_env = SDL_getenv("WW_TEST_RCLICK");
    std::string rclick_str = rclick_env ? rclick_env : "";
    int rclick_x = -1, rclick_y = -1;
    if (!rclick_str.empty()) {
        sscanf(rclick_str.c_str(), "%d,%d", &rclick_x, &rclick_y);
    }
    // A second right-click, fired a few frames after WW_TEST_RCLICK -- lets
    // a test build up a shift-queue (WW_TEST_SHIFTHOLD covering both
    // frames) instead of only ever exercising a single right-click.
    const char* rclick2_env = SDL_getenv("WW_TEST_RCLICK2");
    std::string rclick2_str = rclick2_env ? rclick2_env : "";
    int rclick2_x = -1, rclick2_y = -1;
    if (!rclick2_str.empty()) {
        sscanf(rclick2_str.c_str(), "%d,%d", &rclick2_x, &rclick2_y);
    }

    bool running = true;
    Uint64 last = SDL_GetPerformanceCounter();
    const double pan_speed = 400.0; // px/sec at zoom 1

    // FPS tracking: rolling 1-second window, shown in the window title
    // (cheap and immediate -- no in-canvas font renderer yet, see the
    // SDL_ttf task) and logged to the console.
    int fps_frame_count = 0;
    double fps_accum = 0.0;
    char title_buf[128];

    // "Start Stress Test" (Graphics screen) is a quick, self-ending preview
    // rather than a real match: auto-returns to the menu after 30 seconds,
    // OR Enter/Escape, OR the pause menu's real "Quit Game" -- instead of
    // running until the player manually quits like a normal match would
    // (plain clicks/other keys do nothing here; see GameClient::set_
    // spectator, which is what actually lets the player select/box-select/
    // pan/zoom freely without ending the preview). stress_test_ended_
    // naturally (as opposed to Alt+F4/SDL_QUIT, which should still fully
    // quit the app even during a preview -- see the app_should_exit check
    // below the loop) distinguishes those two so only an actual timeout/
    // Enter/Escape ends up looping back to the menu.
    double stress_test_elapsed = 0.0;
    bool stress_test_ended_naturally = false;

    ww_open_perf_log(); // frame-time watchdog + crash log (once per process)
    while (running) {
        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (now - last) / static_cast<double>(SDL_GetPerformanceFrequency());
        last = now;

        if (stress_test_requested && !stress_test_ended_naturally) {
            stress_test_elapsed += dt;
            if (stress_test_elapsed >= 30.0) {
                running = false;
                stress_test_ended_naturally = true;
            }
        }

        fps_frame_count++;
        fps_accum += dt;
        if (fps_accum >= 1.0) {
            double fps = fps_frame_count / fps_accum;
            std::snprintf(title_buf, sizeof(title_buf), "World War (C++ preview) - %.1f FPS", fps);
            SDL_SetWindowTitle(window, title_buf);
            SDL_Log("FPS: %.1f (%d frames in %.2fs)", fps, fps_frame_count, fps_accum);
            client.set_fps(fps);
            fps_frame_count = 0;
            fps_accum = 0.0;
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // Stress test preview ends on Enter or Escape, on top of the
            // 10-second timeout above and the pause menu's real "Quit Game"
            // (client.wants_quit_to_menu(), checked same as any match --
            // GameClient::handle_left_down's pause-button check is NOT
            // gated on spectator_, so that still opens normally). Plain
            // clicks/other keys do NOT end it anymore -- the player can
            // freely select/box-select and inspect units as a pure
            // spectator (GameClient::set_spectator blocks every order-
            // issuing action but leaves selection and camera control
            // alone) without accidentally ending the preview. Checked
            // independently of the dispatch chain below so it doesn't
            // change how the same keypress is otherwise handled (Escape
            // still also reaches GameClient normally -- harmless, see
            // set_spectator's comment). Doesn't apply to real matches
            // (stress_test_requested is only ever true for a "Start Stress
            // Test" session).
            if (stress_test_requested && !stress_test_ended_naturally && ev.type == SDL_KEYDOWN &&
                (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER ||
                 ev.key.keysym.sym == SDLK_ESCAPE)) {
                running = false;
                stress_test_ended_naturally = true;
            }
            if (ev.type == SDL_QUIT) running = false;
            else if (ev.type == SDL_WINDOWEVENT &&
                     (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                      ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
                // RESIZED only fires for externally-triggered resizes (user
                // dragging the edge); programmatic changes -- SetWindowSize,
                // SetWindowFullscreen (F11/Alt+Enter) -- only fire
                // SIZE_CHANGED, so both must be handled here. The virtual
                // canvas itself never resizes -- only the letterbox scale/
                // offset used to blit it into the real window.
                recompute_letterbox();
            } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F4 &&
                       (ev.key.keysym.mod & KMOD_ALT)) {
                running = false; // Alt+F4 quits (Escape is now an AoE-style
                                  // cancel/deselect key, handled by GameClient)
            } else if (ev.type == SDL_KEYDOWN &&
                       (ev.key.keysym.sym == SDLK_F11 ||
                        (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)))) {
                bool is_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                SDL_SetWindowFullscreen(window, is_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                recompute_letterbox();
            } else {
                // Real OS-originated mouse events arrive in actual window
                // pixel coordinates; translate to the fixed virtual canvas
                // before handing off to GameClient. Synthetic WW_TEST_CLICK*
                // events below are injected directly (bypassing this loop)
                // and already target virtual coordinates, so they're
                // unaffected by window size/letterboxing.
                SDL_Event tev = ev;
                if (tev.type == SDL_MOUSEBUTTONDOWN || tev.type == SDL_MOUSEBUTTONUP) {
                    to_virtual(tev.button.x, tev.button.y);
                } else if (tev.type == SDL_MOUSEMOTION) {
                    to_virtual(tev.motion.x, tev.motion.y);
                }
                client.handle_event(tev);
            }
        }

        // Camera pan keys are player-configurable (Options > Hotkeys),
        // default arrow keys -- looked up by scancode every frame a key is
        // held, converted from the settings' keycodes since this is a
        // continuous SDL_GetKeyboardState poll, not a discrete SDL_KEYDOWN
        // event (see GameClient::handle_hotkey for the event-driven ones).
        // Gated on the chat bar being closed so typing a message doesn't
        // pan the map.
        const Settings& s = client.settings();
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        bool ctrl_down = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
        // A pan key can be rebound to a Ctrl combo like anything else --
        // requires an EXACT match (Ctrl held iff the binding wants it) so a
        // Ctrl-qualified binding elsewhere doesn't also fire this one.
        auto pan_held = [&](const Hotkey& hk) {
            return keys[SDL_GetScancodeFromKey(hk.key)] && ctrl_down == hk.ctrl;
        };
        double dx = 0, dy = 0;
        if (!client.chat_open()) {
            if (pan_held(s.pan_left_key)) dx -= pan_speed * dt;
            if (pan_held(s.pan_right_key)) dx += pan_speed * dt;
            if (pan_held(s.pan_up_key)) dy -= pan_speed * dt;
            if (pan_held(s.pan_down_key)) dy += pan_speed * dt;
        }
        if (dx != 0 || dy != 0) client.camera().pan(dx, dy);

        auto synth_click = [&](int x, int y) {
            SDL_Event down{}, up{};
            down.type = SDL_MOUSEBUTTONDOWN;
            down.button.button = SDL_BUTTON_LEFT;
            down.button.x = x; down.button.y = y;
            up.type = SDL_MOUSEBUTTONUP;
            up.button.button = SDL_BUTTON_LEFT;
            up.button.x = x; up.button.y = y;
            client.handle_event(down);
            client.handle_event(up);
        };
        // WW_TEST_CHAT="<line>" -- type a line into the in-game chat bar and
        // send it, at WW_TEST_CHAT_FRAME (default 30). The bar is how every
        // cheat/dev command is entered (see GameClient::submit_chat), and until
        // now nothing could drive it headlessly: the menu had synthetic key and
        // text hooks but the game loop only had clicks. Same shape as those --
        // Enter to open, the text, Enter to submit -- all through the client's
        // normal handle_event path, so it exercises exactly what a player does.
        auto synth_chat = [&](const char* line) {
            SDL_Event ret{};
            ret.type = SDL_KEYDOWN;
            ret.key.keysym.sym = SDLK_RETURN;
            client.handle_event(ret); // open the bar
            SDL_Event txt{};
            txt.type = SDL_TEXTINPUT;
            std::snprintf(txt.text.text, sizeof(txt.text.text), "%s", line);
            client.handle_event(txt);
            client.handle_event(ret); // send
        };
        // NOTE the std::string copies. SDL_getenv hands back a pointer into an
        // internal buffer that the NEXT SDL_getenv call may overwrite, so
        // holding `chat` across the lookup of its frame variable made the line
        // that got typed be the frame NUMBER. Copy each value before asking for
        // the next one.
        {
            const char* c1 = SDL_getenv("WW_TEST_CHAT");
            std::string chat = c1 ? c1 : "";
            if (!chat.empty()) {
                const char* f1 = SDL_getenv("WW_TEST_CHAT_FRAME");
                int chat_frame = f1 ? std::atoi(f1) : 30;
                if (frame_count == chat_frame) synth_chat(chat.c_str());
            }
        }
        // A second line, for anything that has to be tested as a round trip --
        // "pov 1" then "pov" back, say.
        {
            const char* c2 = SDL_getenv("WW_TEST_CHAT2");
            std::string chat2 = c2 ? c2 : "";
            if (!chat2.empty()) {
                const char* f2 = SDL_getenv("WW_TEST_CHAT_FRAME2");
                int chat_frame2 = f2 ? std::atoi(f2) : 60;
                if (frame_count == chat_frame2) synth_chat(chat2.c_str());
            }
        }
        if (const char* rs = SDL_getenv("WW_TEST_RESIZE")) {
            if (frame_count == 3) {
                int rw = 1280, rh = 800;
                sscanf(rs, "%d,%d", &rw, &rh);
                SDL_SetWindowSize(window, rw, rh);
            }
        }
        if (const char* md = SDL_getenv("WW_TEST_MOUSEDOWN")) {
            if (frame_count == 3) {
                int mx = -1, my = -1;
                sscanf(md, "%d,%d", &mx, &my);
                if (mx >= 0) {
                    SDL_Event dn{};
                    dn.type = SDL_MOUSEBUTTONDOWN;
                    dn.button.button = SDL_BUTTON_LEFT;
                    dn.button.x = mx; dn.button.y = my;
                    client.handle_event(dn);
                }
            }
        }
        if (const char* mdm = SDL_getenv("WW_TEST_MIDDOWN")) {
            if (frame_count == 3) {
                int mx = -1, my = -1;
                sscanf(mdm, "%d,%d", &mx, &my);
                if (mx >= 0) {
                    SDL_Event dn{};
                    dn.type = SDL_MOUSEBUTTONDOWN;
                    dn.button.button = SDL_BUTTON_MIDDLE;
                    dn.button.x = mx; dn.button.y = my;
                    client.handle_event(dn);
                }
            }
        }
        if (const char* mp = SDL_getenv("WW_TEST_MOUSEPOS")) {
            if (frame_count == 6) {
                int mx = -1, my = -1;
                sscanf(mp, "%d,%d", &mx, &my);
                if (mx >= 0) {
                    SDL_Event mv{};
                    mv.type = SDL_MOUSEMOTION;
                    mv.motion.x = mx; mv.motion.y = my;
                    client.handle_event(mv);
                }
            }
        }
        if (const char* mu = SDL_getenv("WW_TEST_MOUSEUP")) {
            if (frame_count == 8) {
                int mx = -1, my = -1;
                sscanf(mu, "%d,%d", &mx, &my);
                if (mx >= 0) {
                    SDL_Event up{};
                    up.type = SDL_MOUSEBUTTONUP;
                    up.button.button = SDL_BUTTON_LEFT;
                    up.button.x = mx; up.button.y = my;
                    client.handle_event(up);
                }
            }
        }
        // Debug/verification-only: exercise the right-drag formation. Box-
        // selects a wide region (grabbing the visible own units), then scripts
        // a right press-drag-release A->B. WW_TEST_FORMATION="ax,ay,bx,by".
        // Frame 12 has the ghost preview live; ranks settle over the next
        // ~150 frames (so shoot at 12 for the preview, later for the result).
        if (const char* ff = SDL_getenv("WW_TEST_FORMATION")) {
            int ax = 500, ay = 300, bx = 780, by = 300;
            sscanf(ff, "%d,%d,%d,%d", &ax, &ay, &bx, &by);
            auto motion = [&](int x, int y) {
                SDL_Event e{}; e.type = SDL_MOUSEMOTION; e.motion.x = x; e.motion.y = y;
                client.handle_event(e);
            };
            auto btn = [&](Uint32 t, Uint8 b, int x, int y) {
                SDL_Event e{}; e.type = t; e.button.button = b; e.button.x = x; e.button.y = y;
                client.handle_event(e);
            };
            if (frame_count == 3) btn(SDL_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 40, 40);
            if (frame_count == 4) motion(600, 400);
            if (frame_count == 5) btn(SDL_MOUSEBUTTONUP, SDL_BUTTON_LEFT, 600, 400);
            if (frame_count == 8) btn(SDL_MOUSEBUTTONDOWN, SDL_BUTTON_RIGHT, ax, ay);
            if (frame_count == 10) motion((ax + bx) / 2, (ay + by) / 2);
            if (frame_count == 12) motion(bx, by); // ghost preview live this frame
            if (frame_count == 15) btn(SDL_MOUSEBUTTONUP, SDL_BUTTON_RIGHT, bx, by);
        }
        if (click_x >= 0 && frame_count == 5) synth_click(click_x, click_y);
        if (click2_x >= 0 && frame_count == 10) synth_click(click2_x, click2_y);
        if (click3_x >= 0 && frame_count == 15) synth_click(click3_x, click3_y);
        if (const char* hv = SDL_getenv("WW_TEST_HOVER")) {
            if (frame_count == 18) {
                int mx = -1, my = -1;
                sscanf(hv, "%d,%d", &mx, &my);
                if (mx >= 0) {
                    SDL_Event mv{};
                    mv.type = SDL_MOUSEMOTION;
                    mv.motion.x = mx; mv.motion.y = my;
                    client.handle_event(mv);
                }
            }
        }
        if (const char* c4 = SDL_getenv("WW_TEST_CLICK4")) {
            if (frame_count == 21) {
                int mx = -1, my = -1;
                sscanf(c4, "%d,%d", &mx, &my);
                if (mx >= 0) synth_click(mx, my);
            }
        }
        if (const char* dc = SDL_getenv("WW_TEST_DBLCLICK")) {
            if (frame_count == 21) {
                int mx = -1, my = -1;
                sscanf(dc, "%d,%d", &mx, &my);
                if (mx >= 0) {
                    SDL_Event down{}, up{};
                    down.type = SDL_MOUSEBUTTONDOWN;
                    down.button.button = SDL_BUTTON_LEFT;
                    down.button.clicks = 2;
                    down.button.x = mx; down.button.y = my;
                    up.type = SDL_MOUSEBUTTONUP;
                    up.button.button = SDL_BUTTON_LEFT;
                    up.button.clicks = 2;
                    up.button.x = mx; up.button.y = my;
                    client.handle_event(down);
                    client.handle_event(up);
                }
            }
        }
        if (rclick_x >= 0 && frame_count == 10) {
            SDL_Event rdown{};
            rdown.type = SDL_MOUSEBUTTONDOWN;
            rdown.button.button = SDL_BUTTON_RIGHT;
            rdown.button.x = rclick_x; rdown.button.y = rclick_y;
            client.handle_event(rdown);
        }
        if (rclick2_x >= 0 && frame_count == 12) {
            SDL_Event rdown{};
            rdown.type = SDL_MOUSEBUTTONDOWN;
            rdown.button.button = SDL_BUTTON_RIGHT;
            rdown.button.x = rclick2_x; rdown.button.y = rclick2_y;
            client.handle_event(rdown);
        }
        if (SDL_getenv("WW_TEST_ESCAPE") && frame_count == 15) {
            SDL_Event esc{};
            esc.type = SDL_KEYDOWN;
            esc.key.keysym.sym = SDLK_ESCAPE;
            client.handle_event(esc);
        }
        if (SDL_getenv("WW_TEST_F3") && frame_count == 6) {
            // Toggles the debug perf overlay -- a separate hook from
            // WW_TEST_KEY since that one only handles single-character keys
            // (SDL_GetKeyFromName(one char)), not function keys.
            SDL_Event f3{};
            f3.type = SDL_KEYDOWN;
            f3.key.keysym.sym = SDLK_F3;
            client.handle_event(f3);
        }
        if (const char* ct = SDL_getenv("WW_TEST_CHAT")) {
            // Verification-only: Enter (opens chat) -> a synthetic
            // SDL_TEXTINPUT carrying the whole string at once (real typing
            // sends one event per keystroke, but handle_event just appends
            // ev.text.text either way) -> Enter again (submits, either as
            // a chat line or a recognized cheat command).
            SDL_Event enter1{}, text{}, enter2{};
            if (frame_count == 6) {
                enter1.type = SDL_KEYDOWN;
                enter1.key.keysym.sym = SDLK_RETURN;
                client.handle_event(enter1);
            } else if (frame_count == 8) {
                text.type = SDL_TEXTINPUT;
                std::snprintf(text.text.text, sizeof(text.text.text), "%s", ct);
                client.handle_event(text);
            } else if (frame_count == 10) {
                enter2.type = SDL_KEYDOWN;
                enter2.key.keysym.sym = SDLK_RETURN;
                client.handle_event(enter2);
            }
        }
        if (const char* k1 = SDL_getenv("WW_TEST_KEY")) {
            if (frame_count == 6 && k1[0]) {
                SDL_Event kev{};
                kev.type = SDL_KEYDOWN;
                kev.key.keysym.sym = SDL_GetKeyFromName(std::string(1, k1[0]).c_str());
                client.handle_event(kev);
            }
        }
        if (const char* k2 = SDL_getenv("WW_TEST_KEY2")) {
            if (frame_count == 7 && k2[0]) {
                SDL_Event kev{};
                kev.type = SDL_KEYDOWN;
                kev.key.keysym.sym = SDL_GetKeyFromName(std::string(1, k2[0]).c_str());
                client.handle_event(kev);
            }
        }
        if (const char* k3 = SDL_getenv("WW_TEST_KEY3")) {
            if (frame_count == 11 && k3[0]) {
                SDL_Event kev{};
                kev.type = SDL_KEYDOWN;
                kev.key.keysym.sym = SDL_GetKeyFromName(std::string(1, k3[0]).c_str());
                client.handle_event(kev);
            }
        }
        if (const char* k4 = SDL_getenv("WW_TEST_KEY4")) {
            if (frame_count == 12 && k4[0]) {
                SDL_Event kev{};
                kev.type = SDL_KEYDOWN;
                kev.key.keysym.sym = SDL_GetKeyFromName(std::string(1, k4[0]).c_str());
                client.handle_event(kev);
            }
        }
        if (SDL_getenv("WW_TEST_SHIFTHOLD")) {
            // Holds shift for frames [9, 20) by default (WW_TEST_SHIFT_START/
            // WW_TEST_SHIFT_END override the window, e.g. to leave an EARLIER
            // click plain while a LATER one still lands as a shift-click) so
            // CLICK2/CLICK3 (fired at frames 10/15) land as shift-clicks and
            // CLICK4 (frame 21) lands as a plain click -- SDL_GetModState()
            // reflects whatever SDL_SetModState() last set, same as it would
            // for a real held key, so GameClient's `SDL_GetModState() &
            // KMOD_SHIFT` check sees it exactly as it would from an actual
            // player holding shift.
            int shift_start = 9, shift_end = 20;
            if (const char* ss = SDL_getenv("WW_TEST_SHIFT_START")) shift_start = std::atoi(ss);
            if (const char* se = SDL_getenv("WW_TEST_SHIFT_END")) shift_end = std::atoi(se);
            if (frame_count == shift_start) SDL_SetModState(KMOD_LSHIFT);
            if (frame_count == shift_end) SDL_SetModState(KMOD_NONE);
        }

        double _pf = static_cast<double>(SDL_GetPerformanceFrequency());
        Uint64 _t0 = SDL_GetPerformanceCounter();
        client.update(dt * speed_mult); // WW_SPEED (1.0 unless set)
        if (lock_cam) { // see lock_cam's declaration
            if (lock_zoom > 0.0) client.camera().zoom = lock_zoom;
            if (lock_centre) client.camera().center_on(lock_cx, lock_cy);
        }
        if (client.wants_quit_to_menu()) running = false; // pause menu's Quit Game -- see below the loop
        Uint64 _t1 = SDL_GetPerformanceCounter();
        SDL_SetRenderTarget(renderer, render_target);
        client.draw(renderer);
        SDL_SetRenderTarget(renderer, nullptr);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer); // letterbox/pillarbox bars
        SDL_RenderCopy(renderer, render_target, nullptr, &letterbox_dst);
        Uint64 _t2 = SDL_GetPerformanceCounter();
        SDL_RenderPresent(renderer);
        Uint64 _t3 = SDL_GetPerformanceCounter();

        // Frame-time watchdog: keep the last frame's stats current (for the
        // crash handler) and log any frame that ran slower than 30 FPS. The
        // present phase includes the vsync wait, so a healthy vsync-capped frame
        // (~14ms) never trips the >33ms threshold -- only a genuine stall does.
        {
            double upd = (_t1 - _t0) * 1000.0 / _pf, drw = (_t2 - _t1) * 1000.0 / _pf,
                   pres = (_t3 - _t2) * 1000.0 / _pf, tot = (_t3 - _t0) * 1000.0 / _pf;
            auto& w = client.match().world();
            // Text-cache columns: `txt=<live>` cached glyph textures, `+N` made
            // and `-N` destroyed THIS frame, `evict=<ms>` spent inside
            // evict_old. A stall that is really a bulk-SDL_DestroyTexture GPU
            // sync shows up as a large `-N` and a large evict figure on exactly
            // the slow frames; if those columns are quiet while pres is high,
            // the cost is somewhere else and this suspect is cleared.
            TextCacheStats tc = ww_text_cache_stats();
            char info[420];
            std::snprintf(info, sizeof(info),
                          "f%lld tot=%.1fms (upd=%.1f draw=%.1f pres=%.1f) | units=%zu bld=%zu "
                          "proj=%zu res=%zu fx=%zu smoke=%zu | txt=%zu +%llu -%llu evict=%.1fms",
                          g_frame_index, tot, upd, drw, pres, w.active_units.size(),
                          w.active_buildings.size(), w.active_projectiles.size(),
                          w.active_resources.size(), client.fx_effect_count(), client.fx_smoke_count(),
                          tc.live, static_cast<unsigned long long>(tc.created),
                          static_cast<unsigned long long>(tc.evicted), tc.evict_ms);
            g_last_frame_info = info;
            ww_text_cache_reset_frame(); // per-frame totals; `live` is unaffected
            ++g_frame_index;
            if (tot > 33.3 && g_perf_log.is_open()) {
                static int logged = 0;
                if (logged < 5000) {
                    g_perf_log << info << (tot > 250.0 ? "   <<< SEVERE STALL\n" : "\n");
                    g_perf_log.flush();
                    if (tot > 250.0) SDL_Log("SEVERE frame stall: %s", info);
                    ++logged;
                } else if (logged == 5000) {
                    g_perf_log << "(further slow-frame lines suppressed after 5000)\n";
                    g_perf_log.flush();
                    ++logged;
                }
            }
        }

        // frame_count must advance every iteration regardless of whether a
        // screenshot was requested -- it used to only increment inside this
        // condition (via `shot_path && ++frame_count`), so every
        // WW_TEST_CLICK*/WW_TEST_KEY*/etc frame-number check above silently
        // never fired unless WW_SHOT was ALSO set (latent bug, masked all
        // session because every synthetic-input test happened to pair the
        // two; found while adding the equivalent menu-loop counter).
        ++frame_count;
        if (!seq_dir.empty() && frame_count % seq_every == 0 && seq_taken < seq_count) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/frame_%04d.bmp", seq_dir.c_str(), seq_taken);
            int w, h;
            SDL_GetRendererOutputSize(renderer, &w, &h);
            SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
            SDL_SaveBMP(surf, path);
            SDL_FreeSurface(surf);
            if (++seq_taken >= seq_count) running = false;
        }
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
    // Either the pause menu's real "Quit Game" or a stress test preview
    // ending on its own (timeout/Enter/Escape, NOT Alt+F4/SDL_QUIT -- those
    // still fully quit the app even mid-preview, see stress_test_ended_
    // naturally's comment above) loops back to the menu instead of exiting.
    if (!skip_menu && (client.wants_quit_to_menu() || stress_test_ended_naturally)) {
        app_should_exit = false;
        // Whichever of the two ways above ended THIS session -- if it was
        // a stress test (stress_test_requested), the next menu loop should
        // land back on Options > Graphics instead of the title screen (see
        // return_to_graphics_options's declaration and where it's
        // consumed). A real match's pause-menu Quit Game leaves this false,
        // so that path is completely unaffected.
        return_to_graphics_options = stress_test_requested;
    }
    }
    } // while (!app_should_exit)

    SDL_DestroyTexture(render_target);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 0;
}

