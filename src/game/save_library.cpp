#include "game/save_library.h"
#include "game/game.h"
#include "game/player_state.h"
#include "paths.h"

#include <ME_SDK.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>

namespace {

using json = nlohmann::json;

struct Stored {
    save_library::Entry info;
    json state;                 // player_state::Export()
};

std::mutex g_mutex;
std::filesystem::path g_folder;     // Empty: the default
std::string g_map;                  // Lower case
bool g_read = false;                // g_entries holds g_map's file
std::vector<Stored> g_entries;
int g_selected = -1;

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::filesystem::path FolderLocked() { return g_folder.empty() ? paths::Dir() / "savestates" : g_folder; }

std::filesystem::path FileFor(const std::string& map)
{
    std::string name;
    for (char c : map) name += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? c : '_';
    return FolderLocked() / (name + ".json");
}

std::string Now()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

// The list's numbers come from the stored state itself
void Describe(Stored& s)
{
    const json& st = s.state;
    if (const auto it = st.find("location"); it != st.end() && it->is_array() && it->size() == 3) {
        s.info.x = (*it)[0].get<float>();
        s.info.y = (*it)[1].get<float>();
        s.info.z = (*it)[2].get<float>();
    }
    if (const auto it = st.find("velocity"); it != st.end() && it->is_array() && it->size() == 3)
        s.info.speed = std::hypot((*it)[0].get<float>(), (*it)[1].get<float>());
}

void ReadLocked()
{
    g_entries.clear();
    g_selected = -1;
    g_read = true;
    if (g_map.empty()) return;

    const auto path = FileFor(g_map);
    std::ifstream in(path);
    if (!in) return;
    const json j = json::parse(in, nullptr, false);
    if (!j.is_object() || !j.contains("states") || !j["states"].is_array()) {
        spdlog::warn("Save states: {} is not a save state file, ignored", path.string());
        return;
    }
    for (const json& e : j["states"]) {
        if (!e.is_object() || !e.contains("state")) continue;
        Stored s;
        s.info.name = e.value("name", std::string("State"));
        s.info.created = e.value("created", std::string());
        s.state = e["state"];
        try {
            Describe(s);
        } catch (const json::exception&) {
            continue;
        }
        g_entries.push_back(std::move(s));
    }
    if (!g_entries.empty()) g_selected = 0;
    spdlog::info("Save states: {} in {}", g_entries.size(), path.string());
}

void WriteLocked()
{
    if (g_map.empty()) return;
    json states = json::array();
    for (const Stored& s : g_entries) states.push_back({{"name", s.info.name}, {"created", s.info.created}, {"state", s.state}});
    const json j = {{"map", g_map}, {"states", std::move(states)}};

    const auto path = FileFor(g_map);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // A temporary file swapped in: a crash mid-write never destroys the previous list
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            spdlog::error("Save states: can't write {}", tmp.string());
            return;
        }
        out << j.dump(1);
        if (!out) {
            spdlog::error("Save states: write failed: {}", tmp.string());
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) spdlog::error("Save states: can't replace {}: {}", path.string(), ec.message());
}

// Switches the list to `map` (lower-cased here); empty keeps the current one
void FollowLocked(const std::string& map)
{
    const std::string m = Lower(map);
    if (!m.empty() && m != g_map) {
        g_map = m;
        g_read = false;
    }
    if (!g_read) ReadLocked();
}

bool Valid(int index) { return index >= 0 && index < static_cast<int>(g_entries.size()); }

bool Fail(std::string* error, std::string text)
{
    if (error) *error = std::move(text);
    return false;
}

// Game thread: the player's state now, as data
bool Capture(json& state, std::string* error)
{
    if (!game::Pawn()) return Fail(error, "no player");
    if (!player_state::Save()) return Fail(error, "failed (see the log)");
    state = player_state::Export();
    return !state.is_null() || Fail(error, "failed (see the log)");
}

} // namespace

std::vector<save_library::Entry> save_library::List(const std::string& map)
{
    std::lock_guard lock(g_mutex);
    FollowLocked(map);
    std::vector<Entry> out;
    out.reserve(g_entries.size());
    for (const Stored& s : g_entries) out.push_back(s.info);
    return out;
}

std::string save_library::Map()
{
    std::lock_guard lock(g_mutex);
    return g_map;
}

int save_library::Selected()
{
    std::lock_guard lock(g_mutex);
    return g_selected;
}

void save_library::Select(int index)
{
    std::lock_guard lock(g_mutex);
    if (Valid(index)) g_selected = index;
}

void save_library::SelectNext(int step)
{
    std::lock_guard lock(g_mutex);
    const int n = static_cast<int>(g_entries.size());
    if (n == 0) return;
    g_selected = g_selected < 0 ? 0 : ((g_selected + step) % n + n) % n;
}

bool save_library::SaveNew(std::string* error)
{
    json state;
    if (!Capture(state, error)) return false;

    std::lock_guard lock(g_mutex);
    FollowLocked(game::MapName());
    if (g_map.empty()) return Fail(error, "not in a level");
    Stored s;
    // "State 3": one more than the highest number used so far, so deleting one doesn't produce a duplicate name
    int highest = 0;
    for (const Stored& e : g_entries)
        if (e.info.name.rfind("State ", 0) == 0) highest = std::max(highest, std::atoi(e.info.name.c_str() + 6));
    s.info.name = "State " + std::to_string(highest + 1);
    s.info.created = Now();
    s.state = std::move(state);
    Describe(s);
    g_entries.push_back(std::move(s));
    g_selected = static_cast<int>(g_entries.size()) - 1;
    WriteLocked();
    spdlog::info("Save states: saved \"{}\" in {}", g_entries.back().info.name, g_map);
    return true;
}

bool save_library::Overwrite(int index, std::string* error)
{
    json state;
    {
        std::lock_guard lock(g_mutex);
        FollowLocked(game::MapName());
        if (!Valid(index)) return Fail(error, "no state selected");
    }
    if (!Capture(state, error)) return false;

    std::lock_guard lock(g_mutex);
    if (!Valid(index)) return Fail(error, "no state selected");
    Stored& s = g_entries[index];
    s.state = std::move(state);
    s.info.created = Now();
    Describe(s);
    g_selected = index;
    WriteLocked();
    return true;
}

bool save_library::Load(int index, std::string* error)
{
    json state;
    {
        std::lock_guard lock(g_mutex);
        FollowLocked(game::MapName());
        if (!Valid(index)) return Fail(error, "no state selected");
        state = g_entries[index].state;
        g_selected = index;
    }
    auto* pawn = game::Pawn();
    if (!pawn || pawn->Health <= 0) return Fail(error, "no player");

    // The current Save State comes back if this one can't be used
    const json previous = player_state::Export();
    if (!player_state::Import(state)) return Fail(error, "the stored state can't be read (see the log)");
    if (player_state::Load()) return true;
    if (previous.is_null()) player_state::Clear();
    else player_state::Import(previous);
    return Fail(error, "not right now (see the log)");
}

void save_library::Rename(int index, const std::string& name)
{
    std::lock_guard lock(g_mutex);
    if (!Valid(index) || name.empty() || g_entries[index].info.name == name) return;
    g_entries[index].info.name = name;
    WriteLocked();
}

void save_library::Remove(int index)
{
    std::lock_guard lock(g_mutex);
    if (!Valid(index)) return;
    g_entries.erase(g_entries.begin() + index);
    const int n = static_cast<int>(g_entries.size());
    if (g_selected >= n) g_selected = n - 1;
    WriteLocked();
}

void save_library::MoveBy(int index, int step)
{
    std::lock_guard lock(g_mutex);
    const int to = index + step;
    if (!Valid(index) || !Valid(to)) return;
    std::swap(g_entries[index], g_entries[to]);
    if (g_selected == index) g_selected = to;
    else if (g_selected == to) g_selected = index;
    WriteLocked();
}

void save_library::SetFolder(const std::filesystem::path& folder)
{
    std::lock_guard lock(g_mutex);
    g_folder = folder;
    g_read = false;     // Read again from the new place
}

std::filesystem::path save_library::Folder()
{
    std::lock_guard lock(g_mutex);
    return FolderLocked();
}
