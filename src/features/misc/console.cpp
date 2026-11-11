// Console: a terminal in the menu. Runs the game's console commands and the menu's own, and shows what they print.
//
//   typing      the line is highlighted as you type, like zsh-syntax-highlighting: the command word green when it
//               exists (blue: a game command, purple: an alias), red when nothing has that name; strings, numbers,
//               ';' and comments in their own colors, an unclosed quote in red, recognized arguments underlined.
//               The rest of the newest matching history entry shows dimmed after the cursor (zsh-autosuggestions):
//               Right or End takes it.
//   Tab         completes what the cursor is on: commands, module and setting names, stat names, levels, scripts,
//               aliases. One match: taken. Several: their common start, then a menu below the prompt (Tab and
//               Shift+Tab, or Up/Down, go through it; Enter takes it, Esc goes back)
//   Up/Down     history; with something typed, only entries that start with it
//   Ctrl+R      search the history (again: older match; Enter runs it, Right/Tab edits it, Esc cancels)
//   Ctrl+L      clear the screen        Ctrl+C  drop the line (^C)      Ctrl+U/K  delete to the start/end
//   Ctrl+W      delete the word before the cursor                      Ctrl+A/E  start/end of the line
//
// Lines: `cmd1; cmd2` runs both, "double quotes" keep spaces together, `# ...` is a comment. Commands the console
// doesn't know go to the game (what they print comes back); `game <cmd>` sends one there even if the menu has a
// command of that name. `help` lists the menu's commands, `help <name>` explains one, `find <text>` searches all.
// History is kept in console_history.txt next to the DLL, aliases with the config, scripts (`run name`) in scripts/.
#include "core/module.h"
#include "core/setting.h"
#include "core/shell.h"
#include "font.h"
#include "game/exec_commands.h"
#include "game/game.h"
#include "game/levels.h"
#include "gui.h"
#include "log.h"
#include "paths.h"
#include "theme.h"

#include "sdk/SDK.hpp"
#include <Windows.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using shell::Style;
namespace fs = std::filesystem;

constexpr std::size_t kMaxLines = 5000;
constexpr std::size_t kMaxHistory = 1000;
constexpr int kMaxDepth = 8;     // Aliases and scripts running each other

// ---- Colors (a dark terminal palette) -------------------------------------------------------------------------------------
ImU32 Col(float r, float g, float b, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32({r, g, b, a}); }
ImVec4 Vec(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

struct Palette {
    ImU32 bg = Col(0.055f, 0.060f, 0.070f);
    ImU32 fg = Col(0.82f, 0.84f, 0.87f);
    ImU32 dim = Col(0.44f, 0.47f, 0.52f);
    ImU32 green = Col(0.47f, 0.84f, 0.50f);
    ImU32 blue = Col(0.42f, 0.70f, 0.98f);
    ImU32 purple = Col(0.78f, 0.58f, 0.96f);
    ImU32 red = Col(0.97f, 0.40f, 0.40f);
    ImU32 yellow = Col(0.93f, 0.80f, 0.42f);
    ImU32 orange = Col(0.97f, 0.62f, 0.32f);
    ImU32 cyan = Col(0.40f, 0.82f, 0.86f);
    ImU32 selection = Col(0.25f, 0.42f, 0.70f, 0.55f);
};
const Palette kP;

ImU32 StyleColor(Style s)
{
    switch (s) {
    case Style::Builtin:     return kP.green;
    case Style::GameCommand: return kP.blue;
    case Style::Alias:       return kP.purple;
    case Style::Unknown:     return kP.red;
    case Style::Known:       return kP.fg;
    case Style::String:      return kP.yellow;
    case Style::Number:      return kP.orange;
    case Style::Separator:   return kP.cyan;
    case Style::Comment:     return kP.dim;
    case Style::Error:       return kP.red;
    case Style::Partial:
    case Style::Default:     return kP.fg;
    }
    return kP.fg;
}

// ---- Output lines ----------------------------------------------------------------------------------------------------------
enum class Kind : std::uint8_t { Echo, Output, Info, Ok, Warn, Error, Game, Log };

ImU32 KindColor(Kind k)
{
    switch (k) {
    case Kind::Info:  return kP.dim;
    case Kind::Ok:    return kP.green;
    case Kind::Warn:  return kP.yellow;
    case Kind::Error: return kP.red;
    case Kind::Game:  return kP.cyan;
    case Kind::Log:   return kP.orange;
    default:          return kP.fg;
    }
}

struct Line {
    Kind kind = Kind::Output;
    std::string text;
    std::vector<Style> styles;      // Echo: the command as it was highlighted
    std::string where;              // Echo: the level in the prompt
};

// Output from other threads (game commands, the engine's messages), taken in by the render thread
std::mutex g_pendingMutex;
std::vector<std::pair<Kind, std::string>> g_pending;
std::atomic<bool> g_captureEngine{false};

void Post(Kind kind, std::string text)
{
    std::lock_guard lock(g_pendingMutex);
    g_pending.emplace_back(kind, std::move(text));
}

std::string Narrow(const Classes::FString* s)
{
    std::string out;
    if (!s || !s->IsValid()) return out;
    for (const wchar_t* c = s->c_str(); *c; ++c) out.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    return out;
}

// Console.OutputText and PlayerController.ClientMessage (cheat manager replies, "God mode on", ...): both take the
// text as their first parameter. One message often comes through both: the same text twice in a row is dropped.
void OnEngineText(Classes::UObject*, void* params)
{
    if (!g_captureEngine || !params) return;
    std::string text = Narrow(static_cast<const Classes::FString*>(params));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.empty()) return;
    static std::string last;
    static ULONGLONG lastAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (text == last && now - lastAt < 250) return;
    last = text;
    lastAt = now;
    Post(Kind::Game, std::move(text));
}

// ---- Commands ----------------------------------------------------------------------------------------------------------------
struct Builtin {
    const char* name;
    const char* usage;
    const char* help;
};

const Builtin kBuiltins[] = {
    {"help", "help [command]", "List the menu's commands, or explain one (game commands too)"},
    {"clear", "clear", "Clear the screen (also cls, Ctrl+L)"},
    {"cls", "cls", "Clear the screen"},
    {"history", "history [count | -c]", "Show the last commands (-c forgets them all)"},
    {"echo", "echo <text>", "Print the text"},
    {"alias", "alias [name [command...]]", "List aliases, show one, or make one: alias fast \"slomo 2; god\""},
    {"unalias", "unalias <name>", "Remove an alias"},
    {"find", "find <text>", "Search every command (menu, game, aliases) by name or description"},
    {"modules", "modules [filter]", "List the menu's modules and whether they are on"},
    {"toggle", "toggle <module>", "Switch a module on or off (an action runs)"},
    {"enable", "enable <module>", "Switch a module on"},
    {"disable", "disable <module>", "Switch a module off"},
    {"setting", "setting <module> [setting [value]]", "List a module's settings, show one, or change it (a button: presses it)"},
    {"pos", "pos", "Print where the player is"},
    {"tp", "tp <x> <y> <z>", "Move the player there"},
    {"run", "run <script>", "Run the commands in scripts/<script>.cfg next to the DLL, one per line"},
    {"game", "game <command>", "Send the command to the game, even if the menu has one by that name"},
    {"log", "log [on | off]", "Show the menu's warnings and errors here"},
    {"exit", "exit", "Close the console"},
};

// Commands the engine handles in C++ (not UnrealScript exec functions, so not found by scanning the classes)
struct EngineCommand {
    const char* name;
    const char* usage;
    const char* help;
};

const EngineCommand kEngineCommands[] = {
    {"stat", "stat <group>", "Engine statistics on screen: fps, unit, game, memory, anim, audio, ..."},
    {"set", "set <class> <property> <value>", "Change a property on every object of a class"},
    {"get", "get <class> <property>", "Print a property of a class's default object"},
    {"getall", "getall <class> <property>", "Print a property of every object of a class"},
    {"obj", "obj <list | gc | garbage | classes>", "Object system: list objects, collect garbage"},
    {"open", "open <map>", "Load a level by package name (edge_p, ...)"},
    {"show", "show <flag>", "Toggle a show flag: collision, bounds, fog, paths, ..."},
    {"viewmode", "viewmode <mode>", "lit, unlit, wireframe, brushwireframe, lightingonly, lightcomplexity, ..."},
    {"shot", "shot", "Take a screenshot"},
    {"setres", "setres <W>x<H>[w|f]", "Change the resolution (w: windowed, f: fullscreen)"},
    {"togglerenderingthread", "togglerenderingthread", "Run rendering on the game thread, or back on its own"},
    {"flushlog", "flushlog", "Write the game's log file out"},
    {"disconnect", "disconnect", "Leave the level (to the main menu)"},
    {"exit", "exit", "Quit the game (the menu's exit closes the console: use game exit)"},
};

const std::vector<std::string> kStats{"fps", "unit", "game", "engine", "memory", "anim", "audio", "collision",
                                      "streaming", "levels", "particles", "physics", "scenerendering", "d3d9rhi", "none"};
const std::vector<std::string> kViewModes{"lit", "unlit", "wireframe", "brushwireframe", "lightingonly",
                                          "lightcomplexity", "texturedensity", "shadercomplexity"};
const std::vector<std::string> kObj{"list", "gc", "garbage", "classes", "hash"};
const std::vector<std::string> kShow{"bounds", "collision", "fog", "paths", "volumes", "bsp", "staticmeshes",
                                     "skeletalmeshes", "particles", "decals", "postprocess", "terrain", "sprites"};

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(std::string s)
{
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

fs::path ScriptsDir() { return paths::Dir() / "scripts"; }
fs::path HistoryPath() { return paths::Dir() / "console_history.txt"; }

struct Candidate {
    std::string text, desc;
};

// ---- The module ----------------------------------------------------------------------------------------------------------------
class Console final : public Module {
public:
    Console() : Module("Console", "Misc", "A terminal: game and menu commands, completion, highlighting, history", 15)
    {
        aliases.VisibleIf([] { return false; });
    }

    void OnEnable() override
    {
        static bool hooked = false;
        if (!hooked) {
            hooked = game::OnScriptEvent("Function Engine.Console.OutputText", &OnEngineText);
            hooked = game::OnScriptEvent("Function Engine.PlayerController.ClientMessage", &OnEngineText) || hooked;
        }
        g_captureEngine = engineMessages.Get();
        refocus_ = true;
    }

    void OnDisable() override { g_captureEngine = false; }

    void OnTick() override
    {
        g_captureEngine = engineMessages.Get();
        if (!execLoaded_.load() && game::Ready()) {     // Once: a walk over the engine's functions
            auto commands = game::ExecCommands();
            std::lock_guard lock(execMutex_);
            exec_ = std::move(commands);
            execLoaded_ = true;
        }
        if (++ticks_ % 30 == 0) {
            std::string map = game::MapName();
            std::lock_guard lock(execMutex_);
            map_ = std::move(map);
        }
    }

    void OnRender() override
    {
        Start();
        Pull();
        if (!gui::visible) return;
        Draw();
    }

private:
    // ---- Setup -------------------------------------------------------------------------------------------------------------
    void Start()
    {
        if (started_) return;
        started_ = true;
        LoadHistory();
        LoadAliases();
        Print(Kind::Info, "Mirror's Edge Menu console. Type help for the commands.");
        Print(Kind::Info, "Tab completes, Up/Down go through history, Ctrl+R searches it, Ctrl+L clears.");
    }

    void LoadHistory()
    {
        std::ifstream in(HistoryPath());
        std::string line;
        while (std::getline(in, line))
            if (!Trim(line).empty()) history_.push_back(line);
        if (history_.size() > kMaxHistory) history_.erase(history_.begin(), history_.end() - kMaxHistory);
    }

    void SaveHistory()
    {
        std::ofstream out(HistoryPath(), std::ios::trunc);
        for (const auto& h : history_) out << h << '\n';
    }

    void LoadAliases()
    {
        aliasMap_.clear();
        try {
            const auto j = nlohmann::json::parse(aliases.Get().empty() ? "{}" : aliases.Get());
            for (auto it = j.begin(); it != j.end(); ++it)
                if (it.value().is_string()) aliasMap_[Lower(it.key())] = it.value().get<std::string>();
        } catch (const std::exception&) {
        }
        lookupsFor_ = -1;
    }

    void SaveAliases()
    {
        nlohmann::json j = nlohmann::json::object();
        for (const auto& [k, v] : aliasMap_) j[k] = v;
        aliases.Set(j.dump());
        lookupsFor_ = -1;
    }

    // ---- Output ------------------------------------------------------------------------------------------------------------
    void Print(Kind kind, const std::string& text)
    {
        std::size_t start = 0;
        do {
            const std::size_t nl = text.find('\n', start);
            std::string part = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!part.empty() && part.back() == '\r') part.pop_back();
            Line l;
            l.kind = kind;
            l.text = std::move(part);
            lines_.push_back(std::move(l));
            if (nl == std::string::npos) break;
            start = nl + 1;
        } while (start <= text.size());
        while (lines_.size() > kMaxLines) lines_.pop_front();
        scrollToEnd_ = true;
    }

    void Pull()
    {
        std::vector<std::pair<Kind, std::string>> pending;
        {
            std::lock_guard lock(g_pendingMutex);
            pending.swap(g_pending);
        }
        for (auto& [k, t] : pending) Print(k, t);

        std::vector<logging::Entry> entries;
        logId_ = logging::CopySince(logId_, entries);
        if (!menuLog.Get() || !started_) return;
        for (const auto& e : entries)
            if (e.level >= logging::Level::Warn) Print(e.level >= logging::Level::Error ? Kind::Error : Kind::Log, e.text);
    }

    // ---- Lookups (what each name is) ------------------------------------------------------------------------------------------
    void BuildLookups()
    {
        const int version = (execLoaded_ ? 1 : 0) + static_cast<int>(aliasMap_.size()) * 2;
        if (lookupsFor_ == version) return;
        lookupsFor_ = version;
        kinds_.clear();
        names_.clear();
        descs_.clear();
        std::lock_guard lock(execMutex_);
        for (const auto& e : kEngineCommands) Add(e.name, Style::GameCommand, e.usage);
        for (const auto& e : exec_) {
            const std::string sig = e.owner + "." + e.name + "(" + e.params + ")";
            Add(e.name, Style::GameCommand, sig);
        }
        for (const auto& b : kBuiltins) Add(b.name, Style::Builtin, b.usage);   // The menu's own win over the game's
        for (const auto& [name, body] : aliasMap_) Add(name, Style::Alias, "alias: " + body);
        std::sort(names_.begin(), names_.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    }

    void Add(const std::string& name, Style style, const std::string& desc)
    {
        const std::string lkey = Lower(name);
        if (!kinds_.count(lkey)) names_.push_back(name);
        kinds_[lkey] = style;
        descs_[lkey] = desc;
    }

    Style CommandStyle(std::string_view word) const
    {
        const std::string lkey = Lower(std::string(word));
        if (const auto it = kinds_.find(lkey); it != kinds_.end()) return it->second;
        for (const auto& n : names_)
            if (shell::StartsWithNoCase(n, word)) return Style::Partial;
        return Style::Unknown;
    }

    bool KnownArg(const shell::Command& c, std::size_t i) const
    {
        const std::string cmd = Lower(c.words[0].text);
        const std::string& arg = c.words[i].text;
        if (i == 1 && (cmd == "toggle" || cmd == "enable" || cmd == "disable" || cmd == "setting"))
            return FindModule(arg, false) != nullptr;
        if (i == 2 && cmd == "setting") {
            const Module* m = FindModule(c.words[1].text, false);
            return m && FindSetting(*m, arg);
        }
        if (i == 1 && (cmd == "help" || cmd == "find")) return kinds_.count(Lower(arg)) != 0 && cmd == "help";
        if (i == 1 && (cmd == "alias" || cmd == "unalias")) return aliasMap_.count(Lower(arg)) != 0;
        if (i == 1 && cmd == "game") return kinds_.count(Lower(arg)) && kinds_.at(Lower(arg)) == Style::GameCommand;
        return false;
    }

    std::vector<Style> HighlightLine(const std::string& line) const
    {
        shell::Highlighter h;
        h.commandStyle = [this](std::string_view w) { return CommandStyle(w); };
        h.knownArg = [this](const shell::Command& c, std::size_t i) { return KnownArg(c, i); };
        return shell::Highlight(line, h);
    }

    // ---- Modules and settings ------------------------------------------------------------------------------------------------------
    // By name (case-insensitive); `prefix`: also the only module starting with it
    static Module* FindModule(const std::string& name, bool prefix)
    {
        Module* found = nullptr;
        int matches = 0;
        Module* exact = nullptr;
        modules::ForEach([&](modules::Tab&, Module& m) {
            if (m.Hidden()) return;
            if (shell::EqualsNoCase(m.Name(), name)) exact = &m;
            else if (prefix && shell::StartsWithNoCase(m.Name(), name)) { found = &m; ++matches; }
        });
        if (exact) return exact;
        return matches == 1 ? found : nullptr;
    }

    static ctl::Setting* FindSetting(const Module& m, const std::string& name)
    {
        for (ctl::Setting* s : m.Settings())
            if (shell::EqualsNoCase(s->Name(), name) && (s->Persistent() || s->Bindable())) return s;
        return nullptr;
    }

    static std::string Value(const ctl::Setting& s)
    {
        if (!s.Persistent()) return "(button)";
        nlohmann::ordered_json j;
        s.Save(j);
        return j.dump();
    }

    // ---- Running ------------------------------------------------------------------------------------------------------------------
    void Submit(const std::string& line)
    {
        Line echo;
        echo.kind = Kind::Echo;
        echo.text = line;
        echo.styles = HighlightLine(line);
        {
            std::lock_guard lock(execMutex_);
            echo.where = map_;
        }
        lines_.push_back(std::move(echo));
        scrollToEnd_ = true;
        if (!Trim(line).empty() && (history_.empty() || history_.back() != line)) {
            history_.push_back(line);
            if (history_.size() > kMaxHistory) history_.erase(history_.begin());
            SaveHistory();
        }
        RunLine(line, 0);
    }

    void RunLine(const std::string& line, int depth)
    {
        if (depth > kMaxDepth) {
            Print(Kind::Error, "too deep: aliases or scripts running each other in a loop?");
            return;
        }
        for (const shell::Command& c : shell::Parse(line)) {
            if (c.comment || c.words.empty()) continue;
            if (c.words.back().unterminated) {
                Print(Kind::Error, "unclosed quote: " + Trim(line.substr(c.begin, c.end - c.begin)));
                continue;
            }
            Execute(c, line.substr(c.begin, c.end - c.begin), depth);
        }
    }

    // `raw`: the command as typed (for the game, which parses it itself)
    void Execute(const shell::Command& c, const std::string& raw, int depth)
    {
        const std::string name = Lower(c.words[0].text);
        std::vector<std::string> args;
        for (std::size_t i = 1; i < c.words.size(); ++i) args.push_back(c.words[i].text);
        // Everything after the command word, as typed
        const std::string rest = Trim(raw.substr(std::min(raw.size(), c.words[0].end - c.begin)));

        if (const auto a = aliasMap_.find(name); a != aliasMap_.end()) {
            RunLine(rest.empty() ? a->second : a->second + " " + rest, depth + 1);
            return;
        }
        if (name == "help") Help(args);
        else if (name == "clear" || name == "cls") lines_.clear();
        else if (name == "history") History(args);
        else if (name == "echo") Print(Kind::Output, rest);
        else if (name == "alias") Alias(c, args, raw);
        else if (name == "unalias") Unalias(args);
        else if (name == "find") Find(rest);
        else if (name == "modules") Modules(rest);
        else if (name == "toggle" || name == "enable" || name == "disable") Switch(name, args);
        else if (name == "setting") SettingCommand(args);
        else if (name == "pos") Pos();
        else if (name == "tp") Teleport(args);
        else if (name == "run") Run(args, depth);
        else if (name == "log") Log(args);
        else if (name == "exit") SetEnabled(false);
        else if (name == "game") GameCommand(rest);
        else GameCommand(Trim(raw));
    }

    void GameCommand(const std::string& cmd)
    {
        if (cmd.empty()) {
            Print(Kind::Error, "usage: game <command>");
            return;
        }
        if (!game::Ready()) {
            Print(Kind::Error, "the game integration isn't ready: game commands can't run");
            return;
        }
        const std::string word = Lower(shell::Parse(cmd).front().words.empty() ? "" : shell::Parse(cmd).front().words[0].text);
        const bool known = kinds_.count(word) != 0;
        game::RunOnGameThread([cmd, known, word] {
            std::string out;
            if (!game::ConsoleCommand(cmd, &out)) {
                Post(Kind::Error, "no player controller to run it (load a level first)");
                return;
            }
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            if (!out.empty()) Post(Kind::Output, out);
            else if (!known) Post(Kind::Info, "sent to the game (no output; '" + word + "' isn't a command the console knows)");
        });
    }

    void Help(const std::vector<std::string>& args)
    {
        if (args.empty()) {
            Print(Kind::Info, "The menu's commands (anything else goes to the game):");
            for (const auto& b : kBuiltins) {
                char line[160];
                std::snprintf(line, sizeof line, "  %-38s %s", b.usage, b.help);
                Print(Kind::Output, line);
            }
            std::size_t game = 0;
            {
                std::lock_guard lock(execMutex_);
                game = exec_.size() + std::size(kEngineCommands);
            }
            Print(Kind::Info, std::to_string(game) + " game commands: Tab lists them, find <text> searches, help <name> explains");
            return;
        }
        const std::string lkey = Lower(args[0]);
        for (const auto& b : kBuiltins)
            if (lkey == b.name) { Print(Kind::Output, std::string(b.usage) + "\n  " + b.help); return; }
        if (const auto a = aliasMap_.find(lkey); a != aliasMap_.end()) { Print(Kind::Output, "alias " + a->first + " = " + a->second); return; }
        for (const auto& e : kEngineCommands)
            if (lkey == e.name) { Print(Kind::Output, std::string(e.usage) + "   (engine)\n  " + e.help); return; }
        std::lock_guard lock(execMutex_);
        for (const auto& e : exec_)
            if (Lower(e.name) == lkey) {
                Print(Kind::Output, e.name + "(" + e.params + ")   (exec function of " + e.owner + ")");
                return;
            }
        Print(Kind::Error, "no command called " + args[0]);
    }

    void History(const std::vector<std::string>& args)
    {
        if (!args.empty() && args[0] == "-c") {
            history_.clear();
            SaveHistory();
            Print(Kind::Ok, "history cleared");
            return;
        }
        const std::size_t n = args.empty() ? 20 : static_cast<std::size_t>(std::max(1, std::atoi(args[0].c_str())));
        const std::size_t from = history_.size() > n ? history_.size() - n : 0;
        for (std::size_t i = from; i < history_.size(); ++i) {
            char num[16];
            std::snprintf(num, sizeof num, "%5zu  ", i + 1);
            Print(Kind::Output, num + history_[i]);
        }
    }

    void Alias(const shell::Command& c, const std::vector<std::string>& args, const std::string& raw)
    {
        if (args.empty()) {
            if (aliasMap_.empty()) Print(Kind::Info, "no aliases: alias <name> <command...>");
            for (const auto& [k, v] : aliasMap_) Print(Kind::Output, "  " + k + " = " + v);
            return;
        }
        const std::string name = Lower(args[0]);
        if (args.size() == 1) {
            const auto it = aliasMap_.find(name);
            Print(it == aliasMap_.end() ? Kind::Error : Kind::Output,
                  it == aliasMap_.end() ? "no alias " + name : name + " = " + it->second);
            return;
        }
        for (const auto& b : kBuiltins)
            if (name == b.name) { Print(Kind::Error, name + " is a menu command: pick another name"); return; }
        // The body as typed: one quoted word is unquoted ("slomo 2; god"), several words are kept as they are
        std::string body = args.size() == 2 && c.words[2].quoted ? args[1]
                                                                    : Trim(raw.substr(std::min(raw.size(), c.words[2].begin - c.begin)));
        aliasMap_[name] = body;
        SaveAliases();
        Print(Kind::Ok, "alias " + name + " = " + body);
    }

    void Unalias(const std::vector<std::string>& args)
    {
        if (args.empty() || !aliasMap_.erase(Lower(args[0]))) {
            Print(Kind::Error, args.empty() ? "usage: unalias <name>" : "no alias " + args[0]);
            return;
        }
        SaveAliases();
        Print(Kind::Ok, "removed " + args[0]);
    }

    void Find(const std::string& text)
    {
        if (text.empty()) { Print(Kind::Error, "usage: find <text>"); return; }
        BuildLookups();
        const std::string q = Lower(text);
        int n = 0;
        for (const auto& name : names_) {
            const std::string lkey = Lower(name);
            const std::string& desc = descs_[lkey];
            if (lkey.find(q) == std::string::npos && Lower(desc).find(q) == std::string::npos) continue;
            char line[200];
            std::snprintf(line, sizeof line, "  %-28s %s", name.c_str(), desc.c_str());
            Print(kinds_[lkey] == Style::Builtin ? Kind::Ok : Kind::Output, line);
            if (++n >= 60) { Print(Kind::Info, "  ... (more: narrow the search)"); break; }
        }
        if (!n) Print(Kind::Info, "nothing matches " + text);
    }

    void Modules(const std::string& filter)
    {
        const std::string q = Lower(filter);
        for (auto& tab : modules::Tabs()) {
            bool header = false;
            for (Module* m : tab->modules) {
                if (m->Hidden() || (!q.empty() && Lower(m->Name()).find(q) == std::string::npos)) continue;
                if (!header) { Print(Kind::Info, tab->name); header = true; }
                const char* state = m->IsAction() ? "action" : m->Enabled() ? "on" : "off";
                char line[160];
                std::snprintf(line, sizeof line, "  %-6s %s", state, m->Name().c_str());
                Print(m->Enabled() ? Kind::Ok : Kind::Output, line);
            }
        }
    }

    void Switch(const std::string& how, const std::vector<std::string>& args)
    {
        if (args.empty()) { Print(Kind::Error, "usage: " + how + " <module>"); return; }
        Module* m = FindModule(args[0], true);
        if (!m) { Print(Kind::Error, "no module called " + args[0] + " (Tab lists them)"); return; }
        if (m == this && how != "enable") { Print(Kind::Info, "use exit to close the console"); return; }
        if (m->IsAction()) {
            m->Toggle();
            Print(Kind::Ok, m->Name() + ": ran");
            return;
        }
        if (how == "toggle") m->SetEnabled(!m->Enabled());
        else m->SetEnabled(how == "enable");
        Print(m->Enabled() ? Kind::Ok : Kind::Output, m->Name() + ": " + (m->Enabled() ? "on" : "off"));
    }

    void SettingCommand(const std::vector<std::string>& args)
    {
        if (args.empty()) { Print(Kind::Error, "usage: setting <module> [setting [value]]"); return; }
        Module* m = FindModule(args[0], true);
        if (!m) { Print(Kind::Error, "no module called " + args[0]); return; }
        if (args.size() == 1) {
            Print(Kind::Info, m->Name() + ":");
            for (ctl::Setting* s : m->Settings()) {
                if (!s->Visible() || !(s->Persistent() || s->Bindable())) continue;
                char line[200];
                std::snprintf(line, sizeof line, "  %-28s %s", s->Name().c_str(), Value(*s).c_str());
                Print(Kind::Output, line);
            }
            return;
        }
        ctl::Setting* s = FindSetting(*m, args[1]);
        if (!s) { Print(Kind::Error, m->Name() + " has no setting called " + args[1]); return; }
        if (args.size() == 2) {
            if (s->Persistent()) Print(Kind::Output, s->Name() + " = " + Value(*s));
            else { s->Press(0); Print(Kind::Ok, s->Name() + ": pressed"); }
            return;
        }
        if (!s->Persistent()) { Print(Kind::Error, s->Name() + " is a button: setting <module> <button> presses it"); return; }
        nlohmann::ordered_json value;
        try {
            value = nlohmann::ordered_json::parse(args[2]);
        } catch (const std::exception&) {
            value = args[2];    // Not JSON: a plain string
        }
        s->Load(value);
        m->MarkDirty();
        Print(Kind::Ok, s->Name() + " = " + Value(*s));
    }

    void Pos()
    {
        game::RunOnGameThread([] {
            auto* pawn = game::Pawn();
            if (!pawn) { Post(Kind::Error, "no player"); return; }
            char line[128];
            std::snprintf(line, sizeof line, "%.1f %.1f %.1f", pawn->Location.X, pawn->Location.Y, pawn->Location.Z);
            Post(Kind::Output, line);
        });
    }

    void Teleport(const std::vector<std::string>& args)
    {
        if (args.size() < 3) { Print(Kind::Error, "usage: tp <x> <y> <z>   (pos prints where you are)"); return; }
        float v[3];
        for (int i = 0; i < 3; ++i) {
            char* end = nullptr;
            v[i] = std::strtof(args[static_cast<std::size_t>(i)].c_str(), &end);
            if (!end || *end) { Print(Kind::Error, "not a number: " + args[static_cast<std::size_t>(i)]); return; }
        }
        game::RunOnGameThread([x = v[0], y = v[1], z = v[2]] {
            auto* pawn = game::Pawn();
            if (!pawn) { Post(Kind::Error, "no player"); return; }
            pawn->Location = {x, y, z};     // SetLocation() doesn't move the player: the field does
            pawn->Velocity = {0.f, 0.f, 0.f};
            Post(Kind::Ok, "moved");
        });
    }

    void Run(const std::vector<std::string>& args, int depth)
    {
        if (args.empty()) { Print(Kind::Error, "usage: run <script>   (files in " + ScriptsDir().string() + ")"); return; }
        fs::path file = ScriptsDir() / args[0];
        if (!fs::exists(file)) file += ".cfg";
        std::ifstream in(file);
        if (!in) { Print(Kind::Error, "no script " + args[0] + " in " + ScriptsDir().string()); return; }
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (Trim(line).empty()) continue;
            RunLine(line, depth + 1);
            ++n;
        }
        Print(Kind::Info, "ran " + std::to_string(n) + " line(s) of " + file.filename().string());
    }

    void Log(const std::vector<std::string>& args)
    {
        if (!args.empty()) menuLog.Set(Lower(args[0]) == "on" || args[0] == "1" || Lower(args[0]) == "true");
        Print(Kind::Info, std::string("menu warnings and errors here: ") + (menuLog.Get() ? "on" : "off"));
    }

    // ---- Completion ------------------------------------------------------------------------------------------------------------
    std::vector<Candidate> Candidates(const shell::Target& t)
    {
        BuildLookups();
        std::vector<Candidate> out;
        const auto commands = [&](bool gameOnly) {
            for (const auto& n : names_) {
                const std::string lkey = Lower(n);
                if (gameOnly && kinds_[lkey] != Style::GameCommand) continue;
                out.push_back({n, descs_[lkey]});
            }
        };
        const auto list = [&](const std::vector<std::string>& items) { for (const auto& i : items) out.push_back({i, {}}); };
        const auto moduleNames = [&] {
            modules::ForEach([&](modules::Tab& tab, Module& m) {
                if (!m.Hidden()) out.push_back({m.Name(), tab.name + (m.IsAction() ? ", action" : m.Enabled() ? ", on" : ", off")});
            });
        };

        if (t.argIndex == 0) { commands(false); return out; }
        const std::string& cmd = t.command;
        const std::size_t i = t.argIndex;
        if (cmd == "game" && i == 1) commands(true);
        else if (cmd == "help" && i == 1) commands(false);
        else if ((cmd == "toggle" || cmd == "enable" || cmd == "disable") && i == 1) moduleNames();
        else if (cmd == "setting" && i == 1) moduleNames();
        else if (cmd == "setting" && i == 2) {
            if (const Module* m = FindModule(t.args[0], true))
                for (ctl::Setting* s : m->Settings())
                    if (s->Visible() && (s->Persistent() || s->Bindable())) out.push_back({s->Name(), Value(*s)});
        } else if (cmd == "setting" && i == 3) {
            if (const Module* m = FindModule(t.args[0], true))
                if (const ctl::Setting* s = FindSetting(*m, t.args[1]); s && dynamic_cast<const ctl::Checkbox*>(s))
                    list({"true", "false"});
        } else if ((cmd == "alias" || cmd == "unalias") && i == 1) {
            for (const auto& [k, v] : aliasMap_) out.push_back({k, v});
        } else if (cmd == "run" && i == 1) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(ScriptsDir(), ec))
                if (e.is_regular_file()) out.push_back({e.path().stem().string(), e.path().filename().string()});
        } else if (cmd == "log" && i == 1) list({"on", "off"});
        else if (cmd == "history" && i == 1) list({"-c"});
        else if (cmd == "stat" && i == 1) list(kStats);
        else if (cmd == "viewmode" && i == 1) list(kViewModes);
        else if (cmd == "obj" && i == 1) list(kObj);
        else if (cmd == "show" && i == 1) list(kShow);
        else if (cmd == "open" && i == 1) {
            for (const auto& m : levels::Maps()) out.push_back({m.file, m.label});
        }
        return out;
    }

    // Tab: one match taken, else the common start, else the menu (Tab again: through it)
    void Complete(ImGuiInputTextCallbackData* d, bool back)
    {
        if (menu_.active && !menu_.items.empty()) {
            const int n = static_cast<int>(menu_.items.size());
            menu_.index = menu_.index < 0 ? (back ? n - 1 : 0) : (menu_.index + (back ? n - 1 : 1)) % n;
            ApplyChoice(d, menu_.items[static_cast<std::size_t>(menu_.index)].text);
            return;
        }
        const std::string line(d->Buf, static_cast<std::size_t>(d->BufTextLen));
        const shell::Target target = shell::Locate(line, static_cast<std::size_t>(d->CursorPos));
        const auto all = Candidates(target);
        std::vector<std::string> names;
        for (const auto& c : all) names.push_back(c.text);
        const auto matches = shell::Filter(names, target.typed);
        if (matches.empty()) {
            bellAt_ = ImGui::GetTime();
            return;
        }
        if (matches.size() == 1) {
            SetText(d, shell::Apply(line, target, matches[0], true));
            return;
        }
        const std::string common = shell::CommonPrefix(matches);
        if (common.size() > target.typed.size()) {
            SetText(d, shell::Apply(line, target, common, false));
            return;
        }
        menu_ = Menu{};
        menu_.active = true;
        menu_.line = line;
        menu_.target = target;
        for (const auto& m : matches) {
            const auto it = std::find_if(all.begin(), all.end(), [&](const Candidate& c) { return shell::EqualsNoCase(c.text, m); });
            menu_.items.push_back({m, it != all.end() ? it->desc : std::string()});
        }
    }

    void ApplyChoice(ImGuiInputTextCallbackData* d, const std::string& choice)
    {
        SetText(d, shell::Apply(menu_.line, menu_.target, choice, true));
    }

    static void SetText(ImGuiInputTextCallbackData* d, const shell::Applied& a)
    {
        d->DeleteChars(0, d->BufTextLen);
        d->InsertChars(0, a.line.c_str());
        d->CursorPos = d->SelectionStart = d->SelectionEnd = static_cast<int>(std::min<std::size_t>(a.cursor, a.line.size()));
    }

    // ---- History (up-line-or-beginning-search) ----------------------------------------------------------------------------------
    void HistoryStep(ImGuiInputTextCallbackData* d, bool up)
    {
        if (menu_.active && menu_.index >= 0) {     // In the completion menu: move through it
            Complete(d, up);
            return;
        }
        menu_.active = false;
        if (historyPos_ < 0) {
            historyPrefix_.assign(d->Buf, static_cast<std::size_t>(d->BufTextLen));
            savedLine_ = historyPrefix_;
        }
        int pos = historyPos_ < 0 ? static_cast<int>(history_.size()) : historyPos_;
        const std::string current(d->Buf, static_cast<std::size_t>(d->BufTextLen));
        for (;;) {
            pos += up ? -1 : 1;
            if (pos < 0) return;    // Nothing older
            if (pos >= static_cast<int>(history_.size())) {
                historyPos_ = -1;
                SetText(d, {savedLine_, savedLine_.size()});
                return;
            }
            const std::string& h = history_[static_cast<std::size_t>(pos)];
            if (h.compare(0, historyPrefix_.size(), historyPrefix_) == 0 && h != current) break;
        }
        historyPos_ = pos;
        const std::string& h = history_[static_cast<std::size_t>(pos)];
        SetText(d, {h, h.size()});
    }

    // ---- Reverse search -----------------------------------------------------------------------------------------------------------
    // The newest history entry containing the query, older than `before` (-1: from the newest)
    int SearchFrom(const std::string& query, int before) const
    {
        for (int i = (before < 0 ? static_cast<int>(history_.size()) : before) - 1; i >= 0; --i)
            if (Lower(history_[static_cast<std::size_t>(i)]).find(Lower(query)) != std::string::npos) return i;
        return -1;
    }

    // ---- The input line's callback -------------------------------------------------------------------------------------------------
    static int InputCallback(ImGuiInputTextCallbackData* d) { return static_cast<Console*>(d->UserData)->OnInput(d); }

    int OnInput(ImGuiInputTextCallbackData* d)
    {
        switch (d->EventFlag) {
        case ImGuiInputTextFlags_CallbackCompletion:
            if (search_.active) { LeaveSearch(true); pending_ = true; break; }
            Complete(d, ImGui::GetIO().KeyShift);
            break;
        case ImGuiInputTextFlags_CallbackHistory:
            if (!search_.active) HistoryStep(d, d->EventKey == ImGuiKey_UpArrow);
            break;
        case ImGuiInputTextFlags_CallbackEdit:
            historyPos_ = -1;
            menu_.active = false;
            if (search_.active) search_.match = SearchFrom(std::string(d->Buf, static_cast<std::size_t>(d->BufTextLen)), -1);
            break;
        case ImGuiInputTextFlags_CallbackAlways: {
            cursor_ = d->CursorPos;
            if (pending_) {     // A line set from outside the callback (search, Esc in the menu, Ctrl keys)
                pending_ = false;
                SetText(d, {pendingText_, pendingCursor_});
                break;
            }
            const ImGuiIO& io = ImGui::GetIO();
            const auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
            const std::string text(d->Buf, static_cast<std::size_t>(d->BufTextLen));
            const int cur = d->CursorPos;
            if (io.KeyCtrl && !io.KeyShift) {
                if (pressed(ImGuiKey_A)) d->CursorPos = d->SelectionStart = d->SelectionEnd = 0;
                else if (pressed(ImGuiKey_E)) d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
                else if (pressed(ImGuiKey_U)) { d->DeleteChars(0, cur); d->CursorPos = 0; }
                else if (pressed(ImGuiKey_K)) d->DeleteChars(cur, d->BufTextLen - cur);
                else if (pressed(ImGuiKey_W) && cur > 0) {
                    int b = cur;
                    while (b > 0 && text[static_cast<std::size_t>(b - 1)] == ' ') --b;
                    while (b > 0 && text[static_cast<std::size_t>(b - 1)] != ' ') --b;
                    d->DeleteChars(b, cur - b);
                    d->CursorPos = b;
                }
            }
            // The suggestion: Right or End at the end of the line takes it
            if (!search_.active && !suggestion_.empty() && cur == d->BufTextLen && !io.KeyCtrl &&
                (pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_End))) {
                d->InsertChars(d->BufTextLen, suggestion_.c_str());
                d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
            }
            if (search_.active && pressed(ImGuiKey_RightArrow)) { LeaveSearch(true); pending_ = true; }
            break;
        }
        default:
            break;
        }
        return 0;
    }

    void LeaveSearch(bool takeMatch)
    {
        search_.active = false;
        pendingText_ = takeMatch && search_.match >= 0 ? history_[static_cast<std::size_t>(search_.match)] : search_.saved;
        pendingCursor_ = pendingText_.size();
    }

    // Replaces the line: right away while the input isn't active, else in its next callback
    void SetLine(const std::string& text)
    {
        if (inputActive_) {
            pendingText_ = text;
            pendingCursor_ = text.size();
            pending_ = true;
        } else {
            std::snprintf(buf_, sizeof buf_, "%s", text.c_str());
        }
    }

    // ---- Drawing --------------------------------------------------------------------------------------------------------------------
    // Text in runs of one style, from `pos`; returns the x after it
    float DrawStyled(ImDrawList* dl, ImVec2 pos, const std::string& text, const std::vector<Style>& styles) const
    {
        ImFont* f = ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        std::size_t i = 0;
        while (i < text.size()) {
            const Style s = i < styles.size() ? styles[i] : Style::Default;
            std::size_t j = i + 1;
            while (j < text.size() && (j < styles.size() ? styles[j] : Style::Default) == s) ++j;
            const char* b = text.data() + i;
            const char* e = text.data() + j;
            const float w = f->CalcTextSizeA(size, FLT_MAX, 0.f, b, e).x;
            dl->AddText(f, size, pos, StyleColor(s), b, e);
            if (s == Style::Known || s == Style::Error)
                dl->AddLine({pos.x, pos.y + size}, {pos.x + w, pos.y + size}, s == Style::Error ? kP.red : kP.dim);
            pos.x += w;
            i = j;
        }
        return pos.x;
    }

    float TextWidth(const char* b, const char* e) const
    {
        return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.f, b, e).x;
    }

    // "memenu edge_p » "
    float DrawPrompt(ImDrawList* dl, ImVec2 pos, const std::string& where) const
    {
        ImFont* f = ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        const auto put = [&](const char* t, ImU32 c) {
            dl->AddText(f, size, pos, c, t);
            pos.x += TextWidth(t, t + std::strlen(t));
        };
        put("memenu", kP.green);
        if (!where.empty()) {
            put(" ", kP.fg);
            put(where.c_str(), kP.blue);
        }
        put(" \xC2\xBB ", kP.dim);   // »
        return pos.x;
    }

    void Draw()
    {
        BuildLookups();
        theme::Begin();
        ImGui::PushFont(font::Mono(), static_cast<float>(fontSize.Get()));
        ImVec4 bg = Vec(kP.bg);
        bg.w = opacity;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_Text, Vec(kP.fg));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, Vec(kP.selection));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{1, 1, 1, 0.08f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.f, 8.f});
        ImGui::SetNextWindowPos({260.f, 140.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({780.f, 440.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints({360.f, 200.f}, {FLT_MAX, FLT_MAX});
        bool open = true;
        if (ImGui::Begin("Console", &open, ImGuiWindowFlags_NoCollapse)) {
            const float status = ImGui::GetTextLineHeight() + 6.f;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.f, 1.f});
            ImGui::BeginChild("##screen", {0.f, -status}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
            DrawScreen();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            DrawStatus();
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(9);
        ImGui::PopFont();
        theme::End();
        if (!open) SetEnabled(false);
    }

    void DrawScreen()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f;

        // Scrollback
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(lines_.size()), lineH);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const Line& l = lines_[static_cast<std::size_t>(row)];
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (l.kind == Kind::Echo) {
                    const float x = DrawPrompt(dl, at, l.where);
                    const float end = DrawStyled(dl, {x, at.y}, l.text, l.styles);
                    ImGui::Dummy({end - at.x, ImGui::GetTextLineHeight()});
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, Vec(KindColor(l.kind)));
                    ImGui::TextUnformatted(l.text.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
        clipper.End();

        DrawInputLine(dl);
        DrawMenu();

        if (ImGui::BeginPopupContextWindow("##screenmenu")) {
            if (ImGui::MenuItem("Copy everything")) CopyAll();
            if (ImGui::MenuItem("Clear", "Ctrl+L")) lines_.clear();
            ImGui::EndPopup();
        }
        // Clicking anywhere on the screen goes back to typing
        if (ImGui::IsWindowHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive())
            refocus_ = true;
        if ((scrollToEnd_ && atEnd) || typedThisFrame_) ImGui::SetScrollHereY(1.f);
        scrollToEnd_ = typedThisFrame_ = false;
    }

    void DrawInputLine(ImDrawList* dl)
    {
        std::string where;
        {
            std::lock_guard lock(execMutex_);
            where = map_;
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        float x;
        if (search_.active) {
            const char* label = "(reverse-i-search) ";
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, kP.purple, label);
            x = at.x + TextWidth(label, label + std::strlen(label));
        } else {
            x = DrawPrompt(dl, at, where);
        }
        ImGui::SetCursorScreenPos({x, at.y});

        // Keys the input doesn't handle itself, while it has the keyboard
        const ImGuiIO& io = ImGui::GetIO();
        if (inputActive_) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L, false)) lines_.clear();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                if (!search_.active) {
                    search_ = Search{};
                    search_.active = true;
                    search_.saved = buf_;
                    search_.match = SearchFrom("", -1);     // The newest entry until something is typed
                    SetLine("");
                } else if (search_.match >= 0) {
                    const int older = SearchFrom(buf_, search_.match);
                    if (older >= 0) search_.match = older;
                    else bellAt_ = ImGui::GetTime();
                }
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                const ImGuiInputTextState* st = ImGui::GetInputTextState(inputId_);
                if (!st || !st->HasSelection()) {
                    Line l;
                    l.kind = Kind::Echo;
                    l.text = std::string(buf_) + "^C";
                    l.styles = HighlightLine(buf_);
                    l.where = where;
                    lines_.push_back(std::move(l));
                    menu_.active = search_.active = false;
                    SetLine("");
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                if (menu_.active) {
                    menu_.active = false;
                    SetLine(menu_.line);
                } else if (search_.active) {
                    LeaveSearch(false);
                    pending_ = true;
                }
            }
        }

        if (refocus_ || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !inputActive_ &&
                         !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
            ImGui::SetKeyboardFocusHere();
            refocus_ = false;
        }
        constexpr ImGuiInputTextFlags kFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion |
                                               ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways |
                                               ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_EscapeClearsAll;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.f, 0.f});
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0, 0, 0, 0});              // Drawn below, highlighted
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImVec4{0, 0, 0, 0});   // A block cursor instead
        ImGui::SetNextItemWidth(-FLT_MIN);
        const std::string before = buf_;
        const bool entered = ImGui::InputText("##line", buf_, sizeof buf_, kFlags, &Console::InputCallback, this);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        inputId_ = ImGui::GetItemID();
        inputActive_ = ImGui::IsItemActive();
        if (before != buf_) typedThisFrame_ = true;

        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const ImGuiInputTextState* st = ImGui::GetInputTextState(inputId_);
        const float scroll = st ? st->Scroll.x : 0.f;
        const std::string line = buf_;
        dl->PushClipRect(min, {max.x + 1.f, max.y}, true);
        float end;
        if (search_.active) {
            end = DrawStyled(dl, {min.x - scroll, min.y}, line, std::vector<Style>(line.size(), Style::Default));
            const std::string match = search_.match >= 0 ? history_[static_cast<std::size_t>(search_.match)] : "";
            ImFont* f = ImGui::GetFont();
            const char* arrow = search_.match >= 0 ? ": " : "  (no match)";
            dl->AddText(f, ImGui::GetFontSize(), {end, min.y}, search_.match >= 0 ? kP.dim : kP.red, arrow);
            const float ax = end + TextWidth(arrow, arrow + std::strlen(arrow));
            if (!match.empty()) DrawStyled(dl, {ax, min.y}, match, HighlightLine(match));
        } else {
            end = DrawStyled(dl, {min.x - scroll, min.y}, line, HighlightLine(line));
            suggestion_ = cursor_ == static_cast<int>(line.size()) && !menu_.active ? shell::Suggest(history_, line) : std::string();
            if (!suggestion_.empty())
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {end, min.y}, kP.dim, suggestion_.c_str());
        }

        // Block cursor: solid and blinking while typing, an outline while the console doesn't have the keyboard
        const int cur = std::clamp(inputActive_ ? cursor_ : static_cast<int>(line.size()), 0, static_cast<int>(line.size()));
        const float cx = min.x - scroll + TextWidth(line.data(), line.data() + cur);
        const char* glyph = cur < static_cast<int>(line.size()) ? line.data() + cur : " ";
        const float cw = std::max(2.f, TextWidth(glyph, glyph + 1));
        const float h = ImGui::GetFontSize();
        if (before != buf_) blinkFrom_ = ImGui::GetTime();
        const bool on = std::fmod(ImGui::GetTime() - blinkFrom_, 1.0) < 0.6;
        if (!inputActive_) dl->AddRect({cx, min.y}, {cx + cw, min.y + h}, kP.dim);
        else if (on) {
            dl->AddRectFilled({cx, min.y}, {cx + cw, min.y + h}, kP.fg);
            if (cur < static_cast<int>(line.size())) dl->AddText(ImGui::GetFont(), h, {cx, min.y}, kP.bg, glyph, glyph + 1);
        }
        dl->PopClipRect();

        if (entered) {
            const std::string submitted = buf_;
            buf_[0] = '\0';
            refocus_ = true;
            historyPos_ = -1;
            if (search_.active) {
                search_.active = false;
                if (search_.match >= 0) Submit(history_[static_cast<std::size_t>(search_.match)]);
            } else if (menu_.active && menu_.index >= 0) {
                menu_.active = false;              // Enter takes the menu's choice; the next one runs the line
                std::snprintf(buf_, sizeof buf_, "%s", submitted.c_str());
                pendingText_ = submitted;
                pendingCursor_ = submitted.size();
                pending_ = true;
            } else {
                menu_.active = false;
                Submit(submitted);
            }
        }
    }

    // The completion menu, below the prompt: one column with descriptions (like zsh's), the selection highlighted
    void DrawMenu()
    {
        if (!menu_.active || menu_.items.size() < 2) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        constexpr int kRows = 10;
        const int n = static_cast<int>(menu_.items.size());
        const int sel = menu_.index;
        const int first = std::clamp(sel - kRows / 2, 0, std::max(0, n - kRows));
        float nameW = 0.f;
        for (const auto& it : menu_.items) nameW = std::max(nameW, TextWidth(it.text.data(), it.text.data() + it.text.size()));
        nameW = std::min(nameW, 260.f);
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        for (int i = first; i < std::min(n, first + kRows); ++i) {
            const auto& it = menu_.items[static_cast<std::size_t>(i)];
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            if (i == sel) dl->AddRectFilled({at.x, at.y}, {at.x + width, at.y + lineH}, kP.selection);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {at.x + 12.f, at.y}, i == sel ? kP.fg : kP.green, it.text.c_str());
            if (!it.desc.empty())
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {at.x + 24.f + nameW, at.y}, kP.dim, ("-- " + it.desc).c_str());
            ImGui::Dummy({width, ImGui::GetTextLineHeight()});
        }
        char more[64];
        std::snprintf(more, sizeof more, "%d of %d", sel < 0 ? 0 : sel + 1, n);
        ImGui::PushStyleColor(ImGuiCol_Text, Vec(kP.dim));
        ImGui::Text("  %s%s", more, sel < 0 ? "   Tab: choose, Esc: cancel" : "");
        ImGui::PopStyleColor();
    }

    // Bottom bar: what the command being typed takes, and the keys
    void DrawStatus()
    {
        ImGui::Separator();
        std::string hint;
        ImU32 color = kP.dim;
        if (ImGui::GetTime() - bellAt_ < 0.6) {
            hint = search_.active ? "no older match" : "no completions";
            color = kP.red;
        } else if (search_.active) {
            hint = "reverse search: Enter runs, Ctrl+R older, Right/Tab edits, Esc cancels";
        } else if (menu_.active) {
            hint = std::to_string(menu_.items.size()) + " matches: Tab / Shift+Tab or Up/Down choose, Enter takes, Esc cancels";
        } else {
            const auto t = shell::Locate(buf_, static_cast<std::size_t>(std::clamp(cursor_, 0, static_cast<int>(std::strlen(buf_)))));
            const auto cmds = shell::Parse(std::string(buf_).substr(0, t.replaceBegin + t.typed.size()));
            std::string word = !cmds.empty() && !cmds.back().words.empty() ? Lower(cmds.back().words[0].text) : "";
            if (word == "game" && cmds.back().words.size() > 1) word = Lower(cmds.back().words[1].text);
            if (const auto it = descs_.find(word); it != descs_.end()) {
                hint = it->second;
                color = kinds_[word] == Style::Builtin ? kP.green : kinds_[word] == Style::Alias ? kP.purple : kP.blue;
            } else if (!word.empty() && kinds_.find(word) == kinds_.end() && CommandStyle(word) == Style::Unknown) {
                hint = "unknown: it would go to the game as typed";
                color = kP.red;
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Vec(color));
        ImGui::TextUnformatted(hint.c_str());
        ImGui::PopStyleColor();
        const char* keys = "Tab complete  Up/Down history  Ctrl+R search  Ctrl+L clear";
        const float w = ImGui::CalcTextSize(keys).x;
        ImGui::SameLine();
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > w + 30.f) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
            ImGui::TextDisabled("%s", keys);
        }
    }

    void CopyAll() const
    {
        std::string out;
        for (const Line& l : lines_) out += (l.kind == Kind::Echo ? "> " : "") + l.text + '\n';
        ImGui::SetClipboardText(out.c_str());
    }

    // ---- State ------------------------------------------------------------------------------------------------------------------------
    struct Menu {
        bool active = false;
        std::vector<Candidate> items;
        int index = -1;             // -1: listed, nothing chosen yet
        std::string line;           // The line before the menu (Esc goes back to it)
        shell::Target target;
    };
    struct Search {
        bool active = false;
        int match = -1;             // Index into history_
        std::string saved;          // The line before searching
    };

    std::deque<Line> lines_;
    std::vector<std::string> history_;          // Oldest first
    std::map<std::string, std::string> aliasMap_;
    char buf_[2048] = {};
    bool started_ = false, refocus_ = true, inputActive_ = false, scrollToEnd_ = false, typedThisFrame_ = false;
    ImGuiID inputId_ = 0;
    int cursor_ = 0;
    int historyPos_ = -1;
    std::string historyPrefix_, savedLine_, suggestion_;
    bool pending_ = false;
    std::string pendingText_;
    std::size_t pendingCursor_ = 0;
    Menu menu_;
    Search search_;
    double bellAt_ = -10.0, blinkFrom_ = 0.0;
    std::uint64_t logId_ = 0;

    // What each command name is (built from the lists below, the game's exec functions and the aliases)
    std::unordered_map<std::string, Style> kinds_;
    std::unordered_map<std::string, std::string> descs_;
    std::vector<std::string> names_;
    int lookupsFor_ = -1;

    // Filled on the game thread
    std::mutex execMutex_;
    std::vector<game::ExecCommand> exec_;
    std::atomic<bool> execLoaded_{false};
    std::string map_;
    unsigned ticks_ = 0;

    ctl::SliderInt fontSize{this, "Font size", 15, 11, 24};
    ctl::Slider opacity{this, "Opacity", 0.96f, 0.5f, 1.f, "%.2f"};
    ctl::Checkbox engineMessages{this, "Game messages", true, "Show what the game prints (its console and chat messages)"};
    ctl::Checkbox menuLog{this, "Menu warnings", true, "Show the menu's own warnings and errors"};
    ctl::Button clear{this, "Clear", [this] { lines_.clear(); }};
    ctl::Text aliases{this, "Aliases", "{}"};
};
MEM_MODULE(Console)

} // namespace
