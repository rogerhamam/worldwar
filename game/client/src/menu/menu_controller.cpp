#include "menu_controller.h"

#include "audio/audio.h"
#include "civ_data.h"
#include "item_hotkeys.h"
#include "net/session.h" // the multiplayer lobby drives this directly
#include "net/socket.h"  // local_addresses(), for the host's status panel
#include "release_notes_data.h"
#include "tech_tree_data.h"
#include "../hud/item_tooltips.h"

#include <SDL_image.h>

#include <algorithm>
#include <cctype>
#include <chrono> // zero-timeout poll of the lobby's UPnP future
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>

using ww::sim::SkirmishSettings;

namespace {
const char* kMapSizeNames[4] = {"Tiny", "Normal", "Large", "Huge"};
// Bumped up so the "Normal" map is what "Huge" used to be (64) -- small maps
// were forcing hyper-aggressive play. Every tier scaled up correspondingly.
const int kMapSizeValues[4] = {48, 64, 80, 96};
// Map themes -- display name + the World map_type key its generators key off,
// a one-line description, tree-density blurb, a Europe-map marker (fraction of
// the spr_europe_map rect), and a terrain-swatch colour used as the thumbnail
// until a real photo (bg_map_<key>) is dropped in.
struct MapInfo {
    const char* name;
    const char* value;
    const char* desc;
    int tree_density;
    double eu_x, eu_y;   // 0..1 marker position within this map's REGION map
    SDL_Color swatch;
    const char* region;  // which real-world map to show: "europe" / "pacific" / "africa" ("" = none)
};
const MapInfo kMaps[9] = {
    {"Random", "random", "A freshly generated battlefield -- terrain, forests and resources vary every match.", 5, 0.50, 0.45, {90, 120, 70, 255}, ""},
    {"Ostland", "ostland", "A lush Baltic frontier of green plains, birch forests and quiet lakes.", 6, 0.62, 0.30, {70, 140, 60, 255}, "europe"},
    {"Negev Desert", "negev desert", "Endless pale dunes and rocky outcrops broken only by rare, life-giving oases.", 1, 0.64, 0.13, {225, 205, 150, 255}, "africa"},
    {"Guam", "guam", "A Pacific island of golden beaches and palm groves, ringed entirely by ocean.", 4, 0.52, 0.36, {90, 175, 150, 255}, "pacific"},
    {"Stalingrad", "stalingrad", "A devastated urban sprawl of bombed-out factories, shattered blocks and frozen riverbanks.", 4, 0.78, 0.42, {200, 205, 210, 255}, "europe"},
    {"Ardennes", "ardennes", "Dense evergreen forest smothered in snow -- perfect ambush country.", 9, 0.45, 0.38, {210, 225, 210, 255}, "europe"},
    {"Normandy Beach", "normandy", "Open landing beaches backed by hedgerow bocage and windblown dune grass.", 3, 0.34, 0.36, {215, 200, 155, 255}, "europe"},
    {"Santa Cruz Islands", "santa cruz islands", "Two great island homelands split by open sea, with a scatter of contested isles of oil, iron and timber to seize between them.", 4, 0.62, 0.58, {70, 150, 160, 255}, "pacific"},
    {"Pacific Islands", "pacific islands", "Endless blue ocean, a home island for each commander, and a lone neutral atoll to fight over in the middle.", 3, 0.50, 0.45, {60, 140, 168, 255}, "pacific"},
};
constexpr int kNumMaps = 9;
const int kMaxPopValues[4] = {50, 100, 150, 200};
const char* kRevealNames[3] = {"Standard", "No fog", "Revealed"};
// 3 = "Hardest": Hard's decision cadence PLUS open handicaps (units train
// 50% faster, every tech of the current era granted free on arrival). See
// Team::difficulty -- 0-2 are skill, 3 is skill plus a head start.
const char* kDifficultyNames[4] = {"Easy", "Normal", "Hard", "Hardest"};
const char* kModeNames[2] = {"Standard", "Deathmatch"};

// Tech tree grid content spans x=[160, 160+64*(max_col+1)); the viewport
// showing it is x=[160,640) (640-160=480px wide) -- clamp scroll so the
// grid's right edge never scrolls past the viewport's right edge, matching
// the original's clamped scroll (there hardcoded to 2100 for its own fixed
// content width; computed here instead so it stays correct if the grid
// table above is ever edited).
double tech_tree_max_scroll(const std::string& data_dir) {
    int max_col = 0;
    for (auto& e : ww::menu::tech_tree_grid(data_dir)) max_col = std::max(max_col, e.col);
    double content_w = 64.0 * (max_col + 1);
    return std::max(0.0, content_w - (640.0 - 160.0));
}

// "fishing boat" -> "Fishing Boat" -- a compact display label for the Units
// & Research listing (draw_hotkeys_options). Deliberately not item_
// tooltips()'s title (e.g. "Train Fishing Boat"/"Research Hydrodynamics"):
// that's meant for a standalone tooltip box, and its verb prefixes push
// some names too wide for this screen's narrow two-column key_row layout.
std::string title_case(const std::string& s) {
    std::string out = s;
    bool start = true;
    for (char& c : out) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            start = true;
        } else {
            if (start) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            start = false;
        }
    }
    return out;
}

// Layout constants shared between draw_hotkeys_options (which uses them to
// advance its scroll cursor) and hotkeys_max_scroll (which uses the exact
// same numbers to compute how far there is to scroll) -- kept as one set of
// named constants specifically so the two can't drift apart.
constexpr int kHotkeysOtherSectionH = 18 + 3 * 28 + 14;       // header + 2x3 rows + gap
constexpr int kHotkeysBuildingsSectionH = 18 + 5 * 28 + 14;   // header + 2x5 rows + gap
constexpr int kHotkeysCameraSectionH = 18 + 3 * 28 + 14;      // header + 2x3 rows + gap
constexpr int kHotkeysUnitsHeaderH = 18;                      // generic section/sub-header title height
constexpr int kHotkeysGroupHeaderH = 18;                      // per-building sub-header
constexpr int kHotkeysGroupRowH = 28;
constexpr int kHotkeysGroupGap = 10;
constexpr int kHotkeysContentTopOffset = 50; // kPanel_.y + this = where scrolling content starts
constexpr int kHotkeysContentBottomMargin = 4;

// `construction_count` is settings_.construction_keys.size() -- passed in
// rather than hardcoded so this can't silently drift from Settings' own
// default_construction_keys() table. `panel_h` is the caller's kPanel_.h --
// a free function (not a MenuController member) can't reach that instance
// member itself, so it's passed in like construction_count.
double hotkeys_max_scroll(size_t construction_count, int panel_h) {
    int h = kHotkeysOtherSectionH + kHotkeysBuildingsSectionH + kHotkeysCameraSectionH;
    // Villager Commands: Build Eco/Build Mil/Next Page (3 dedicated keys)
    // plus one row per building's construction key.
    int villager_items = 3 + static_cast<int>(construction_count);
    int villager_rows = (villager_items + 1) / 2;
    h += kHotkeysUnitsHeaderH + villager_rows * kHotkeysGroupRowH + kHotkeysGroupGap;
    h += kHotkeysUnitsHeaderH; // "Units & Research" section title
    for (auto& g : ww::hotkeys::building_groups()) {
        int rows = (static_cast<int>(g.items.size()) + 1) / 2;
        if (g.building == "shipyard") ++rows;      // extra "Next Page" row (shipyard_page_key)
        else if (g.building == "airbase") ++rows;  // extra "Build Nuke" row (build_nuke_key)
        h += kHotkeysGroupHeaderH + rows * kHotkeysGroupRowH + kHotkeysGroupGap;
    }
    int visible = panel_h - kHotkeysContentTopOffset - kHotkeysContentBottomMargin;
    return std::max(0, h - visible);
}

const ww::menu::TechTreeEntry* tech_tree_find(const std::string& data_dir, int col, int row) {
    for (auto& e : ww::menu::tech_tree_grid(data_dir)) {
        if (e.col == col && e.row == row) return &e;
    }
    return nullptr;
}

// Splits `text` into as many lines as needed so each one fits within max_w
// at a FIXED size, breaking on word boundaries -- used instead of shrinking
// the font per-string, which made sibling bonus bullets (or the civ name
// across different civs) render at visibly different, inconsistent sizes
// depending on how long each one happened to be.
std::vector<std::string> wrap_text(TextRenderer& tr, const std::string& text, int size, int max_w) {
    std::vector<std::string> lines;
    std::string current;
    size_t pos = 0;
    while (pos <= text.size()) {
        // Breaks on '\n' as well as ' ' -- civ bonus bullets never contain
        // one, but a campaign level's multi-line description (see the
        // campaign editor's paragraph field) does, and its authored
        // paragraph breaks need to survive here, not just get swallowed
        // into a run-on line.
        size_t brk = text.find_first_of(" \n", pos);
        std::string word = text.substr(pos, brk == std::string::npos ? std::string::npos : brk - pos);
        std::string trial = current.empty() ? word : current + " " + word;
        int tw, th;
        tr.measure(trial, size, tw, th);
        if (tw > max_w && !current.empty()) {
            lines.push_back(current);
            current = word;
        } else {
            current = trial;
        }
        if (brk == std::string::npos) break;
        if (text[brk] == '\n') {
            lines.push_back(current);
            current.clear();
        }
        pos = brk + 1;
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

// Largest size (from a small candidate range) at which EVERY string in
// `items` fits within max_w on one line -- used for the civ name so all 9
// names render at one consistent size instead of each auto-shrinking
// independently.
int fixed_fit_size(TextRenderer& tr, const std::vector<std::string>& items, int max_w, int max_size,
                   int min_size) {
    for (int size = max_size; size > min_size; --size) {
        bool fits = true;
        for (auto& s : items) {
            int tw, th;
            tr.measure(s, size, tw, th);
            if (tw > max_w) { fits = false; break; }
        }
        if (fits) return size;
    }
    return min_size;
}
} // namespace

MenuController::MenuController(SDL_Renderer* renderer, const std::string& asset_dir,
                                const std::string& data_dir, int view_w, int view_h)
    : atlas_(renderer, asset_dir),
      text_(renderer, "C:\\Windows\\Fonts\\serife.fon", "C:\\Windows\\Fonts\\segoeui.ttf", 0, true),
      text_small_(renderer, "C:\\Windows\\Fonts\\serife.fon", "C:\\Windows\\Fonts\\segoeui.ttf", 0, false),
      data_dir_(data_dir), asset_dir_(asset_dir), data_(data_dir), view_w_(view_w), view_h_(view_h),
      // Same 32px-inset/native-corner formulas the old file-scope 640x480
      // constants used, just parameterized on the actual view size (see
      // kPanel_'s comment in the header).
      kPanel_{32, 32, view_w - 64, view_h - 64}, kBackRect_{0, 0, 32, 32},
      kQuitRect_{view_w - 16, 0, 16, 16} {
    SDL_ShowCursor(SDL_DISABLE);
    std::srand(static_cast<unsigned>(SDL_GetPerformanceCounter()));
    for (int i = 0; i < 8; ++i) {
        teams_[i].civ = -1;
        teams_[i].colour = i;
        // Defaults every row to its own alliance slot (1-4, wrapping) so
        // 2-4 players start free-for-all exactly as before the team system
        // existed; with 5-8 players, rows wrap and pair up by default
        // (row 5 starts on the same team as row 1, etc.) -- freely
        // reassignable via the "Team" cycle button either way.
        teams_[i].ally = (i % 4) + 1;
    }
    teams_[0].civ = 0; // UK, matches the sim's existing default_settings()
    teams_[1].civ = 2; // Nazi Germany

    // civ_exclude.json: {"0": [...], "1": [...], ...} -> int-keyed sets,
    // same parse as Control's constructor (sim/src/control.cpp) -- not
    // reused directly since building a full Control here would also need
    // an unrelated Bonuses/team-array just to reach this one lookup table.
    for (auto& [k, v] : data_.civ_exclude().items()) {
        std::set<std::string> excl;
        for (auto& item : v) excl.insert(item.get<std::string>());
        civ_exclude_[std::stoi(k)] = std::move(excl);
    }

    settings_.load();
}

MenuController::~MenuController() {
    if (briefing_tex_) SDL_DestroyTexture(briefing_tex_);
}

void MenuController::resize(int view_w, int view_h) {
    view_w_ = view_w;
    view_h_ = view_h;
    kPanel_ = {32, 32, view_w - 64, view_h - 64};
    kBackRect_ = {0, 0, 32, 32};
    kQuitRect_ = {view_w - 16, 0, 16, 16};
}

void MenuController::handle_event(const SDL_Event& ev) {
    // ---- lobby text entry, ahead of everything else ------------------------
    // The address/port fields are the menu's only typed input, so the handling
    // is local to them rather than a general focus system: while a field has the
    // caret it consumes text, backspace, Tab and Enter, and nothing else in this
    // function sees them. Escape is deliberately NOT consumed -- it still means
    // "back", which is what a player expects from every other screen.
    if (screen_ == Screen::Multiplayer && mp_focus_ != 0) {
        std::string& field = (mp_focus_ == 1) ? mp_addr_ : mp_port_;
        if (ev.type == SDL_TEXTINPUT) {
            for (const char* c = ev.text.text; *c; ++c) {
                // The port field takes digits only; an address takes hostnames
                // and dotted quads, so letters, digits, dots, dashes and colons.
                unsigned char u = static_cast<unsigned char>(*c);
                bool ok = mp_focus_ == 2 ? (std::isdigit(u) != 0)
                                         : (std::isalnum(u) || *c == '.' || *c == '-' || *c == ':');
                if (ok && field.size() < (mp_focus_ == 2 ? 5u : 64u)) field.push_back(*c);
            }
            return;
        }
        if (ev.type == SDL_KEYDOWN) {
            SDL_Keycode k = ev.key.keysym.sym;
            if (k == SDLK_BACKSPACE) {
                if (!field.empty()) field.pop_back();
                return;
            }
            if (k == SDLK_TAB) {
                mp_focus_ = (mp_focus_ == 1) ? 2 : 1;
                return;
            }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                mp_focus_ = 0;
                SDL_StopTextInput();
                return;
            }
        }
    }
    if (ev.type == SDL_MOUSEMOTION) {
        mouse_pos_ = {ev.motion.x, ev.motion.y};
    } else if (ev.type == SDL_KEYDOWN && rebinding_.kind != RebindTarget::Kind::None) {
        // Listening for a rebind's next keypress -- consumes EVERY key
        // (including Escape, which cancels instead of navigating back) so
        // none of the normal click/Escape handling below runs meanwhile.
        SDL_Keycode key = ev.key.keysym.sym;
        if (key == SDLK_ESCAPE) {
            rebinding_.kind = RebindTarget::Kind::None;
            rebinding_.pending_key = SDLK_UNKNOWN;
            return;
        }
        // Ctrl on its own is the dedicated combo-modifier (its held-state is
        // captured below, not bindable as a standalone trigger); Alt/GUI
        // stay reserved too (they're OS-level shortcuts -- Alt+Tab etc).
        // Shift CAN be bound standalone: unlike Ctrl it doesn't act as a
        // combo-qualifier in this system, so pressing it alone is just a
        // normal candidate key like any other.
        switch (key) {
            case SDLK_LCTRL: case SDLK_RCTRL:
            case SDLK_LALT: case SDLK_RALT: case SDLK_LGUI: case SDLK_RGUI:
                return;
            default: break;
        }
        // Only Delete is truly reserved -- it's its own fixed, non-
        // rebindable key (see GameClient::handle_hotkey), so binding
        // something else to it would just be permanently unreachable.
        // Everything else (Z, the digits, ...) CAN be reassigned, even
        // though some of them already have a hardcoded use (age-up,
        // command groups) elsewhere -- a resulting collision isn't shown
        // as a red conflict in this screen (those aren't Settings-tracked
        // bindings to check against), same "flag what we can, don't
        // block" philosophy as every other conflict check here.
        if (key == SDLK_DELETE) return;
        // Don't commit on keydown -- remember it, and whatever Ctrl state
        // is held right now, and wait for THIS key to be released (see the
        // SDL_KEYUP branch below). Reacting on keydown made a Ctrl+<letter>
        // attempt register as just "<letter>" the instant it was pressed,
        // before the player had actually finished the gesture (and ignored
        // Ctrl entirely besides), and would also re-fire on key repeat if
        // held. Any slot can end up with a Ctrl combo now -- it's just
        // whatever the player was actually holding at the time.
        rebinding_.pending_key = key;
        rebinding_.pending_ctrl = (ev.key.keysym.mod & KMOD_CTRL) != 0;
    } else if (ev.type == SDL_KEYUP && rebinding_.kind != RebindTarget::Kind::None &&
               rebinding_.pending_key != SDLK_UNKNOWN && ev.key.keysym.sym == rebinding_.pending_key) {
        apply_rebind({rebinding_.pending_key, rebinding_.pending_ctrl});
        rebinding_.pending_key = SDLK_UNKNOWN;
    } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        handle_click(ev.button.x, ev.button.y);
    } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_RIGHT) {
        // Right-click is a "cycle backwards" gesture, currently only on the
        // colour swatch -- handle_click drops it for every other hit-rect.
        handle_click(ev.button.x, ev.button.y, /*right=*/true);
    } else if (ev.type == SDL_KEYDOWN &&
               (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) &&
               screen_ == Screen::Title && show_update_notes_) {
        show_update_notes_ = false; // Enter also dismisses the notes popup, same as its Close button
    } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
        if (screen_ == Screen::TechTree) screen_ = Screen::CivChooser;
        else if (screen_ == Screen::CivChooser)
            screen_ = mp_from_civ_chooser_ ? Screen::MpSetup : Screen::RandomMapSetup;
        else if (screen_ == Screen::RandomMapSetup) screen_ = Screen::SinglePlayer;
        // Leaving the roster screen leaves the MATCH -- there is a peer on the
        // other end of it, so this drops the connection rather than pretending
        // the lobby is still alive behind us.
        else if (screen_ == Screen::MpSetup) { mp_reset(); screen_ = Screen::Multiplayer; }
        else if (screen_ == Screen::HotkeysOptions) screen_ = Screen::Options;
        else if (screen_ == Screen::GraphicsOptions) screen_ = Screen::Options;
        else if (screen_ == Screen::AudioOptions) screen_ = Screen::Options;
        else if (screen_ == Screen::Options) screen_ = Screen::MainMenu;
        else if (screen_ == Screen::SinglePlayer) screen_ = Screen::MainMenu;
        else if (screen_ == Screen::Multiplayer) { mp_reset(); screen_ = Screen::MainMenu; }
        else if (screen_ == Screen::MainMenu) screen_ = Screen::Title;
        else if (screen_ == Screen::CampaignMap) {
            if (selected_level_ >= 0) selected_level_ = -1;
            else screen_ = Screen::CampaignList;
        }
        else if (screen_ == Screen::CampaignList) screen_ = Screen::SinglePlayer;
    } else if (ev.type == SDL_MOUSEWHEEL && screen_ == Screen::TechTree) {
        // Same +-64px-per-notch scroll as the left/right arrow buttons,
        // matching the original's mouse-wheel handling (control::Step.gml).
        tech_tree_scroll_ = std::clamp(tech_tree_scroll_ - ev.wheel.y * 64.0, 0.0, tech_tree_max_scroll(data_dir_));
    } else if (ev.type == SDL_MOUSEWHEEL && screen_ == Screen::HotkeysOptions) {
        hotkeys_scroll_ = std::clamp(hotkeys_scroll_ - ev.wheel.y * 64.0, 0.0,
                                     hotkeys_max_scroll(settings_.construction_keys.size(), kPanel_.h));
    }
}

void MenuController::cycle_colour(int idx, int step) {
    const int n = static_cast<int>(ww::menu::team_colours().size());
    // Only the rows actually in the game reserve a colour -- teams_ always
    // holds 8 rows, but rows past n_players_ aren't drawn and don't play,
    // so letting them block a colour would make the button dead-end (with 8
    // players there'd be nothing left to cycle to at all).
    auto taken = [&](int c) {
        for (int i = 0; i < n_players_ && i < static_cast<int>(teams_.size()); ++i) {
            if (i != idx && teams_[i].colour == c) return true;
        }
        return false;
    };
    int c = teams_[idx].colour;
    // At most one full lap: if every other colour is spoken for, this walks
    // all the way back to the row's current one (never "taken", since the
    // row itself is excluded) and simply stays put.
    for (int tries = 0; tries < n; ++tries) {
        c = ((c + step) % n + n) % n; // real modulo -- step is -1 going backwards
        if (!taken(c)) break;
    }
    teams_[idx].colour = c;
}

void MenuController::handle_click(int mx, int my, bool right) {
    SDL_Point p{mx, my};
    for (auto& hr : hit_rects_) {
        if (!SDL_PointInRect(&p, &hr.rect)) continue;
        // A right-click means one specific thing -- cycle backwards -- and
        // the colour swatch is the only control that offers it. Anywhere
        // else it's swallowed, not silently re-run as a left-click (which
        // would fire "start_war"/"quit"/... off the wrong button).
        if (right && hr.id != "cycle_colour") return;
        if (audio_) audio_->play("click", 0, 0, false); // UI click feedback on any menu button
        const std::string& id = hr.id;
        if (id == "quit") {
            wants_quit_ = true;
        } else if (id == "title_start") {
            screen_ = Screen::MainMenu;
        } else if (id == "update_notes") {
            show_update_notes_ = true;
            update_notes_index_ = 0; // always reopen showing the newest entry
        } else if (id == "notes_left") {
            ++update_notes_index_; // toward older entries (clamped in draw_title)
        } else if (id == "notes_right") {
            --update_notes_index_; // toward newer entries (clamped in draw_title)
        } else if (id == "notes_close") {
            show_update_notes_ = false;
        } else if (id == "open_mapselect") {
            screen_ = Screen::MapSelect;
        } else if (id == "pick_map") {
            map_type_idx_ = hr.arg;
        } else if (id == "select_map") {
            screen_ = Screen::RandomMapSetup;
        } else if (id == "back" || id == "close_tech_tree") {
            if (screen_ == Screen::MapSelect) { screen_ = Screen::RandomMapSetup; return; }
            // Civ chooser's back discards civ_chooser_preview_ (never
            // written into teams_[]) -- cancel, not confirm, per design.
            if (screen_ == Screen::TechTree) screen_ = Screen::CivChooser;
            else if (screen_ == Screen::CivChooser)
                screen_ = mp_from_civ_chooser_ ? Screen::MpSetup : Screen::RandomMapSetup;
            else if (screen_ == Screen::RandomMapSetup) screen_ = Screen::SinglePlayer;
            else if (screen_ == Screen::MpSetup) { mp_reset(); screen_ = Screen::Multiplayer; }
            else if (screen_ == Screen::HotkeysOptions) screen_ = Screen::Options;
            else if (screen_ == Screen::GraphicsOptions) screen_ = Screen::Options;
            else if (screen_ == Screen::AudioOptions) screen_ = Screen::Options;
            else if (screen_ == Screen::Options) screen_ = Screen::MainMenu;
            else if (screen_ == Screen::SinglePlayer) screen_ = Screen::MainMenu;
            else if (screen_ == Screen::Multiplayer) { mp_reset(); screen_ = Screen::MainMenu; }
            else if (screen_ == Screen::MainMenu) screen_ = Screen::Title;
            else if (screen_ == Screen::CampaignMap) {
                if (selected_level_ >= 0) selected_level_ = -1; // popup open -> just close it, stay on the map
                else screen_ = Screen::CampaignList;
            }
            else if (screen_ == Screen::CampaignList) screen_ = Screen::SinglePlayer;
        } else if (id == "single_player") {
            screen_ = Screen::SinglePlayer;
        } else if (id == "multiplayer") {
            mp_reset();
            // Convenience shared with the env hook: WW_MP_JOIN pre-fills the
            // address so a tester (or anyone bringing two builds up repeatedly)
            // only has to press Join. Typing still overrides it.
            if (mp_addr_.empty()) {
                if (const char* a = SDL_getenv("WW_MP_JOIN")) mp_addr_ = a;
            }
            if (const char* p = SDL_getenv("WW_MP_PORT")) mp_port_ = p;
            screen_ = Screen::Multiplayer;
        } else if (id == "mp_focus") {
            // hr.arg: 1 = address, 2 = port. Clicking a field takes the caret;
            // SDL text input is only ever on while a field actually holds it.
            mp_focus_ = hr.arg;
            SDL_StartTextInput();
        } else if (id == "mp_host") {
            mp_focus_ = 0;
            SDL_StopTextInput();
            mp_error_.clear();
            // Same settings shape the env-hook path uses (app.cpp): two players,
            // and reveal_mode 2 because only team 0 is fog-restricted when
            // placing buildings, which would otherwise handicap the joiner
            // specifically. WW_SEED is honoured so a desync can be reproduced.
            SkirmishSettings s = build_settings();
            s.n_players = 2;
            s.reveal_mode = 2;
            uint64_t seed = static_cast<uint64_t>(SDL_GetPerformanceCounter());
            if (const char* sv = SDL_getenv("WW_SEED")) seed = std::strtoull(sv, nullptr, 10);
            if (session_ && session_->host(mp_parsed_port(), s, seed)) {
                mp_stage_ = MpStage::Hosting;
                mp_local_addrs_ = ww::net::local_addresses();
                // Fired now, read later (see mp_upnp_): the socket is ALREADY
                // listening, so a joiner who connects while the router is still
                // being discovered is served immediately -- which is the whole
                // reason app.cpp opens the port before mapping it too.
                mp_upnp_done_ = false;
                mp_upnp_ = std::async(std::launch::async, ww::net::map_port, mp_parsed_port());
            } else {
                mp_error_ = session_ ? session_->error() : "no network session";
            }
        } else if (id == "mp_join") {
            mp_focus_ = 0;
            SDL_StopTextInput();
            mp_error_.clear();
            if (mp_addr_.empty()) {
                mp_error_ = "Enter the host's address first.";
            } else if (session_ && session_->join(mp_addr_, mp_parsed_port())) {
                mp_stage_ = MpStage::Joining;
            } else {
                mp_error_ = session_ ? session_->error() : "no network session";
            }
        } else if (id == "mp_cancel") {
            mp_reset();
        } else if (id == "random_map") {
            screen_ = Screen::RandomMapSetup;
        } else if (id == "options") {
            screen_ = Screen::Options;
        } else if (id == "hotkeys") {
            hotkeys_scroll_ = 0.0;
            screen_ = Screen::HotkeysOptions;
        } else if (id == "graphics") {
            screen_ = Screen::GraphicsOptions;
        } else if (id == "set_resolution") {
            if (settings_.resolution_index != hr.arg) {
                settings_.resolution_index = hr.arg;
                settings_.save();
                wants_resize_ = true; // app.cpp applies this before the frame's draw() call
            }
        } else if (id == "audio") {
            screen_ = Screen::AudioOptions;
        } else if (id == "set_sfx_volume" || id == "set_music_volume") {
            // Same click-to-jump math as hotkeys_scroll_track below, just
            // horizontal (x within the track) instead of vertical.
            double frac = hr.rect.w > 0 ? std::clamp((mx - hr.rect.x) / static_cast<double>(hr.rect.w), 0.0, 1.0) : 0.0;
            if (id == "set_sfx_volume") {
                settings_.sfx_volume = frac;
                if (audio_) audio_->set_sfx_volume(frac);
            } else {
                settings_.music_volume = frac;
                if (audio_) audio_->set_music_volume(frac);
            }
            settings_.save();
        } else if (id == "hotkeys_scroll_track") {
            double max_scroll = hotkeys_max_scroll(settings_.construction_keys.size(), kPanel_.h);
            double frac = hr.rect.h > 0 ? std::clamp((my - hr.rect.y) / static_cast<double>(hr.rect.h), 0.0, 1.0) : 0.0;
            hotkeys_scroll_ = frac * max_scroll;
        } else if (id == "rebind_building") {
            rebinding_ = {RebindTarget::Kind::Building, hr.arg};
        } else if (id == "rebind_construction") {
            rebinding_ = {RebindTarget::Kind::Construction, hr.arg};
        } else if (id == "rebind_item") {
            rebinding_ = {RebindTarget::Kind::Item, hr.arg};
        } else if (id == "rebind_formation_column") {
            rebinding_ = {RebindTarget::Kind::FormationColumn, 0};
        } else if (id == "rebind_formation_box") {
            rebinding_ = {RebindTarget::Kind::FormationBox, 0};
        } else if (id == "rebind_formation_stagger") {
            rebinding_ = {RebindTarget::Kind::FormationStagger, 0};
        } else if (id == "rebind_formation_split") {
            rebinding_ = {RebindTarget::Kind::FormationSplit, 0};
        } else if (id == "rebind_land") {
            rebinding_ = {RebindTarget::Kind::Land, 0};
        } else if (id == "rebind_unload") {
            rebinding_ = {RebindTarget::Kind::Unload, 0};
        } else if (id == "rebind_shipyard_page") {
            rebinding_ = {RebindTarget::Kind::ShipyardPage, 0};
        } else if (id == "rebind_build_nuke") {
            rebinding_ = {RebindTarget::Kind::BuildNuke, 0};
        } else if (id == "rebind_build_back") {
            rebinding_ = {RebindTarget::Kind::BuildBack, 0};
        } else if (id == "rebind_idle") {
            rebinding_ = {RebindTarget::Kind::Idle, 0};
        } else if (id == "rebind_pan_up") {
            rebinding_ = {RebindTarget::Kind::PanUp, 0};
        } else if (id == "rebind_pan_down") {
            rebinding_ = {RebindTarget::Kind::PanDown, 0};
        } else if (id == "rebind_pan_left") {
            rebinding_ = {RebindTarget::Kind::PanLeft, 0};
        } else if (id == "rebind_pan_right") {
            rebinding_ = {RebindTarget::Kind::PanRight, 0};
        } else if (id == "rebind_build_eco") {
            rebinding_ = {RebindTarget::Kind::BuildEco, 0};
        } else if (id == "rebind_build_military") {
            rebinding_ = {RebindTarget::Kind::BuildMilitary, 0};
        } else if (id == "rebind_attack_move") {
            rebinding_ = {RebindTarget::Kind::AttackMove, 0};
        } else if (id == "apply_preset_grid") {
            settings_.apply_preset("grid");
            settings_.save();
            rebinding_.kind = RebindTarget::Kind::None;
        } else if (id == "apply_preset_classic") {
            settings_.apply_preset("classic");
            settings_.save();
            rebinding_.kind = RebindTarget::Kind::None;
        } else if (id == "campaign") {
            // Loaded fresh every time the screen's opened (not cached across
            // the menu's lifetime) so a campaign the player just added to
            // data/campaigns/ shows up without relaunching.
            campaigns_ = ww::campaign::load_all_campaigns(data_dir_);
            selected_campaign_ = -1;
            selected_level_ = -1;
            screen_ = Screen::CampaignList;
        } else if (id == "noop") {
            // Inert stub -- Change Map isn't built yet.
        } else if (id == "open_campaign") {
            selected_campaign_ = hr.arg;
            selected_level_ = -1;
            screen_ = Screen::CampaignMap;
        } else if (id == "campaign_level_dot") {
            selected_level_ = hr.arg;
        } else if (id == "play_level") {
            wants_play_level_ = true;
        } else if (id == "tech_tree") {
            tech_tree_scroll_ = 0.0;
            screen_ = Screen::TechTree;
        } else if (id == "scroll_left") {
            tech_tree_scroll_ = std::clamp(tech_tree_scroll_ - 64.0, 0.0, tech_tree_max_scroll(data_dir_));
        } else if (id == "scroll_right") {
            tech_tree_scroll_ = std::clamp(tech_tree_scroll_ + 64.0, 0.0, tech_tree_max_scroll(data_dir_));
        } else if (id == "pick_civ") {
            civ_chooser_target_ = hr.arg;
            civ_chooser_preview_ = teams_[hr.arg].civ;
            civ_chooser_leader_ = teams_[hr.arg].leader;
            mp_from_civ_chooser_ = mp_setup(); // where to return to on select/back
            screen_ = Screen::CivChooser;
        } else if (id == "cycle_colour") {
            cycle_colour(hr.arg, right ? -1 : 1); // right-click walks the palette backwards
            if (mp_setup()) mp_push_slot();
        } else if (id == "cycle_team") {
            teams_[hr.arg].ally = (teams_[hr.arg].ally % 4) + 1;
            if (mp_setup()) mp_push_slot();
        } else if (id == "mp_toggle_ready") {
            // Readiness is just another field of this machine's slot, so it
            // travels by the same path as a civ or colour change -- one way to
            // publish, one thing to get right.
            mp_ready_ = !mp_ready_;
            mp_push_slot();
        } else if (id == "mp_start") {
            // Host only, and Session re-checks both-ready itself rather than
            // trusting this button to have been drawn correctly.
            if (session_) session_->start_lobby_match();
        } else if (id == "cycle_players") {
            n_players_ = (n_players_ >= 8) ? 2 : n_players_ + 1;
            // Rows past n_players_ aren't drawn and don't reserve a colour,
            // so growing the roster can pull one back in that collides with
            // a row already on the map (set the 8th row's colour, drop to 2
            // players, re-take a colour, go back up to 8). Walk the newly
            // active set in order and push any duplicate to the next free
            // slot, so the "no two teams share a colour" rule the swatch
            // enforces holds however the player got here.
            for (int i = 1; i < n_players_; ++i) {
                for (int j = 0; j < i; ++j) {
                    if (teams_[j].colour == teams_[i].colour) { cycle_colour(i, 1); break; }
                }
            }
        } else if (id == "cycle_maptype") {
            map_type_idx_ = (map_type_idx_ + 1) % kNumMaps;
            mp_push_settings();
        } else if (id == "cycle_mapsize") {
            map_size_idx_ = (map_size_idx_ + 1) % 4;
            mp_push_settings();
        } else if (id == "cycle_maxpop") {
            max_pop_idx_ = (max_pop_idx_ + 1) % 4;
            mp_push_settings();
        } else if (id == "cycle_reveal") {
            reveal_idx_ = (reveal_idx_ + 1) % 3;
            mp_push_settings();
        } else if (id == "cycle_difficulty") {
            difficulty_idx_ = (difficulty_idx_ + 1) % 4;
            mp_push_settings();
        } else if (id == "cycle_mode") {
            mode_idx_ = (mode_idx_ + 1) % 2;
            mp_push_settings();
        } else if (id == "start_stress_test") {
            // Same one-shot flow as "start_war" (wants_start_ -> app.cpp
            // calls build_settings() and constructs GameClient) -- stress_
            // test_ being set makes build_settings() return the fixed
            // stress-test config below instead of the player's own Random
            // Map Setup choices, and doubles as the existing wants_stress_
            // test() signal app.cpp already uses to call populate_stress_
            // test() and to auto-return after 30s/Enter/Escape/Quit Game.
            stress_test_ = true;
            wants_start_ = true;
        } else if (id == "start_war") {
            wants_start_ = true;
        } else if (id == "preview_civ") {
            if (civ_chooser_preview_ != hr.arg) civ_chooser_leader_ = 0; // new civ -> first leader
            civ_chooser_preview_ = hr.arg;
        } else if (id == "cycle_leader") {
            // hr.arg is +1 (next, right arrow) or -1 (prev, left arrow).
            civ_chooser_leader_ = (civ_chooser_leader_ + hr.arg + 3) % 3;
        } else if (id == "select_civ") {
            teams_[civ_chooser_target_].civ = civ_chooser_preview_;
            teams_[civ_chooser_target_].leader = (civ_chooser_preview_ >= 0) ? civ_chooser_leader_ : 0;
            // The civ chooser is shared too, so it has to return to whichever
            // roster screen opened it -- and in a network match publish the
            // choice, since the opponent's screen shows it live.
            screen_ = mp_from_civ_chooser_ ? Screen::MpSetup : Screen::RandomMapSetup;
            if (mp_from_civ_chooser_) mp_push_slot();
        }
        return;
    }
}

void MenuController::draw_panel(SDL_Renderer* renderer, const SDL_Rect& r, bool highlight) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &r);
    if (highlight) SDL_SetRenderDrawColor(renderer, 255, 220, 60, 255);
    else SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &r);
}

void MenuController::draw_button(SDL_Renderer* renderer, const SDL_Rect& r, const std::string& label,
                                 const std::string& id, int arg, SDL_Color border,
                                 SDL_Color label_col) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &r);
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(renderer, &r);
    if (!label.empty()) {
        int size = std::clamp(r.h - 10, 10, 24);
        int tw, th;
        text_.measure(label, size, tw, th);
        while (tw > r.w - 8 && size > 8) {
            size -= 1;
            text_.measure(label, size, tw, th);
        }
        text_.draw(label, r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, label_col, size);
    }
    if (!id.empty()) hit_rects_.push_back({r, id, arg});
}

void MenuController::draw_panning_bg(SDL_Renderer* renderer) {
    (void)renderer;
    // Eight WW2 photos cycle behind every menu in RANDOM order: each pans slowly
    // left-to-right the WHOLE time it's up, and consecutive images overlap by
    // `fade` seconds so the incoming one is already moving before the outgoing
    // one has finished fading -- a seamless crossfade rather than a hard cut.
    static const char* imgs[8] = {"bg_title0", "bg_title1", "bg_title2", "bg_title3",
                                  "bg_title4", "bg_title5", "bg_title6", "bg_title7"};
    constexpr int N = 8;
    double t = SDL_GetTicks() / 1000.0;
    const double step = 5.0;        // a new image begins every `step` seconds
    const double fade = 1.4;        // fade in/out length (= the overlap window)
    const double dur = step + fade; // each image is on screen this long
    // Deterministic pseudo-random image for cycle c, never repeating the one
    // immediately before it (so the order looks shuffled, not sequential).
    auto pick = [](int c) {
        auto h = [](int x) {
            uint32_t v = static_cast<uint32_t>(x) * 2654435761u + 40503u;
            v ^= v >> 15; v *= 2246822519u; v ^= v >> 13;
            return static_cast<int>(v % N);
        };
        int v = h(c);
        if (c > 0 && h(c - 1) == v) v = (v + 1) % N;
        return v;
    };
    auto draw_img = [&](const char* name, double phase01, Uint8 a) {
        int iw = static_cast<int>(view_w_ * 1.30); // wider than the view so it can pan
        int max_pan = iw - view_w_;
        int px = -static_cast<int>(std::clamp(phase01, 0.0, 1.0) * max_pan);
        SDL_Rect dst{px, 0, iw, view_h_};
        atlas_.draw_stretched(name, dst, 0, false, a);
    };
    int cur = static_cast<int>(t / step);
    // The current image and the previous one (still fading out) can both be up.
    for (int c = cur - 1; c <= cur; ++c) {
        if (c < 0) continue;
        double age = t - c * step; // 0..dur while this image is on screen
        if (age < 0 || age > dur) continue;
        double a = 1.0;
        if (age < fade) a = age / fade;                 // fading in
        else if (age > dur - fade) a = (dur - age) / fade; // fading out
        draw_img(imgs[pick(c)], age / dur, static_cast<Uint8>(std::clamp(a, 0.0, 1.0) * 255));
    }
}

void MenuController::draw_title(SDL_Renderer* renderer) {
    // "WORLD WAR" start screen over the panning backdrop (ref screenshot 1168):
    // big yellow shadowed title, red version line, a Start button, an
    // Update-notes button (bottom-left), and the close X (top-right).
    auto shadow_text = [&](const std::string& s, int x, int y, int size, SDL_Color col) {
        text_.draw(s, x + 3, y + 3, {0, 0, 0, 255}, size);
        text_.draw(s, x, y, col, size);
    };
    // Title: centred, another 20% larger (90 -> 108), with an underline.
    {
        const int tsize = 108;
        int tw, th;
        text_.measure("WORLD WAR", tsize, tw, th);
        int tx = kPanel_.x + (kPanel_.w - tw) / 2, ty = kPanel_.y + 40;
        shadow_text("WORLD WAR", tx, ty, tsize, {245, 215, 30, 255});
        // Underline: a shadowed yellow bar just under the letters.
        int uy = ty + th + 4;
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_Rect ul_sh{tx + 3, uy + 3, tw, 5};
        SDL_RenderFillRect(renderer, &ul_sh);
        SDL_SetRenderDrawColor(renderer, 245, 215, 30, 255);
        SDL_Rect ul{tx, uy, tw, 5};
        SDL_RenderFillRect(renderer, &ul);
    }
    // WW_VERSION comes from the repo-root VERSION file via CMake, so this
    // label always matches the released build (see CMakeLists.txt / publish.bat).
#ifndef WW_VERSION
#define WW_VERSION "dev"
#endif
    text_.draw(std::string("Version ") + WW_VERSION, kPanel_.x + 44, kPanel_.y + 158,
               {210, 40, 40, 255}, 18);
    text_.draw("New Fronts Update", kPanel_.x + 44, kPanel_.y + 182, {210, 40, 40, 255}, 18);

    // The Update Notes popup is modal -- none of the title screen's own
    // controls (Start, the notes button, Quit) get hit rects while it's
    // open, so a click can only hit the popup's own Left/Right/Close
    // buttons. That replaces the old "any click closes it" special case
    // entirely (see handle_click).
    if (!show_update_notes_) {
        // Start must be clicked deliberately -- no more Enter-to-advance or
        // click-anywhere-on-the-screen shortcut (see handle_event), so this
        // button is the ONLY way past the title screen now.
        SDL_Rect start_btn{kPanel_.x + (kPanel_.w - 200) / 2, kPanel_.y + kPanel_.h - 90, 200, 40};
        draw_button(renderer, start_btn, "Start", "title_start", 0, {60, 220, 60, 255});

        SDL_Rect notes_btn{kPanel_.x + 8, kPanel_.y + kPanel_.h - 38, 150, 30};
        draw_button(renderer, notes_btn, "Update notes", "update_notes", 0, {40, 180, 40, 255});
        // Close X (top-right) -- reuse the chrome quit button.
        if (atlas_.meta("spr_button_quit")) {
            atlas_.draw("spr_button_quit", kQuitRect_.x, kQuitRect_.y);
            hit_rects_.push_back({kQuitRect_, "quit", 0});
        }
    }

    if (show_update_notes_) {
        SDL_Rect box{kPanel_.x + 80, kPanel_.y + 60, kPanel_.w - 160, kPanel_.h - 120};
        draw_panel(renderer, box);
        SDL_SetRenderDrawColor(renderer, 245, 215, 30, 255);
        SDL_RenderDrawRect(renderer, &box);

        const auto& notes = ww::menu::release_notes(data_dir_);
        if (notes.empty()) {
            text_.draw("No release notes available.", box.x + 20, box.y + 16, {235, 235, 225, 255}, 15);
        } else {
            if (update_notes_index_ < 0) update_notes_index_ = 0;
            if (update_notes_index_ >= static_cast<int>(notes.size()))
                update_notes_index_ = static_cast<int>(notes.size()) - 1;
            const auto& entry = notes[update_notes_index_];

            std::string header = entry.title.empty() ? ("v" + entry.version)
                                                       : (entry.title + "  (v" + entry.version + ")");
            text_.draw(header, box.x + 20, box.y + 16, {245, 215, 30, 255}, 20);

            // Wrapped at a smaller size (12, regular-weight font) -- the
            // original single-line-per-bullet draw ran text straight off
            // the right edge of the box for anything longer than a few
            // words. Clipped (not scrolled) once it'd run into the nav
            // row below, same "stop rather than overflow" rule the
            // campaign briefing's objectives list already uses.
            int ly = box.y + 50;
            int max_ly = box.y + box.h - 46;
            for (auto& l : entry.lines) {
                for (auto& wrapped : wrap_text(text_small_, l, 12, box.w - 40)) {
                    if (ly > max_ly) break;
                    text_small_.draw(wrapped, box.x + 20, ly, {235, 235, 225, 255}, 12);
                    ly += 15;
                }
            }

            // Left steps toward older entries, Right toward newer -- dimmed
            // and unclickable (empty id) at whichever end has nowhere left
            // to go, same disabled-button convention as the civ chooser's
            // Tech Tree button.
            bool has_older = update_notes_index_ + 1 < static_cast<int>(notes.size());
            bool has_newer = update_notes_index_ > 0;
            SDL_Rect left_btn{box.x + 12, box.y + box.h - 38, 56, 26};
            SDL_Rect right_btn{box.x + box.w - 68, box.y + box.h - 38, 56, 26};
            draw_button(renderer, left_btn, "<", has_older ? "notes_left" : "", 0,
                       has_older ? SDL_Color{245, 215, 30, 255} : SDL_Color{90, 90, 80, 255});
            draw_button(renderer, right_btn, ">", has_newer ? "notes_right" : "", 0,
                       has_newer ? SDL_Color{245, 215, 30, 255} : SDL_Color{90, 90, 80, 255});

            std::string page = std::to_string(update_notes_index_ + 1) + " / " + std::to_string(notes.size());
            int ptw, pth;
            text_.measure(page, 13, ptw, pth);
            text_.draw(page, box.x + (box.w - ptw) / 2, box.y + box.h - 34, {200, 200, 190, 255}, 13);
        }

        SDL_Rect close_btn{box.x + box.w - 82, box.y + 10, 64, 26};
        draw_button(renderer, close_btn, "Close", "notes_close", 0, {210, 60, 60, 255});
    }
}

void MenuController::draw_map_select(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);
    // Left half: a grid of map thumbnails (photo bg_map_<key> if present, else a
    // terrain-colour swatch). Right half: the selected map's details -- a Europe
    // map with a location marker, a description, tree density, and a SELECT.
    const int split = kPanel_.x + static_cast<int>(kPanel_.w * 0.58); // grid | details divider
    SDL_SetRenderDrawColor(renderer, 90, 90, 90, 255);
    SDL_RenderDrawLine(renderer, split, kPanel_.y + 12, split, kPanel_.y + kPanel_.h - 12);
    const int left_x = kPanel_.x + 20;
    const int tw = 92, thh = 60, gapx = 12, gapy = 28;
    // As many columns as fit the grid region's actual width (was a flat 3,
    // sized for the original 640x480 canvas) -- at a larger resolution the
    // grid gets more columns instead of just more empty margin next to the
    // divider. Clamped to at least 1 (degenerate/tiny-window safety) and at
    // most kNumMaps (no point computing more columns than there are maps).
    int per_row = std::clamp((split - left_x) / (tw + gapx), 1, kNumMaps);
    // One font size that fits EVERY map name inside a thumbnail's width, so long
    // names ("Santa Cruz Islands", "Normandy Beach") no longer spill sideways
    // into the neighbouring column's label (the reported text overlap).
    std::vector<std::string> all_map_names;
    for (int i = 0; i < kNumMaps; ++i) all_map_names.emplace_back(kMaps[i].name);
    int map_name_size = fixed_fit_size(text_, all_map_names, tw - 2, 13, 8);
    int col = 0, r = 0;
    for (int i = 0; i < kNumMaps; ++i) {
        int x = left_x + col * (tw + gapx);
        int y = kPanel_.y + 26 + r * (thh + gapy);
        SDL_Rect tr{x, y, tw, thh};
        std::string thumb = std::string("bg_map_") + kMaps[i].value;
        if (std::string(kMaps[i].value) == "random" && atlas_.meta("spr_random_civ")) {
            SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
            SDL_RenderFillRect(renderer, &tr);
            atlas_.draw_in_rect(tr, "spr_random_civ", 0, 8); // "?" tile
        } else if (atlas_.meta(thumb)) {
            atlas_.draw_stretched(thumb, tr);
        } else {
            SDL_Color c = kMaps[i].swatch;
            SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
            SDL_RenderFillRect(renderer, &tr);
        }
        bool sel = (i == map_type_idx_);
        SDL_SetRenderDrawColor(renderer, sel ? 60 : 255, sel ? 220 : 255, sel ? 60 : 255, 255);
        SDL_RenderDrawRect(renderer, &tr);
        int nw, nh;
        text_.measure(kMaps[i].name, map_name_size, nw, nh);
        text_.draw(kMaps[i].name, x + (tw - nw) / 2, y + thh + 4,
                   sel ? SDL_Color{60, 220, 60, 255} : SDL_Color{255, 255, 255, 255}, map_name_size);
        hit_rects_.push_back({tr, "pick_map", i});
        if (++col >= per_row) { col = 0; ++r; }
    }

    // ---- details panel (right) ----
    const MapInfo& mi = kMaps[map_type_idx_];
    const int rx = split + 20;
    const int rw = kPanel_.x + kPanel_.w - rx - 20;
    text_.draw(mi.name, rx, kPanel_.y + 22, {255, 255, 255, 255}, 24);
    // Real-world location map: a per-map geographical image (spr_map_<value>)
    // with the location marker already baked in. "Random" has no real location,
    // so it just shows open ocean. (Map value -> sprite: spaces become
    // underscores, e.g. "santa cruz islands" -> "spr_map_santa_cruz_islands".)
    SDL_Rect eu{rx, kPanel_.y + 58, rw, 156};
    std::string loc_map;
    if (mi.region && *mi.region) {
        loc_map = std::string("spr_map_") + mi.value;
        for (char& c : loc_map) if (c == ' ') c = '_';
    }
    if (!loc_map.empty() && atlas_.meta(loc_map)) {
        atlas_.draw_stretched(loc_map, eu);
    } else {
        SDL_SetRenderDrawColor(renderer, 149, 198, 247, 255); // open ocean (Random / missing image)
        SDL_RenderFillRect(renderer, &eu);
    }

    // Word-wrapped description below the map. Font size is chosen so the whole
    // wrapped block (plus the tree-density line that follows it) fits between the
    // map and the SELECT button -- previously a fixed size 15 let long map
    // descriptions overrun the fixed-position tree-density line (they overlapped).
    int ty = eu.y + eu.h + 14;
    const int desc_bottom = kPanel_.y + kPanel_.h - 66; // leave room for SELECT below
    int desc_size = 15, line_h = 20;
    for (; desc_size >= 10; --desc_size) {
        line_h = desc_size + 3;
        int lines = 0, wsum;
        std::string word, line;
        std::string s = mi.desc;
        s.push_back(' ');
        for (char ch : s) {
            if (ch == ' ') {
                std::string cand = line.empty() ? word : line + " " + word;
                int w, h;
                text_.measure(cand, desc_size, w, h);
                if (w > rw && !line.empty()) { ++lines; line = word; }
                else line = cand;
                word.clear();
            } else word.push_back(ch);
        }
        if (!line.empty()) ++lines;
        (void)wsum;
        if (ty + lines * line_h + line_h /*tree density*/ <= desc_bottom) break; // fits
    }
    {
        std::string word, line;
        std::string s = mi.desc;
        s.push_back(' ');
        for (char ch : s) {
            if (ch == ' ') {
                std::string cand = line.empty() ? word : line + " " + word;
                int w, h;
                text_.measure(cand, desc_size, w, h);
                if (w > rw && !line.empty()) {
                    text_.draw(line, rx, ty, {235, 235, 225, 255}, desc_size);
                    ty += line_h;
                    line = word;
                } else {
                    line = cand;
                }
                word.clear();
            } else {
                word.push_back(ch);
            }
        }
        if (!line.empty()) { text_.draw(line, rx, ty, {235, 235, 225, 255}, desc_size); ty += line_h; }
    }
    char tb[32];
    std::snprintf(tb, sizeof(tb), "Tree density: %d", mi.tree_density);
    text_.draw(tb, rx, ty + 2, {235, 235, 225, 255}, desc_size); // directly below the description

    SDL_Rect sel_btn{rx + (rw - 140) / 2, kPanel_.y + kPanel_.h - 62, 140, 40};
    draw_button(renderer, sel_btn, "SELECT", "select_map", 0, {220, 40, 40, 255});
}

void MenuController::draw_frame_chrome(SDL_Renderer* renderer, bool show_back) {
    draw_panel(renderer, kPanel_);
    // spr_button_back/spr_button_quit are self-contained button graphics
    // (red box + icon already baked into the art) -- drawn directly at
    // native size rather than through draw_button's black-fill+border box,
    // which just duplicated/clashed with the sprite's own border.
    if (show_back) {
        if (atlas_.meta("spr_button_back")) {
            atlas_.draw("spr_button_back", kBackRect_.x, kBackRect_.y);
            hit_rects_.push_back({kBackRect_, "back", 0});
        } else {
            draw_button(renderer, kBackRect_, "<", "back", 0, {255, 60, 60, 255});
        }
    }
    if (atlas_.meta("spr_button_quit")) {
        atlas_.draw("spr_button_quit", kQuitRect_.x, kQuitRect_.y);
        hit_rects_.push_back({kQuitRect_, "quit", 0});
    } else {
        draw_button(renderer, kQuitRect_, "X", "quit", 0, {255, 60, 60, 255});
    }
}

void MenuController::draw_main_menu(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true); // back to Title

    const std::string title = "Select Game Mode";
    int tw, th;
    text_.measure(title, 28, tw, th);
    text_.draw(title, kPanel_.x + (kPanel_.w - tw) / 2, kPanel_.y + 24, {255, 255, 255, 255}, 28);

    SDL_Rect single_btn{kPanel_.x + (kPanel_.w - 260) / 2, kPanel_.y + 120, 260, 60};
    draw_button(renderer, single_btn, "Single Player", "single_player");

    // Multiplayer is only offered when app.cpp has actually lent us a Session
    // (set_session). Without one the lobby has nothing to drive, so the button
    // draws dimmed with NO hit rect rather than opening a screen that cannot
    // work -- the same convention the disabled buttons elsewhere use.
    const SDL_Color kDim = {110, 110, 110, 255};
    SDL_Rect multi_btn{kPanel_.x + (kPanel_.w - 260) / 2, kPanel_.y + 220, 260, 60};
    if (session_) {
        draw_button(renderer, multi_btn, "Multiplayer", "multiplayer");
    } else {
        draw_button(renderer, multi_btn, "Multiplayer", /*id=*/"", 0, kDim, kDim);
    }

    SDL_Rect options_btn{kPanel_.x + (kPanel_.w - 260) / 2, kPanel_.y + 320, 260, 60};
    draw_button(renderer, options_btn, "Options", "options");
}

// The Random Map / Campaign choice that used to BE the main menu, moved one
// level down so Single Player and Multiplayer can sit above it. Everything
// below this screen (setup, civ chooser, campaign list) is unchanged -- only
// what "back" returns to had to move with it.
void MenuController::draw_single_player(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true); // back to MainMenu

    const std::string title = "Single Player";
    int tw, th;
    text_.measure(title, 28, tw, th);
    text_.draw(title, kPanel_.x + (kPanel_.w - tw) / 2, kPanel_.y + 24, {255, 255, 255, 255}, 28);

    SDL_Rect random_map_btn{kPanel_.x + (kPanel_.w - 260) / 2, kPanel_.y + 150, 260, 60};
    draw_button(renderer, random_map_btn, "Random Map", "random_map");

    SDL_Rect campaign_btn{kPanel_.x + (kPanel_.w - 260) / 2, kPanel_.y + 250, 260, 60};
    draw_button(renderer, campaign_btn, "Campaign", "campaign");
}

int MenuController::mp_local_row() const { return session_ ? session_->local_team() : 0; }

// ---- lobby <-> shared roster screen -----------------------------------------
// draw_random_map_setup renders from teams_[] and the option indices and knows
// nothing about the network. These three functions are the whole bridge: pull
// the session's state into those fields before drawing, push local edits back
// out. Keeping the bridge here rather than threading a Session through the draw
// code is what lets the two screens stay literally the same function.
void MenuController::mp_pull() {
    if (!session_) return;
    n_players_ = 2; // a lockstep match is 1v1; the Players row is not editable
    reveal_idx_ = 2; // forced Revealed -- see the Reveal Map row below for why
    for (int i = 0; i < 2; ++i) {
        const ww::net::LobbySlot& s = session_->lobby_slot(i);
        teams_[i].civ = s.civ;
        teams_[i].leader = s.leader;
        teams_[i].colour = s.colour;
        teams_[i].ally = s.ally;
    }
    // The joiner mirrors the host's map/rule choices; the host is the authority
    // and must NOT have its own controls overwritten by the echo of its own
    // broadcast, which would fight every click.
    if (session_->role() != ww::net::Role::Host) {
        const ww::sim::SkirmishSettings& s = session_->settings();
        for (int i = 0; i < 4; ++i)
            if (kMapSizeValues[i] == s.map_size) map_size_idx_ = i;
        for (int i = 0; i < 4; ++i)
            if (kMaxPopValues[i] == s.max_pop) max_pop_idx_ = i;
        for (int i = 0; i < kNumMaps; ++i)
            if (s.map_type == kMaps[i].value) map_type_idx_ = i;
        reveal_idx_ = std::clamp(s.reveal_mode, 0, 2);
        difficulty_idx_ = std::clamp(s.difficulty, 0, 3);
        mode_idx_ = s.deathmatch ? 1 : 0;
    }
}

void MenuController::mp_push_slot() {
    if (!session_) return;
    int row = mp_local_row();
    ww::net::LobbySlot s;
    s.civ = teams_[row].civ;
    s.leader = teams_[row].leader;
    s.colour = teams_[row].colour;
    s.ally = teams_[row].ally;
    s.ready = mp_ready_;
    session_->set_local_slot(s);
}

void MenuController::mp_push_settings() {
    // Screen-gated as well as role-gated: a player who hosted, backed out, and
    // started a skirmish still has role_ == Host on a closed session, and every
    // settings click would otherwise be publishing into it.
    if (!mp_setup() || !session_ || session_->role() != ww::net::Role::Host) return;
    // Rules only -- civs/colours/allies stay empty here and are folded in by
    // Session::start_lobby_match from the two slots. build_settings() would
    // resolve every Random civ through std::rand on the spot, which at one call
    // per click would reroll the roster continuously.
    ww::sim::SkirmishSettings s;
    s.n_players = 2;
    s.map_size = kMapSizeValues[map_size_idx_];
    s.max_pop = kMaxPopValues[max_pop_idx_];
    s.water = true;
    s.map_type = kMaps[map_type_idx_].value;
    s.deathmatch = (mode_idx_ == 1);
    s.reveal_mode = reveal_idx_;
    s.difficulty = difficulty_idx_;
    session_->set_lobby_settings(s);
}

uint16_t MenuController::mp_parsed_port() const {
    int p = std::atoi(mp_port_.c_str());
    if (p <= 0 || p > 65535) p = ww::net::kDefaultPort;
    return static_cast<uint16_t>(p);
}

void MenuController::mp_reset() {
    if (session_) session_->close();
    mp_stage_ = MpStage::Choose;
    mp_focus_ = 0;
    mp_error_.clear();
    mp_upnp_done_ = false;
    mp_upnp_result_ = ww::net::PortMapResult{};
    mp_local_addrs_.clear();
    // The UPnP future is deliberately NOT waited on here: map_port is a
    // self-contained blocking call with its own timeouts and no reference to
    // anything in this object, so a probe still in flight when the player backs
    // out simply finishes into a future nobody reads. Waiting would freeze the
    // menu for exactly as long as the thing the thread exists to avoid.
    SDL_StopTextInput();
    wants_start_mp_ = false;
}

// ---- multiplayer lobby ----------------------------------------------------
// Drives ww::net::Session directly. The net layer has been complete for a while
// (lockstep turns, input delay, per-turn checksum desync detection, UPnP); what
// was missing was only this screen, which is why entry was WW_MP_HOST /
// WW_MP_JOIN. See net/session.h, and app.cpp's env hook, which still works.
void MenuController::draw_multiplayer(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true); // back to MainMenu

    // Sockets are pumped HERE rather than from app.cpp's loop because draw() is
    // the only per-frame hook MenuController has. poll() is explicitly cheap and
    // safe to call every frame (net/session.h), and doing it in the menu is what
    // lets the lobby show progress instead of blocking on a console wait the way
    // the env-hook path does.
    if (session_) session_->poll();

    const SDL_Color kWhite = {255, 255, 255, 255};
    const SDL_Color kDim = {170, 170, 170, 255};
    const SDL_Color kRed = {235, 90, 90, 255};
    const SDL_Color kGreen = {90, 220, 90, 255};

    const std::string title = "Multiplayer";
    int tw, th;
    text_.measure(title, 28, tw, th);
    text_.draw(title, kPanel_.x + (kPanel_.w - tw) / 2, kPanel_.y + 20, kWhite, 28);

    const int cx = kPanel_.x + kPanel_.w / 2;
    int y = kPanel_.y + 76;

    if (!session_) {
        // set_session was never called -- a build with no net layer wired in.
        // Say so rather than drawing controls that cannot do anything.
        const std::string msg = "Networking is not available in this build.";
        text_.measure(msg, 16, tw, th);
        text_.draw(msg, cx - tw / 2, y, kRed, 16);
        return;
    }

    auto centred = [&](const std::string& s, int size, SDL_Color col, int yy) {
        int w, h;
        text_.measure(s, size, w, h);
        text_.draw(s, cx - w / 2, yy, col, size);
    };

    if (mp_stage_ == MpStage::Choose) {
        centred("Host a game, or join one by address.", 15, kDim, y);
        y += 30;

        SDL_Rect host_btn{cx - 130, y, 260, 52};
        draw_button(renderer, host_btn, "Host Game", "mp_host");
        y += 74;

        // ---- address + port fields ----
        // Drawn as panels with their own hit rects rather than a general
        // widget system: these are the only two typed inputs in the whole menu.
        centred("or join a host:", 14, kDim, y);
        y += 24;

        const int addr_w = 250, port_w = 78, gap = 8;
        int fx = cx - (addr_w + gap + port_w) / 2;
        auto field = [&](const SDL_Rect& r, const std::string& value, const std::string& hint,
                         int which) {
            draw_panel(renderer, r, /*highlight=*/mp_focus_ == which);
            bool empty = value.empty();
            // A blinking caret only while this field holds focus, so it is
            // obvious which one typing goes into.
            std::string shown = empty ? hint : value;
            if (mp_focus_ == which && ((SDL_GetTicks() / 500) % 2) == 0) shown += "_";
            int w, h;
            text_.measure(shown, 15, w, h);
            text_.draw(shown, r.x + 8, r.y + (r.h - h) / 2, empty ? kDim : kWhite, 15);
            hit_rects_.push_back({r, "mp_focus", which});
        };
        SDL_Rect addr_r{fx, y, addr_w, 34};
        SDL_Rect port_r{fx + addr_w + gap, y, port_w, 34};
        field(addr_r, mp_addr_, "address", 1);
        field(port_r, mp_port_, "port", 2);
        y += 46;

        SDL_Rect join_btn{cx - 130, y, 260, 46};
        draw_button(renderer, join_btn, "Join Game", "mp_join");
        y += 60;

        if (!mp_error_.empty()) centred(mp_error_, 14, kRed, y);
        return;
    }

    // ---- hosting / joining: live status -----------------------------------
    using ww::net::Status;
    Status st = session_->status();

    std::string headline;
    SDL_Color headline_col = kWhite;
    switch (st) {
        case Status::Listening:   headline = "Waiting for the other player..."; break;
        case Status::Connecting:  headline = "Connecting to " + mp_addr_ + "..."; break;
        case Status::Handshaking: headline = "Connected. Agreeing on the match..."; break;
        case Status::Ready:
            headline = "Ready -- starting the match";
            headline_col = kGreen;
            break;
        case Status::Failed:
            headline = "Failed: " + session_->error();
            headline_col = kRed;
            break;
        case Status::Closed:
            headline = "The other player disconnected.";
            headline_col = kRed;
            break;
        default: headline = "..."; break;
    }
    centred(headline, 17, headline_col, y);
    y += 34;

    if (mp_stage_ == MpStage::Hosting) {
        // Poll the UPnP probe (started on the Host click, off this thread).
        if (!mp_upnp_done_ && mp_upnp_.valid() &&
            mp_upnp_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            mp_upnp_result_ = mp_upnp_.get();
            mp_upnp_done_ = true;
        }
        centred("Give the other player one of these addresses:", 14, kDim, y);
        y += 22;
        for (const std::string& a : mp_local_addrs_) {
            centred(a + ":" + std::to_string(mp_parsed_port()), 16, kWhite, y);
            y += 22;
        }
        y += 8;
        if (!mp_upnp_done_) {
            centred("Checking your router for port forwarding...", 13, kDim, y);
        } else if (mp_upnp_result_.mapped) {
            std::string over = mp_upnp_result_.external_ip.empty()
                                   ? std::string("Port opened on your router.")
                                   : "Over the internet: " + mp_upnp_result_.external_ip + ":" +
                                         std::to_string(mp_parsed_port());
            centred(over, 14, kGreen, y);
        } else {
            // Not an error: a LAN or VPN game needs no mapping at all, and a
            // manually forwarded port works just as well. Say what it means.
            std::string why = mp_upnp_result_.discovered
                                  ? "Router found but would not open the port"
                                  : "No router answered";
            centred(why + " -- local network games still work.", 13, kDim, y);
            y += 18;
            if (!mp_upnp_result_.error.empty()) centred(mp_upnp_result_.error, 12, kDim, y);
        }
        y += 26;
    }

    if (session_->ping_ms() >= 0) {
        centred("Ping " + std::to_string(session_->ping_ms()) + " ms", 14, kDim, y);
        y += 22;
    }

    // Connected. Hand off to the ROSTER screen (Screen::MpSetup) -- the same
    // one single player uses -- where both players pick civ/leader/colour/team
    // and mark themselves ready. This screen's job ends at "we have a peer".
    if (st == Status::Ready) {
        mp_ready_ = false;
        mp_push_slot(); // publish our starting row so the peer sees us at once
        screen_ = Screen::MpSetup;
        return;
    }

    SDL_Rect cancel_btn{cx - 90, kPanel_.y + kPanel_.h - 80, 180, 42};
    draw_button(renderer, cancel_btn, "Cancel", "mp_cancel");
}

// Draws BOTH the single-player Random Map Setup and the multiplayer lobby
// roster (Screen::MpSetup). One function, not two, because the requirement is
// that they look the same -- a copy would drift the first time either is
// touched. Everything network-specific is behind `mp` below, and the difference
// is only ever about WHO MAY EDIT WHAT:
//
//   * the host owns the map and the rules; the joiner sees them read-only;
//   * each player owns exactly their own row (civ, leader, colour, team);
//   * both must mark ready before the host's Start button does anything.
void MenuController::draw_random_map_setup(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);

    const bool mp = mp_setup();
    const bool mp_host = mp && session_ && session_->role() == ww::net::Role::Host;
    const int mp_row = mp ? mp_local_row() : -1;
    if (mp) {
        if (!session_) { screen_ = Screen::MainMenu; return; }
        // Same per-frame pump as the connect screen: draw() is the only hook
        // this class gets, and without it a remote edit never arrives.
        session_->poll();
        ww::net::Status st = session_->status();
        if (st == ww::net::Status::Failed || st == ww::net::Status::Closed ||
            st == ww::net::Status::Desync) {
            // The peer went away mid-lobby. Back to the connect screen with the
            // reason still on it rather than silently sitting on a dead roster.
            mp_error_ = session_->error().empty() ? "The other player left." : session_->error();
            mp_reset();
            screen_ = Screen::Multiplayer;
            return;
        }
        // The host pressed Start (or we ARE the host and just did): settings are
        // final on both machines. Hand off to app.cpp exactly as before.
        if (session_->match_starting()) {
            wants_start_mp_ = true;
            return;
        }
        mp_pull();
    }

    // ---- left column: up to 8 team rows, top to bottom ----
    const int left_x = kPanel_.x + 15, left_w = 300;
    // Reserve a ~92px band at the bottom for the map display (left) and the
    // START WAR button (right) so neither ever collides with the team rows.
    const int avail_h = kPanel_.h - 30 - 92;
    // Fixed row size, sized for the max of 8 rows regardless of the
    // CURRENT n_players_, so rows never resize as the player count
    // changes -- only how many of them are drawn changes.
    const int row_gap = 4;
    const int row_h = std::clamp((avail_h - 7 * row_gap) / 8, 24, 60);
    // Flag box sized to spr_flags_mini's real 48x32 frame (plus a little
    // padding) instead of a fixed 46-wide box the flag was stretched
    // (squashed horizontally) to fill -- draw_in_rect distorts non-matching
    // aspect ratios, so the flag now draws at native size, centered in the
    // box, same as the civ chooser's preview flag.
    const auto* flag_meta = atlas_.meta("spr_flags_mini");
    const int flag_fw = flag_meta ? flag_meta->fw : 48, flag_fh = flag_meta ? flag_meta->fh : 32;
    const int civ_box_w = flag_fw + 4;
    const int name_x = left_x + 34 + civ_box_w + 4;
    // Colour swatch shrunk from its old 30px and a new Team cycle-button
    // (values 1-4, see TeamRow::ally) added directly after it, per the
    // user's instruction to compress the row to fit a team selector after
    // the colour column -- the name column absorbs the difference.
    const int colour_w = 22, team_w = 28, strip_gap = 3;
    const int name_w = left_x + left_w - team_w - strip_gap - colour_w - strip_gap - name_x - 4;
    // One font size fits every row (name column here, "Random" included) --
    // computed once like the civ chooser's own name text (see
    // draw_civ_chooser), not shrunk per-row, which previously made shorter
    // names render larger than longer ones.
    std::vector<std::string> all_names(ww::menu::civ_names().begin(), ww::menu::civ_names().end());
    all_names.push_back("Random");
    // Multiplayer tags each row YOU/OPP on the right of the name box, so the
    // civ name has to be fitted into a correspondingly narrower column or a
    // long one ("United Kingdom") runs straight under the tag and the colour
    // swatch beyond it.
    const int tag_w = mp ? 40 : 0;
    int name_size = fixed_fit_size(text_, all_names, name_w - 10 - tag_w, 20, 10);
    const int n_rows = mp ? 2 : n_players_;
    for (int i = 0; i < n_rows; ++i) {
        // In a network match you may only touch your own row -- the other one is
        // the opponent's live choice, arriving over the wire. Everything is
        // still DRAWN identically; it simply registers no hit rect, so a click
        // on it does nothing rather than editing someone else's civ.
        const bool mine = !mp || i == mp_row;
        const bool row_ready = mp && session_ && session_->lobby_slot(i).ready;
        int ry = kPanel_.y + 15 + i * (row_h + row_gap);
        SDL_Rect num_r{left_x, ry, 30, row_h};
        draw_panel(renderer, num_r);
        int tw, th;
        std::string num_s = std::to_string(i + 1);
        int nsize = std::clamp(row_h - 10, 10, 20);
        text_.measure(num_s, nsize, tw, th);
        // The row number doubles as the per-player ready lamp in multiplayer:
        // green once that side has locked in. Costs no layout, and the roster
        // has to show BOTH players' state, not just this machine's button.
        SDL_Color num_col = row_ready ? SDL_Color{60, 220, 60, 255} : SDL_Color{255, 255, 255, 255};
        text_.draw(num_s, num_r.x + (num_r.w - tw) / 2, num_r.y + (num_r.h - th) / 2, num_col, nsize);

        SDL_Rect civ_r{left_x + 34, ry, civ_box_w, row_h};
        int civ = teams_[i].civ;
        if (civ >= 0 && atlas_.meta("spr_flags_mini")) {
            draw_panel(renderer, civ_r);
            atlas_.draw("spr_flags_mini", civ_r.x + (civ_r.w - flag_fw) / 2,
                       civ_r.y + (civ_r.h - flag_fh) / 2, ww::menu::kCivFlagFrame[civ]);
            if (mine) hit_rects_.push_back({civ_r, "pick_civ", i});
        } else {
            draw_button(renderer, civ_r, "?", mine ? "pick_civ" : "", i, {255, 220, 60, 255});
        }

        SDL_Rect name_r{name_x, ry, name_w, row_h};
        draw_panel(renderer, name_r);
        std::string name_s = civ >= 0 ? ww::menu::civ_names()[civ] : "Random";
        text_.measure(name_s, name_size, tw, th);
        text_.draw(name_s, name_r.x + 8, name_r.y + (name_r.h - th) / 2, {255, 255, 255, 255}, name_size);
        // Multiplayer marks the two seats, so it is obvious which row is yours
        // before you have picked anything -- at the start both rows can read
        // "Random" and nothing else would distinguish them. Right-aligned in
        // its own reserved strip (tag_w), so it can never collide with the name.
        if (mp) {
            std::string tag = (i == mp_row) ? "YOU" : "OPP";
            SDL_Color tag_col = (i == mp_row) ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255};
            text_.measure(tag, 11, tw, th);
            text_.draw(tag, name_r.x + name_r.w - tw - 6, name_r.y + (name_r.h - th) / 2, tag_col, 11);
        }

        SDL_Rect colour_r{left_x + left_w - team_w - strip_gap - colour_w, ry, colour_w, row_h};
        SDL_Color tc = ww::menu::team_colours()[teams_[i].colour];
        SDL_SetRenderDrawColor(renderer, tc.r, tc.g, tc.b, 255);
        SDL_RenderFillRect(renderer, &colour_r);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawRect(renderer, &colour_r);
        if (mine) hit_rects_.push_back({colour_r, "cycle_colour", i});

        // Team cycle-button (1-4, see TeamRow::ally) -- positioned right
        // after the colour swatch per the user's instruction.
        SDL_Rect team_r{left_x + left_w - team_w, ry, team_w, row_h};
        draw_button(renderer, team_r, std::to_string(teams_[i].ally), mine ? "cycle_team" : "", i,
                    {120, 200, 255, 255});
    }

    // ---- right column: condensed options (smaller than the reference,
    // per the user's instruction, to make room for up to 8 team rows on
    // the left) ----
    const int right_x = kPanel_.x + 330, right_w = kPanel_.x + kPanel_.w - right_x - 4;
    const int opt_h = 26, opt_gap = 14;
    int ry = kPanel_.y + 15;
    // The rules belong to the host. The joiner sees exactly the same rows, drawn
    // the same way, dimmed and with no hit rect -- deliberately shown rather
    // than hidden, because "what am I about to play" is the main thing a joiner
    // needs off this screen.
    const SDL_Color kLocked = {120, 120, 120, 255};
    auto option_row = [&](const std::string& label, const std::string& value, const std::string& id,
                          bool editable = true) {
        int tw, th;
        int lsize = 14;
        text_.measure(label, lsize, tw, th);
        text_.draw(label, right_x, ry + (opt_h - th) / 2, editable ? SDL_Color{255, 255, 255, 255} : kLocked,
                   lsize);
        SDL_Rect val_r{right_x + 120, ry, right_w - 120, opt_h};
        if (editable) draw_button(renderer, val_r, value, id);
        else draw_button(renderer, val_r, value, "", 0, kLocked, kLocked);
        ry += opt_h + opt_gap;
    };
    const bool rules = !mp || mp_host;
    // Player count is fixed at 2 for a network match: lockstep here is 1v1
    // (Session::local_team is host-or-joiner and nothing else), so offering the
    // control would be offering something that cannot work.
    option_row("Players:", std::to_string(mp ? 2 : n_players_), "cycle_players", !mp);
    option_row("Map Size:", kMapSizeNames[map_size_idx_], "cycle_mapsize", rules);
    option_row("Max pop:", std::to_string(kMaxPopValues[max_pop_idx_]), "cycle_maxpop", rules);
    // Reveal is NOT a choice in a network match, and offering it would be a lie:
    // Session::start_lobby_match forces Revealed regardless, because only team 0
    // is fog-restricted when placing buildings, which would handicap the joiner
    // specifically. Locked and shown as what it will actually be, on both sides.
    option_row("Reveal Map:", mp ? "Revealed" : kRevealNames[reveal_idx_], "cycle_reveal",
               rules && !mp);
    option_row("Difficulty:", kDifficultyNames[difficulty_idx_], "cycle_difficulty", rules);
    option_row("Mode:", kModeNames[mode_idx_], "cycle_mode", rules);
    if (mp) {
        int tw, th;
        std::string who = mp_host ? "You are the host." : "The host sets the map and rules.";
        text_.measure(who, 13, tw, th);
        text_.draw(who, right_x, ry + 2, kLocked, 13);
        if (session_ && session_->ping_ms() >= 0) {
            std::string png = "Ping " + std::to_string(session_->ping_ms()) + " ms";
            text_.draw(png, right_x, ry + 20, kLocked, 13);
        }
    }

    // ---- map display (bottom-LEFT band) + START WAR (bottom-RIGHT band) ----
    // Both live in the reserved bottom band (see avail_h), on opposite sides,
    // so they never overlap each other or the team rows/options above.
    const int band_y = kPanel_.y + kPanel_.h - 84;
    text_.draw(kMaps[map_type_idx_].name, kPanel_.x + 20, band_y - 20, {255, 255, 255, 255}, 15);
    SDL_Rect map_thumb{kPanel_.x + 20, band_y, 92, 56};
    std::string thumb = std::string("bg_map_") + kMaps[map_type_idx_].value;
    bool is_random = std::string(kMaps[map_type_idx_].value) == "random";
    if (is_random && atlas_.meta("spr_random_civ")) {
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderFillRect(renderer, &map_thumb);
        atlas_.draw_in_rect(map_thumb, "spr_random_civ", 0, 6); // "?" tile
    } else if (atlas_.meta(thumb)) {
        atlas_.draw_stretched(thumb, map_thumb);
    } else {
        SDL_Color c = kMaps[map_type_idx_].swatch;
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
        SDL_RenderFillRect(renderer, &map_thumb);
    }
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &map_thumb);
    SDL_Rect change_map_btn{kPanel_.x + 122, band_y + 16, 110, 24};
    if (rules) draw_button(renderer, change_map_btn, "Change Map", "open_mapselect");
    else draw_button(renderer, change_map_btn, "Change Map", "", 0, kLocked, kLocked);

    if (!mp) {
        SDL_Rect start_btn{kPanel_.x + kPanel_.w - 220, band_y + 8, 200, 44};
        draw_button(renderer, start_btn, "START WAR", "start_war", 0, {60, 220, 60, 255});
        return;
    }

    // ---- multiplayer: ready toggle, then the host's start -------------------
    // Two buttons rather than one. Readiness is each player's own statement that
    // their row is settled -- it is the only thing the joiner can assert, and
    // the only thing that makes the host's Start meaningful. The host still has
    // to press Start, so a player who readies up early does not drag everyone
    // into a match before the host is done with the map.
    const bool both_ready =
        session_ && session_->lobby_slot(0).ready && session_->lobby_slot(1).ready;

    SDL_Rect ready_btn{kPanel_.x + kPanel_.w - 460, band_y + 8, 210, 44};
    // Green once pressed -- the requested feedback, and it is the button itself
    // that changes rather than a separate lamp, so there is nothing to hunt for.
    SDL_Color ready_col = mp_ready_ ? SDL_Color{60, 220, 60, 255} : SDL_Color{255, 255, 255, 255};
    draw_button(renderer, ready_btn, mp_ready_ ? "READY" : "I AM READY", "mp_toggle_ready", 0,
                ready_col, ready_col);

    SDL_Rect start_btn{kPanel_.x + kPanel_.w - 230, band_y + 8, 210, 44};
    if (!mp_host) {
        // The joiner sees the same button, inert, so the flow is legible from
        // both seats: you can see you are waiting on the host, not on a bug.
        draw_button(renderer, start_btn, "HOST STARTS", "", 0, kLocked, kLocked);
    } else if (!both_ready) {
        draw_button(renderer, start_btn, "WAITING...", "", 0, kLocked, kLocked);
    } else {
        draw_button(renderer, start_btn, "START GAME", "mp_start", 0, {60, 220, 60, 255},
                    {60, 220, 60, 255});
    }
}

void MenuController::draw_civ_chooser(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);

    // Full native flag size (96x64), no border/panel box -- was previously
    // squeezed into a smaller bordered cell, which both rescaled the art
    // and added an unwanted white outline around every flag.
    const int grid_x = kPanel_.x + 15, grid_y = kPanel_.y + 15;
    const int cell_w = 96, cell_h = 64, cell_gap = 12;
    for (int i = 0; i < 9; ++i) {
        int col = i % 3, row = i / 3;
        int cx = grid_x + col * (cell_w + cell_gap), cy = grid_y + row * (cell_h + cell_gap);
        if (atlas_.meta("spr_flags")) atlas_.draw("spr_flags", cx, cy, ww::menu::kCivFlagFrame[i]);
        if (civ_chooser_preview_ == i) {
            SDL_Rect hl{cx - 2, cy - 2, cell_w + 4, cell_h + 4};
            SDL_SetRenderDrawColor(renderer, 255, 220, 60, 255);
            SDL_RenderDrawRect(renderer, &hl);
        }
        hit_rects_.push_back({SDL_Rect{cx, cy, cell_w, cell_h}, "preview_civ", i});
    }

    SDL_Rect random_btn{grid_x, grid_y + 3 * (cell_h + cell_gap) + 20, 60, 44};
    draw_button(renderer, random_btn, "?", "preview_civ", -1, {255, 220, 60, 255});

    // Only enabled once a real civ is previewed (matches the original's
    // obj_tech_tree_open, visible only when display_civ != -1) -- there's
    // nothing meaningful to show a tech tree for "Random" since it isn't
    // resolved to an actual civ until the match starts.
    SDL_Rect tech_tree_btn{grid_x + 3 * (cell_w + cell_gap) - 200, grid_y + 3 * (cell_h + cell_gap) + 28, 130,
                           30};
    bool tech_tree_enabled = civ_chooser_preview_ >= 0;
    draw_button(renderer, tech_tree_btn, "Tech Tree", tech_tree_enabled ? "tech_tree" : "",
               0, tech_tree_enabled ? SDL_Color{255, 255, 255, 255} : SDL_Color{110, 110, 110, 255});

    // ---- divider + preview panel ----
    int div_x = grid_x + 3 * (cell_w + cell_gap) + 14;
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawLine(renderer, div_x, kPanel_.y + 15, div_x, kPanel_.y + kPanel_.h - 15);

    int prev_x = div_x + 20;
    if (civ_chooser_preview_ >= 0) {
        // Small flag (spr_flags_mini) -- the same version shown in-game
        // next to a selected unit/building's name, not the big picker-grid
        // flag above.
        SDL_Rect flag_r{prev_x, kPanel_.y + 15, 48, 32};
        if (atlas_.meta("spr_flags_mini")) {
            atlas_.draw("spr_flags_mini", flag_r.x, flag_r.y, ww::menu::kCivFlagFrame[civ_chooser_preview_]);
        }
        std::string name = ww::menu::civ_names()[civ_chooser_preview_];
        int name_max_w = kPanel_.x + kPanel_.w - 15 - (prev_x + 58);
        // One size fits ALL 9 names, computed once from the whole list --
        // not shrunk per-name, which made e.g. "Nazi Germany" render
        // visibly larger than "French Republic" just because it was
        // shorter and never needed to shrink.
        std::vector<std::string> all_names(ww::menu::civ_names().begin(), ww::menu::civ_names().end());
        int nsize = fixed_fit_size(text_, all_names, name_max_w, 22, 10);
        text_.draw(name, prev_x + 58, kPanel_.y + 24, {255, 255, 255, 255}, nsize);

        // Tagline (e.g. "Plane Empire" for the UK) in the small font,
        // between the name and the bonus list -- matches the original's
        // fnt_tiny "type + Empire" text (objects/control/Draw.gml).
        text_small_.draw(ww::menu::civ_types()[civ_chooser_preview_], prev_x, kPanel_.y + 58,
                         {200, 200, 200, 255}, 13);

        int by = kPanel_.y + 86;
        int bullet_max_w = kPanel_.x + kPanel_.w - 15 - prev_x;
        constexpr int kBulletSize = 14;
        for (auto& bullet : ww::menu::civ_bonuses()[civ_chooser_preview_]) {
            for (auto& line : wrap_text(text_, "* " + bullet, kBulletSize, bullet_max_w)) {
                text_.draw(line, prev_x, by, {255, 255, 255, 255}, kBulletSize);
                by += 20;
            }
            by += 8; // extra gap between bullets, beyond the per-line advance
        }
        // ---- Leader picker: the civ's 3 leaders shown one at a time, with the
        // single bonus that leader grants right under the civ's bonus stack.
        // The arrows cycle; the choice rides into the match as Team::leader.
        {
            int cvp = civ_chooser_preview_;
            int lead = std::clamp(civ_chooser_leader_, 0, 2);
            by += 4;
            text_small_.draw("LEADER", prev_x, by, {255, 215, 120, 255}, 12);
            by += 16;
            SDL_Rect face_r{prev_x, by, 46, 46};
            if (atlas_.meta("spr_leaders")) atlas_.draw_in_rect(face_r, "spr_leaders", cvp * 3 + lead, 1);
            int arrow_y = by + 4;
            SDL_Rect la{prev_x + 54, arrow_y, 20, 20};
            SDL_Rect ra{prev_x + bullet_max_w - 20, arrow_y, 20, 20};
            draw_button(renderer, la, "<", "cycle_leader", -1, {255, 215, 120, 255});
            draw_button(renderer, ra, ">", "cycle_leader", 1, {255, 215, 120, 255});
            std::string lname = ww::menu::leader_name(cvp, lead);
            int name_x = la.x + la.w + 4, name_max = std::max(10, (ra.x - 4) - name_x);
            int lsize = fixed_fit_size(text_, {lname}, name_max, 14, 9);
            int tw = 0, th = 0;
            text_.measure(lname, lsize, tw, th);
            text_.draw(lname, name_x + (name_max - tw) / 2, arrow_y + (20 - th) / 2, {255, 255, 255, 255}, lsize);
            // Leader bonus in a distinct green so it reads as the leader's line,
            // not another civ bullet.
            int bonus_y = by + 50;
            for (auto& line : wrap_text(text_, "+ " + ww::menu::leader_bonuses()[cvp][lead], kBulletSize, bullet_max_w)) {
                text_.draw(line, prev_x, bonus_y, {170, 235, 150, 255}, kBulletSize);
                bonus_y += 20;
            }
            by = bonus_y + 8;
        }
        // Unique units + technologies this civ fields, each with its catalog
        // icon and display name (see civ_data.h's civ_unique_items).
        const auto& uniques = ww::menu::civ_unique_items(civ_chooser_preview_);
        if (!uniques.empty()) {
            const auto& cat = data_.catalog();
            // Two labelled groups: "Unique Units" then the units, "Unique
            // Technologies" then the techs -- each row is just an icon + display
            // name (no "(unit)"/"(tech)" tag; the header already says which).
            auto draw_group = [&](const char* header, bool want_unit) {
                bool any = false;
                for (auto& [nm, is_unit] : uniques)
                    if (is_unit == want_unit) { any = true; break; }
                if (!any) return;
                by += 6;
                text_.draw(header, prev_x, by, {255, 215, 120, 255}, kBulletSize);
                by += 22;
                const char* section = want_unit ? "units" : "techs";
                for (auto& [nm, is_unit] : uniques) {
                    if (is_unit != want_unit) continue;
                    std::string icon, disp = nm;
                    if (cat.contains(section) && cat.at(section).contains(nm)) {
                        icon = cat.at(section).at(nm).value("icon_sprite", "");
                        disp = cat.at(section).at(nm).value("display", nm);
                    }
                    int ic = 20;
                    if (!icon.empty() && atlas_.meta(icon)) {
                        SDL_Rect ir{prev_x, by, ic, ic};
                        atlas_.draw_in_rect(ir, icon, 0, 0);
                    }
                    text_.draw(disp, prev_x + ic + 6, by + 3, {225, 225, 225, 255}, 13);
                    by += 24;
                }
            };
            draw_group("Unique Units", true);
            draw_group("Unique Technologies", false);
        }
    } else {
        text_.draw("Random", prev_x, kPanel_.y + 30, {255, 255, 255, 255}, 24);
    }

    SDL_Rect select_btn{kPanel_.x + kPanel_.w - 150, kPanel_.y + kPanel_.h - 60, 130, 44};
    draw_button(renderer, select_btn, "SELECT", "select_civ", 0, {220, 40, 40, 255});
}

void MenuController::draw_options(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);

    const std::string title = "Options";
    int tw, th;
    text_.measure(title, 28, tw, th);
    text_.draw(title, kPanel_.x + (kPanel_.w - tw) / 2, kPanel_.y + 24, {255, 255, 255, 255}, 28);

    SDL_Rect hotkeys_btn{kPanel_.x + (kPanel_.w - 260) / 2, kPanel_.y + 120, 260, 60};
    draw_button(renderer, hotkeys_btn, "Hotkeys", "hotkeys");

    SDL_Rect graphics_btn{kPanel_.x + (kPanel_.w - 260) / 2, kPanel_.y + 200, 260, 60};
    draw_button(renderer, graphics_btn, "Graphics", "graphics");

    SDL_Rect audio_btn{kPanel_.x + (kPanel_.w - 260) / 2, kPanel_.y + 280, 260, 60};
    draw_button(renderer, audio_btn, "Audio", "audio");
}

void MenuController::draw_graphics_options(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);

    const std::string title = "Graphics";
    int tw, th;
    text_.measure(title, 28, tw, th);
    text_.draw(title, kPanel_.x + (kPanel_.w - tw) / 2, kPanel_.y + 24, {255, 255, 255, 255}, 28);

    // ---- Resolution: a single row (label + one button per kResolutions
    // entry, side by side) instead of a stack of full-width buttons, so
    // more Graphics settings can each get their own row below this one
    // without the screen growing without bound. Button width is measured
    // per-label rather than fixed, so a longer label added to kResolutions
    // later just takes the width it needs.
    const int row_y = kPanel_.y + 70;
    const std::string label = "Resolution:";
    int lw, lh;
    text_.measure(label, 16, lw, lh);
    text_.draw(label, kPanel_.x + 20, row_y, {220, 220, 220, 255}, 16);

    const int btn_h = 34, btn_pad_x = 16, btn_gap = 10;
    int bx = kPanel_.x + 20 + lw + 14;
    for (size_t i = 0; i < std::size(kResolutions); ++i) {
        bool sel = (settings_.resolution_index == static_cast<int>(i));
        int btw, bth;
        text_.measure(kResolutions[i].label, 16, btw, bth);
        SDL_Rect btn{bx, row_y - (btn_h - lh) / 2, btw + btn_pad_x * 2, btn_h};
        draw_button(renderer, btn, kResolutions[i].label, "set_resolution", static_cast<int>(i),
                   sel ? SDL_Color{60, 220, 60, 255} : SDL_Color{255, 255, 255, 255});
        bx += btn.w + btn_gap;
    }

    // ---- Start Stress Test, pinned to the bottom -- moved here from the
    // Random Map Setup screen (was a toggle that also silently jumped the
    // player's own Players/Map Size/Max pop/Reveal choices to 8/Huge/200/
    // Revealed as a side effect). Now a standalone one-shot preview launched
    // directly from here with its own fixed settings (see handle_click's
    // "start_stress_test" and build_settings()'s stress_test_ branch) --
    // doesn't touch the player's Random Map Setup state at all anymore, and
    // sits next to the resolution picker it's mainly useful for sanity-
    // checking (a pure AI-vs-AI spectator preview -- see GameClient::set_
    // spectator -- that auto-returns after 30s, Enter, Escape, or the
    // pause menu's Quit Game; see app.cpp's stress_test_requested handling
    // in the game loop).
    SDL_Rect stress_btn{kPanel_.x + (kPanel_.w - 260) / 2, kPanel_.y + kPanel_.h - 74, 260, 50};
    draw_button(renderer, stress_btn, "Start Stress Test", "start_stress_test");
}

void MenuController::draw_audio_options(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);

    const std::string title = "Audio";
    int tw, th;
    text_.measure(title, 28, tw, th);
    text_.draw(title, kPanel_.x + (kPanel_.w - tw) / 2, kPanel_.y + 24, {255, 255, 255, 255}, 28);

    // One row per slider: a label, a horizontal track/thumb (click anywhere
    // on the track to jump straight to that value -- same click-to-jump
    // convention the Hotkeys screen's vertical scrollbar uses, see
    // hotkeys_scroll_track), then a percentage readout. A local lambda
    // since SFX/Music are otherwise identical rows.
    auto slider_row = [&](int row_y, const std::string& label, double value, const char* track_id) {
        int lw, lh;
        text_.measure(label, 16, lw, lh);
        text_.draw(label, kPanel_.x + 20, row_y, {220, 220, 220, 255}, 16);

        int track_h = 10;
        SDL_Rect track{kPanel_.x + 180, row_y + (lh - track_h) / 2, 260, track_h};
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderFillRect(renderer, &track);
        SDL_SetRenderDrawColor(renderer, 120, 120, 130, 255);
        SDL_RenderDrawRect(renderer, &track);

        int thumb_w = 10;
        int thumb_x = track.x + static_cast<int>((track.w - thumb_w) * std::clamp(value, 0.0, 1.0));
        SDL_Rect thumb{thumb_x, track.y - 3, thumb_w, track_h + 6};
        SDL_SetRenderDrawColor(renderer, 60, 220, 60, 255);
        SDL_RenderFillRect(renderer, &thumb);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawRect(renderer, &thumb);

        char pct[8];
        std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(std::lround(value * 100.0)));
        int pw, ph;
        text_.measure(pct, 14, pw, ph);
        text_.draw(pct, track.x + track.w + 14, row_y + (lh - ph) / 2, {220, 220, 220, 255}, 14);

        // Registered last (drawn on top means hit-tested first isn't a
        // concern here -- these tracks never overlap anything else), same
        // "push a hit rect per drawn widget" convention every other
        // clickable thing on this screen already follows.
        hit_rects_.push_back({track, track_id, 0});
    };

    slider_row(kPanel_.y + 80, "SFX Volume:", settings_.sfx_volume, "set_sfx_volume");
    slider_row(kPanel_.y + 120, "Music Volume:", settings_.music_volume, "set_music_volume");
}

// Building catalog keys, in the exact order Settings::default_building_
// keys() lists them (settings_.building_keys stays in that order across
// load/save since it's a vector, not a map) -- paired here with a
// friendlier display name for the options screen ("base" -> "Town Center").
namespace {
const char* building_display_name(const std::string& key) {
    if (key == "base") return "Town Center";
    if (key == "barracks") return "Barracks";
    if (key == "academy") return "Academy";
    if (key == "factory") return "Factory";
    if (key == "fortress") return "Fortress";
    if (key == "market") return "Market";
    if (key == "refinery") return "Refinery";
    if (key == "university") return "University";
    if (key == "shipyard") return "Shipyard";
    if (key == "airbase") return "Airbase";
    // The rest only ever appear in Villager Commands (settings_.
    // construction_keys), never building_keys -- BUILDABLE's remaining
    // entries.
    if (key == "house") return "House";
    if (key == "farm") return "Farm";
    if (key == "tower") return "Tower";
    if (key == "outpost") return "Outpost";
    if (key == "palisade") return "Palisade";
    if (key == "nuclear reactor") return "Nuclear Reactor";
    return key.c_str();
}
} // namespace

void MenuController::draw_hotkeys_options(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);

    const std::string title = "Hotkeys";
    int tw, th;
    text_.measure(title, 24, tw, th);
    text_.draw(title, kPanel_.x + (kPanel_.w - tw) / 2, kPanel_.y + 8, {255, 255, 255, 255}, 24);

    // Grid/Classic preset buttons: each one-click-overwrites EVERY hotkey
    // below with that preset's full defaults (Settings::apply_preset) --
    // individual rebinding still works fine afterward, same as today.
    // Tucked in the top-right corner, clear of the centered title/status
    // text either side of it.
    const int preset_btn_w = 70;
    SDL_Rect grid_preset_r{kPanel_.x + kPanel_.w - 2 * preset_btn_w - 6 - 8, kPanel_.y + 8, preset_btn_w, 22};
    SDL_Rect classic_preset_r{kPanel_.x + kPanel_.w - preset_btn_w - 8, kPanel_.y + 8, preset_btn_w, 22};
    std::string layout_label = "Layout:";
    text_.measure(layout_label, 14, tw, th);
    text_.draw(layout_label, grid_preset_r.x - tw - 10, grid_preset_r.y + (grid_preset_r.h - th) / 2,
              {200, 200, 220, 255}, 14);
    draw_button(renderer, grid_preset_r, "Grid", "apply_preset_grid", 0, {120, 170, 255, 255});
    draw_button(renderer, classic_preset_r, "Classic", "apply_preset_classic", 0, {120, 170, 255, 255});

    // Status line, always reserved (even blank) so nothing else shifts
    // depending on whether a rebind is in progress.
    if (rebinding_.kind != RebindTarget::Kind::None) {
        std::string msg = "Press a key... (Esc to cancel)";
        text_.measure(msg, 13, tw, th);
        text_.draw(msg, kPanel_.x + (kPanel_.w - tw) / 2, kPanel_.y + 34, {255, 220, 60, 255}, 13);
    }

    // Everything below scrolls -- the full per-building unit/tech listing
    // (Units & Research) is far too much content to fit the panel at once,
    // unlike the three sections above it used to be on their own. Clipped
    // to the panel body so scrolled-past rows don't paint over the fixed
    // header above; `visible_row` additionally skips pushing a hit-rect for
    // any row currently scrolled out of the clipped band, so it can't still
    // catch a click meant for the header controls drawn over it.
    const int content_top = kPanel_.y + kHotkeysContentTopOffset;
    const int content_bottom = kPanel_.y + kPanel_.h - kHotkeysContentBottomMargin;
    SDL_Rect clip{kPanel_.x, content_top, kPanel_.w, content_bottom - content_top};
    SDL_RenderSetClipRect(renderer, &clip);
    int y = content_top - static_cast<int>(hotkeys_scroll_);

    auto visible_row = [&](int row_y, int row_h) {
        return row_y + row_h > content_top && row_y < content_bottom;
    };

    // A key button: label black-panel box on the left, current binding as
    // a smaller clickable box on the right that starts listening for a
    // rebind when clicked -- gold while it's the one waiting, red if
    // `conflicted` (caller-computed -- building_keys, item_keys/
    // construction_keys, and the bare single-field kinds are each scored by
    // a DIFFERENT, correctly-scoped function; see key_conflicts/
    // building_key_conflicts/item_key_conflicts/construction_key_conflicts).
    // Rebinding no longer auto-resolves collisions, just flags them. The
    // key label is whatever Settings::key_label(Hotkey) renders (a bare
    // "Q" or a "Ctrl+Q") -- shrinks to a smaller font if that's too wide
    // for the box, same auto-shrink idea as draw_button.
    auto key_row = [&](int x, int row_y, int w, const std::string& label, Hotkey key,
                       const std::string& rebind_id, int arg, bool listening, bool conflicted) {
        if (!visible_row(row_y, 26)) return;
        int key_w = 64;
        SDL_Rect label_r{x, row_y, w - key_w - 4, 26};
        draw_panel(renderer, label_r);
        text_.measure(label, 12, tw, th);
        text_.draw(label, label_r.x + 6, label_r.y + (label_r.h - th) / 2, {255, 255, 255, 255}, 12);

        conflicted = conflicted && !listening;
        SDL_Color color = listening ? SDL_Color{255, 220, 60, 255}
                          : conflicted ? SDL_Color{255, 80, 80, 255}
                                       : SDL_Color{255, 255, 255, 255};
        SDL_Rect key_r{x + w - key_w, row_y, key_w, 26};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &key_r);
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderDrawRect(renderer, &key_r);
        std::string key_text = Settings::key_label(key);
        int key_size = 12;
        text_.measure(key_text, key_size, tw, th);
        if (tw > key_w - 6) { key_size = 10; text_.measure(key_text, key_size, tw, th); }
        text_.draw(key_text, key_r.x + (key_r.w - tw) / 2, key_r.y + (key_r.h - th) / 2, color, key_size);
        hit_rects_.push_back({key_r, rebind_id, arg});
    };

    // Same as key_row, but for one settings_.item_keys[flat_index] entry --
    // conflict-checked against just its OWN building group (item_key_
    // conflicts), not globally, since every group's own Q/W/E/... defaults
    // deliberately repeat across buildings that never show at once.
    auto item_row = [&](int x, int row_y, int w, const std::string& label, size_t flat_index, bool listening) {
        if (!visible_row(row_y, 26)) return;
        Hotkey key = settings_.item_keys[flat_index].second;
        int key_w = 64;
        SDL_Rect label_r{x, row_y, w - key_w - 4, 26};
        draw_panel(renderer, label_r);
        text_.measure(label, 11, tw, th);
        text_.draw(label, label_r.x + 6, label_r.y + (label_r.h - th) / 2, {255, 255, 255, 255}, 11);

        bool conflicted = !listening && item_key_conflicts(flat_index);
        SDL_Color color = listening ? SDL_Color{255, 220, 60, 255}
                          : conflicted ? SDL_Color{255, 80, 80, 255}
                                       : SDL_Color{255, 255, 255, 255};
        SDL_Rect key_r{x + w - key_w, row_y, key_w, 26};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &key_r);
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderDrawRect(renderer, &key_r);
        std::string key_text = Settings::key_label(key);
        int key_size = 12;
        text_.measure(key_text, key_size, tw, th);
        if (tw > key_w - 6) { key_size = 10; text_.measure(key_text, key_size, tw, th); }
        text_.draw(key_text, key_r.x + (key_r.w - tw) / 2, key_r.y + (key_r.h - th) / 2, color, key_size);
        hit_rects_.push_back({key_r, "rebind_item", static_cast<int>(flat_index)});
    };

    // ---- Unit Actions: 2 columns x 3 rows (6 items) -- command-card kinds
    // that aren't build/train/tech/trade (identity-keyed elsewhere) and
    // aren't specific to one particular building (Shipyard's Next Page and
    // Airbase's Build Nuke live in their own Units & Research groups
    // instead). Each is a single dedicated key, not positional -- there's no
    // Command Card grid anymore (see GameClient::handle_hotkey).
    text_.draw("Unit Actions", kPanel_.x + 20, y, {200, 200, 220, 255}, 14);
    y += 18;
    const int o_col_w = 260;
    const int o_x[2] = {kPanel_.x + 20, kPanel_.x + 20 + o_col_w + 16};
    const int o_row_h = 28;
    key_row(o_x[0], y, o_col_w, "Formation: Column", settings_.formation_column_key,
           "rebind_formation_column", 0, rebinding_.kind == RebindTarget::Kind::FormationColumn,
           key_conflicts(RebindTarget::Kind::FormationColumn));
    key_row(o_x[1], y, o_col_w, "Formation: Box", settings_.formation_box_key,
           "rebind_formation_box", 0, rebinding_.kind == RebindTarget::Kind::FormationBox,
           key_conflicts(RebindTarget::Kind::FormationBox));
    key_row(o_x[0], y + o_row_h, o_col_w, "Formation: Stagger", settings_.formation_stagger_key,
           "rebind_formation_stagger", 0, rebinding_.kind == RebindTarget::Kind::FormationStagger,
           key_conflicts(RebindTarget::Kind::FormationStagger));
    key_row(o_x[1], y + o_row_h, o_col_w, "Land", settings_.land_key,
           "rebind_land", 0, rebinding_.kind == RebindTarget::Kind::Land,
           key_conflicts(RebindTarget::Kind::Land));
    key_row(o_x[0], y + 2 * o_row_h, o_col_w, "Unload", settings_.unload_key,
           "rebind_unload", 0, rebinding_.kind == RebindTarget::Kind::Unload,
           key_conflicts(RebindTarget::Kind::Unload));
    key_row(o_x[1], y + 2 * o_row_h, o_col_w, "Formation: Split", settings_.formation_split_key,
           "rebind_formation_split", 0, rebinding_.kind == RebindTarget::Kind::FormationSplit,
           key_conflicts(RebindTarget::Kind::FormationSplit));
    y += 3 * o_row_h + 14; // keep in sync with kHotkeysOtherSectionH

    // ---- Buildings: cycles/selects an EXISTING building of that type on
    // the map (2 columns x 5 rows), Ctrl+<letter> by default -- but that's
    // just a default; any entry can be rebound to drop Ctrl or use a
    // different key entirely. Distinct from Villager Commands below, which
    // constructs a NEW one.
    text_.draw("Buildings", kPanel_.x + 20, y, {200, 200, 220, 255}, 14);
    y += 18;
    const int b_col_w = 260;
    const int b_x[2] = {kPanel_.x + 20, kPanel_.x + 20 + b_col_w + 16};
    const int b_row_h = 28;
    for (size_t i = 0; i < settings_.building_keys.size(); ++i) {
        int col = static_cast<int>(i) / 5, row = static_cast<int>(i) % 5;
        int x = b_x[col], ry = y + row * b_row_h;
        bool listening = rebinding_.kind == RebindTarget::Kind::Building && rebinding_.index == static_cast<int>(i);
        key_row(x, ry, b_col_w, "Select " + std::string(building_display_name(settings_.building_keys[i].first)),
               settings_.building_keys[i].second, "rebind_building", static_cast<int>(i), listening,
               building_key_conflicts(i));
    }
    y += 5 * b_row_h + 14; // keep in sync with kHotkeysBuildingsSectionH

    // ---- Camera & Actions: 2 columns x 3 rows -- idle-villager cycling,
    // attack-move, and the continuous camera-pan keys (app.cpp).
    text_.draw("Camera & Actions", kPanel_.x + 20, y, {200, 200, 220, 255}, 14);
    y += 18;
    const int c_col_w = 260;
    const int c_x[2] = {kPanel_.x + 20, kPanel_.x + 20 + c_col_w + 16};
    const int c_row_h = 28;
    key_row(c_x[0], y, c_col_w, "Idle Vill.", settings_.idle_villager_key,
           "rebind_idle", 0, rebinding_.kind == RebindTarget::Kind::Idle,
           key_conflicts(RebindTarget::Kind::Idle));
    key_row(c_x[1], y, c_col_w, "Atk Move", settings_.attack_move_key,
           "rebind_attack_move", 0, rebinding_.kind == RebindTarget::Kind::AttackMove,
           key_conflicts(RebindTarget::Kind::AttackMove));
    key_row(c_x[0], y + c_row_h, c_col_w, "Pan Up", settings_.pan_up_key,
           "rebind_pan_up", 0, rebinding_.kind == RebindTarget::Kind::PanUp,
           key_conflicts(RebindTarget::Kind::PanUp));
    key_row(c_x[1], y + c_row_h, c_col_w, "Pan Down", settings_.pan_down_key,
           "rebind_pan_down", 0, rebinding_.kind == RebindTarget::Kind::PanDown,
           key_conflicts(RebindTarget::Kind::PanDown));
    key_row(c_x[0], y + 2 * c_row_h, c_col_w, "Pan Left", settings_.pan_left_key,
           "rebind_pan_left", 0, rebinding_.kind == RebindTarget::Kind::PanLeft,
           key_conflicts(RebindTarget::Kind::PanLeft));
    key_row(c_x[1], y + 2 * c_row_h, c_col_w, "Pan Right", settings_.pan_right_key,
           "rebind_pan_right", 0, rebinding_.kind == RebindTarget::Kind::PanRight,
           key_conflicts(RebindTarget::Kind::PanRight));
    y += 3 * c_row_h + 14; // keep in sync with kHotkeysCameraSectionH

    // ---- Villager Commands: opening a build category (Build Eco/Build
    // Mil., which vary by hotkey preset -- see GameClient::handle_hotkey),
    // paging through it (Next Page), and each building's OWN construction
    // key (settings_.construction_keys -- a separate binding from its
    // map-select key above, defaulting to its slot in the eco/military
    // build list, no Ctrl).
    text_.draw("Villager Commands", kPanel_.x + 20, y, {200, 200, 220, 255}, 14);
    y += kHotkeysUnitsHeaderH;
    const int v_col_w = 260;
    const int v_x[2] = {kPanel_.x + 20, kPanel_.x + 20 + v_col_w + 16};
    key_row(v_x[0], y, v_col_w, "Build Eco", settings_.build_eco_key,
           "rebind_build_eco", 0, rebinding_.kind == RebindTarget::Kind::BuildEco,
           key_conflicts(RebindTarget::Kind::BuildEco));
    key_row(v_x[1], y, v_col_w, "Build Mil.", settings_.build_military_key,
           "rebind_build_military", 0, rebinding_.kind == RebindTarget::Kind::BuildMilitary,
           key_conflicts(RebindTarget::Kind::BuildMilitary));
    key_row(v_x[0], y + kHotkeysGroupRowH, v_col_w, "Next Page", settings_.build_back_key,
           "rebind_build_back", 0, rebinding_.kind == RebindTarget::Kind::BuildBack,
           key_conflicts(RebindTarget::Kind::BuildBack));
    for (size_t i = 0; i < settings_.construction_keys.size(); ++i) {
        size_t flat = i + 3; // after Build Eco/Build Mil./Next Page above
        int col = static_cast<int>(flat) % 2, row = static_cast<int>(flat) / 2;
        bool listening = rebinding_.kind == RebindTarget::Kind::Construction && rebinding_.index == static_cast<int>(i);
        key_row(v_x[col], y + row * kHotkeysGroupRowH, v_col_w,
               building_display_name(settings_.construction_keys[i].first), settings_.construction_keys[i].second,
               "rebind_construction", static_cast<int>(i), listening, construction_key_conflicts(settings_.construction_keys[i].first));
    }
    {
        int villager_items = 3 + static_cast<int>(settings_.construction_keys.size());
        int villager_rows = (villager_items + 1) / 2;
        y += villager_rows * kHotkeysGroupRowH + kHotkeysGroupGap; // keep in sync with hotkeys_max_scroll
    }

    // ---- Units & Research: every trainable unit and researchable tech in
    // the game, grouped by the building that offers it (ww::hotkeys::
    // building_groups), 2 columns per group. Fixed per-item identity keys,
    // not positional -- see Settings::item_keys and GameClient::
    // handle_hotkey's comment on why train/tech buttons no longer use the
    // Command Card grid above.
    text_.draw("Units & Research", kPanel_.x + 20, y, {200, 200, 220, 255}, 14);
    y += kHotkeysUnitsHeaderH;
    const int g_col_w = 260;
    const int g_x[2] = {kPanel_.x + 20, kPanel_.x + 20 + g_col_w + 16};
    size_t flat = 0;
    for (auto& group : ww::hotkeys::building_groups()) {
        if (visible_row(y, 16)) {
            text_.draw(building_display_name(group.building), kPanel_.x + 20, y, {180, 205, 240, 255}, 13);
        }
        y += kHotkeysGroupHeaderH;
        for (size_t i = 0; i < group.items.size(); ++i) {
            int col = static_cast<int>(i) % 2, row = static_cast<int>(i) / 2;
            bool listening = rebinding_.kind == RebindTarget::Kind::Item && rebinding_.index == static_cast<int>(flat);
            item_row(g_x[col], y + row * kHotkeysGroupRowH, g_col_w, title_case(group.items[i].item), flat, listening);
            ++flat;
        }
        int rows = (static_cast<int>(group.items.size()) + 1) / 2;
        // Shipyard's page toggle and airbase's nuke-stockpile button live
        // here, right alongside their own units & techs, rather than in
        // the generic Unit Actions section.
        if (group.building == "shipyard") {
            key_row(g_x[0], y + rows * kHotkeysGroupRowH, g_col_w, "Next Page", settings_.shipyard_page_key,
                   "rebind_shipyard_page", 0, rebinding_.kind == RebindTarget::Kind::ShipyardPage,
                   key_conflicts(RebindTarget::Kind::ShipyardPage));
            ++rows;
        } else if (group.building == "airbase") {
            key_row(g_x[0], y + rows * kHotkeysGroupRowH, g_col_w, "Build Nuke", settings_.build_nuke_key,
                   "rebind_build_nuke", 0, rebinding_.kind == RebindTarget::Kind::BuildNuke,
                   key_conflicts(RebindTarget::Kind::BuildNuke));
            ++rows;
        }
        y += rows * kHotkeysGroupRowH + kHotkeysGroupGap;
    }

    SDL_RenderSetClipRect(renderer, nullptr);

    // Scrollbar: a slim track along the panel's right edge, thumb sized to
    // the visible fraction of the content and positioned to match
    // hotkeys_scroll_. Clicking anywhere on the track (not just dragging
    // the thumb) jumps the scroll proportionally to where you clicked.
    {
        double max_scroll = hotkeys_max_scroll(settings_.construction_keys.size(), kPanel_.h);
        SDL_Rect track{kPanel_.x + kPanel_.w - 14, content_top, 10, content_bottom - content_top};
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderFillRect(renderer, &track);
        SDL_SetRenderDrawColor(renderer, 120, 120, 130, 255);
        SDL_RenderDrawRect(renderer, &track);
        if (max_scroll > 0) {
            double content_h = track.h + max_scroll; // total scrollable height, visible + hidden
            int thumb_h = std::max(20, static_cast<int>(track.h * (track.h / content_h)));
            int thumb_travel = track.h - thumb_h;
            int thumb_y = track.y + static_cast<int>(thumb_travel * (hotkeys_scroll_ / max_scroll));
            SDL_Rect thumb{track.x, thumb_y, track.w, thumb_h};
            SDL_SetRenderDrawColor(renderer, 200, 200, 210, 255);
            SDL_RenderFillRect(renderer, &thumb);
        }
        hit_rects_.push_back({track, "hotkeys_scroll_track", 0});
    }
}

// Browsing-only, per the user's own scoping of this request ("obviously i
// wont be able to actually play yet"): lists campaigns_ (loaded on demand
// when "Campaign" is clicked, see handle_click), then a per-campaign
// Europe-map view with a dot per level, then a level info popup. No
// "start mission" button exists yet -- that's future work once campaigns
// carry actual playable content (starting units/buildings/win-lose
// conditions), not just metadata.
void MenuController::draw_campaign_list(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);
    std::string title = "Campaigns";
    int tw, th;
    text_.measure(title, 26, tw, th);
    text_.draw(title, kPanel_.x + (kPanel_.w - tw) / 2, kPanel_.y + 12, {255, 255, 255, 255}, 26);

    int row_y = kPanel_.y + 56;
    const int row_h = 46;
    for (int i = 0; i < static_cast<int>(campaigns_.size()); ++i) {
        const auto& c = campaigns_[i];
        SDL_Rect row{kPanel_.x + 20, row_y, kPanel_.w - 40, row_h - 6};
        draw_panel(renderer, row);

        std::string civ_name = (c.civ >= 0 && c.civ < 9) ? ww::menu::civ_names()[c.civ] : "?";
        if (atlas_.meta("spr_flags_mini")) {
            atlas_.draw("spr_flags_mini", row.x + 6, row.y + (row.h - 32) / 2, ww::menu::kCivFlagFrame[c.civ]);
        }
        text_.draw(c.name, row.x + 60, row.y + 4, {255, 255, 255, 255}, 16);
        text_small_.draw(civ_name, row.x + 60, row.y + 24, {200, 200, 200, 255}, 12);

        int completed = 0;
        for (auto& lvl : c.levels) {
            if (settings_.completed_campaign_levels.count(campaign_level_key(c.name, lvl.id))) ++completed;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d level%s * %d completed", static_cast<int>(c.levels.size()),
                     c.levels.size() == 1 ? "" : "s", completed);
        text_small_.draw(buf, row.x + row.w - 200, row.y + (row.h - 13) / 2, {180, 200, 255, 255}, 13);

        hit_rects_.push_back({row, "open_campaign", i});
        row_y += row_h;
    }
    if (campaigns_.empty()) {
        text_small_.draw("No campaigns installed yet.", kPanel_.x + 20, row_y + 10, {180, 180, 180, 255}, 14);
    }
}

void MenuController::draw_campaign_map(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);
    if (selected_campaign_ < 0 || selected_campaign_ >= static_cast<int>(campaigns_.size())) return;
    const auto& c = campaigns_[selected_campaign_];

    // Civ flag top-left, campaign name centred next to/above it -- the
    // civ name itself is dropped from the title text since the flag
    // already conveys it (previously "<name> (<civ>)").
    if (atlas_.meta("spr_flags_mini")) {
        SDL_Rect flag_r{kPanel_.x + 6, kPanel_.y + 4, 30, 20};
        atlas_.draw_in_rect(flag_r, "spr_flags_mini", ww::menu::kCivFlagFrame[std::clamp(c.civ, 0, 8)]);
    }
    int tw, th;
    text_.measure(c.name, 18, tw, th);
    text_.draw(c.name, kPanel_.x + (kPanel_.w - tw) / 2, kPanel_.y + 6, {255, 255, 255, 255}, 18);

    // Fixed two-column layout (map left, level info right) whether or not
    // a level is currently selected -- avoids the map jumping size/
    // position (and its markers' screen coordinates along with it) the
    // moment a level gets clicked. Same fractional (0..1) loc_x/loc_y
    // scheme the campaign editor writes (see campaign_data.h) -- native
    // ~1.25:1 spr_europe_map aspect ratio. Narrower than the original pass
    // (372 wide) but not as cramped as the very next one (300) -- the
    // Allies/Enemies flag boxes below are now packed tightly enough that
    // they don't need as much width as they first did.
    SDL_Rect map_r{kPanel_.x + 14, kPanel_.y + 36, 350, 280};
    draw_panel(renderer, map_r);
    if (atlas_.meta("spr_europe_map")) atlas_.draw_stretched("spr_europe_map", map_r, 0);

    // Level 1 (index 0) is always unlocked; level i (i>0) unlocks once
    // the level immediately before it is in completed_campaign_levels
    // (see campaign_level_key, settings.h, and GameClient::update's
    // game-over handling, which is what actually inserts into that set).
    for (int i = 0; i < static_cast<int>(c.levels.size()); ++i) {
        const auto& lvl = c.levels[i];
        bool unlocked =
            (i == 0) || settings_.completed_campaign_levels.count(campaign_level_key(c.name, c.levels[i - 1].id));
        int dx = map_r.x + static_cast<int>(lvl.loc_x * map_r.w);
        int dy = map_r.y + static_cast<int>(lvl.loc_y * map_r.h);
        bool selected = (i == selected_level_);
        double scale = 1.6;
        if (atlas_.meta("spr_map_icon")) {
            SDL_Color tint = unlocked ? SDL_Color{255, 255, 255, 255} : SDL_Color{95, 95, 95, 255};
            atlas_.draw("spr_map_icon", dx, dy, selected ? 1 : 0, scale, 0.0, false, 255, tint);
        } else {
            SDL_SetRenderDrawColor(renderer, unlocked ? 255 : 90, 40, 40, 255);
            SDL_Rect dot{dx - 5, dy - 5, 10, 10};
            SDL_RenderFillRect(renderer, &dot);
        }
        // "Level N" numbering is 1-based array order -- no separate
        // numbering field exists (or is needed) on Level itself. A cheap
        // 1px drop-shadow (black offset, then white on top) keeps the
        // digit legible against whatever's under it on the map art.
        std::string num = std::to_string(i + 1);
        int ntw, nth;
        text_.measure(num, 11, ntw, nth);
        text_.draw(num, dx - ntw / 2 + 1, dy - nth / 2 + 1, {0, 0, 0, 220}, 11);
        text_.draw(num, dx - ntw / 2, dy - nth / 2, unlocked ? SDL_Color{255, 255, 255, 255} : SDL_Color{170, 170, 170, 255}, 11);
        // Locked levels get no hit-rect at all -- simplest correct way to
        // make them inert, no separate "is this locked" guard needed in
        // handle_click.
        if (unlocked) {
            SDL_Rect hit{dx - 11, dy - 11, 22, 22};
            hit_rects_.push_back({hit, "campaign_level_dot", i});
        }
    }

    // Description sits below the map, spanning its width.
    if (selected_level_ < 0 || selected_level_ >= static_cast<int>(c.levels.size())) {
        text_small_.draw("Click a level on the map to view its details.", map_r.x, map_r.y + map_r.h + 8,
                         {180, 180, 180, 255}, 12);
        return;
    }
    const auto& lvl = c.levels[selected_level_];
    {
        int top = map_r.y + map_r.h + 8;
        int avail_h = (kPanel_.y + kPanel_.h - 14) - top;
        std::string desc = lvl.description.empty() ? "(no description)" : lvl.description;
        // Auto-fit: pick the largest font (12 down to a readable floor) whose
        // wrapped lines all fit in the space below the map, so a long briefing
        // squeezes in fully instead of being clipped at the panel edge.
        int size = 12, line_h = 14;
        std::vector<std::string> lines;
        for (;; --size) {
            line_h = std::max(7, size + 2); // keep the original 12->14 leading ratio
            lines = wrap_text(text_small_, desc, size, map_r.w);
            // Floor 6: the box below the map is short (~78px), so a long briefing
            // has to go quite small to fit fully rather than clip. Typical
            // briefings settle well above this.
            if (static_cast<int>(lines.size()) * line_h <= avail_h || size <= 6) break;
        }
        int dy2 = top;
        for (auto& line : lines) {
            if (dy2 > kPanel_.y + kPanel_.h - line_h) break; // safety clip once at the floor size
            text_small_.draw(line, map_r.x, dy2, {220, 220, 220, 255}, size);
            dy2 += line_h;
        }
    }

    // ---- right column: Level N + name, Allies/Enemies flag columns,
    // shown (non-hidden) objectives, Start button. ----
    int rx = map_r.x + map_r.w + 12;
    int rw = kPanel_.x + kPanel_.w - rx - 8;
    int ry = kPanel_.y + 36;

    std::string level_heading = "Level " + std::to_string(selected_level_ + 1);
    text_.draw(level_heading, rx, ry, {255, 220, 60, 255}, 14);
    ry += 18;
    std::string lname = lvl.name.empty() ? "(untitled)" : lvl.name;
    for (auto& line : wrap_text(text_, lname, 13, rw)) {
        text_.draw(line, rx, ry, {255, 255, 255, 255}, 13);
        ry += 15;
    }
    ry += 4;
    // Which age the scenario begins in (Level::start_age, 0..3): shown as the
    // era's own icon -- the same spr_era_icon art as the in-game HUD era
    // indicator (frame = era) -- next to its name, so the age reads at a glance.
    static const char* kStartAgeName[4] = {"Victorian", "Industrial", "War", "Scientific"};
    int sa = std::clamp(lvl.start_age, 0, 3);
    text_small_.draw("Starts in:", rx, ry, {150, 220, 150, 255}, 12);
    int stw, sth;
    text_small_.measure("Starts in:", 12, stw, sth);
    int era_icon_x = rx + stw + 6;
    SDL_Rect era_ir{era_icon_x, ry - 3, 18, 18};
    if (atlas_.meta("spr_era_icon")) atlas_.draw_in_rect(era_ir, "spr_era_icon", sa, /*pad=*/0);
    text_small_.draw(std::string(kStartAgeName[sa]) + " Era", era_icon_x + 22, ry, {150, 220, 150, 255}, 12);
    ry += 20;

    // Split into Allies (same team as the fixed P1 slot, players[0]) vs
    // Enemies (everyone else) -- matches the original GML's
    // ally0-vs-everyone-else bucketing, just generalized from a binary
    // flag to team-number equality (see LevelPlayer::team's comment).
    std::vector<int> allies, enemies;
    int p1_team = lvl.players.empty() ? 1 : lvl.players[0].team;
    for (auto& p : lvl.players) (p.team == p1_team ? allies : enemies).push_back(p.civ);

    text_small_.draw("Allies:", rx, ry, {150, 200, 255, 255}, 12);
    int box_y = ry + 16;

    // Flags drawn at spr_flags_mini's own native 48x32 size (not scaled to
    // fit some column width -- the user explicitly wants them full size),
    // packed with just a 2px gap between them inside a white-bordered box
    // (kept visually separate per the user's request) -- a single column
    // when there are 4 or fewer, splitting into a second sub-column once
    // there are more than 4 (per the user's spec). Draws at side_x and
    // returns the box actually used (its width is whatever this side's own
    // flag count needs, NOT a fixed half of the available space, so a
    // small side doesn't waste room the other side could use, and a
    // 5+-flag side isn't squeezed into a fixed half either).
    constexpr int kFlagW = 48, kFlagH = 32, kFlagGap = 2, kBoxPad = 3;
    auto draw_flag_side = [&](int side_x, const std::vector<int>& civs) {
        int n = static_cast<int>(civs.size());
        int cols = (n > 4) ? 2 : 1;
        int rows = std::max(1, (n + cols - 1) / cols);
        int grid_w = cols * kFlagW + (cols - 1) * kFlagGap;
        int grid_h = rows * kFlagH + (rows - 1) * kFlagGap;
        SDL_Rect border{side_x, box_y, grid_w + 2 * kBoxPad, grid_h + 2 * kBoxPad};
        draw_panel(renderer, border);
        for (int i = 0; i < n; ++i) {
            int col = i / rows, row = i % rows;
            SDL_Rect fr{side_x + kBoxPad + col * (kFlagW + kFlagGap), box_y + kBoxPad + row * (kFlagH + kFlagGap),
                       kFlagW, kFlagH};
            if (atlas_.meta("spr_flags_mini")) {
                // draw_in_rect's own `pad` defaults to 4 -- fits the sprite
                // within a further-inset (rect.w-2*pad)x(rect.h-2*pad) area,
                // which was silently shrinking every flag well below fr's
                // actual 48x32 (to ~36x24) and, since each shrunk flag was
                // still centred in its full-size cell, inflating the visual
                // gap between neighbours far past the real kFlagGap. Passing
                // 0 here makes it fit fr exactly -- true native 1:1 size.
                atlas_.draw_in_rect(fr, "spr_flags_mini", ww::menu::kCivFlagFrame[std::clamp(civs[i], 0, 8)], 0);
            }
        }
        return border;
    };
    SDL_Rect allies_box = draw_flag_side(rx, allies);
    int enemies_x = allies_box.x + allies_box.w + 10;
    text_small_.draw("Enemies:", enemies_x, ry, {255, 150, 130, 255}, 12);
    SDL_Rect enemies_box = draw_flag_side(enemies_x, enemies);
    ry = std::max(allies_box.y + allies_box.h, enemies_box.y + enemies_box.h) + 6;

    text_.draw("Objectives", rx, ry, {255, 255, 255, 255}, 13);
    ry += 16;
    int objectives_bottom = kPanel_.y + kPanel_.h - 30; // leave room for the Start button below
    bool any_shown = false;
    for (auto& obj : lvl.objectives) {
        if (obj.hidden) continue; // hidden objectives never show here, see Objective::hidden's comment
        any_shown = true;
        std::string oname = obj.name.empty() ? "(untitled)" : obj.name;
        for (auto& line : wrap_text(text_small_, "* " + oname, 11, rw)) {
            if (ry > objectives_bottom) break; // clip rather than overflow the panel
            text_small_.draw(line, rx, ry, {220, 220, 220, 255}, 11);
            ry += 13;
        }
    }
    if (!any_shown) text_small_.draw("(none)", rx, ry, {150, 150, 150, 255}, 11);
    ry += 16;

    // Briefing image (Level::briefing_image, set in the campaign editor): shown
    // just below the objectives, above the Start button. Loaded lazily from the
    // asset dir; silently skipped if the file isn't present (e.g. the campaign
    // was copied over without its image).
    if (briefing_tex_key_ != lvl.briefing_image) {
        if (briefing_tex_) { SDL_DestroyTexture(briefing_tex_); briefing_tex_ = nullptr; }
        if (!lvl.briefing_image.empty()) {
            std::string full = asset_dir_ + "/" + lvl.briefing_image;
            briefing_tex_ = IMG_LoadTexture(renderer, full.c_str());
        }
        briefing_tex_key_ = lvl.briefing_image;
    }
    if (briefing_tex_) {
        int img_bottom = kPanel_.y + kPanel_.h - 34; // clear of the Start button row
        int avail_h = img_bottom - ry;
        if (avail_h > 24) {
            int tw = 0, th = 0;
            SDL_QueryTexture(briefing_tex_, nullptr, nullptr, &tw, &th);
            SDL_Rect box{rx, ry, rw, avail_h};
            SDL_Rect dst = box;
            if (tw > 0 && th > 0) {
                double s = std::min(static_cast<double>(box.w) / tw, static_cast<double>(box.h) / th);
                dst.w = static_cast<int>(tw * s);
                dst.h = static_cast<int>(th * s);
                dst.x = box.x;
                dst.y = box.y;
            }
            SDL_RenderCopy(renderer, briefing_tex_, nullptr, &dst);
        }
    }

    SDL_Rect start_btn{kPanel_.x + kPanel_.w - 90, kPanel_.y + kPanel_.h - 26, 80, 20};
    draw_button(renderer, start_btn, "Start", "play_level", 0, {60, 220, 60, 255});
}

// Single-Settings-field rebind kinds (everything except Building/
// Construction/Item, which are vectors) -- a small lookup so apply_rebind's
// assignment and key_conflicts' scan don't need a long if-chain apiece.
// Returns nullptr for Building/Construction/Item/None.
Hotkey* MenuController::single_key_field(RebindTarget::Kind kind) {
    switch (kind) {
        case RebindTarget::Kind::Idle: return &settings_.idle_villager_key;
        case RebindTarget::Kind::PanUp: return &settings_.pan_up_key;
        case RebindTarget::Kind::PanDown: return &settings_.pan_down_key;
        case RebindTarget::Kind::PanLeft: return &settings_.pan_left_key;
        case RebindTarget::Kind::PanRight: return &settings_.pan_right_key;
        case RebindTarget::Kind::BuildEco: return &settings_.build_eco_key;
        case RebindTarget::Kind::BuildMilitary: return &settings_.build_military_key;
        case RebindTarget::Kind::AttackMove: return &settings_.attack_move_key;
        case RebindTarget::Kind::FormationColumn: return &settings_.formation_column_key;
        case RebindTarget::Kind::FormationBox: return &settings_.formation_box_key;
        case RebindTarget::Kind::FormationStagger: return &settings_.formation_stagger_key;
        case RebindTarget::Kind::FormationSplit: return &settings_.formation_split_key;
        case RebindTarget::Kind::Land: return &settings_.land_key;
        case RebindTarget::Kind::Unload: return &settings_.unload_key;
        case RebindTarget::Kind::ShipyardPage: return &settings_.shipyard_page_key;
        case RebindTarget::Kind::BuildNuke: return &settings_.build_nuke_key;
        case RebindTarget::Kind::BuildBack: return &settings_.build_back_key;
        default: return nullptr;
    }
}

void MenuController::apply_rebind(Hotkey new_hotkey) {
    // No more auto-swapping a colliding key away -- rebinding just sets
    // the new key outright, even if something else already uses it.
    // draw_hotkeys_options' key_conflicts check flags the result in red
    // instead of silently rewriting a DIFFERENT binding out from under
    // the player.
    if (rebinding_.kind == RebindTarget::Kind::Building) settings_.building_keys[rebinding_.index].second = new_hotkey;
    else if (rebinding_.kind == RebindTarget::Kind::Construction)
        settings_.construction_keys[rebinding_.index].second = new_hotkey;
    else if (rebinding_.kind == RebindTarget::Kind::Item) settings_.item_keys[rebinding_.index].second = new_hotkey;
    else if (Hotkey* f = single_key_field(rebinding_.kind)) *f = new_hotkey;

    settings_.save();
    rebinding_.kind = RebindTarget::Kind::None;
}

bool MenuController::key_conflicts(RebindTarget::Kind kind) {
    Hotkey* self = single_key_field(kind);
    if (!self || self->key == SDLK_UNKNOWN) return false;
    // "Always active" kinds fire regardless of what's selected/open (Idle
    // villager cycling, continuous camera pan), so they can collide with
    // literally anything.
    static const std::set<RebindTarget::Kind> kAlways = {
        RebindTarget::Kind::Idle, RebindTarget::Kind::PanUp, RebindTarget::Kind::PanDown,
        RebindTarget::Kind::PanLeft, RebindTarget::Kind::PanRight,
    };
    // Fires with ONLY civilians selected (build_eco/build_military's own
    // precondition) -- AttackMove and all 4 formation buttons ALSO show in
    // that same state (a civilian selection is never air/attack-ground/
    // nuke-capable, so none of the four ever suppress each other here --
    // see draw_command_card), so all six are real neighbors. Land/Unload
    // never are: a laden transport or aircraft in the selection isn't a
    // civilian, so own_civilian is false whenever either of those could
    // show.
    static const std::set<RebindTarget::Kind> kCivilianOpen = {
        RebindTarget::Kind::BuildEco,       RebindTarget::Kind::BuildMilitary,
        RebindTarget::Kind::AttackMove,     RebindTarget::Kind::FormationColumn,
        RebindTarget::Kind::FormationBox,   RebindTarget::Kind::FormationStagger,
        RebindTarget::Kind::FormationSplit,
    };
    // Fires with a non-civilian unit selected. AttackMove doesn't care what
    // unit type is selected, so it's a real neighbor of everything here.
    // FormationBox/FormationStagger are suppressed only by attack-ground-
    // capable/nuke-carrying units respectively (own_ag/own_nuke) -- NEITHER
    // is related to own_air, so both can still show alongside Land (e.g.
    // 2+ own aircraft, none of them attack-ground-capable) or Unload --
    // real neighbors of both. FormationSplit isn't suppressed by ANYTHING
    // (unlike the other three, its command-card slot (4,1) is never claimed
    // by a more specialized per-unit-type button), so it's a real neighbor
    // of Land and Unload too. FormationColumn is different: it's suppressed
    // by own_air specifically (draw_command_card), the SAME condition that
    // shows Land -- the two are actually mutually exclusive on screen, so
    // FormationColumn is deliberately left OUT of this set (see
    // kUnitOpenNoLand below for its own, narrower membership) rather than
    // flagging a same-key default as a conflict that can never really
    // happen.
    static const std::set<RebindTarget::Kind> kUnitOpen = {
        RebindTarget::Kind::AttackMove,     RebindTarget::Kind::FormationBox,
        RebindTarget::Kind::FormationStagger, RebindTarget::Kind::FormationSplit,
        RebindTarget::Kind::Land, RebindTarget::Kind::Unload,
    };
    // FormationColumn's non-civilian neighbors, MINUS Land -- own_transport
    // (Unload) is unrelated to own_air, so a transport-ship group can still
    // show FormationColumn alongside Unload; only Land is genuinely
    // impossible to see at the same time as FormationColumn.
    static const std::set<RebindTarget::Kind> kUnitOpenNoLand = {
        RebindTarget::Kind::AttackMove, RebindTarget::Kind::FormationColumn, RebindTarget::Kind::Unload,
    };
    // Fires only with a civilian selected AND a build-category list open
    // -- mutually exclusive with the two sets above (both require the
    // list CLOSED) and with building selection (kShipyard/kAirbase)
    // entirely.
    static const std::set<RebindTarget::Kind> kUnitListOpen = {RebindTarget::Kind::BuildBack};
    // Shipyard Page / Build Nuke each fire only with THAT specific
    // building selected -- never both, since selecting a building always
    // means exactly one building, so these two can never collide with
    // each other even if they share a key.
    static const std::set<RebindTarget::Kind> kShipyard = {RebindTarget::Kind::ShipyardPage};
    static const std::set<RebindTarget::Kind> kAirbase = {RebindTarget::Kind::BuildNuke};
    // Two kinds can actually collide if they share ANY one of these
    // contexts (not just the same single group -- AttackMove and several
    // formation kinds deliberately belong to more than one at once).
    static const std::set<RebindTarget::Kind>* kContexts[] = {
        &kCivilianOpen, &kUnitOpen, &kUnitOpenNoLand, &kUnitListOpen, &kShipyard, &kAirbase,
    };
    auto shares_context = [&](RebindTarget::Kind a, RebindTarget::Kind b) {
        for (auto* ctx : kContexts) {
            if (ctx->count(a) && ctx->count(b)) return true;
        }
        return false;
    };
    bool self_always = kAlways.count(kind) > 0;

    static const RebindTarget::Kind kAllSingleKinds[] = {
        RebindTarget::Kind::Idle,       RebindTarget::Kind::PanUp,        RebindTarget::Kind::PanDown,
        RebindTarget::Kind::PanLeft,    RebindTarget::Kind::PanRight,     RebindTarget::Kind::BuildEco,
        RebindTarget::Kind::BuildMilitary, RebindTarget::Kind::AttackMove, RebindTarget::Kind::FormationColumn,
        RebindTarget::Kind::FormationBox, RebindTarget::Kind::FormationStagger, RebindTarget::Kind::FormationSplit,
        RebindTarget::Kind::Land,       RebindTarget::Kind::Unload,       RebindTarget::Kind::ShipyardPage,
        RebindTarget::Kind::BuildNuke,  RebindTarget::Kind::BuildBack,
    };
    int count = 0;
    for (auto k : kAllSingleKinds) {
        if (*single_key_field(k) != *self) continue;
        bool other_always = kAlways.count(k) > 0;
        if (self_always || other_always || shares_context(kind, k)) ++count;
    }
    // "Always active" kinds (Idle villager cycling, continuous camera pan)
    // fire regardless of what's selected or open, so they ALSO conflict
    // with building_keys/construction_keys/item_keys entries sharing their
    // key -- not just the other single-field kinds counted above. (When
    // `kind` itself isn't an "always" kind, this adds nothing: an item/
    // construction/building key sharing ITS key is only a real conflict if
    // that other binding is ALSO always-active, which always_active_
    // conflicts already checks for from their own conflict functions.)
    if (self_always) {
        for (auto& [name, hk] : settings_.building_keys) { (void)name; count += (hk == *self); }
        for (auto& [name, hk] : settings_.construction_keys) { (void)name; count += (hk == *self); }
        for (auto& [id, hk] : settings_.item_keys) { (void)id; count += (hk == *self); }
    }
    return count > 1;
}

bool MenuController::always_active_conflicts(Hotkey hk) {
    if (hk.key == SDLK_UNKNOWN) return false;
    static const RebindTarget::Kind kAlwaysKinds[] = {
        RebindTarget::Kind::Idle, RebindTarget::Kind::PanUp, RebindTarget::Kind::PanDown,
        RebindTarget::Kind::PanLeft, RebindTarget::Kind::PanRight,
    };
    for (auto k : kAlwaysKinds) {
        if (*single_key_field(k) == hk) return true;
    }
    return false;
}

bool MenuController::building_key_conflicts(size_t index) {
    if (index >= settings_.building_keys.size()) return false;
    Hotkey hk = settings_.building_keys[index].second;
    if (hk.key == SDLK_UNKNOWN) return false;
    if (always_active_conflicts(hk)) return true;
    // Scoped to building_keys alone -- every entry lives in the same
    // "building selected on the map" context, so any two sharing both the
    // key AND the Ctrl state genuinely can collide with each other.
    int count = 0;
    for (auto& [name, k] : settings_.building_keys) {
        (void)name;
        count += (k == hk);
    }
    return count > 1;
}

bool MenuController::item_key_conflicts(size_t flat_index) {
    if (flat_index >= settings_.item_keys.size()) return false;
    Hotkey hk = settings_.item_keys[flat_index].second;
    if (hk.key == SDLK_UNKNOWN) return false;
    if (always_active_conflicts(hk)) return true;
    // Scoped to the SAME building group as flat_index -- different
    // buildings' command cards never show at once, so every group
    // independently defaulting to Q/W/E/R/T/... (see ww::hotkeys::
    // building_groups, Settings::default_item_keys) is intentional, not a
    // real conflict. settings_.item_keys is built by iterating
    // building_groups() in this exact order, so flat_index lines up with it.
    size_t start = 0;
    for (auto& group : ww::hotkeys::building_groups()) {
        size_t end = start + group.items.size();
        if (flat_index >= start && flat_index < end) {
            int count = 0;
            for (size_t i = start; i < end; ++i) count += (settings_.item_keys[i].second == hk);
            return count > 1;
        }
        start = end;
    }
    return false;
}

bool MenuController::construction_key_conflicts(const std::string& building) {
    Hotkey hk = settings_.construction_key(building);
    if (hk.key == SDLK_UNKNOWN) return false;
    if (always_active_conflicts(hk)) return true;
    // Same "eco"/"military" split as GameClient::eco_buildings()/
    // military_buildings() -- the two build-category lists never show at
    // once, so e.g. "house" and "barracks" both defaulting to Q is
    // intentional, not a real conflict.
    static const std::set<std::string> kEco = {"house", "farm",       "refinery",        "market",
                                                "university", "nuclear reactor", "base"};
    static const std::set<std::string> kMilitary = {"barracks", "factory",  "airbase", "academy", "shipyard",
                                                     "tower",    "fortress", "outpost", "palisade"};
    const std::set<std::string>& category = kEco.count(building) ? kEco : kMilitary;
    int count = 0;
    for (auto& [name, k] : settings_.construction_keys) {
        if (category.count(name) && k == hk) ++count;
    }
    return count > 1;
}

void MenuController::draw_tech_tree(SDL_Renderer* renderer) {
    // Flat tan background + black divider lines -- deliberately NOT the
    // black-panel-on-bg_menu-photo chrome the other screens share (see
    // draw()'s comment and the header declaration's comment). Same tan as
    // the in-game HUD panel (game_client.cpp's kPanelColor).
    //
    // Unlike every other screen, this one does NOT use kPanel_/kQuitRect_
    // (the resolution-aware chrome) -- it tiles native-sized sprite art
    // (4 era bands of native 640x120 each) to exactly fill a 640x480
    // canvas, matching the original game pixel-for-pixel. At a larger
    // resolution (Options > Graphics), reflowing or stretching this to
    // fill the bigger canvas would need new art or introduce the exact
    // stretching the resolution feature is meant to avoid, so it just
    // stays pinned to native 640x480 in the top-left corner; any extra
    // canvas space beyond that shows as plain black (draw()'s initial
    // SDL_RenderClear), same look as the app's own window letterboxing.
    // Fill the current resolution by laying the whole (native 640x480) tech-tree
    // screen out at a uniform scale S -- every element drawn at S x its native
    // size/position. Unlike the old SDL_RenderSetScale zoom (which linearly
    // blurred the already-letterbox-upscaled canvas -- reported as "stretched"),
    // this re-rasterises TEXT crisply at the larger size and scales sprites once,
    // exactly like every other menu screen. 4:3 into a 4:3 render target, so no
    // aspect distortion. Coordinates are now in render-target space, so
    // handle_click needs no separate remap.
    double S = std::max(1.0, std::min(view_w_ / 640.0, view_h_ / 480.0));
    auto Z = [S](double v) { return static_cast<int>(std::lround(v * S)); };

    constexpr SDL_Color kTan{213, 185, 172, 255};
    SDL_SetRenderDrawColor(renderer, kTan.r, kTan.g, kTan.b, 255);
    SDL_Rect native_bg{0, 0, Z(640), Z(480)};
    SDL_RenderFillRect(renderer, &native_bg);

    // 4 stacked era bands, one per age, tiling the grid area (x>=160).
    bool have_row_sprite = atlas_.meta("spr_tech_tree_row") != nullptr;
    for (int i = 0; i < 4; ++i) {
        SDL_Rect band{Z(160), Z(i * 120), Z(640 - 160), Z(120)};
        if (have_row_sprite) atlas_.draw_stretched("spr_tech_tree_row", band);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawLine(renderer, Z(160), Z((i + 1) * 120) - 1, Z(640), Z((i + 1) * 120) - 1);
    }

    int civ = civ_chooser_preview_;
    if (civ < 0) civ = 0; // defensive -- the button that opens this screen is disabled for Random

    // ---- left sidebar (back arrow, civ flag/name, bonuses), divider from grid ----
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderDrawLine(renderer, Z(160), 0, Z(160), Z(480));

    if (atlas_.meta("spr_button_back")) {
        atlas_.draw("spr_button_back", kBackRect_.x, kBackRect_.y, 0, S);
        hit_rects_.push_back({SDL_Rect{0, 0, Z(32), Z(32)}, "back", 0});
    } else {
        SDL_Rect br{0, 0, Z(32), Z(32)};
        draw_button(renderer, br, "<", "back", 0, {255, 60, 60, 255});
    }
    if (atlas_.meta("spr_flags_mini")) {
        SDL_Rect flag_r{Z(44), Z(8), Z(48), Z(32)};
        atlas_.draw_in_rect(flag_r, "spr_flags_mini", ww::menu::kCivFlagFrame[civ], 0);
    }
    // One consistent size for all 9 names (fit in NATIVE units, then scaled).
    std::vector<std::string> all_names(ww::menu::civ_names().begin(), ww::menu::civ_names().end());
    int name_size = fixed_fit_size(text_, all_names, 152 - 16, 18, 9); // native
    text_.draw(ww::menu::civ_names()[civ], Z(8), Z(44), {0, 0, 0, 255}, Z(name_size));
    text_small_.draw(ww::menu::civ_types()[civ], Z(8), Z(44 + name_size + 4), {60, 60, 60, 255}, Z(10));

    int by = 44 + name_size + 22; // native layout units
    constexpr int kSidebarBulletSize = 11;
    for (auto& bullet : ww::menu::civ_bonuses()[civ]) {
        for (auto& line : wrap_text(text_, "* " + bullet, kSidebarBulletSize, 152 - 16)) {
            text_.draw(line, Z(8), Z(by), {0, 0, 0, 255}, Z(kSidebarBulletSize));
            by += 16;
        }
        by += 6;
    }

    SDL_Rect quit_r{Z(640 - 16), 0, Z(16), Z(16)};
    if (atlas_.meta("spr_button_quit")) {
        atlas_.draw("spr_button_quit", Z(640 - 16), 0, 0, S);
        hit_rects_.push_back({quit_r, "quit", 0});
    } else {
        draw_button(renderer, quit_r, "X", "quit", 0, {255, 60, 60, 255});
    }

    // ---- item grid ----
    auto catalog_entry = [&](const std::string& name) -> const nlohmann::json* {
        for (const char* section : {"units", "buildings", "techs"}) {
            auto& sec = data_.catalog().at(section);
            if (sec.contains(name)) return &sec.at(name);
        }
        return nullptr;
    };
    auto cell_visible = [&](const ww::menu::TechTreeEntry* e) {
        return e && (e->name == "through" || ww::menu::resolve_civ_unit(e->name, civ).has_value());
    };

    // Icons/arrows scroll under a fixed sidebar (x<160) -- without clipping,
    // a scrolled-left item's icon/arrow can be drawn AT a negative-relative
    // x that lands on top of the sidebar's flag/name/bonus text.
    SDL_Rect grid_clip{Z(160), 0, Z(640 - 160), Z(480)};
    SDL_RenderSetClipRect(renderer, &grid_clip);
    // Hover tooltip: note which visible cell the mouse is over during this
    // draw pass, then render a name/cost/stats/effect box on top after the
    // grid (so it isn't clipped by grid_clip and sits above every icon).
    const nlohmann::json* hover_item = nullptr;
    std::string hover_name;
    int hover_frame = 1;
    for (auto& entry : ww::menu::tech_tree_grid(data_dir_)) {
        double sx = 160.0 + 64.0 * entry.col - tech_tree_scroll_; // native
        double sy = 64.0 * entry.row;

        if (entry.name == "through") {
            // Skip a through filler whose connected unit below is hidden for
            // this civ (e.g. Heavy Artillery is Soviet-only) so no arrow dangles.
            const ww::menu::TechTreeEntry* below = tech_tree_find(data_dir_, entry.col, entry.row + 1);
            if (below && below->name != "through" && !ww::menu::resolve_civ_unit(below->name, civ)) continue;
            if (atlas_.meta("tech_tree_through_arrow")) {
                atlas_.draw("tech_tree_through_arrow", Z(sx), Z(sy), 0, S,
                           0.0, false, 255, {255, 255, 255, 255});
            }
        } else {
            auto resolved = ww::menu::resolve_civ_unit(entry.name, civ);
            if (!resolved) continue; // hidden for this civ (e.g. b29 on a non-USA civ)
            const nlohmann::json* item = catalog_entry(*resolved);
            if (!item) continue; // defensive -- every grid name is verified against catalog.json

            int frame = 1; // unit (default)
            if (data_.catalog().at("buildings").contains(*resolved)) frame = 0;
            else if (data_.catalog().at("techs").contains(*resolved)) frame = 2;
            else if (ww::menu::is_unique_unit(*resolved)) frame = 3;

            bool available = ww::sim::Control::civ_has(*resolved, civ, civ_exclude_) &&
                             ww::sim::Control::civ_upgrade_allowed(*resolved, civ);
            int isx = Z(sx), isy = Z(sy);
            if (atlas_.meta("spr_tech_tree_icon")) {
                atlas_.draw("spr_tech_tree_icon", isx, isy, frame, S, 0.0, false, 255, {255, 255, 255, 255});
                if (!available) {
                    // Approximates the original's draw_sprite_ext(..., c_black, 0.5) --
                    // a 50%-alpha black pass over the same frame darkens it in place.
                    atlas_.draw("spr_tech_tree_icon", isx, isy, frame, S, 0.0, false, 128, {0, 0, 0, 255});
                }
            }
            std::string icon_sprite = item->value("icon_sprite", "");
            if (!icon_sprite.empty() && atlas_.meta(icon_sprite)) {
                atlas_.draw(icon_sprite, Z(sx + 10), Z(sy + 5), 0, S, 0.0, false, 255, {255, 255, 255, 255});
            }
            if (!available && atlas_.meta("spr_tech_tree_cross")) {
                atlas_.draw("spr_tech_tree_cross", isx, isy, 0, S, 0.0, false, 255, {255, 255, 255, 255});
            }

            // Original truncates to 10 characters (string_copy(display,1,10))
            // -- kept verbatim, including the resulting mid-word cutoffs
            // visible in the reference image ("Swordmaste", "Infantryma").
            std::string display = item->value("display", *resolved);
            if (display.size() > 10) display = display.substr(0, 10);
            text_small_.draw(display, isx, Z(sy + 32), {255, 255, 255, 255}, Z(8));

            // Hover hit-test over the icon cell (guarded to the grid area so a
            // cell scrolled partly under the sidebar can't hover from over it).
            if (mouse_pos_.x >= std::max(isx, Z(160)) && mouse_pos_.x < isx + Z(44) &&
                mouse_pos_.y >= isy && mouse_pos_.y < isy + Z(45)) {
                hover_item = item;
                hover_name = *resolved;
                hover_frame = frame;
            }
        }

        // Horizontal arrow from the cell one column to the left. Buildings
        // never receive one (matches the original's `image_index!=0` gate --
        // a building is always frame 0); data_.catalog() naturally reports
        // "through" as not-a-building, no special case needed.
        const ww::menu::TechTreeEntry* left = tech_tree_find(data_dir_, entry.col - 1, entry.row);
        bool is_building = data_.catalog().at("buildings").contains(entry.name);
        if (left && cell_visible(left) && cell_visible(&entry) && !is_building && entry.older_sibling &&
            left->child && atlas_.meta("tech_tree_arrow_side")) {
            int arrow_frame = (left->name == "through" && entry.name == "through") ? 2 : 0;
            atlas_.draw("tech_tree_arrow_side", Z(sx - 64), Z(sy - 9),
                       arrow_frame, S, 0.0, false, 255, {255, 255, 255, 255});
        }

        // Vertical arrow from the cell one row above.
        const ww::menu::TechTreeEntry* above = tech_tree_find(data_dir_, entry.col, entry.row - 1);
        bool draw_down = entry.force;
        if (above && cell_visible(above) && above->parent && entry.name != "frigate") draw_down = true;
        if (draw_down && cell_visible(&entry) && atlas_.meta("tech_tree_arrow_down")) {
            atlas_.draw("tech_tree_arrow_down", Z(sx), Z(sy - 20), 0, S, 0.0,
                       false, 255, {255, 255, 255, 255});
        }
    }
    SDL_RenderSetClipRect(renderer, nullptr);

    // ---- bottom-right: scroll + close ----
    // Pinned to the very bottom edge (y = 480-44), BELOW the lowest grid row
    // (row 6 icons end ~y429 + their labels ~y424) so they never cover a
    // Fortress unit/tech no matter how far the tree is scrolled right. Drawn
    // BEFORE the hover tooltip so the tooltip box renders on top of them.
    {
        SDL_Rect prev_btn{Z(640 - 3 * 90 - 20), Z(480 - 44), Z(80), Z(42)};
        draw_button(renderer, prev_btn, "<", "scroll_left");
        SDL_Rect next_btn{Z(640 - 2 * 90 - 10), Z(480 - 44), Z(80), Z(42)};
        draw_button(renderer, next_btn, ">", "scroll_right");
        SDL_Rect close_btn{Z(640 - 90), Z(480 - 44), Z(80), Z(42)};
        draw_button(renderer, close_btn, "Close", "close_tech_tree");
    }

    // ---- hover tooltip (name / cost / stats / effect) -- drawn LAST so it
    // sits on top of everything, including the scroll/close buttons above ----
    if (hover_item) {
        const nlohmann::json& it = *hover_item;
        struct TipLine { std::string s; int sz; SDL_Color c; };
        std::vector<TipLine> lines;
        auto num = [&](const char* key) -> std::string {
            char b[32];
            std::snprintf(b, sizeof b, "%g", it[key].get<double>());
            return b;
        };
        lines.push_back({it.value("display", hover_name), 13, {255, 255, 255, 255}});
        const char* type_label = hover_frame == 0 ? "Building" : (hover_frame == 2 ? "Technology" : "Unit");
        lines.push_back({type_label, 9, {150, 180, 255, 255}});
        if (it.contains("cost") && it["cost"].is_object() && !it["cost"].empty()) {
            std::string cost = "Cost:";
            for (auto& [k, v] : it["cost"].items())
                cost += " " + std::to_string(static_cast<int>(v.get<double>())) + " " + k;
            lines.push_back({cost, 10, {255, 215, 90, 255}});
        }
        // Numeric stats only for units/buildings (techs carry none).
        if (hover_frame != 2) {
            const SDL_Color kStat{210, 210, 210, 255};
            std::string l1;
            if (it.contains("max_life")) l1 = "HP " + num("max_life");
            if (it.contains("armor") || it.contains("pierce")) {
                if (!l1.empty()) l1 += "   ";
                l1 += "Armour " + (it.contains("armor") ? num("armor") : std::string("0")) + "/" +
                      (it.contains("pierce") ? num("pierce") : std::string("0"));
            }
            if (!l1.empty()) lines.push_back({l1, 10, kStat});
            std::string l2;
            if (it.contains("attack")) l2 = "Attack " + num("attack");
            if (it.contains("range") && it["range"].get<double>() > 0) {
                if (!l2.empty()) l2 += "   ";
                l2 += "Range " + num("range");
            }
            if (it.contains("sight")) {
                if (!l2.empty()) l2 += "   ";
                l2 += "LOS " + num("sight");
            }
            if (!l2.empty()) lines.push_back({l2, 10, kStat});
            if (it.contains("speed") && it["speed"].get<double>() > 0)
                lines.push_back({"Speed " + num("speed"), 10, kStat});
            // Blast radius (tiles) for area-of-effect units -- artillery, tanks,
            // warships, bombers, the ballistic missile.
            if (it.contains("blast_radius") && it["blast_radius"].get<double>() > 0)
                lines.push_back({"Blast " + num("blast_radius"), 10, kStat});
        }
        // Effect / description, wrapped at the draw size so the box bounds it.
        auto tt = ww::client::item_tooltips().find(hover_name);
        if (tt != ww::client::item_tooltips().end() && !tt->second.desc.empty()) {
            for (auto& wl : wrap_text(text_, tt->second.desc, Z(9), Z(190)))
                lines.push_back({wl, 9, {235, 235, 235, 255}});
        }
        int pad = Z(8), gap = Z(3);
        int content_w = 0, total_h = 0;
        for (auto& ln : lines) {
            int tw, th;
            text_.measure(ln.s, Z(ln.sz), tw, th);
            content_w = std::max(content_w, tw);
            total_h += th + gap;
        }
        int box_w = content_w + pad * 2, box_h = total_h - gap + pad * 2;
        int bx = mouse_pos_.x + Z(16), by = mouse_pos_.y + Z(8);
        if (bx + box_w > Z(640)) bx = mouse_pos_.x - box_w - Z(8);
        if (bx < 0) bx = 0;
        if (by + box_h > Z(480)) by = Z(480) - box_h;
        if (by < 0) by = 0;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 235);
        SDL_Rect box{bx, by, box_w, box_h};
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 120, 120, 130, 255);
        SDL_RenderDrawRect(renderer, &box);
        int ty = by + pad;
        for (auto& ln : lines) {
            int tw, th;
            text_.measure(ln.s, Z(ln.sz), tw, th);
            text_.draw(ln.s, bx + pad, ty, ln.c, Z(ln.sz));
            ty += th + gap;
        }
    }
}

void MenuController::draw(SDL_Renderer* renderer) {
    hit_rects_.clear();
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    // Tech tree paints its own full-canvas tan background (see
    // draw_tech_tree) instead of the bg_menu photo the other three screens
    // share, so skip drawing bg_menu underneath it entirely.
    if (screen_ != Screen::TechTree) {
        if (atlas_.meta("bg_title0")) draw_panning_bg(renderer);
        else if (atlas_.meta("bg_menu")) atlas_.draw_stretched("bg_menu", SDL_Rect{0, 0, view_w_, view_h_});
    }

    switch (screen_) {
        case Screen::Title: draw_title(renderer); break;
        case Screen::MapSelect: draw_map_select(renderer); break;
        case Screen::MainMenu: draw_main_menu(renderer); break;
        case Screen::SinglePlayer: draw_single_player(renderer); break;
        case Screen::Multiplayer: draw_multiplayer(renderer); break;
        // Same function as RandomMapSetup on purpose -- see its header comment.
        case Screen::MpSetup: draw_random_map_setup(renderer); break;
        case Screen::RandomMapSetup: draw_random_map_setup(renderer); break;
        case Screen::CivChooser: draw_civ_chooser(renderer); break;
        case Screen::TechTree: draw_tech_tree(renderer); break;
        case Screen::Options: draw_options(renderer); break;
        case Screen::HotkeysOptions: draw_hotkeys_options(renderer); break;
        case Screen::GraphicsOptions: draw_graphics_options(renderer); break;
        case Screen::AudioOptions: draw_audio_options(renderer); break;
        case Screen::CampaignList: draw_campaign_list(renderer); break;
        case Screen::CampaignMap: draw_campaign_map(renderer); break;
    }

    if (atlas_.meta("spr_mouse")) atlas_.draw("spr_mouse", mouse_pos_.x, mouse_pos_.y, 0, 1.0);
}

SkirmishSettings MenuController::build_settings() const {
    // "Start Stress Test" (Graphics screen) -- a fixed, standalone config
    // (8 players/Huge map/200 pop/Revealed, so the whole thing is visible
    // at a glance) instead of the player's own Random Map Setup choices.
    // Reuses teams_[] for civ/colour/ally exactly like the normal path
    // below -- all 8 slots are always populated regardless of n_players_,
    // so this doesn't need its own copy of that loop.
    if (stress_test_) {
        SkirmishSettings s;
        s.n_players = 8;
        s.map_size = kMapSizeValues[3];  // Huge -- room for 8 full-size armies
        s.max_pop = kMaxPopValues[3];    // 200
        s.water = true;
        s.map_type = "random";
        s.deathmatch = false;
        s.reveal_mode = 2; // Revealed -- watch every team's battles, not just team 0's sight
        s.spectator = true; // every team AI-controlled -- see SkirmishSettings::spectator's comment
        for (int i = 0; i < s.n_players; ++i) {
            int civ = teams_[i].civ;
            if (civ < 0) civ = std::rand() % 9;
            s.civs.push_back(civ);
            s.leaders.push_back(civ == teams_[i].civ ? teams_[i].leader : 0);
            s.colours.push_back(teams_[i].colour);
            s.allies.push_back(teams_[i].ally);
        }
        return s;
    }

    SkirmishSettings s;
    s.n_players = n_players_;
    s.map_size = kMapSizeValues[map_size_idx_];
    s.max_pop = kMaxPopValues[max_pop_idx_];
    s.water = true; // only consulted by the "random" map_type; themed maps set their own
    s.map_type = kMaps[map_type_idx_].value;
    s.deathmatch = (mode_idx_ == 1);
    s.reveal_mode = reveal_idx_;
    s.difficulty = difficulty_idx_;
    for (int i = 0; i < n_players_; ++i) {
        int civ = teams_[i].civ;
        if (civ < 0) civ = std::rand() % 9; // resolve "Random" to a concrete civ now
        s.civs.push_back(civ);
        // Only carry the chosen leader when the civ wasn't randomised (a random
        // civ's leader defaults to the first one).
        s.leaders.push_back(civ == teams_[i].civ ? teams_[i].leader : 0);
        s.colours.push_back(teams_[i].colour);
        s.allies.push_back(teams_[i].ally);
    }
    return s;
}
