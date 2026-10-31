#include "theme.h"
#include "theme_backend.h"
#include "paths.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kDefaultTheme = "MirrorsEdge";
const ImVec4 kRed{0.90f, 0.20f, 0.18f, 1.00f};

struct Theme {
    ImGuiStyle style;            // only the colors/rounding listed below are used
    theme::MenuColors menu;
};

// The structural style (padding/spacing) stays fixed: the clickgui layout relies on it, so themes
// can only change colors and rounding.
struct FloatVar { const char* key; float ImGuiStyle::* member; };
constexpr FloatVar kRounding[] = {
    {"WindowRounding",    &ImGuiStyle::WindowRounding},
    {"ChildRounding",     &ImGuiStyle::ChildRounding},
    {"PopupRounding",     &ImGuiStyle::PopupRounding},
    {"FrameRounding",     &ImGuiStyle::FrameRounding},
    {"ScrollbarRounding", &ImGuiStyle::ScrollbarRounding},
    {"GrabRounding",      &ImGuiStyle::GrabRounding},
    {"TabRounding",       &ImGuiStyle::TabRounding},
};

struct MenuColorVar { const char* key; ImVec4 theme::MenuColors::* member; };
constexpr MenuColorVar kMenuColors[] = {
    {"accent",      &theme::MenuColors::accent},
    {"row_bg",      &theme::MenuColors::rowBg},
    {"row_hover",   &theme::MenuColors::rowHover},
    {"text",        &theme::MenuColors::text},
    {"text_active", &theme::MenuColors::textActive},
    {"text_dim",    &theme::MenuColors::textDim},
    {"settings_bg", &theme::MenuColors::settingsBg},
};

// Colors written into generated theme files: the classic, long-stable ImGuiCol names the menu
// actually uses. (Any ImGuiCol name is accepted when *reading* a file.)
constexpr const char* kExportedColors[] = {
    "Text", "TextDisabled", "WindowBg", "PopupBg", "Border",
    "TitleBg", "TitleBgActive", "TitleBgCollapsed",
    "FrameBg", "FrameBgHovered", "FrameBgActive",
    "CheckMark", "SliderGrab", "SliderGrabActive",
    "Button", "ButtonHovered", "ButtonActive",
    "Header", "HeaderHovered", "HeaderActive", "Separator",
};

std::vector<std::string> g_names;
std::string              g_current = kDefaultTheme;
std::string              g_text;          // sanitized {"Colors":..,"Rounding":..} handed to imjson every frame
bool                     g_pushed = false;
theme::MenuColors        g_menu;

fs::path Dir() { return paths::Dir() / "themes"; }
fs::path ThemePath(const std::string& name) { return Dir() / (name + ".json"); }

// ---- colors ---------------------------------------------------------------------------------
ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
{
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
}
ImVec4 Scale(ImVec4 c, float f) { return { c.x * f, c.y * f, c.z * f, c.w }; }

int ColorIndex(const std::string& name)
{
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        if (name == ImGui::GetStyleColorName(i)) return i;
    return -1;
}

// "#RRGGBB" or "#RRGGBBAA" (the '#' is optional)
bool ParseHex(std::string s, ImVec4& out)
{
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8) return false;
    for (char ch : s) if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    std::uint32_t v = static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
    if (s.size() == 6) v = (v << 8) | 0xFFu;
    out = { static_cast<float>((v >> 24) & 0xFF) / 255.f, static_cast<float>((v >> 16) & 0xFF) / 255.f,
            static_cast<float>((v >> 8)  & 0xFF) / 255.f, static_cast<float>( v        & 0xFF) / 255.f };
    return true;
}

// [r,g,b] / [r,g,b,a] (0..1) or a hex string
bool ToColor(const json& v, ImVec4& out)
{
    if (v.is_string()) return ParseHex(v.get<std::string>(), out);
    if (!v.is_array() || (v.size() != 3 && v.size() != 4)) return false;
    float c[4] = {0.f, 0.f, 0.f, 1.f};
    for (size_t i = 0; i < v.size(); ++i) {
        if (!v[i].is_number()) return false;
        c[i] = std::clamp(v[i].get<float>(), 0.f, 1.f);
    }
    out = {c[0], c[1], c[2], c[3]};
    return true;
}

// ---- built-in themes ------------------------------------------------------------------------
void CommonStyle(ImGuiStyle& s)
{
    s.WindowRounding   = 4.f;
    s.PopupRounding    = 4.f;
    s.FrameRounding    = 3.f;
    s.GrabRounding     = 3.f;
    s.WindowBorderSize = 0.f;
    s.FramePadding     = {6.f, 4.f};
    s.WindowTitleAlign = {0.5f, 0.5f};
}

Theme MakeDark(ImVec4 accent)
{
    Theme t;
    ImGui::StyleColorsDark(&t.style);
    CommonStyle(t.style);

    ImVec4* c = t.style.Colors;
    c[ImGuiCol_WindowBg]         = {0.07f, 0.07f, 0.08f, 0.96f};
    c[ImGuiCol_PopupBg]          = {0.09f, 0.09f, 0.10f, 0.98f};
    c[ImGuiCol_TitleBg]          = Scale(accent, 0.60f);
    c[ImGuiCol_TitleBgCollapsed] = Scale(accent, 0.60f);
    c[ImGuiCol_TitleBgActive]    = accent;
    c[ImGuiCol_FrameBg]          = {0.16f, 0.16f, 0.18f, 1.f};
    c[ImGuiCol_FrameBgHovered]   = {0.22f, 0.22f, 0.25f, 1.f};
    c[ImGuiCol_FrameBgActive]    = {0.26f, 0.26f, 0.30f, 1.f};
    c[ImGuiCol_CheckMark]        = accent;
    c[ImGuiCol_SliderGrab]       = accent;
    c[ImGuiCol_SliderGrabActive] = Mix(accent, {1.f, 1.f, 1.f, 1.f}, 0.25f);
    c[ImGuiCol_Button]           = {0.16f, 0.16f, 0.18f, 1.f};
    c[ImGuiCol_ButtonHovered]    = Scale(accent, 0.60f);
    c[ImGuiCol_ButtonActive]     = accent;
    c[ImGuiCol_Header]           = Scale(accent, 0.60f);
    c[ImGuiCol_HeaderHovered]    = Scale(accent, 0.78f);
    c[ImGuiCol_HeaderActive]     = accent;

    t.menu = {};
    t.menu.accent = accent;
    return t;
}

Theme MakeLight(ImVec4 accent)
{
    Theme t;
    ImGui::StyleColorsLight(&t.style);
    CommonStyle(t.style);

    const ImVec4 white{1.f, 1.f, 1.f, 1.f};
    ImVec4* c = t.style.Colors;
    c[ImGuiCol_Text]             = {0.10f, 0.10f, 0.12f, 1.f};
    c[ImGuiCol_WindowBg]         = {0.95f, 0.95f, 0.96f, 0.97f};
    c[ImGuiCol_PopupBg]          = {0.98f, 0.98f, 0.99f, 0.98f};
    c[ImGuiCol_TitleBg]          = Mix(accent, white, 0.20f);
    c[ImGuiCol_TitleBgCollapsed] = Mix(accent, white, 0.20f);
    c[ImGuiCol_TitleBgActive]    = accent;
    c[ImGuiCol_FrameBg]          = {0.86f, 0.86f, 0.89f, 1.f};
    c[ImGuiCol_FrameBgHovered]   = {0.80f, 0.80f, 0.85f, 1.f};
    c[ImGuiCol_FrameBgActive]    = {0.74f, 0.74f, 0.80f, 1.f};
    c[ImGuiCol_CheckMark]        = accent;
    c[ImGuiCol_SliderGrab]       = accent;
    c[ImGuiCol_SliderGrabActive] = Scale(accent, 0.85f);
    c[ImGuiCol_Button]           = {0.84f, 0.84f, 0.87f, 1.f};
    c[ImGuiCol_ButtonHovered]    = Mix(accent, white, 0.35f);
    c[ImGuiCol_ButtonActive]     = accent;
    c[ImGuiCol_Header]           = Mix(accent, white, 0.35f);
    c[ImGuiCol_HeaderHovered]    = Mix(accent, white, 0.20f);
    c[ImGuiCol_HeaderActive]     = accent;

    t.menu.accent     = accent;
    t.menu.rowBg      = {0.90f, 0.90f, 0.92f, 1.f};
    t.menu.rowHover   = {0.84f, 0.84f, 0.88f, 1.f};
    t.menu.text       = {0.25f, 0.25f, 0.30f, 1.f};
    t.menu.textActive = {0.05f, 0.05f, 0.07f, 1.f};
    t.menu.textDim    = {0.45f, 0.45f, 0.50f, 1.f};
    t.menu.settingsBg = {0.93f, 0.93f, 0.95f, 1.f};
    return t;
}

std::vector<std::pair<std::string, Theme>> Builtins()
{
    return {
        {"MirrorsEdge", MakeDark({0.90f, 0.20f, 0.18f, 1.f})},
        {"Midnight",    MakeDark({0.30f, 0.52f, 1.00f, 1.f})},
        {"Emerald",     MakeDark({0.15f, 0.78f, 0.48f, 1.f})},
        {"Light",       MakeLight({0.85f, 0.25f, 0.22f, 1.f})},
    };
}

// ---- Theme -> JSON text (hand-formatted so the files stay readable) ---------------------------
std::string Num(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (s.size() > 3 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
    return s;
}
std::string Arr(ImVec4 c) { return "[" + Num(c.x) + ", " + Num(c.y) + ", " + Num(c.z) + ", " + Num(c.w) + "]"; }

std::string ToJsonText(const Theme& t)
{
    std::ostringstream o;
    o << "// Mirror's Edge menu theme (JSON; comments are allowed).\n"
         "// Colors:   ImGuiCol names without the prefix, as [r, g, b, a] in 0..1 or \"#RRGGBB[AA]\".\n"
         "// Rounding: WindowRounding, FrameRounding, ... (applied through moleium/imjson).\n"
         "// Menu:     the custom-drawn parts: module rows, accent bar, settings panels.\n"
         "// Anything missing falls back to the default dark theme. Spacing/padding can't be themed.\n"
         "{\n  \"Colors\": {\n";
    for (size_t i = 0; i < std::size(kExportedColors); ++i) {
        const int idx = ColorIndex(kExportedColors[i]);
        o << "    \"" << kExportedColors[i] << "\": " << Arr(t.style.Colors[idx])
          << (i + 1 < std::size(kExportedColors) ? ",\n" : "\n");
    }
    o << "  },\n  \"Rounding\": {\n";
    for (size_t i = 0; i < std::size(kRounding); ++i)
        o << "    \"" << kRounding[i].key << "\": " << Num(t.style.*kRounding[i].member)
          << (i + 1 < std::size(kRounding) ? ",\n" : "\n");
    o << "  },\n  \"Menu\": {\n";
    for (const auto& cv : kMenuColors)
        o << "    \"" << cv.key << "\": " << Arr(t.menu.*cv.member) << ",\n";
    o << "    \"row_height\": " << Num(t.menu.rowHeight) << "\n  }\n}\n";
    return o.str();
}

// ---- JSON text -> what imjson + the menu need ------------------------------------------------
struct Loaded {
    std::string imjson;          // {"Colors": {...}, "Rounding": {...}} - exactly what imjson gets
    theme::MenuColors menu;
};

// Throws std::runtime_error on invalid JSON. imjson only ever sees a rebuilt, validated document
// (known color names, numeric values, no layout keys, no fonts), never the raw file.
Loaded ParseTheme(const std::string& text, const std::string& label)
{
    const json doc = json::parse(text, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
    if (doc.is_discarded() || !doc.is_object())
        throw std::runtime_error("not a valid JSON object");

    Loaded out;
    json colors = json::object(), rounding = json::object();

    for (auto it = doc.begin(); it != doc.end(); ++it)
        if (it.key() != "Colors" && it.key() != "Rounding" && it.key() != "Menu")
            spdlog::warn("Theme '{}': key '{}' ignored (supported: Colors, Rounding, Menu)", label, it.key());

    if (const auto c = doc.find("Colors"); c != doc.end() && c->is_object()) {
        for (auto it = c->begin(); it != c->end(); ++it) {
            ImVec4 col;
            if (ColorIndex(it.key()) < 0) { spdlog::warn("Theme '{}': unknown color '{}'", label, it.key()); continue; }
            if (!ToColor(it.value(), col)) { spdlog::warn("Theme '{}': bad value for color '{}'", label, it.key()); continue; }
            colors[it.key()] = json::array({col.x, col.y, col.z, col.w});
        }
    }

    if (const auto r = doc.find("Rounding"); r != doc.end() && r->is_object()) {
        for (auto it = r->begin(); it != r->end(); ++it) {
            const bool known = std::any_of(std::begin(kRounding), std::end(kRounding),
                                           [&](const FloatVar& v) { return it.key() == v.key; });
            if (!known || !it.value().is_number()) { spdlog::warn("Theme '{}': rounding '{}' ignored", label, it.key()); continue; }
            rounding[it.key()] = std::max(0.f, it.value().get<float>());
        }
    }

    if (const auto m = doc.find("Menu"); m != doc.end() && m->is_object()) {
        for (auto it = m->begin(); it != m->end(); ++it) {
            if (it.key() == "row_height") {
                if (it.value().is_number()) out.menu.rowHeight = std::clamp(it.value().get<float>(), 18.f, 48.f);
                continue;
            }
            const auto cv = std::find_if(std::begin(kMenuColors), std::end(kMenuColors),
                                         [&](const MenuColorVar& v) { return it.key() == v.key; });
            ImVec4 col;
            if (cv == std::end(kMenuColors) || !ToColor(it.value(), col)) {
                spdlog::warn("Theme '{}': menu.{} ignored", label, it.key());
                continue;
            }
            out.menu.*(cv->member) = col;
        }
    }

    json clean = json::object();
    if (!colors.empty())   clean["Colors"]   = std::move(colors);
    if (!rounding.empty()) clean["Rounding"] = std::move(rounding);
    if (!clean.empty()) out.imjson = clean.dump();
    return out;
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

// -------------------------------------------------------------------------------------------------

void theme::Init()
{
    std::error_code ec;
    fs::create_directories(Dir(), ec);
    for (const auto& [name, t] : Builtins()) {
        const auto path = ThemePath(name);
        if (fs::exists(path)) continue;                    // never overwrite user edits
        std::ofstream out(path);
        if (out && (out << ToJsonText(t))) spdlog::info("Wrote built-in theme {}", path.string());
        else                               spdlog::warn("Could not write theme {}", path.string());
    }
    Rescan();
}

void theme::Rescan()
{
    g_names.clear();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(Dir(), ec))
        if (e.is_regular_file() && e.path().extension() == ".json")
            g_names.push_back(e.path().stem().string());
    std::sort(g_names.begin(), g_names.end());
}

bool theme::Apply(const std::string& name)
{
    Loaded t;
    bool ok = false;
    const auto path = ThemePath(name);
    if (fs::exists(path)) {
        try {
            t = ParseTheme(ReadFile(path), name);
            ok = true;
        } catch (const std::exception& e) {
            spdlog::error("Theme '{}' failed to load, using the default: {}", name, e.what());
        }
    } else {
        spdlog::warn("Theme '{}' not found, using the default", name);
    }

    if (!ok) t = ParseTheme(ToJsonText(MakeDark(kRed)), kDefaultTheme);   // built-in fallback, no file needed

    g_current = ok ? name : kDefaultTheme;
    g_text    = std::move(t.imjson);
    g_menu    = t.menu;
    if (ok) spdlog::info("Theme '{}' applied", name);
    return ok;
}

void theme::ApplyBaseStyle()
{
    Theme base = MakeDark(kRed);
    base.style.WindowPadding = {0.f, 0.f};     // the clickgui layout needs flush rows
    base.style.ItemSpacing   = {0.f, 0.f};
    ImGui::GetStyle() = base.style;
}

void theme::Begin()
{
    g_pushed = false;
    if (g_text.empty()) return;

    g_pushed = backend::Push(g_text);
    if (!g_pushed) {
        spdlog::error("imjson failed to apply theme '{}'; using the built-in style instead", g_current);
        g_text.clear();                        // don't retry (and fail) every frame
    }
}

void theme::End()
{
    if (!g_pushed) return;
    backend::Pop();
    g_pushed = false;
}

void theme::OpenFolder()
{
    ShellExecuteW(nullptr, L"open", Dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

const std::vector<std::string>& theme::Names() { return g_names; }
const std::string& theme::Current() { return g_current; }
const theme::MenuColors& theme::Menu() { return g_menu; }

int theme::CurrentIndex()
{
    const auto it = std::find(g_names.begin(), g_names.end(), g_current);
    return it == g_names.end() ? -1 : static_cast<int>(it - g_names.begin());
}
