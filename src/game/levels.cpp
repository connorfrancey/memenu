#include "levels.h"
#include "game.h"

#include <Windows.h>

#include <mirrorsedge_sdk.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <mutex>
#include <set>

namespace levels
{
    namespace
    {
        struct CheckpointEntry
        {
            std::string name, label;
        };

        struct MapEntry
        {
            std::string file; // Package name, e.g. "edge_p"
            std::string label;
            std::string gameMode;
            std::vector<CheckpointEntry> checkpoints;
        };

        std::mutex g_mutex;
        std::vector<MapEntry> g_maps;
        std::vector<SublevelView> g_sublevels;
        bool g_haveGameMaps = false;        // False while only the built-in list is known
        std::string g_message;

        void SetMessage(std::string message)
        {
            spdlog::info("Levels: {}", message);
            std::lock_guard lock(g_mutex);
            g_message = std::move(message);
        }

        std::wstring Widen(const std::string& s)
        {
            if (s.empty()) return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        // FString::ToString() calls wcslen on the buffer, which crashes for an empty (null) FString.
        std::string Str(const Classes::FString& s) { return s.IsValid() ? s.ToString() : std::string(); }

        bool IsDefaultObject(const Classes::UObject* o) { return o->GetName().rfind("Default__", 0) == 0; }

        // Story levels (names from mmultiplayer's util.h), used until the game's own list is available.
        const struct { const char* file; const char* label; } kBuiltin[] =
        {
            {"tdmainmenu", "Main menu"}, {"edge_p", "Prologue"}, {"escape_p", "Flight"}, {"stromdrain_p", "Jacknife"},
            {"cranes_p", "Heat"}, {"subway_p", "Ropeburn"}, {"mall_p", "New Eden"}, {"factory_p", "Factory"},
            {"boat_p", "The Boat"}, {"convoy_p", "Kate"}, {"scraper_p", "The Shard"},
        };

        // ---- Game-thread lookups ------------------------------------------------------------------------------------
        template <class F>
        void ForEachObject(F&& f)
        {
            const auto& objects = Classes::UObject::GetGlobalObjects();
            for (size_t i = 0; i < objects.Num(); ++i)
            {
                auto* o = objects.GetByIndex(i);
                if (o && !IsDefaultObject(o)) f(o);
            }
        }

        Classes::UUIDataStore_TdGameData* FindGameData()
        {
            if (auto* world = game::World())
            {
                if (world->Game && world->Game->IsA(Classes::ATdGameInfo::StaticClass()))
                {
                    if (auto* data = static_cast<Classes::ATdGameInfo*>(world->Game)->TdGameData) return data;
                }
            }

            // Main menu etc.: the controller lookup in game.cpp can fail there, so find the data store itself.
            Classes::UUIDataStore_TdGameData* found = nullptr;
            ForEachObject([&](Classes::UObject* o)
            {
                if (!found && o->IsA(Classes::UUIDataStore_TdGameData::StaticClass()))
                    found = static_cast<Classes::UUIDataStore_TdGameData*>(o);
            });
            return found;
        }

        Classes::APlayerController* FindPlayerController()
        {
            if (auto* pc = game::Controller()) return pc;
            Classes::APlayerController* found = nullptr;
            ForEachObject([&](Classes::UObject* o)
            {
                if (found || !o->IsA(Classes::AWorldInfo::StaticClass())) return;
                for (auto c = static_cast<Classes::AWorldInfo*>(o)->ControllerList; c && !found; c = c->NextController)
                    if (c->IsA(Classes::APlayerController::StaticClass())) found = static_cast<
                        Classes::APlayerController*>(c);
            });
            return found;
        }

        // ---- List building (game thread) ----------------------------------------------------------------------------
        void RefreshMaps()
        {
            std::vector<MapEntry> maps;
            std::set<std::string> seen;

            ForEachObject([&](Classes::UObject* o)
            {
                if (!o->IsA(Classes::UUIDataProvider_TdMaps::StaticClass())) return;
                auto* p = static_cast<Classes::UUIDataProvider_TdMaps*>(o);

                MapEntry e;
                e.file = Str(p->Filename);
                if (e.file.empty()) return;
                const std::string title = Str(p->MapName);
                e.label = title.empty() ? e.file : title + " (" + e.file + ")";
                e.gameMode = Str(p->GameMode);
                if (!seen.insert(e.file + "|" + title).second) return;

                for (size_t i = 0; i < p->Checkpoints.Num(); ++i)
                {
                    const auto& cp = p->Checkpoints[i];
                    CheckpointEntry c;
                    c.name = Str(cp.CheckpointName);
                    const std::string friendly = Str(cp.CheckpointFriendlyName);
                    c.label = friendly.empty() ? c.name : friendly + " (" + c.name + ")";
                    if (!c.name.empty()) e.checkpoints.push_back(std::move(c));
                }
                maps.push_back(std::move(e));
            });

            const bool haveGameMaps = !maps.empty();
            if (!haveGameMaps)
            {
                for (const auto& b : kBuiltin) maps.push_back({b.file, std::string(b.label) + " (" + b.file + ")", {}, {}});
            }

            std::lock_guard lock(g_mutex);
            g_haveGameMaps = haveGameMaps;
            g_maps = std::move(maps);
        }

        void RefreshSublevels()
        {
            std::vector<SublevelView> views;
            if (auto* world = game::World())
            {
                for (size_t i = 0; i < world->StreamingLevels.Num(); ++i)
                {
                    auto* ls = world->StreamingLevels[i];
                    if (!ls) continue;
                    views.push_back({ls->PackageName.GetName(), ls->bShouldBeLoaded != 0, ls->bShouldBeVisible != 0});
                }
            }
            std::lock_guard lock(g_mutex);
            g_sublevels = std::move(views);
        }

        void TickCallback(float)
        {
            static int n = 0;
            ++n;
            if (n % 30 == 0) RefreshSublevels();
            bool haveGameMaps;
            {
                std::lock_guard lock(g_mutex);
                haveGameMaps = g_haveGameMaps;
            }
            if (n % 120 == 0 && !haveGameMaps) RefreshMaps(); // The game's map list may appear a bit after startup
        }
    }

    // ---- Public API -------------------------------------------------------------------------------------------------
    void Init()
    {
        {
            std::lock_guard lock(g_mutex);
            for (const auto& b : kBuiltin) g_maps.push_back(
                {b.file, std::string(b.label) + " (" + b.file + ")", {}, {}});
        }
        game::OnTick(&TickCallback);
        Refresh();
    }

    void Refresh()
    {
        game::RunOnGameThread([]
        {
            RefreshMaps();
            RefreshSublevels();
            SetMessage("Lists refreshed");
        });
    }

    std::vector<MapView> Maps()
    {
        std::lock_guard lock(g_mutex);
        std::vector<MapView> v;
        for (const auto& m : g_maps)
        {
            MapView view{m.label, {}};
            for (const auto& c : m.checkpoints) view.checkpoints.push_back(c.label);
            v.push_back(std::move(view));
        }
        return v;
    }

    void Load(int mapIndex, int checkpoint, Method method, bool saveProgress)
    {
        MapEntry map;
        CheckpointEntry cp;
        {
            std::lock_guard lock(g_mutex);
            if (mapIndex < 0 || mapIndex >= static_cast<int>(g_maps.size())) return;
            map = g_maps[static_cast<size_t>(mapIndex)];
            if (checkpoint > 0 && checkpoint - 1 < static_cast<int>(map.checkpoints.size()))
                cp = map.checkpoints[static_cast<size_t>(checkpoint - 1)];
        }
        if (!game::Ready())
        {
            SetMessage("Game integration is not ready");
            return;
        }

        game::RunOnGameThread([map, cp, method, saveProgress]
        {
            if (method == Method::Console)
            {
                auto* pc = FindPlayerController();
                if (!pc)
                {
                    SetMessage("No player controller found");
                    return;
                }
                const std::wstring command = L"open " + Widen(map.file);
                pc->ConsoleCommand(Classes::FString(command.c_str()), false);
                SetMessage("open " + map.file + (cp.name.empty() ? "" : " (checkpoints need the Game method)"));
                return;
            }

            auto* data = FindGameData();
            if (!data)
            {
                SetMessage("Game data store not found: try 'Console load'");
                return;
            }

            const std::wstring level = Widen(map.file), checkpointName = Widen(cp.name), mode = Widen(map.gameMode),
                               url;
            // Same call the game's own level-select menu makes. Achievements stay off for loads made from here.
            data->StartGame(Classes::FString(level.c_str()), Classes::FString(checkpointName.c_str()),
                            Classes::FString(mode.c_str()), Classes::FString(url.c_str()), saveProgress, false);
            SetMessage("StartGame " + map.file + (cp.name.empty() ? "" : " @ " + cp.name));
        });
    }

    void RestartFromLastCheckpoint()
    {
        game::RunOnGameThread([]
        {
            if (auto* data = FindGameData())
            {
                data->RestartFromLastCheckpoint(false, false);
                SetMessage("Restarting from last checkpoint");
            }
            else SetMessage("Game data store not found");
        });
    }

    void QuitToMainMenu()
    {
        game::RunOnGameThread([]
        {
            if (auto* data = FindGameData())
            {
                data->QuitToMainMenu();
                SetMessage("Quitting to main menu");
            }
            else SetMessage("Game data store not found");
        });
    }

    std::vector<SublevelView> Sublevels()
    {
        std::lock_guard lock(g_mutex);
        return g_sublevels;
    }

    namespace
    {
        // Runs `change` on the streaming level called `name` (game thread).
        template <class F>
        void WithSublevel(const std::string& name, const char* what, F change)
        {
            game::RunOnGameThread([name, what, change]
            {
                auto* world = game::World();
                if (!world)
                {
                    SetMessage("No level loaded");
                    return;
                }
                for (size_t i = 0; i < world->StreamingLevels.Num(); ++i)
                {
                    auto* ls = world->StreamingLevels[i];
                    if (!ls || ls->PackageName.GetName() != name) continue;
                    change(ls);
                    RefreshSublevels();
                    SetMessage(std::string("Sublevel ") + name + ": " + what);
                    return;
                }
                SetMessage("Sublevel " + name + " not found: press Refresh");
            });
        }
    }

    void ToggleSublevelLoaded(const std::string& name)
    {
        WithSublevel(name, "toggled load", [](Classes::ULevelStreaming* ls)
        {
            if (ls->bShouldBeLoaded)
            {
                ls->bShouldBeLoaded = 0;
                ls->bShouldBeVisible = 0;
            }
            else
            {
                ls->bShouldBeLoaded = 1;
                ls->bShouldBeVisible = 1;
                ls->bShouldBlockOnLoad = 1;
            }
        });
    }

    void ToggleSublevelVisible(const std::string& name)
    {
        WithSublevel(name, "toggled visibility", [](Classes::ULevelStreaming* ls)
        {
            if (ls->bShouldBeVisible) { ls->bShouldBeVisible = 0; }
            else
            {
                ls->bShouldBeLoaded = 1;
                ls->bShouldBeVisible = 1;
            }
        });
    }

    std::string LastMessage()
    {
        std::lock_guard lock(g_mutex);
        return g_message;
    }
}
