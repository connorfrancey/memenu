#include "features/misc/key_bindings.h"

#include "core/module.h"
#include "game/game.h"
#include "paths.h"

#include <ME_SDK.hpp>
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace {

using Binding = std::pair<std::string, std::string>;   // key, command
using BindingSet = std::set<Binding>;

std::mutex g_fileMutex;
std::filesystem::path g_file;   // Empty: the default

std::filesystem::path File()
{
    std::lock_guard lock(g_fileMutex);
    return g_file.empty() ? paths::Dir() / "keybindings.json" : g_file;
}

// The game keeps one key per action when it starts: it puts back the keys of its profile (primary and alternate are
// packed in one value per action) and only one of them survives. So an alternate key set in the game's controls is gone
// the next time the game runs, or a level loads. This module remembers every key the player has bound to one of the
// game's actions (the GBA_ commands) and puts back the missing ones whenever the game resets them.
//
// When is a change the game's reset and when the player's own? A reset comes with a new PlayerInput (game start, level
// load): for the first seconds after one, keys that went missing are put back. Any change after that is the player's
// (the controls menu) and becomes what is kept.
class KeepKeyBindings final : public Module {
public:
    KeepKeyBindings()
        : Module("Keep Key Bindings", "Misc", "Keeps the alternate keys you set in the game's controls between sessions", 30)
    {
        enabledByDefault = true;
        schedule.interval = 0.5f;   // Twice a second is plenty
    }

    void OnEnable() override
    {
        game::RunOnGameThread([this] {
            input_ = nullptr;       // Next check is a reset moment: what is missing comes back
            loaded_ = false;
        });
    }

    void OnTick() override
    {
        auto* pc = game::Controller();
        if (!pc || !pc->PlayerInput) return;
        auto* in = pc->PlayerInput;

        if (!loaded_) {
            loaded_ = true;
            stored_ = Read(hasFile_);
        }
        const unsigned long long now = GetTickCount64();
        if (in != input_) {                         // A new PlayerInput: the game (re)applied its bindings
            input_ = in;
            graceUntil_ = now + kGraceMs;
            fingerprint_ = 0;
        }

        // The names are only read when the list changed: turning ~280 FNames into strings costs milliseconds
        const std::uint64_t fp = Fingerprint(in);
        const bool firstSave = !hasFile_ && now >= graceUntil_;
        if (fp == fingerprint_ && !firstSave) return;
        fingerprint_ = fp;
        BindingSet cur = Current(in);

        if (now < graceUntil_) {
            if (hasFile_ && Restore(in, cur)) {
                cur = Current(in);
                fingerprint_ = Fingerprint(in);
            }
        } else if (!hasFile_ || cur != last_) {     // The player changed them (or first run): keep what is there now
            if (hasFile_ && cur != stored_) Log(stored_, cur);
            stored_ = cur;
            hasFile_ = Write(stored_);
        }
        last_ = cur;
    }

private:
    static constexpr unsigned long long kGraceMs = 15000;

    // Changes when a binding is added, removed or given another command (SetBind gives it a new string)
    static std::uint64_t Fingerprint(Classes::UPlayerInput* in)
    {
        std::uint64_t h = 1469598103934665603ull ^ in->Bindings.Num();
        for (std::size_t i = 0; i < in->Bindings.Num(); ++i) {
            const auto& b = in->Bindings[i];
            const auto cmd = reinterpret_cast<std::uintptr_t>(b.Command.IsValid() ? b.Command.c_str() : nullptr);
            h = (h ^ static_cast<std::uint32_t>(b.Name.Index)) * 1099511628211ull;
            h = (h ^ cmd) * 1099511628211ull;
        }
        return h;
    }

    // Keyboard and mouse keys bound to one of the game's actions (gamepad buttons and the GBA_ aliases themselves are
    // the game's own business)
    static BindingSet Current(Classes::UPlayerInput* in)
    {
        BindingSet out;
        for (std::size_t i = 0; i < in->Bindings.Num(); ++i) {
            const auto& b = in->Bindings[i];
            if (b.Control || b.Shift || b.Alt || !b.Command.IsValid()) continue;
            std::string key = b.Name.GetName(), cmd = b.Command.ToString();
            if (key.rfind("GBA_", 0) == 0 || key.rfind("XboxTypeS_", 0) == 0) continue;
            if (cmd.rfind("GBA_", 0) != 0) continue;
            out.emplace(std::move(key), std::move(cmd));
        }
        return out;
    }

    // Keys bound to anything at all (an alternate is never put on a key the player uses for something else)
    static std::set<std::string> UsedKeys(Classes::UPlayerInput* in)
    {
        std::set<std::string> out;
        for (std::size_t i = 0; i < in->Bindings.Num(); ++i) {
            const auto& b = in->Bindings[i];
            if (b.Control || b.Shift || b.Alt) continue;
            if (b.Command.IsValid() && !b.Command.ToString().empty()) out.insert(b.Name.GetName());
        }
        return out;
    }

    // Puts back the kept bindings the game dropped. True if any was.
    bool Restore(Classes::UPlayerInput* in, const BindingSet& cur)
    {
        const auto used = UsedKeys(in);
        int restored = 0;
        for (const Binding& b : stored_) {
            if (cur.count(b) || used.count(b.first)) continue;
            const std::wstring cmd(b.second.begin(), b.second.end());
            // The game's own exec: adds the binding and saves TdInput.ini, like the controls menu does
            in->SetBind(Classes::FName(b.first.c_str()), Classes::FString(cmd.c_str()));
            spdlog::info("Key bindings: put back {} = {}", b.first, b.second);
            ++restored;
        }
        return restored > 0;
    }

    static void Log(const BindingSet& before, const BindingSet& after)
    {
        for (const Binding& b : after)
            if (!before.count(b)) spdlog::info("Key bindings: now {} = {}", b.first, b.second);
        for (const Binding& b : before)
            if (!after.count(b)) spdlog::info("Key bindings: no longer {} = {}", b.first, b.second);
    }

    static BindingSet Read(bool& found)
    {
        found = false;
        BindingSet out;
        std::ifstream f(File());
        if (!f) return out;
        const auto j = nlohmann::json::parse(f, nullptr, false);
        if (!j.is_object() || !j.contains("bindings") || !j["bindings"].is_array()) {
            spdlog::warn("Key bindings: {} can't be read, starting over", File().string());
            return out;
        }
        for (const auto& b : j["bindings"])
            if (b.is_object() && b.contains("key") && b.contains("command") && b["key"].is_string() && b["command"].is_string())
                out.emplace(b["key"].get<std::string>(), b["command"].get<std::string>());
        found = true;
        spdlog::info("Key bindings: {} kept bindings in {}", out.size(), File().string());
        return out;
    }

    static bool Write(const BindingSet& set)
    {
        nlohmann::json list = nlohmann::json::array();
        for (const Binding& b : set) list.push_back({{"key", b.first}, {"command", b.second}});
        const auto path = File();
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) return false;
            out << nlohmann::json{{"bindings", std::move(list)}}.dump(1);
            if (!out) return false;
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
        spdlog::info("Key bindings: kept {} bindings", set.size());
        return true;
    }

    // Game thread
    Classes::UPlayerInput* input_ = nullptr;
    unsigned long long graceUntil_ = 0;
    bool loaded_ = false, hasFile_ = false;
    BindingSet stored_, last_;
    std::uint64_t fingerprint_ = 0;
};
MEM_MODULE(KeepKeyBindings)

} // namespace

void keep_bindings::SetFile(const std::filesystem::path& file)
{
    std::lock_guard lock(g_fileMutex);
    g_file = file;
}
