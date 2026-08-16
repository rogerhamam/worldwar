#pragma once
#include "audio/audio.h"
#include "match_stats.h"
#include "render/camera.h"
#include "render/sprite_atlas.h"
#include "render/text_renderer.h"
#include "settings.h"
#include "sim/command.h"
#include "sim/event.h"
#include "sim/match.h"

// Forward-declared so the client header stays free of the socket layer; only
// game_client.cpp includes net/session.h.
namespace ww::net { class Session; }

#include <SDL.h>
#include <cmath>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Client-owned visual effect (spark/slice/explosion/corpse/rubble/debris),
// driven by SimEvent::Effect events and by client-side cosmetic generators
// (on-hit debris, wounded-building smoke). All the fields past `frame` keep
// their zero/false defaults for a plain transient on-top effect (an
// explosion or slice); the death/corpse/debris paths set the ones they need.
struct ClientEffect {
    std::string sprite;
    double x, y;
    double t = 0.0;
    double lifetime = 0.4;
    int frame = 0;
    // Draw `frame` verbatim (a corpse's team colour, a rubble pile's frame 0)
    // instead of stepping through the sprite's frames at effect_fps.
    bool fixed_frame = false;
    // Persistent corpse / vehicle wreck / building rubble: drawn UNDER units
    // and buildings (game/session.py "corpses + rubble under everything"),
    // not in the on-top effects pass.
    bool ground = false;
    bool flip = false;       // mirror horizontally (dead entity faced left)
    // >0 => scale the sprite to this target footprint WIDTH in world px
    // (building rubble fills the destroyed building's footprint); 0 => draw
    // at the sprite's own native size, like every other effect.
    double scale = 0.0;
    double fade = 0.0;       // seconds of alpha fade-out at the end of life (0 => pop out)
    // >0: the drawn scale eases from 1.0 down to (1-shrink) across the effect's
    // life -- a sinking boat's successive-shrinking-sprite look (gmk).
    double shrink = 0.0;
    // Ballistic motion for debris/embers (px/s, px/s^2). Zero => stationary.
    double vx = 0.0, vy = 0.0, gravity = 0.0;
    // On-top effects only: an RGB colour-multiply and a direct size multiplier
    // (on top of camera zoom). Used by the cannonball-in-water splash, which
    // reuses spr_shockwave tinted ocean-blue and blown up a bit. Defaults
    // leave every other effect drawn exactly as before.
    SDL_Color tint = {255, 255, 255, 255};
    double draw_scale = 1.0;
};

// A single obj_smoke puff (assets/gmk/objects/obj_smoke): unlike
// ClientEffect above, this DOES drift and fade -- it's spawned at a
// ship's muzzle each shot (see drain_events's "spr_smoke" case) and rises,
// decelerating, while swelling in scale and fading out, matching the
// original's per-frame `vspeed += 0.005`, `swell += 0.008`,
// `image_alpha -= 0.006` (all *60 here for our real-seconds tick -- see
// update()'s per-frame-rate comment convention used elsewhere).
struct ClientSmoke {
    double x, y;
    double vy;               // px/GML-frame, negative = rising; eases toward 0
    double scale = 0.4;
    double alpha = 1.0;      // 0..1, GML's image_alpha
};

// Right-click order feedback: a strobing light-green marker pinned to the
// order's target entity (ellipse for units/resources, rectangle outline
// for buildings -- see draw()'s target flash pass). Direct port of the
// original's per-instance "targeted" countdown (obj_unit/obj_building/
// obj_tree's Draw events), just hoisted into the client instead of being
// per-entity sim state, since it's pure UI feedback. Tracks the entity by
// ref (not a snapshotted position) so it follows a moving target, same as
// the original re-reading the instance's live x/y every frame; vanishes
// automatically once the target dies (world.common(ref) returns null).
struct TargetFlash {
    ww::sim::EntityRef ref;
    double t = 0.0;
    double lifetime = 2.0; // ~40 steps at the original's 20Hz step rate
};

// One line in the on-screen event log (research complete, age advance,
// building/unit finished -- see EventType::Notify and drain_events).
// Unlike the original GameMaker version's ds_list + single shared
// countdown timer (control.messages/message_timer, objects/control/
// Step.gml), which resets EVERY message's remaining time to 3s whenever a
// new one arrives and only ever pops the single oldest entry once that
// timer runs out, each notification here just ages out independently --
// simpler to reason about and avoids the original's quirk where a busy
// stretch of events effectively pauses the whole queue from draining.
struct ClientNotification {
    std::string text;
    double t = 0.0;
    double lifetime = 5.0;
};

// One sent line in the chat log (Enter opens the bar, Enter again sends).
// Multiplayer doesn't exist yet -- there's only ever "you" to show it to --
// but the display side is already what a real chat feed needs, so this is
// the seam a future network layer hangs off of. A recognized cheat command
// (see GameClient::submit_chat) doesn't get broadcast as a literal chat
// line; it shows its own "[Cheat] ..." confirmation here instead.
struct ClientChatMessage {
    std::string text;
    double t = 0.0;
    double lifetime = 10.0;
};

// A transient marker at a world position on the minimap -- currently only the
// attack warning (EventType::Notify "under_attack", see drain_events), which
// needs to say WHERE as well as WHAT. Drawn as a pulsing red box so it reads
// against the team-colour dots already on the minimap, and ages out on its own
// like ClientNotification/ClientChatMessage do.
struct MinimapPing {
    double x = 0.0, y = 0.0; // world px
    double t = 0.0;
    double lifetime = 6.0;
};

// Top-level client session: owns the Match (sim), camera, sprite atlas,
// selection state, and input handling. Roughly the C++ analogue of
// game/session.py's GameSession, but scoped down for an initial playable
// pass -- see the Phase C task list for exactly what's simplified
// (no control groups, formation-drag, minimap, tech-tree overlay, civ
// voice lines, or menu/campaign screens; a skirmish auto-starts).
class GameClient {
public:
    // Delegates to the settings-taking overload below with default_settings()
    // (a 2-player UK-vs-Nazi-Germany skirmish) -- kept around so every
    // existing call site/test that doesn't care about menu-chosen settings
    // keeps working unchanged.
    GameClient(SDL_Renderer* renderer, Audio& audio, const std::string& asset_dir,
              const std::string& data_dir, int view_w, int view_h);
    GameClient(SDL_Renderer* renderer, Audio& audio, const std::string& asset_dir,
              const std::string& data_dir, int view_w, int view_h,
              const ww::sim::SkirmishSettings& settings);
    // Plays a hand-authored campaign Level (editor)
    // instead of a random skirmish -- see ww::sim::new_from_level.
    // `campaign_name` (the parent Campaign's name, not the level's own)
    // is stored alongside level.id and used only to record campaign
    // progress on a win -- see update()'s game-over handling and
    // campaign_level_key() in settings.h.
    GameClient(SDL_Renderer* renderer, Audio& audio, const std::string& asset_dir,
              const std::string& data_dir, int view_w, int view_h,
              const ww::campaign::Level& level, const std::string& campaign_name);

    void handle_event(const SDL_Event& ev);
    void update(double real_dt); // fixed-timestep accumulator drives match_.step()
    void draw(SDL_Renderer* renderer);
    void resize(int w, int h) { cam_.resize(w, h); view_w_ = w; view_h_ = h; }
    Camera& camera() { return cam_; }
    // Test-only: select whatever entity is at world (wx,wy) (WW_TEST_* hooks).
    void test_select(double wx, double wy) { select_at(wx, wy, false); }
    ww::sim::Match& match() { return match_; }
    void set_fps(double fps) { fps_ = fps; }

    // ---- multiplayer: the command sink -----------------------------------
    // EVERY player action that changes the world goes through issue(). In a
    // single-player match it applies the command immediately, which is exactly
    // what the input handlers used to do inline -- so single-player behaviour
    // is unchanged. In a network match it hands the command to the lockstep
    // session instead, which schedules it to execute on BOTH machines on the
    // same turn (see net/session.h).
    //
    // The rule this enforces, and the reason the input handlers no longer touch
    // entity fields directly: a mutation that does not go through here happens
    // on one machine only, and the two simulations diverge. That failure is
    // silent -- no crash, no error, just two players in different games.
    void issue(const ww::sim::Command& cmd);
    // Attach a lockstep session. Null (the default) = single-player: issue()
    // applies inline and update() runs the sim on its own clock, exactly as
    // before. Non-null = the sim advances only when the session says both
    // players' commands for the next turn have arrived.
    void set_session(ww::net::Session* s, int local_team);
    int local_team() const { return local_team_; }
    // In a network match, why it ended (desync/disconnect); empty otherwise.
    const std::string& net_error() const { return net_error_; }
    // Live client-side particle counts, for the frame-time watchdog / crash log
    // in app.cpp (a runaway effect/smoke count is a common cause of a sudden
    // frame-rate collapse, so it's worth recording alongside the sim counts).
    size_t fx_effect_count() const { return effects_.size(); }
    size_t fx_smoke_count() const { return smoke_.size(); }
    // Debug/perf-testing only: instantly fills every active team's base out
    // into a full late-game army (see ww::sim::populate_stress_test) --
    // called once, right after construction, when app.cpp sees the menu's
    // "Start Stress Test" button was clicked (MenuController::wants_stress_
    // test).
    void populate_stress_test();
    // Set alongside populate_stress_test() for the same "Start Stress Test"
    // session. Selection (click/box-select) and camera control (pan/zoom)
    // work completely normally -- a spectator can watch and inspect any
    // team's units/buildings just like a real player would -- but nothing
    // that would ISSUE an order is reachable: right-click (handle_event),
    // command-card/queue-cancel button activation (handle_left_down), and
    // every keyboard hotkey (handle_hotkey) are all individually skipped
    // when this is set (search spectator_ in game_client.cpp for each
    // guard). Matches SkirmishSettings::spectator's sim-side "every team is
    // AI" -- both halves are needed, since spectator alone wouldn't stop a
    // click from manually overriding an AI-controlled unit's order.
    void set_spectator(bool on) { spectator_ = on; }
    // Set once the pause menu's "Quit Game" is clicked -- app.cpp checks
    // this after each frame and, if set, tears this match down and
    // re-shows the main menu instead of exiting the process.
    bool wants_quit_to_menu() const { return quit_to_menu_; }
    // True while the chat bar is open and capturing text -- app.cpp checks
    // this before applying camera panning so typing doesn't pan the map.
    bool chat_open() const { return chat_open_; }
    // Read-only: app.cpp's per-frame camera-pan scancode poll needs the
    // player's configured pan_up_key/etc (SDL_GetScancodeFromKey them),
    // since that poll runs continuously by scancode rather than reacting
    // to discrete SDL_KEYDOWN events the way handle_hotkey does.
    const Settings& settings() const { return settings_; }

private:
    // ---- multiplayer state (all inert in single-player) -------------------
    ww::net::Session* net_ = nullptr;
    // Which team this client's player controls. 0 in single-player and for the
    // host; 1 for the joiner. Every ownership check that used to be a literal
    // `team == 0` has to consult this instead, or the joiner would be unable to
    // order its own units and able to order its opponent's.
    int local_team_ = 0;
    std::string net_error_;
    // Ids of the current selection, for commands that act on a whole selection.
    // Entity ids (stable across the match) rather than EntityRefs, because that
    // is what crosses the network -- see command.h.
    std::vector<uint32_t> selected_ids() const;
    // One turn's worth of commands, refilled by the session each turn.
    std::vector<ww::sim::Command> turn_commands_;
    // Advance the sim under lockstep instead of the local accumulator; returns
    // false when the match has ended (desync/disconnect).
    bool step_networked();

    // Shared tail of every constructor, once match_/cam_ are already built:
    // loads settings, hides the OS cursor (replaced by the custom spr_mouse
    // one drawn in draw()), sets the default zoom, and centres the camera
    // on the local (team 0) player's own base, if it has one yet.
    void post_construct();

    // Command-card button hit-rects, rebuilt each draw() and consulted by
    // handle_left_down (mirrors ui.py's "layout then hit-test last frame's
    // rects" pattern). col/row is the button's grid slot, used to look up
    // its QWERT/ASDFG hotkey (see handle_hotkey).
    struct CardButton {
        SDL_Rect rect;
        std::string kind; // "train" | "tech" | "build"
        std::string item;
        int col = 0, row = 0;
    };

    // Production-queue icon hit-rects, same "rebuilt each draw(), consulted
    // by handle_left_down" pattern as card_buttons_ -- clicking one cancels
    // that slot (World::cancel_queue), refunding its cost.
    struct QueueButton {
        SDL_Rect rect;
        ww::sim::EntityRef building;
        int index = 0;
    };

    void handle_left_down(int mx, int my);
    void handle_left_up(int mx, int my);
    void handle_right_down(int mx, int my);
    void issue_attack_move(double wx, double wy);
    // Artillery attack-ground (T + right-click): sets every selected artillery
    // to shell that fixed point forever (Unit::attack_ground). Cleared by any
    // ordinary right-click order.
    void issue_attack_ground(double wx, double wy);
    // Per-unit-type acknowledgement audio, keyed off the primary selected own
    // unit. Vehicles get an engine rev, planes/ships stay quiet (their engine
    // plays on move), mounts get their animal noise, and only infantry
    // (military + villagers) use the civ voice line -- so factory/airbase
    // units never speak. play_order_ack(attack=true) tries the civ attack shout.
    void play_select_ack();
    void play_order_ack(bool attack);
    // Rome:Total-War-style formation drag (right button). A right-drag past
    // the click threshold lays the selected units out in ranks along the
    // drawn line A->B (its length = the front-rank width, its direction =
    // facing); a right button that didn't drag falls through to the normal
    // right-click order. formation_slots() computes one target (x,y) per unit
    // (same order as `units`) and is shared by the release handler and the
    // live ghost preview so what you see is exactly where they'll go.
    bool formation_drag_eligible();
    void issue_formation(double ax, double ay, double bx, double by);
    std::vector<std::pair<double, double>> formation_slots(
        double ax, double ay, double bx, double by,
        const std::vector<ww::sim::EntityRef>& units);
    void draw_formation_preview(SDL_Renderer* renderer);
    void handle_hotkey(SDL_Keycode key);
    void activate_card_button(const CardButton& btn);
    // Kills the FIRST own, alive unit in `selected_` (in selection order)
    // and drops it from the selection -- direct port of the original's
    // control.delete_command flag (assets/gmk/objects/control/Step.gml's
    // vk_delete check + obj_unit/Step.gml's "kill unit with delete" block),
    // which every selected instance's Step event raced to consume-and-clear
    // that same tick, so only whichever one ran first actually died. One
    // call = one dead unit, even with many selected; press/click again for
    // the next.
    void delete_one_selected();
    // Command groups (Ctrl+1-9 assigns the current selection, replacing
    // that group's prior members; the bare digit selects the group, or --
    // if pressed again within kGroupDoubleTapTicks of the last digit press
    // -- centers the camera on the group's first member instead). Purely
    // client-side bookkeeping, same as selected_ itself: group membership
    // doesn't affect the sim, so it isn't stored on Unit/Building.
    void assign_command_group(int group);
    void select_command_group(int group);
    // Plays ONE building identity clip for a whole selection of buildings: the
    // clip of whichever building type appears most often in `refs`, with ties
    // broken by a uniform random pick among the tied types (so a group of 2
    // barracks + 2 towers is a genuine 50/50). Ignores units and anything not
    // on the player's team, and is silent when the selection holds no
    // buildings. Without this a control group full of buildings played nothing
    // at all, while single-clicking any one of them spoke.
    void play_selection_building_sound(const std::vector<ww::sim::EntityRef>& refs);
    // 0 if `ref` isn't in any of groups 1-9.
    int group_of(ww::sim::EntityRef ref) const;
    std::vector<ww::sim::EntityRef> command_groups_[10]; // index 0 unused, 1-9 are the real groups
    int last_group_key_ = 0;
    uint64_t last_group_tick_ = 0;
    static constexpr uint64_t kGroupDoubleTapTicks = 10; // 0.5s at the 20Hz fixed tick rate
    // Looks up an item's real icon_sprite from the catalog (units/buildings/
    // techs) instead of guessing "spr_<item>_icon" -- shared by
    // draw_command_card (button icons) and draw_hud (multi-select unit-type
    // count icons).
    std::string item_icon(const std::string& item);
    // Which FRAME of that icon to draw. Unit icons are genuinely team-coloured
    // (8 frames, one per palette colour), so they take the team's colour index.
    // Tech and upgrade icons are not -- and the code that assumed "techs are
    // single-frame so the colour index clamps to 0" was wrong about exactly one
    // of them: spr_rifleman_upgrade_icon is a TWO-frame sheet whose second
    // frame holds a completely different icon (the cavalry upgrade). Any team
    // colour other than 0 therefore drew a cavalry icon for the rifleman
    // upgrade. Techs always take frame 0, which is correct for every one of
    // them and immune to another mis-authored sheet.
    int item_icon_frame(const std::string& item);

    // ---- dev POV (chat command "pov", see submit_chat) --------------------
    // Which team the HUD is showing. Normally the team this machine plays
    // (local_team_, which is 1 for a multiplayer joiner -- the HUD used to
    // hardcode team 0, so a joiner was shown the HOST's resources). The `pov`
    // cheat repoints it at an AI so its economy and production can be watched
    // live without pausing or altering it.
    int pov_team_ = -1; // -1 = follow local_team_
    int view_team() const {
        int t = (pov_team_ >= 0) ? pov_team_ : local_team_;
        return (t >= 0 && t < 8) ? t : 0;
    }
    // Watching somebody else. Orders are refused while this holds: the point is
    // to observe the AI undisturbed, and issuing commands for it would both
    // corrupt what is being observed and, in a network match, desync it.
    bool pov_active() const { return pov_team_ >= 0 && pov_team_ != local_team_; }
    // Anything that issues a command is refused when either holds: a stress-test
    // spectator has no team to command, and a POV watcher must not disturb the
    // AI it is inspecting.
    bool orders_locked() const { return spectator_ || pov_active(); }
    // Switch the whole viewpoint -- HUD, fog and camera -- to `team`, or back
    // to your own when it equals local_team_. Defined in game_client.cpp.
    void set_pov(int team);
    // Your own fog grid and camera, put back verbatim when you return. Without
    // the fog copy, everything the watched AI could see would stay marked
    // "explored" on your map afterwards -- permanently revealing terrain you
    // never scouted, which is a real gameplay change rather than a dev view.
    std::vector<std::vector<int>> pov_fog_backup_;
    double pov_cam_x_ = 0.0, pov_cam_y_ = 0.0;
    bool pov_has_backup_ = false;
    // H key: centers the camera on the player's own bases, cycling to the
    // next one on each subsequent press (wrapping back to the first).
    // base_cycle_i_ persists across presses so repeated H taps step through
    // every base in turn instead of re-centering on the same one. Generalized
    // building-hotkey scheme: Ctrl+<letter> cycles through (centers camera
    // on, AND selects) the player's own buildings of the type mapped to
    // that letter; Ctrl+Shift+<letter> selects all of them at once instead.
    // H is special-cased in handle_hotkey to also work bare (no Ctrl
    // needed) for "base" specifically, since it predates this scheme.
    void cycle_to_building_type(const std::string& name);
    void select_all_of_building_type(const std::string& name);
    // Every tech `team` currently has sitting in some building's production
    // queue (the one in progress plus anything queued behind it). Techs are
    // TEAM-wide, so this is scanned across all of the team's living buildings,
    // not just the selected one: a tech being researched at university A must
    // also vanish from university B's card, and World::enqueue's duplicate
    // guard is per-building, so without this the player could pay for the same
    // tech twice. Derived fresh from the queues each call, which is what makes
    // the icon come back on its own the moment the item is cancelled or the
    // researching building dies -- there is no separate state to unwind.
    // Not const: Match::world()/World::get_building() are non-const accessors.
    std::set<std::string> techs_in_progress(int team);
    std::unordered_map<std::string, size_t> building_cycle_i_;
    // "." cycles through (centers camera on, and selects) own idle
    // civilians one at a time; Shift+"." selects all of them at once.
    // "Idle" uses the exact same no-order-of-any-kind test as the
    // red-exclamation-mark marker in draw() (see is_idle_civilian).
    void cycle_idle_villager();
    void select_all_idle_villagers();
    bool is_idle_civilian(const ww::sim::Unit& u) const;
    size_t idle_cycle_i_ = 0;
    void jump_minimap_to(int mx, int my);
    // Original GML swaps the civilian sprite for a tool-holding variant
    // while gathering/building/repairing (Draw.gml's obj_unit, keyed off
    // Unit.working), oscillating two frames to animate the hammer/pickaxe
    // swing (sim's Unit::swing_down mirrors that oscillation, see
    // unit_behavior.cpp). Returns u.sprite unchanged for every other unit.
    const std::string& animated_sprite(const ww::sim::Unit& u) const;
    void select_at(double wx, double wy, bool add_to_selection, bool double_click = false);
    void box_select(double wx0, double wy0, double wx1, double wy1);
    // AoE-style double-click: selects every living, own-team unit sharing
    // `name` that's currently within the camera's on-screen viewport.
    void select_all_of_type_in_viewport(const std::string& name, int team);
    // `shift_queue`: true appends the resolved order to each selected
    // unit's shift-queue (Unit::order_queue) instead of replacing its
    // current order -- see World::queue_order and unit_behavior.cpp's
    // advance_order_queue. Only the four in-scope order kinds (move,
    // attack, gather, build) actually queue; everything else this method
    // can resolve (transport boarding, forced drop-off, repair, the dead-
    // farm re-sow) stays immediate-only even with shift held.
    void right_click_order(double wx, double wy, bool shift_queue = false);
    void draw_hud(SDL_Renderer* renderer);
    // Campaign objective tracker (top-right of the view): each shown objective,
    // struck through + green once World::cleared_objectives contains its id.
    void draw_objectives(SDL_Renderer* renderer);
    void draw_command_card(SDL_Renderer* renderer);
    // Hover tooltip for a single command-card button (title/cost/desc box),
    // drawn on top of the command card after all its buttons -- see
    // draw_command_card's hover tracking. `btn.item` is empty for the
    // fixed buttons (build_eco/build_military/attack_move/build_back),
    // which are looked up by `btn.kind` instead of a catalog item name.
    // The title line gets " (<key>)" appended for whichever key currently
    // triggers this exact button, if any -- see hotkey_label_for.
    void draw_item_tooltip(SDL_Renderer* renderer, const CardButton& btn, int panel_top);
    // The key that currently activates `btn`, as a display label (e.g.
    // "B"), or "" if nothing reaches it (a mouse-only button, e.g.
    // "nuke_indicator"). Every other CardButton::kind resolves to exactly
    // one Settings field (a dedicated action key, a building's own
    // mnemonic, or an item_keys entry), matching handle_hotkey's own
    // resolution order exactly.
    std::string hotkey_label_for(const CardButton& btn) const;
    void draw_minimap(SDL_Renderer* renderer);
    // Everything the viewed team has in production, top-left under the resource
    // bar: one icon per queued item across every building, with a light grey
    // wipe covering it in proportion to how far it has got. Sets
    // global_queue_h_ so the notification stack can start below it.
    void draw_global_queue(SDL_Renderer* renderer);
    int global_queue_h_ = 0;
    // Idle-villager button, just left of the minimap -- spr_idle_icon_none
    // (grey) with 0 idle, spr_idle_icon (red) otherwise, with the count in
    // small white-on-black text overlapping its bottom-right corner.
    // Clicking it is cycle_idle_villager() (handle_left_down), the same
    // action the idle_villager_key hotkey's plain (non-Shift) press does.
    void draw_idle_button(SDL_Renderer* renderer);
    // Event-log stack (research complete/age advance/building & unit
    // finished) -- see ClientNotification's comment for how this differs
    // from the original's single-shared-timer ds_list.
    void draw_notifications(SDL_Renderer* renderer);
    // Bottom-right per-team scoreboard (leader name, score, era icon) --
    // objects/control/Draw.gml's score display.
    void draw_score_hud(SDL_Renderer* renderer);
    void drain_events();
    // Translates a death_unit/death_air/death_building Effect event into the
    // real corpse/wreck/rubble sprite (+ explosion/collapse boom), picking
    // the sprite variant that actually exists in the atlas -- a direct port
    // of game/world.py's _make_corpse / _make_rubble.
    void spawn_death_effect(const ww::sim::SimEvent& ev);
    // F3-toggled debug overlay: FPS, sim step wall-clock time (avg/max over
    // the last second's worth of ticks), and live entity counts -- added
    // for the stress-test debug tool, but works in any match.
    void draw_perf_overlay(SDL_Renderer* renderer);
    // FPS + elapsed seconds, centred in the (otherwise empty -- nothing is
    // ever selected in spectator mode) bottom command panel. Only ever
    // drawn while spectator_ is set (see draw()) -- unlike draw_perf_
    // overlay this isn't F3-gated, it's meant to always be visible for the
    // whole "Start Stress Test" preview.
    void draw_spectator_stats(SDL_Renderer* renderer);

    // Every HUD/cursor pixel constant below is written as a "native"
    // reference-resolution value (as if UI scale were 1) and passed
    // through ui() at the point of use, so the whole HUD/cursor scales
    // together with kUiScale instead of staying a fixed size while world
    // sprites render at cam_.zoom. UI scale is intentionally a fixed
    // constant, not tied to the LIVE camera zoom (which the player can
    // change with the mouse wheel) -- panel/button/text sizes shouldn't
    // shrink to unusable sizes just because the player zoomed out to see
    // more of the map.
    int ui(double native_px) const { return static_cast<int>(std::lround(native_px * kUiScale)); }
    int panel_height() const { return ui(3 * kUnit); } // 3 command-card rows
    int top_bar_height() const { return ui(kUnit); }
    // Left edge of the unit-info block (flag/name/HP/stats) in the bottom
    // panel -- all command-card buttons live to the left of this, in the
    // native-px range [0, kStatsColNative), matching the reference layout.
    // 300 - 64 - 32 = 204: shifted left of the original reference position
    // (twice: 64px, then a further 32px) to make room for a 2-row x 5-col,
    // full-32x32-icon production queue grid to its right (see
    // draw_command_card's queue-strip block).
    static constexpr double kStatsColNative = 204.0;
    int stats_col_x() const { return ui(kStatsColNative); }
    // Left edge of the production-queue grid (draw_command_card) -- kept
    // as an absolute anchor (not stats_col_x() + a fixed offset) so it
    // stays clear of the minimap regardless of the stats column's own
    // width, and is also used by draw_hud to cap the selected entity's
    // name width so a long name (e.g. a civ's "base" display name, some of
    // which run longer than "Westminster Palace") never draws over it.
    static constexpr double kQueueColNative = 400.0;
    int queue_col_x() const { return ui(kQueueColNative); }
    // (audio dispatch lives inline in drain_events(); Audio itself is the
    // reusable "play by key" utility)

    // Render-side interpolation: units/projectiles only move once per sim
    // tick (20Hz), which looks stepped/juddery at a 60Hz render rate --
    // this smooths it out. prev_*_pos_ holds each entity's position as of
    // the END of the PREVIOUS tick (captured just before stepping to the
    // next one); draw() lerps from there to the live (just-simulated)
    // position by how far accumulator_ is into the next tick.
    struct Pos { double x, y; };
    std::unordered_map<uint32_t, Pos> prev_unit_pos_, prev_proj_pos_;
    void snapshot_positions();

    // Tracked from incoming mouse events rather than read via
    // SDL_GetMouseState(), which reports raw OS window coordinates -- those
    // no longer match our fixed virtual canvas once the window is letterboxed
    // (see app.cpp's real-window-to-virtual-canvas mouse transform).
    SDL_Point mouse_pos_{0, 0};

    ww::sim::Match match_;
    Camera cam_;
    SpriteAtlas atlas_;
    // Owned by app.cpp main() and shared by reference, NOT by value: music
    // (title theme) has to keep playing across the menu -> match -> menu
    // transitions, but GameClient is constructed/destroyed per match. A single
    // long-lived Audio (one Mix_OpenAudio) owns the SFX cache + music stream.
    Audio& audio_;
    // Loaded fresh in the constructor -- the Hotkeys options screen (menu,
    // which runs before GameClient exists) already saved anything the
    // player rebound to the same settings.json this reads back.
    Settings settings_;
    TextRenderer text_;         // bold MS Serif -- names/HUD labels
    TextRenderer text_regular_; // non-bold MS Serif -- HP/armor/attack/carry stat numbers
    double fps_ = 0.0;
    // See set_spectator()'s comment.
    bool spectator_ = false;
    // Debug perf overlay (F3): wall-clock cost of each match_.step() call,
    // in milliseconds, over roughly the last second of ticks (20 at the
    // fixed 20Hz rate) -- see update()'s instrumentation and
    // draw_perf_overlay.
    bool show_perf_overlay_ = false;
    std::deque<double> tick_ms_history_;
    // Debug resource readout (F1): appends "food/wood/oil/iron vN" to each
    // team's line in the bottom-right scoreboard, for diagnosing what the AI
    // is doing with its economy. Toggled in handle_event.
    bool show_res_debug_ = false;

    double accumulator_ = 0.0;
    // Wall-clock seconds since match start, advanced every real frame (not
    // gated by the fixed sim step) -- purely visual, drives the ocean-tile
    // shimmer (water tiles flip horizontally on a per-tile stagger).
    double render_clock_ = 0.0;
    // Atomic-detonation cinematic (see drain_events' "nuke_flash"): a fading
    // full-screen white flash and a decaying camera shake.
    double nuke_flash_t_ = 0.0;
    double nuke_shake_t_ = 0.0;
    static constexpr double kFixedDt = 1.0 / 20.0;
    // 1.0 = native pixels: at the 640x480 virtual canvas, every sprite
    // (world tiles and HUD icons alike) draws at its true native
    // resolution with no scaling, so nothing is blurred/resampled from
    // being rendered non-integer size.
    static constexpr double kDefaultZoom = 1.0;
    // HUD/cursor scale, matched to kDefaultZoom so native-resolution UI
    // art (buttons, icons, the cursor) reads at the same pixel density as
    // world sprites do at the default zoom. Named separately from
    // kDefaultZoom since it's conceptually a different knob (UI density,
    // not camera framing) even though they start equal.
    static constexpr double kUiScale = kDefaultZoom;
    // 1 "unit" = a civilian's sprite height (32 native px) -- the HUD's
    // base grid size. Both HUD bars are 2 units tall; buttons (using the
    // real spr_button border sprite) are 1 unit square.
    static constexpr double kUnit = 32.0;
    // AoE-style edge-pan: camera scrolls when the cursor sits within this
    // many native px of the window edge, ramping from 0 speed at the zone's
    // inner boundary up to kEdgePanSpeed (matches app.cpp's arrow-key
    // pan_speed, 400 px/sec at zoom 1) right at the edge.
    static constexpr double kEdgePanMarginNative = 20.0;
    static constexpr double kEdgePanSpeed = 400.0;

    std::vector<ww::sim::EntityRef> selected_;
    std::optional<SDL_Point> drag_start_;
    bool dragging_ = false;
    // Double-click ("select all of this type in view") detection. Tracked
    // manually rather than via SDL's ev.button.clicks, whose OS-driven interval
    // is too lenient here (ordinary quick single-clicks were registering as
    // double-clicks): a double-click requires the two presses within
    // kDoubleClickMs AND landing on nearly the same spot. double_click_ is set
    // on left-button-down and consumed by handle_left_up's plain-click branch.
    bool double_click_ = false;
    Uint32 last_left_click_ms_ = 0;
    SDL_Point last_left_click_pos_{-1000, -1000};
    static constexpr Uint32 kDoubleClickMs = 250; // tighter than the OS default (~500ms)
    bool minimap_dragging_ = false; // left button went down on the minimap; follow mouse until release
    bool mid_dragging_ = false; // AoE-style middle-mouse-button drag-to-pan
    SDL_Point mid_drag_last_{0, 0};
    // Right-button formation drag (see formation_drag_eligible / issue_formation):
    // set on right-down when a unit selection is active, promoted to a real
    // formation drag once the cursor moves past the click threshold.
    std::optional<SDL_Point> rdrag_start_;
    bool r_dragging_ = false;
    // Right-drag formation shape: "staggered" / "box" (line) / "column"
    // (single file) / "split" (flanking). Set directly by the 4 formation
    // command-card buttons/hotkeys (formation_column_key/formation_box_key/
    // formation_stagger_key/formation_split_key, see activate_card_button
    // and handle_hotkey's dedicated[] table). Consumed by formation_slots.
    std::string formation_type_ = "box";
    std::string placing_; // non-empty => next left-click places this building
    // Wall drag-build: while placing_ is a wall (palisade / iron wall), a
    // left press starts a drag and the release lays a whole connected LINE of
    // wall segments from the press cell to the release cell (a plain click
    // with no drag just drops one). wall_line() rasterizes that cell path --
    // supporting orthogonal and 45-degree diagonal runs (and staircases in
    // between) -- and is shared by both the live ghost preview and the
    // release-time placement so the preview never lies about what gets built.
    bool wall_dragging_ = false;
    std::optional<SDL_Point> wall_drag_start_;
    static bool is_wall(const std::string& name) {
        return name == "palisade" || name == "iron wall";
    }
    // Snapped world-space centre of every wall segment along the drag line
    // from world (wx0,wy0) to (wx1,wy1), for the current `placing_` wall.
    std::vector<std::pair<double, double>> wall_line(double wx0, double wy0, double wx1, double wy1);
    bool attack_move_armed_ = false; // next right-click issues an attack-move instead of a plain move
    bool attack_ground_armed_ = false; // artillery: next right-click sets a bombard-this-point target
    bool land_armed_ = false; // aircraft: next left-click on an own airbase sends them there to land
    bool unload_armed_ = false; // transport: next left-click on a shoreline unloads its cargo
    std::string building_category_; // "" (all) | "eco" | "military" -- filters the build button list
    int shipyard_page_ = 0; // shipyard command card: 0 = passive (fishing/transport), 1 = warships
    // Market command card: 0 = trade table (buy/sell/replant), 1 = techs. The
    // 6 buy/sell cells + replant leave too few cells for the market's era-0
    // techs to coexist without overprinting the trade icons, so techs live on
    // their own page behind a toggle (same pattern as shipyard_page_).
    int market_page_ = 0;

    std::vector<ClientEffect> effects_;
    std::vector<ClientSmoke> smoke_;
    std::vector<TargetFlash> target_flashes_;
    std::vector<ClientNotification> notifications_;
    std::vector<ClientChatMessage> chat_log_;
    std::vector<MinimapPing> minimap_pings_;
    // Enter opens the chat bar and pauses the sim (see update()'s early-out
    // below); Enter again submits (either as a chat line, or as a
    // recognized cheat command -- see submit_chat), Escape cancels without
    // sending. While open, all other input (hotkeys, clicks) is swallowed
    // by handle_event so typing can't also issue orders/fire hotkeys.
    bool chat_open_ = false;
    bool paused_ = false;
    std::string chat_input_;
    void open_chat();
    void submit_chat();
    void draw_chat(SDL_Renderer* renderer);

    // Top-right pause button (spr_pausemenu) -- always visible, click to
    // pause and bring up a centered menu (currently just Resume/Quit Game).
    // Shares `paused_` with the chat bar (update() freezes the sim the
    // same way either way), but tracks its own open/closed state since
    // it's a different overlay.
    bool pause_menu_open_ = false;
    bool quit_to_menu_ = false;
    // Set only by the campaign constructor (empty for a skirmish match) --
    // "is this a campaign level, and if so which one" for the game-over
    // handling in update(), which records a win via campaign_level_key()
    // (settings.h) before setting quit_to_menu_. game_over_handled_ makes
    // that a one-shot: Control::game_over stays true for every remaining
    // tick once set (see sim/src/control_ai.cpp's check_win), so without
    // this guard update() would try to re-save settings_ every tick until
    // the match actually tears down.
    std::string campaign_name_;
    std::string campaign_level_id_;
    // Shown (non-hidden) campaign objectives for the top-right tracker overlay:
    // {objective id, display name}. Completion is read live from
    // World::cleared_objectives (by id). Empty for skirmish matches.
    std::vector<std::pair<std::string, std::string>> campaign_objectives_;
    bool game_over_handled_ = false;
    SDL_Rect pause_button_rect_{0, 0, 0, 0};
    SDL_Rect pause_resume_rect_{0, 0, 0, 0};
    SDL_Rect pause_quit_rect_{0, 0, 0, 0};
    void draw_pause_button(SDL_Renderer* renderer);
    void draw_pause_menu(SDL_Renderer* renderer);
    void handle_pause_menu_click(int mx, int my);

    // ---- post-game statistics screen (AoE2-style, shown when the player
    // quits the match; see open_stats_screen / draw_stats_screen). Tabs:
    // Score / Military / Largest Battle / Economy / Technology / Society /
    // Timeline. "Return to Menu" then sets quit_to_menu_. ----
    // Victory/Defeat banner raised the moment the match is decided (skirmish +
    // campaign). Two buttons: keep watching the map, or open the stats screen.
    bool game_over_banner_open_ = false;
    bool game_over_won_ = false;
    SDL_Rect banner_watch_rect_{0, 0, 0, 0};
    SDL_Rect banner_stats_rect_{0, 0, 0, 0};
    void draw_game_over_banner(SDL_Renderer* renderer);
    bool handle_game_over_click(int mx, int my); // true if a banner button was hit

    bool stats_open_ = false;
    int stats_tab_ = 0; // index into the tab list
    ww::stats::MatchStats stats_;
    std::vector<std::pair<SDL_Rect, int>> stats_tab_rects_; // rect -> tab index
    SDL_Rect stats_return_rect_{0, 0, 0, 0};
    void open_stats_screen();
    void draw_stats_screen(SDL_Renderer* renderer);
    void draw_stats_timeline(SDL_Renderer* renderer, const SDL_Rect& body);
    void handle_stats_click(int mx, int my);

    std::vector<CardButton> card_buttons_;
    std::vector<QueueButton> queue_buttons_;

    // Screen-space minimap bounds, rebuilt each draw_minimap() and
    // consulted by handle_left_down (same "layout then hit-test last
    // frame's rect" pattern as card_buttons_) for click-to-jump.
    SDL_Rect minimap_rect_{0, 0, 0, 0};
    // Cached minimap background (terrain + forests + fog) as a single
    // streaming texture at world-grid resolution. Rebuilt each frame into a
    // CPU pixel buffer (cheap memory writes) then uploaded once and blitted
    // once -- replaces tens of thousands of per-tile SDL_RenderFillRect calls
    // (two full-map loops, most alpha-blended) that dominated the frame at
    // large map sizes. Recreated when the map dimensions change; the dynamic
    // dots (units/buildings) and viewport rect still draw per-frame on top.
    SDL_Texture* minimap_bg_tex_ = nullptr;
    int minimap_bg_cols_ = 0, minimap_bg_rows_ = 0;
    std::vector<uint32_t> minimap_bg_px_;
    // Screen-space idle-villager button bounds, rebuilt each draw_idle_
    // button() and consulted by handle_left_down, same pattern as
    // minimap_rect_/pause_button_rect_ above.
    SDL_Rect idle_button_rect_{0, 0, 0, 0};

    int view_w_, view_h_;
};
