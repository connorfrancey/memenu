#include "config.h"
#include "modules.h"
#include "paths.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::ordered_json;    // Keeps keys in insertion order, so the file stays readable

static std::filesystem::path ConfigPath() { return paths::Dir() / "MedgeMenu.json"; }

static bool Persistent(const Setting& s)
{
    if (!s.persist) return false;
    return s.type != Setting::Type::Button && s.type != Setting::Type::Label && s.type != Setting::Type::Separator;
}

// Keyed by "Tab/Module" so equal module names in different tabs don't collide.
static std::string ModuleKey(const Tab& t, const Module& m) { return t.name + "/" + m.name; }

// 0.6f -> 0.6 instead of 0.6000000238418579
static double Round(float v) { return std::round(static_cast<double>(v) * 10000.0) / 10000.0; }

static json ToJson(const Setting& s)
{
    switch (s.type)
    {
    case Setting::Type::Bool:  return s.b;
    case Setting::Type::Float: return Round(s.f);
    case Setting::Type::Color:
        {
            json a = json::array();
            for (float c : s.color) a.push_back(Round(c));
            return a;
        }
    default:                   return s.i;   // Int, Combo, Key
    }
}

// Wrong-typed or out-of-range values are ignored/clamped instead of failing the whole load.
static void FromJson(Setting& s, const json& v)
{
    switch (s.type) {
        case Setting::Type::Bool:
            if (v.is_boolean()) s.b = v.get<bool>();
            break;
        case Setting::Type::Float:
            if (v.is_number()) s.f = std::clamp(v.get<float>(), s.fmin, s.fmax);
            break;
        case Setting::Type::Int:
            if (v.is_number()) s.i = std::clamp(v.get<int>(), s.imin, s.imax);
            break;
        case Setting::Type::Combo:
            if (v.is_number()) s.i = std::clamp(v.get<int>(), 0, std::max(0, static_cast<int>(s.options.size()) - 1));
            break;
        case Setting::Type::Key:
            if (v.is_number()) s.i = v.get<int>();
            break;
        case Setting::Type::Color:
            if (v.is_array() && v.size() == 4) {
                for (size_t k = 0; k < 4; ++k)
                    if (v[k].is_number()) s.color[k] = std::clamp(v[k].get<float>(), 0.f, 1.f);
            }
            break;
        default:
            break;
    }
}

bool config::Load()
{
    std::ifstream in(ConfigPath());
    if (!in) {
        spdlog::info("No config at {}, using defaults", ConfigPath().string());
        return false;
    }
    try {
        const json root = json::parse(in, nullptr, /* allow_exceptions*/ true, /* ignore_comments */ true);

        if (const auto t = root.find("theme"); t != root.end() && t->is_string())
            theme::Apply(t->get<std::string>());

        const auto mods = root.find("modules");
        if (mods == root.end() || !mods->is_object()) return true;

        modules::ForEachModule([&](Tab& t, Module& m)
        {
            const auto mi = mods->find(ModuleKey(t, m));
            if (mi == mods->end() || !mi->is_object()) return;

            if (const auto k = mi->find("key"); k != mi->end() && k->is_number()) m.key = k->get<int>();

            if (const auto sj = mi->find("settings"); sj != mi->end() && sj->is_object())
            {
                for (auto& s : m.settings)
                    if (Persistent(s))
                        if (const auto v = sj->find(s.name); v != sj->end()) FromJson(s, *v);
            }
            // After the settings, so onEnable sees them
            if (const auto e = mi->find("enabled"); e != mi->end() && e->is_boolean()) m.SetEnabled(e->get<bool>());
        });

        spdlog::info("Config loaded");
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Config error: {}", e.what());
        return false;
    }
}

bool config::Save()
{
    json mods = json::object();
    modules::ForEachModule([&](Tab& t, Module& m) {
        json settings = json::object();
        for (const auto& s : m.settings)
            if (Persistent(s)) settings[s.name] = ToJson(s);

        json mt = json::object();
        if (!m.isAction) mt["enabled"] = m.enabled;
        mt["key"] = m.key;                           // Windows virtual-key code, 0 = unbound
        if (!settings.empty()) mt["settings"] = std::move(settings);
        mods[ModuleKey(t, m)] = std::move(mt);
    });

    json root = json::object();
    root["theme"]   = theme::Current();
    root["modules"] = std::move(mods);

    std::ofstream out(ConfigPath());
    if (!out) {
        spdlog::error("Cannot write config to {}", ConfigPath().string());
        return false;
    }
    out << root.dump(2) << '\n';
    spdlog::info("Config saved");
    return out.good();
}
