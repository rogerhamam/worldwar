#include "audio/audio.h"

#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace {
// Direct port of game/audio.py's SFX dict (logical key -> wav basename(s),
// random choice among alternatives). Where the Python dict had a literal
// key defined twice, this uses the value that actually wins at runtime
// (the later entry overwrites the earlier one in a Python dict literal).
const std::unordered_map<std::string, std::vector<std::string>>& sfx_table() {
    static const std::unordered_map<std::string, std::vector<std::string>> table = {
        {"gunshot", {"snd_gunshot"}}, {"cannon", {"snd_cannon"}}, {"cannon_fire", {"snd_cannon_fire"}},
        {"gunshot_deep", {"snd_gunshot_deep"}}, // Royal Marine: gunshot pitched 20% deeper
        {"heavy_cannon", {"snd_heavy_cannon"}}, // Heavy Artillery: cannon_fire pitched deeper + 20% louder

        {"sword", {"snd_sword", "snd_sword2", "snd_sword3", "snd_sword4"}},
        {"explosion", {"snd_explosion1"}}, {"missile", {"snd_missile_impact1"}},
        {"build", {"snd_build1"}}, {"wall", {"snd_wall_create"}},
        {"chop", {"snd_cut_wood"}}, {"mine", {"snd_mine"}}, {"forage", {"snd_collect_food"}},
        {"fish", {"snd_fishing"}},
        {"plane_gun", {"snd_gunshot"}}, {"plane_crash", {"snd_plane_crash"}},
        {"plane_move", {"snd_plane_backing"}}, {"plane_engine", {"snd_plane_backing"}},
        {"plane_spawn", {"snd_plane"}},
        {"cavalry_move", {"snd_cavalry_move"}},
        {"tank", {"snd_tank_backing"}}, // ground-vehicle engine (no voice line)
        {"artillery_move", {"snd_tank_move2"}}, // snd_tank_move2 is now artillery-only (artillery1/artillery)
        {"chat", {"snd_chat"}},                 // map message / resource-pickup activation blip
        {"pop_full", {"snd_population_limit"}}, {"error", {"snd_error_click"}},
        {"trade", {"snd_collect_food"}}, {"warning", {"snd_warning_attack"}},
        {"farm_exhausted", {"snd_farm_exhausted"}}, {"farm_built", {"snd_farm_select"}},
        {"ship_horn", {"snd_ship_horn"}},
        {"building_destroyed", {"snd_building_destroy_0", "snd_building_destroy_1", "snd_building_destroy_2"}},
        {"select", {"snd_menu_select"}}, {"click", {"snd_click_button"}},
        {"military_spawn", {"snd_military_spawn"}}, {"villager_spawn", {"snd_villager_spawn"}},
        {"age_advance", {"snd_age_advance"}}, {"victory", {"snd_victory"}}, {"lose", {"snd_lose"}},
        {"waypoint", {"snd_waypoint"}}, {"research", {"snd_research_complete"}},
        {"population", {"snd_population_limit"}},
        {"ship_move", {"snd_ship_move"}}, {"ship_select", {"snd_ship_select"}},
        {"male_death", {"snd_male_death1", "snd_male_death2"}},
        {"cavalry_death", {"snd_cavalry_death1", "snd_cavalry_death2"}},
        {"ship_sink", {"snd_ship_sink1", "snd_ship_sink2"}},
        {"camel", {"snd_camel_select1", "snd_camel_select2"}},
        {"camel_death", {"snd_camel_death1", "snd_camel_death2"}},
    };
    return table;
}

// game/audio.py CIV_VOICE: civ id -> nation prefix for voice-line filenames.
// Civs with no recorded voice set (capitol=1, china=7, unknown) -> no voice.
const std::unordered_map<int, std::string>& civ_voice_table() {
    static const std::unordered_map<int, std::string> table = {
        {0, "uk"}, {2, "nazi"}, {3, "soviet"}, {4, "japan"},
        {5, "italy"}, {6, "french"}, {8, "ottoman"},
    };
    return table;
}

// Per-SFX gain multiplier, keyed by the same logical key used in sfx_table()
// above -- a quick lever to balance one sound against the rest of the mix
// (which all otherwise play at the same sfx_volume_) without writing a new
// `if (key == ...)` special case each time. Absent from this table => 1.0
// (unchanged). When a newly-added sound turns out too loud or too quiet
// next to everything else, add/adjust its entry here first.
const std::unordered_map<std::string, double>& sfx_gain_table() {
    static const std::unordered_map<std::string, double> table = {
        {"tank", 0.47},       // engine SFX -- reduced a further 33% (0.7 -> 0.47)
        {"explosion", 0.225}, // halved (was 0.45) -- still too loud next to the rest of the mix
    };
    return table;
}

const std::unordered_map<std::string, std::string>& building_sound_table() {
    static const std::unordered_map<std::string, std::string> table = {
        {"base", "snd_base"}, {"barracks", "snd_barracks"}, {"factory", "snd_factory"},
        {"house", "snd_house"}, {"university", "snd_university"}, {"tower", "snd_tower"},
        {"aa tower", "snd_tower"}, {"shipyard", "snd_shipyard"}, {"academy", "snd_academy"},
        {"fortress", "snd_fortress"}, {"airbase", "snd_factory"}, {"farm", "snd_farm_select"},
        {"outpost", "snd_tower"}, // outpost shares the tower's select/built sound
        // The market had no entry at all, so selecting one (or finishing one)
        // was silent while every other building spoke up. snd_govt_center is the
        // one civic-building clip in assets/sounds that nothing else claims --
        // the market is the civic/trade building, so it takes it.
        {"market", "snd_govt_center"},
        // The refinery and reactor were mute for the same reason; both are
        // industrial plant, so they take the factory's clip. Walls (palisade,
        // iron wall) stay deliberately absent -- building_ready fires per
        // SEGMENT, so a clip here would machine-gun across a wall line.
        {"refinery", "snd_factory"}, {"nuclear reactor", "snd_factory"},
    };
    return table;
}
} // namespace

Audio::Audio(const std::string& sfx_dir) : sfx_dir_(sfx_dir) {
    // The music files are .mp3, which SDL_mixer only decodes once the MP3
    // codec is enabled -- without this, Mix_LoadMUS silently returns null and
    // there's no music. SFX (.wav) don't need it. Best-effort: modern
    // SDL_mixer has built-in mp3 support that may not report the flag, so
    // don't treat a missing flag as fatal.
    Mix_Init(MIX_INIT_MP3);
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 512) == 0) {
        Mix_AllocateChannels(24);
        ok_ = true;
    } else {
        SDL_Log("Audio disabled (Mix_OpenAudio failed): %s", Mix_GetError());
    }

    // Music lives in the sibling ".../assets/music" dir; the in-game playlist
    // is every "bgm*" file, sorted (game/audio.py: startswith("bgm"), sorted).
    music_dir_ = sfx_dir_.substr(0, sfx_dir_.find_last_of("/\\")) + "/music";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(music_dir_, ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("bgm", 0) == 0) playlist_.push_back(name);
    }
    std::sort(playlist_.begin(), playlist_.end());
}

Audio::~Audio() {
    for (auto& [name, chunk] : cache_) {
        if (chunk) Mix_FreeChunk(chunk);
    }
    if (music_) Mix_FreeMusic(music_);
    if (ok_) Mix_CloseAudio();
    Mix_Quit();
}

Mix_Chunk* Audio::load(const std::string& base) {
    auto it = cache_.find(base);
    if (it != cache_.end()) return it->second;
    std::string path = sfx_dir_ + "/" + base + ".wav";
    Mix_Chunk* chunk = Mix_LoadWAV(path.c_str());
    cache_[base] = chunk; // nullptr cached too, so a missing file isn't retried every call
    return chunk;
}

void Audio::set_listener(double vx, double vy, double vw, double vh) {
    lx_ = vx; ly_ = vy; lw_ = vw; lh_ = vh;
    have_listener_ = true;
}

void Audio::set_sfx_volume(double v) { sfx_volume_ = std::clamp(v, 0.0, 1.0); }

void Audio::set_music_volume(double v) {
    music_volume_ = std::clamp(v, 0.0, 1.0);
    if (ok_) Mix_VolumeMusic(static_cast<int>(MIX_MAX_VOLUME * music_volume_));
}

void Audio::play(const std::string& key, double x, double y, bool has_pos, int cooldown_ms) {
    if (!ok_) return;
    double vol = 1.0;
    if (has_pos && have_listener_) {
        double cx = lx_ + lw_ / 2, cy = ly_ + lh_ / 2;
        double dist = std::max(std::abs(x - cx) / (lw_ / 2 + 1), std::abs(y - cy) / (lh_ / 2 + 1));
        if (dist > 1.6) return;       // well off-screen -> don't play
        if (dist > 1.0) vol *= 0.4;   // just off-screen -> quieter
    }
    Uint32 now = SDL_GetTicks();
    auto cd_it = cooldown_until_.find(key);
    if (cd_it != cooldown_until_.end() && now < cd_it->second) return;
    cooldown_until_[key] = now + static_cast<Uint32>(cooldown_ms);

    auto it = sfx_table().find(key);
    const std::string& base = it != sfx_table().end()
        ? it->second[it->second.size() == 1 ? 0 : std::rand() % it->second.size()]
        : key; // unknown key -> treat as a literal basename, matching Python's SFX.get(key, key)
    Mix_Chunk* chunk = load(base);
    if (!chunk) return;
    auto git = sfx_gain_table().find(key);
    if (git != sfx_gain_table().end()) vol *= git->second;
    Mix_VolumeChunk(chunk, static_cast<int>(MIX_MAX_VOLUME * sfx_volume_ * vol));
    Mix_PlayChannel(-1, chunk, 0);
}

void Audio::building_sound(const std::string& building_name) {
    if (!ok_) return;
    auto it = building_sound_table().find(building_name);
    if (it == building_sound_table().end()) return;
    Mix_Chunk* chunk = load(it->second);
    if (!chunk) return;
    Mix_VolumeChunk(chunk, static_cast<int>(MIX_MAX_VOLUME * sfx_volume_));
    Mix_PlayChannel(-1, chunk, 0);
}

bool Audio::voice(const std::string& kind, int civ) {
    if (!ok_) return false;
    auto it = civ_voice_table().find(civ);
    if (it == civ_voice_table().end()) return false; // this civ has no voice set
    const std::string& nation = it->second;
    std::string line;
    if (kind == "attack" && (civ == 5 || civ == 6)) { // Italy / France have attack shouts
        static const char* atk[] = {"attack", "attack2", "attack3"};
        line = atk[std::rand() % 3];
    } else {
        static const char* ack[] = {"me", "understood", "yes"};
        line = ack[std::rand() % 3];
    }
    Mix_Chunk* chunk = load("snd_" + nation + "_" + line); // load() caches nullptr for missing files
    if (!chunk) return false; // that variant doesn't exist for this civ -> silent, same as Python
    // Acks play a touch louder than world SFX (Python: min(1.0, sfx_vol+0.2)),
    // and never distance-gated (voice() takes no position).
    Mix_VolumeChunk(chunk, static_cast<int>(MIX_MAX_VOLUME * std::min(1.0, sfx_volume_ + 0.2)));
    Mix_PlayChannel(-1, chunk, 0);
    return true;
}

void Audio::load_and_play_music(const std::string& filename, int loops) {
    if (!ok_) return;
    if (music_) {
        Mix_FreeMusic(music_);
        music_ = nullptr;
    }
    music_ = Mix_LoadMUS((music_dir_ + "/" + filename).c_str());
    if (!music_) return; // missing/undecodable -> silent no-op
    Mix_VolumeMusic(static_cast<int>(MIX_MAX_VOLUME * music_volume_));
    Mix_PlayMusic(music_, loops);
}

void Audio::play_title() {
    ingame_ = false;
    // The Battle of Britain theme is the ONLY main-menu track now (the other
    // menu theme, _title.mp3, was dropped from the rotation per the user).
    load_and_play_music("_battle_of_britain.mp3", -1); // loop it
    if (!music_) load_and_play_music("_title.mp3", -1); // fallback only if that file is missing
    if (!music_ && !playlist_.empty()) load_and_play_music(playlist_[0], -1); // last-ditch fallback
}

void Audio::play_ingame() {
    if (playlist_.empty()) return;
    // Fisher-Yates with the client-side std::rand (cosmetic; determinism N/A),
    // matching game/audio.py's random.shuffle before the first in-game track.
    for (size_t i = playlist_.size(); i > 1; --i) {
        std::swap(playlist_[i - 1], playlist_[std::rand() % i]);
    }
    pi_ = 0;
    ingame_ = true;
    load_and_play_music(playlist_[0], 0); // each track plays once; update_music advances
}

void Audio::stop_music() {
    if (ok_) Mix_HaltMusic();
    ingame_ = false;
}

void Audio::update_music() {
    // Replacement for pygame's MUSIC_END event: when the current in-game track
    // finishes, roll on to the next (endless shuffle-order cycle).
    if (ingame_ && ok_ && !playlist_.empty() && Mix_PlayingMusic() == 0) {
        pi_ = (pi_ + 1) % playlist_.size();
        load_and_play_music(playlist_[pi_], 0);
    }
}
