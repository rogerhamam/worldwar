#pragma once
#include <SDL_mixer.h>

#include <string>
#include <unordered_map>
#include <vector>

// Direct port of game/audio.py's SFX playback (not the civ-voice lines or
// music playlist -- documented deferral, see the Phase C task list).
// Degrades to a silent no-op if the mixer can't init, same as the Python
// version, so headless/test runs keep working.
class Audio {
public:
    explicit Audio(const std::string& sfx_dir);
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // (vx, vy, vw, vh) of the camera's visible world rect -- world sounds
    // fade/cut out with distance from screen center, matching Python's
    // set_listener()/play() pair.
    void set_listener(double vx, double vy, double vw, double vh);

    // has_pos=false plays at full volume regardless of listener (UI/global
    // cues like research/error/age_advance).
    void play(const std::string& key, double x, double y, bool has_pos, int cooldown_ms = 110);

    // Per-building-type voice line on construction complete (game/audio.py
    // Audio.building_sound) -- looked up by building name, not the SFX table.
    void building_sound(const std::string& building_name);

    // Per-civ unit voice acknowledgement (game/audio.py Audio.voice): `kind`
    // is "attack" (Italy/France get a shout) or anything else (a random
    // me/understood/yes ack); `civ` is the local player's civ id. Returns
    // true iff a clip actually played, so callers can do
    // `voice("attack", civ) || voice("me", civ)` to fall back to an ack.
    // Never distance-gated (no pos) -- always full volume, like the original.
    bool voice(const std::string& kind, int civ);

    // Music (game/audio.py + main.py). _title.mp3 loops on the menu;
    // bgm_theme*.mp3 shuffle-cycle in-game (each plays once, update_music()
    // advances to the next when it ends). All no-ops if the mixer is dead.
    void play_title();
    void play_ingame();
    void stop_music();
    void update_music(); // call once per frame; advances the in-game playlist

    // Options > Audio sliders. Both 0.0-1.0, defaulting to exactly the
    // literals every Mix_Volume*/Mix_VolumeChunk call here used to hardcode
    // (0.5 SFX, 0.35 music -- game/audio.py's music_vol), so a player who's
    // never touched either slider hears exactly what they always did.
    // set_sfx_volume just updates the member -- play()/building_sound()/
    // voice() all read it fresh on their next call, no propagation needed.
    // set_music_volume ALSO calls Mix_VolumeMusic immediately, since that's
    // otherwise only re-applied when a track (re)loads (load_and_play_
    // music) -- without it, dragging the music slider wouldn't affect the
    // track already playing until it happened to swap to the next one.
    void set_sfx_volume(double v);
    void set_music_volume(double v);
    double sfx_volume() const { return sfx_volume_; }
    double music_volume() const { return music_volume_; }

private:
    Mix_Chunk* load(const std::string& base);
    void load_and_play_music(const std::string& filename, int loops);

    bool ok_ = false;
    double sfx_volume_ = 0.5;
    double music_volume_ = 0.35;
    std::string sfx_dir_;
    std::unordered_map<std::string, Mix_Chunk*> cache_;
    std::unordered_map<std::string, Uint32> cooldown_until_;
    double lx_ = 0, ly_ = 0, lw_ = 0, lh_ = 0;
    bool have_listener_ = false;

    std::string music_dir_;            // sibling of sfx_dir_ (.../assets/music)
    Mix_Music* music_ = nullptr;       // currently-loaded track (freed on swap/dtor)
    std::vector<std::string> playlist_; // "bgm_theme*.mp3" filenames, sorted
    size_t pi_ = 0;                    // index into playlist_ while in-game
    bool ingame_ = false;              // true => update_music() auto-advances
};
