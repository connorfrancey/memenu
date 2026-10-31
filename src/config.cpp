#include "config.h"
#include "core/module.h"
#include "core/setting.h"
#include "paths.h"
#include "theme.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::ordered_json;    // Keeps keys in insertion order, so the file stays readable

static std::filesystem::path ConfigPath() { return paths::Dir() / "MedgeMenu.json"; }

// Keyed by "Tab/Module" so equal module names in different tabs don't collide.
static std::string ModuleKey(const modules::Tab& t, const Module& m) { return t.name + "/" + m.Name(); }

static int ReadKey(const json& obj, const char* name, int fallback)
{
    const auto it = obj.find(name);
    return it != obj.end() && it->is_number_integer() ? it->get<int>() : fallback;
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

        modules::ForEach([&](modules::Tab& t, Module& m)
        {
            const auto mi = mods->find(ModuleKey(t, m));
            if (mi == mods->end() || !mi->is_object()) return;

            m.key = ReadKey(*mi, "key", m.key);

            if (const auto sj = mi->find("settings"); sj != mi->end() && sj->is_object())
            {
                for (ctl::Setting* s : m.Settings())
                {
                    const auto v = sj->find(s->Name());
                    if (v == sj->end()) continue;
                    if (s->Persistent()) s->Load(*v);   // Wrong types are ignored, values clamped by the control
                }
            }
            if (const auto kj = mi->find("keys"); kj != mi->end() && kj->is_object())
            {
                for (ctl::Setting* s : m.Settings())
                {
                    const auto k = kj->find(s->Name());
                    if (k == kj->end() || !k->is_object()) continue;
                    s->key = ReadKey(*k, "key", s->key);
                    s->keyAlt = ReadKey(*k, "keyAlt", s->keyAlt);
                }
            }

            // After the settings, so OnEnable sees them
            if (const auto e = mi->find("enabled"); e != mi->end() && e->is_boolean()) m.SetEnabled(e->get<bool>());
        });

        spdlog::info("Config loaded");
        return true;
    }
    catch (const std::exception& e)
    {
        spdlog::error("Config error: {}", e.what());
        return false;
    }
}

bool config::Save()
{
    json mods = json::object();
    modules::ForEach([&](modules::Tab& t, Module& m)
    {
        json settings = json::object();
        json keys = json::object();
        for (const ctl::Setting* s : m.Settings())
        {
            if (s->Persistent()) s->Save(settings[s->Name()]);
            if (s->key || s->keyAlt) keys[s->Name()] = {{"key", s->key}, {"keyAlt", s->keyAlt}};
        }

        json mt = json::object();
        if (!m.IsAction()) mt["enabled"] = m.Enabled();
        mt["key"] = m.key;                           // Windows virtual-key code, 0 = unbound
        if (!settings.empty()) mt["settings"] = std::move(settings);
        if (!keys.empty()) mt["keys"] = std::move(keys);
        mods[ModuleKey(t, m)] = std::move(mt);
    });

    json root = json::object();
    root["theme"]   = theme::Current();
    root["modules"] = std::move(mods);

    std::ofstream out(ConfigPath());
    if (!out)
    {
        spdlog::error("Cannot write config to {}", ConfigPath().string());
        return false;
    }
    out << root.dump(2) << '\n';
    spdlog::info("Config saved");
    return out.good();
}
