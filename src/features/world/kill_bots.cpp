#include "core/module.h"
#include "core/notify.h"
#include "game/game.h"
#include "game/uobject.h"

#include <ME_SDK.hpp>
#include <spdlog/spdlog.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace Classes;

// Kills the enemies (TdBotPawn: cops, SWAT, ...) in the level, or within a range of the player. They die as if you
// had hit them (a lot), so the game's AI and level scripts see a normal death; bots the level made invulnerable lose
// that first. What is left alive after the damage (a bot that ignores it) is killed outright.
class KillBots final : public ActionModule {
public:
    KillBots() : ActionModule("Kill Bots", "World", "Kill every enemy in the level, or the ones near you", 30) {}

    void OnTrigger() override
    {
        game::RunOnGameThread([this] {
            const int n = KillAll(range.Get());
            notify::Push(n == 0 ? std::string("Kill Bots: no enemies") + (range.Get() > 0.f ? " in range" : "")
                                : "Killed " + std::to_string(n) + (n == 1 ? " enemy" : " enemies"));
        });
    }

    // Game thread. Returns how many were killed.
    static int KillAll(float rangeMeters)
    {
        auto* world = game::World();
        auto* player = game::Pawn();
        auto* controller = game::Controller();
        if (!world) return 0;

        // Collected first: a death can take the pawn off the list (and destroy it) while it is walked
        std::vector<ATdBotPawn*> bots;
        for (APawn* p = world->PawnList; p; p = p->NextPawn) {
            if (p == player || !game::IsA(p, ATdBotPawn::StaticClass())) continue;
            auto* bot = static_cast<ATdBotPawn*>(p);
            if (bot->Health <= 0 || bot->bDeleteMe) continue;
            if (rangeMeters > 0.f && player) {
                const float dx = bot->Location.X - player->Location.X, dy = bot->Location.Y - player->Location.Y,
                            dz = bot->Location.Z - player->Location.Z;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) > rangeMeters * 100.f) continue;     // 100 units = 1 m
            }
            bots.push_back(bot);
        }

        int killed = 0;
        for (ATdBotPawn* bot : bots) {
            bot->bAIGodMode = 0;
            const FVector at = bot->Location;
            FTraceHitInfo hit{};
            bot->TakeDamage(bot->Health + 10000, controller, at, FVector{}, UTdDmgType_Melee::StaticClass(), hit, player);
            bool dead = bot->Health <= 0 || bot->bDeleteMe;
            if (!dead) dead = bot->Died(controller, UTdDmgType_Melee::StaticClass(), at);
            if (dead) ++killed;
        }
        spdlog::info("Kill Bots: {} of {} killed", killed, bots.size());
        return killed;
    }

private:
    ctl::Slider range{this, "Range (m)", 0.f, 0.f, 200.f, "%.0f", 5.f, "0: the whole level"};
};
MEM_MODULE(KillBots)

} // namespace
