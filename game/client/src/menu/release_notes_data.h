#pragma once
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Title screen's "Update Notes" popup, loaded at runtime from
// <data_dir>/release_notes.json instead of the single hardcoded changelog
// entry it used to be -- same "presentational-only, load directly instead
// of going through DataStore" pattern as tech_tree_data.h. Ordered
// newest-first; add a new entry to the front of the JSON array with each
// release instead of overwriting the old one, so players can browse back
// through past versions (see MenuController's update_notes_index_ and the
// Left/Right buttons in draw_title).
namespace ww::menu {

struct ReleaseNotesEntry {
    std::string version;
    std::string title;
    std::vector<std::string> lines;
};

inline std::vector<ReleaseNotesEntry> load_release_notes(const std::string& data_dir) {
    std::vector<ReleaseNotesEntry> out;
    std::ifstream f(data_dir + "/release_notes.json");
    if (!f) return out; // no file yet -- draw_title shows a graceful "none available" message
    nlohmann::json j;
    try {
        f >> j;
    } catch (...) {
        return out; // corrupt/hand-edited-invalid -- same as an empty list, not a crash
    }
    for (auto& item : j) {
        ReleaseNotesEntry e;
        e.version = item.value("version", "");
        e.title = item.value("title", "");
        if (item.contains("lines") && item["lines"].is_array()) {
            for (auto& line : item["lines"]) {
                if (line.is_string()) e.lines.push_back(line.get<std::string>());
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

// `data_dir` only matters on the first call -- cached for the process's
// lifetime, same as tech_tree_grid()/DataStore's one-shot loads.
inline const std::vector<ReleaseNotesEntry>& release_notes(const std::string& data_dir) {
    static const std::vector<ReleaseNotesEntry> notes = load_release_notes(data_dir);
    return notes;
}

} // namespace ww::menu
