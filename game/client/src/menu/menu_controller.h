#pragma once
#include "campaign/campaign_data.h"
#include "net/upnp.h" // PortMapResult, held by value in the lobby's UPnP probe
#include "render/sprite_atlas.h"
#include "render/text_renderer.h"
#include "settings.h"
#include "sim/catalog.h"
#include "sim/scenario.h"

#include <SDL.h>

#include <array>
#include <future>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Audio;
// Only ever held as a pointer here -- app.cpp owns the Session, since it has to
// outlive the menu and be handed on to GameClient. menu_controller.cpp includes
// net/session.h for the calls.
namespace ww::net { class Session; }

// Pre-game menu flow: Main Menu -> Random Map Setup -> Civ Chooser -> (Start
// War hands off to GameClient). Structurally parallel to GameClient (owns
// its own renderer-bound resources, exposes handle_event/draw) but for
// menu screens only -- deliberately kept as a separate class rather than
// folded into GameClient, since a match's lifetime (GameClient) and the
// pre-match setup UI's lifetime never overlap and have very different
// responsibilities. See app.cpp for how the two are sequenced.
class MenuController {
public:
    MenuController(SDL_Renderer* renderer, const std::string& asset_dir, const std::string& data_dir,
                  int view_w, int view_h);
    ~MenuController();

    void handle_event(const SDL_Event& ev);
    void draw(SDL_Renderer* renderer);
    // Optional: menu button clicks play the UI click sound through this.
    void set_audio(Audio* a) { audio_ = a; }

    bool wants_start() const { return wants_start_; }
    bool wants_quit() const { return wants_quit_; }
    // True after the Graphics screen's "Start Stress Test" button is
    // clicked (also sets wants_start_ -- see handle_click's "start_
    // stress_test"). app.cpp checks this after constructing GameClient
    // from build_settings() and, if set, immediately fills out every team
    // into a full late-game army (see ww::sim::populate_stress_test),
    // locks the player into spectating rather than playing (GameClient::
    // set_spectator), and auto-returns to this menu after 30 seconds or
    // Enter/Escape/the pause menu's Quit Game, instead of running until
    // the player plays out and quits a real match.
    bool wants_stress_test() const { return stress_test_; }
    ww::sim::SkirmishSettings build_settings() const;

    // ---- multiplayer lobby (Screen::Multiplayer) --------------------------
    // The net layer has been complete and self-tested for a while (net/session.h
    // -- deterministic lockstep, desync detection, UPnP); the only missing piece
    // was ever the screen that drives it, which is why entry was an env hook
    // (WW_MP_HOST / WW_MP_JOIN, see app.cpp). This is that screen.
    //
    // app.cpp OWNS the Session -- it outlives the menu and is handed to
    // GameClient once the match starts -- and lends it here. Left null (the
    // default), the Multiplayer button draws disabled, so nothing in the menu
    // depends on the net layer being wired up.
    void set_session(ww::net::Session* s) { session_ = s; }
    // True once the lobby has a peer connected AND both sides have agreed on
    // the seed and settings (Session::Status::Ready). app.cpp treats this like
    // wants_start(): stop the menu loop and build the match from the session
    // rather than from build_settings().
    bool wants_start_mp() const { return wants_start_mp_; }

    // Set when the campaign level popup's "Play" button is clicked (see
    // draw_campaign_map) -- app.cpp checks this alongside wants_start() and,
    // if set, constructs GameClient from chosen_level() instead of
    // build_settings(). Only ever true when selected_campaign_/
    // selected_level_ both still point at a valid Level (the button isn't
    // drawn otherwise), so chosen_level() is safe to call without its own
    // bounds re-check.
    bool wants_play_level() const { return wants_play_level_; }
    const ww::campaign::Level& chosen_level() const {
        return campaigns_[selected_campaign_].levels[selected_level_];
    }
    // The chosen level's PARENT campaign's name -- same validity precondition
    // as chosen_level() above (only call when wants_play_level() is true).
    // GameClient needs this alongside the Level itself since Level::id alone
    // isn't globally unique across campaigns (see campaign_level_key,
    // settings.h).
    const std::string& chosen_campaign_name() const { return campaigns_[selected_campaign_].name; }

    // Jumps straight to Options > Graphics, skipping the usual Title start
    // -- app.cpp calls this right after constructing a fresh MenuController
    // when the menu loop it's about to run is the one right after a "Start
    // Stress Test" preview ended, so the player lands back where they
    // launched it from instead of at the title screen.
    void open_graphics_options() { screen_ = Screen::GraphicsOptions; }

    // Test-only: jump straight to the Tech Tree screen previewing `civ`,
    // skipping the several menu clicks it normally takes to get there. Used
    // by app.cpp's WW_TEST_MENU_TECHTREE screenshot hook so a hover tooltip
    // (and the grid layout) can be captured without driving the full flow.
    void test_open_tech_tree(int civ, double scroll = 0.0) {
        civ_chooser_preview_ = civ;
        tech_tree_scroll_ = scroll;
        screen_ = Screen::TechTree;
    }
    // Test-only: jump to the civ-chooser screen previewing `civ` (and leader).
    void test_open_civ_chooser(int civ, int leader = 0) {
        civ_chooser_preview_ = civ;
        civ_chooser_leader_ = leader;
        screen_ = Screen::CivChooser;
    }
    // Test-only: open the Update Notes popup at entry `idx` (0 = newest).
    void test_open_update_notes(int idx) {
        screen_ = Screen::Title;
        show_update_notes_ = true;
        update_notes_index_ = idx;
    }

    // Set for exactly one frame after a Graphics options resolution button
    // is clicked (see handle_click's "set_resolution" case). app.cpp checks
    // this once per menu-loop iteration and, if set, recreates its
    // render-target texture at the new size and calls resize() below --
    // MenuController never resizes its own render target/window (it
    // doesn't own the renderer), it only reports the request and adapts its
    // own layout once app.cpp confirms the resize actually happened.
    bool wants_resize() const { return wants_resize_; }
    // Reads the pending size AND clears wants_resize_ in one call, so a
    // single click can't be applied twice across two loop iterations.
    void consume_resize(int& w, int& h) {
        w = kResolutions[settings_.resolution_index].w;
        h = kResolutions[settings_.resolution_index].h;
        wants_resize_ = false;
    }
    // Re-derives every view-size-dependent field (view_w_/view_h_ and the
    // kPanel_/kBackRect_/kQuitRect_ chrome rects) in place, same formulas
    // the constructor uses -- called by app.cpp right after it actually
    // resizes its render target, so MenuController's own layout is never
    // out of sync with the real canvas size for more than the one frame it
    // takes app.cpp to notice wants_resize().
    void resize(int view_w, int view_h);

private:
    enum class Screen {
        // MainMenu is now the Single Player / Multiplayer split; SinglePlayer is
        // the Random Map / Campaign choice that used to live on MainMenu itself.
        // MpSetup is the network lobby's roster screen. It is drawn by
        // draw_random_map_setup -- the SAME function as the single-player one,
        // not a copy -- because it is supposed to look identical; only who may
        // edit what differs. See that function's `mp` branches.
        Title, MainMenu, SinglePlayer, Multiplayer, MpSetup, RandomMapSetup, MapSelect, CivChooser,
        TechTree, Options, HotkeysOptions, GraphicsOptions, AudioOptions, CampaignList, CampaignMap,
    };

    // ---- multiplayer lobby state -----------------------------------------
    enum class MpStage {
        Choose,  // Host / Join buttons + address & port fields
        Hosting, // socket open, waiting for a joiner
        Joining, // connecting / handshaking
    };
    ww::net::Session* session_ = nullptr;
    bool wants_start_mp_ = false;
    MpStage mp_stage_ = MpStage::Choose;
    std::string mp_addr_;              // what the joiner types
    std::string mp_port_ = "27015";    // kDefaultPort, as text
    int mp_focus_ = 0;                 // which field has the caret: 0 none, 1 address, 2 port
    std::string mp_error_;             // last host()/join() failure, shown in red
    // UPnP off the render thread. Discovery is SSDP with per-target timeouts and
    // can take the better part of a minute on a network with no router to find
    // (see net/upnp.h, which explicitly says to call it off this thread) -- so
    // the lobby fires it once on Host and polls the future while drawing rather
    // than freezing on the click.
    std::future<ww::net::PortMapResult> mp_upnp_;
    bool mp_upnp_done_ = false;
    ww::net::PortMapResult mp_upnp_result_;
    std::vector<std::string> mp_local_addrs_; // read once on Host, for the status panel
    // Close the session, drop the lobby back to Choose, and stop text input.
    void mp_reset();
    uint16_t mp_parsed_port() const;
    // True while the roster screen is being drawn for a NETWORK match rather
    // than a skirmish -- the one thing draw_random_map_setup branches on.
    bool mp_setup() const { return screen_ == Screen::MpSetup; }
    // Which row on that screen is this machine's player (0 host, 1 joiner).
    int mp_local_row() const;
    // Copy the session's authoritative roster/settings into teams_[] and the
    // option indices, so the shared draw code can render network state without
    // knowing anything about the network. Called once per frame while on
    // MpSetup.
    void mp_pull();
    // Publish this machine's row back to the session (civ/leader/colour/team/
    // ready). Called after any local edit.
    void mp_push_slot();
    // Host only: publish the map/rule settings after a local edit.
    void mp_push_settings();
    bool mp_ready_ = false; // this machine's "I am ready" toggle
    // Which roster screen opened the (shared) civ chooser, so select/back
    // return to the right one.
    bool mp_from_civ_chooser_ = false;

    // All player-configurable state, independent of Screen -- CivChooser
    // just edits team_civ_[civ_chooser_target_] in place and either commits
    // (SELECT) or discards (back) its own civ_chooser_preview_ scratch copy.
    struct TeamRow {
        int civ = -1;     // -1 = Random
        int leader = 0;   // 0-2, index into the civ's 3 leaders (civ_data.h leader_names)
        int colour = 0;   // index into ww::menu::team_colours()
        // Alliance group (1-4, cycled via "cycle_team") -- teams sharing a
        // value fight on the same side (see sim/include/sim/control.h's
        // Team::ally). Defaults to a distinct value per row (set in the
        // constructor) so every team is its own side unless the player
        // groups them, matching pre-team-system free-for-all behavior.
        int ally = 1;
    };
    std::array<TeamRow, 8> teams_;
    int n_players_ = 2;
    int map_type_idx_ = 0;   // Random / Ostland / Negev Desert / Guam / Stalingrad / Ardennes
    int map_size_idx_ = 1;   // Tiny/Normal/Large/Huge
    int max_pop_idx_ = 1;    // 50/100/150/200
    int reveal_idx_ = 0;     // Standard/No fog/Revealed -- wired to SkirmishSettings::reveal_mode, see build_settings()
    int difficulty_idx_ = 1; // Easy/Normal/Hard -- see build_settings()/SkirmishSettings::difficulty
    int mode_idx_ = 0;       // Standard/Deathmatch -- wired to SkirmishSettings::deathmatch, see build_settings()
    // Debug/perf-testing only: set for the duration of a "Start Stress
    // Test" preview (Graphics screen) -- makes build_settings() return a
    // fixed 8-player/Huge/200-pop/Revealed config (instead of the player's
    // own Random Map Setup choices) and every team's base fill out into a
    // full late-game army (see ww::sim::populate_stress_test). Left set for
    // the preview's whole lifetime, not just the launching click, since
    // app.cpp also reads wants_stress_test() every frame of that session
    // to drive its 10-second/first-input auto-return.
    bool stress_test_ = false;

    int civ_chooser_target_ = -1;  // which teams_[] slot is being edited
    int civ_chooser_preview_ = -1; // scratch civ, committed on SELECT, discarded on back
    int civ_chooser_leader_ = 0;   // scratch leader (0-2), committed on SELECT

    double tech_tree_scroll_ = 0.0; // horizontal pixel offset, see draw_tech_tree

    Screen screen_ = Screen::Title;
    bool show_update_notes_ = false; // changelog popup over the Title screen
    // Which ww::menu::release_notes() entry the popup is currently showing
    // -- 0 is always the newest (index into the newest-first array); reset
    // to 0 every time the popup is (re)opened. Left/Right buttons in
    // draw_title step it toward older/newer entries.
    int update_notes_index_ = 0;
    bool wants_start_ = false;
    bool wants_quit_ = false;
    bool wants_play_level_ = false; // see wants_play_level()'s comment

    SpriteAtlas atlas_;
    TextRenderer text_;
    // Non-bold, used only for the tech tree's small under-icon item labels
    // (same bold/regular split GameClient's HUD already uses, see its
    // text_/text_regular_ comment) -- the bold font read as too large/heavy
    // for those tightly-packed 44px-wide labels.
    TextRenderer text_small_;
    // Needed for the tech tree's catalog lookups (display name/icon_sprite/
    // which section an item belongs to) and civ_exclude_ below -- the other
    // three screens don't touch sim data at all, so this is new to the menu
    // system as of the tech tree.
    // Kept (not just passed through the ctor) so the "Campaign" button can
    // (re)load campaigns_ on demand -- the campaign list isn't fetched
    // until the player actually opens that screen.
    std::string data_dir_;
    std::string asset_dir_; // for loading a campaign level's briefing image at runtime
    ww::sim::DataStore data_;
    std::unordered_map<int, std::set<std::string>> civ_exclude_;
    Audio* audio_ = nullptr; // set by app.cpp; menu clicks play through it
    // Cached texture for the currently-previewed level's briefing image (see
    // Level::briefing_image, set in the campaign editor). Reloaded lazily when
    // the previewed level changes; freed in the destructor.
    SDL_Texture* briefing_tex_ = nullptr;
    std::string briefing_tex_key_;

    // ---- Campaign browsing (list -> Europe map -> level info popup ->
    // Play, see wants_play_level()).
    std::vector<ww::campaign::Campaign> campaigns_;
    int selected_campaign_ = -1; // index into campaigns_
    int selected_level_ = -1;    // index into campaigns_[selected_campaign_].levels; -1 = no popup shown
    SDL_Point mouse_pos_{0, 0};
    int view_w_, view_h_;

    // Menu chrome geometry, derived from view_w_/view_h_ at construction
    // AND recomputed by resize() -- used throughout every draw_* screen
    // except draw_tech_tree, which deliberately stays pinned to a native
    // 640x480 layout regardless of resolution (see its own comment).
    SDL_Rect kPanel_, kBackRect_, kQuitRect_;
    // See wants_resize()'s comment.
    bool wants_resize_ = false;

    // Options > Hotkeys. Loaded once at construction, saved to disk (see
    // Settings::save) every time a rebind is applied -- GameClient loads
    // its own fresh copy of the same settings.json when it's constructed
    // later, since the menu and the match never run at the same time.
    Settings settings_;
    // Which rebindable slot (if any) is currently "listening" for its next
    // keypress -- set by clicking a key button in draw_hotkeys_options,
    // consumed by handle_event.
    struct RebindTarget {
        enum class Kind {
            None, Building, Construction, Item, Idle,
            PanUp, PanDown, PanLeft, PanRight, // single Settings fields --
            BuildEco, BuildMilitary, AttackMove, // index unused for these
            FormationColumn, FormationBox, FormationStagger, FormationSplit,
            Land, Unload, ShipyardPage, BuildNuke, BuildBack,
        } kind = Kind::None;
        int index = 0; // building_keys[]/construction_keys[]/item_keys[] index; unused for single-field kinds
        // Set on keydown, applied on the matching keyup (see handle_event)
        // -- a rebind commits on RELEASE, not on the initial press, so a
        // Ctrl+<letter> attempt reads as one deliberate gesture instead of
        // registering as just "<letter>" the instant Ctrl+<letter> starts.
        // pending_ctrl records whether Ctrl was actually held at the
        // keydown -- ANY slot can take a Ctrl combo now, not just Buildings.
        SDL_Keycode pending_key = SDLK_UNKNOWN;
        bool pending_ctrl = false;
    };
    RebindTarget rebinding_;
    void apply_rebind(Hotkey new_hotkey);
    // Idle/Pan*/Build*/AttackMove/FormationColumn/FormationBox/Formation
    // Stagger/FormationSplit/Land/Unload/ShipyardPage/BuildNuke/BuildBack ->
    // its Settings field.
    Hotkey* single_key_field(RebindTarget::Kind kind);
    // True if settings_'s Hotkey for `kind` is ALSO bound to another
    // single-field action/pan key kind that could actually be reachable at
    // the same time in a real match -- e.g. Land and a formation key can
    // both be on screen together (2+ own aircraft selected), so sharing a
    // key is a
    // real conflict; ShipyardPage (shipyard selected) and BuildNuke
    // (airbase selected) never can be, since only one specific building
    // type is ever selected at once, so sharing a key there is NOT flagged.
    // Idle/Pan* are "always active" (they fire no matter what's selected
    // or open -- see GameClient::handle_hotkey/app.cpp's pan poll), so for
    // those specifically this ALSO scans building_keys/construction_keys/
    // item_keys, not just the other single-field kinds. Rebinding no
    // longer auto-resolves collisions (see apply_rebind), so
    // draw_hotkeys_options highlights these in red instead of preventing
    // them.
    bool key_conflicts(RebindTarget::Kind kind);
    // True if `hk` matches an Idle/Pan* binding -- those are "always
    // active" (see key_conflicts' comment), so ANY other binding sharing
    // their key is a real conflict regardless of that other binding's own
    // context. Called from building_key_conflicts/item_key_conflicts/
    // construction_key_conflicts so the always-active side of the
    // comparison only needs to live in one place.
    bool always_active_conflicts(Hotkey hk);
    // Same idea, but scoped to settings_.building_keys alone -- entries
    // sharing both the same key AND the same Ctrl state can collide with
    // each other (they're all reachable in the same "building selected on
    // the map" context); a bare-vs-Ctrl pair on the same letter can't.
    bool building_key_conflicts(size_t index);
    // Same idea as key_conflicts, but scoped to the single building group
    // settings_.item_keys[flat_index] belongs to (see ww::hotkeys::
    // building_groups) -- every building's first slot defaulting to Q is
    // intentional (their command cards never show at once), so a global
    // item_keys scan would wrongly flag almost everything as conflicted.
    bool item_key_conflicts(size_t flat_index);
    // Same idea again, but for settings_.construction_keys -- scoped to the
    // SAME eco/military category as `building`, since the two build-list
    // submenus never show at once (e.g. "house" and "barracks" both
    // defaulting to Q is intentional, not a real conflict).
    bool construction_key_conflicts(const std::string& building);
    // Vertical scroll offset for the Hotkeys screen's per-building item
    // listing (Units & Research) -- far too much content to fit the panel
    // at once, unlike every other section on that screen. Same "mouse
    // wheel + clamp to content height" pattern as tech_tree_scroll_.
    double hotkeys_scroll_ = 0.0;

    // Screen-space hit-rects, rebuilt each draw() and consulted by
    // handle_event -- same "layout then hit-test last frame's rects"
    // pattern GameClient's command-card buttons already use.
    struct HitRect {
        SDL_Rect rect;
        std::string id; // opaque action tag, interpreted in handle_click
        int arg = 0;
    };
    std::vector<HitRect> hit_rects_;

    // `right` = the right mouse button. Only the colour swatch reacts to
    // it (cycling backwards); every other hit-rect ignores a right-click
    // entirely rather than treating it as a second left-click.
    void handle_click(int mx, int my, bool right = false);

    // Move team `idx` to the next free colour in the palette, `step` = +1
    // forward / -1 backward, skipping any index another ACTIVE team
    // (row < n_players_) has already taken and wrapping around the ends.
    // Two players sharing a colour is indistinguishable in-game -- unit
    // sprites, minimap dots and the scoreboard are all keyed off it -- so
    // the button simply can't land on one that's in use.
    void cycle_colour(int idx, int step);
    void draw_frame_chrome(SDL_Renderer* renderer, bool show_back);
    void draw_panning_bg(SDL_Renderer* renderer); // cycling WW2 photo backdrop
    void draw_title(SDL_Renderer* renderer);      // "WORLD WAR" start screen
    void draw_map_select(SDL_Renderer* renderer); // photo-grid map picker
    void draw_main_menu(SDL_Renderer* renderer);   // Single Player / Multiplayer / Options
    void draw_single_player(SDL_Renderer* renderer); // Random Map / Campaign
    void draw_multiplayer(SDL_Renderer* renderer);   // host/join lobby, see set_session
    void draw_random_map_setup(SDL_Renderer* renderer);
    void draw_civ_chooser(SDL_Renderer* renderer);
    void draw_options(SDL_Renderer* renderer);
    void draw_hotkeys_options(SDL_Renderer* renderer);
    void draw_graphics_options(SDL_Renderer* renderer);
    void draw_audio_options(SDL_Renderer* renderer);
    // Campaign list: name/civ/level-count/completed-or-not per campaign
    // found under <data_dir>/campaigns/*.json (see campaigns_ above).
    void draw_campaign_list(SDL_Renderer* renderer);
    // Europe map with a dot per level (loc_x/loc_y) for the open campaign,
    // plus an info popup (name/description/allies vs belligerents) when
    // selected_level_ >= 0.
    void draw_campaign_map(SDL_Renderer* renderer);
    // Deliberately does NOT call draw_frame_chrome -- the original's tech
    // tree screen has a distinct flat-tan-background/black-divider look
    // (matching the in-game HUD panel colour) rather than the black-panel-
    // on-bg_menu-photo chrome the other three screens share.
    void draw_tech_tree(SDL_Renderer* renderer);

    // Black-fill/white-1px-border box, the panel/button look used
    // throughout the reference screenshots.
    void draw_panel(SDL_Renderer* renderer, const SDL_Rect& r, bool highlight = false);
    // Registers a panel as clickable and draws a centered label inside it.
    // An EMPTY `id` draws the button without registering a hit rect, i.e. a
    // disabled one -- pass a dimmed `border`/`label_col` to match (see the
    // Multiplayer button in draw_main_menu, which is deliberately inert until
    // there is a network transport behind it).
    void draw_button(SDL_Renderer* renderer, const SDL_Rect& r, const std::string& label,
                     const std::string& id, int arg = 0, SDL_Color border = {255, 255, 255, 255},
                     SDL_Color label_col = {255, 255, 255, 255});
};
