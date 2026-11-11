#include "core/module.h"
#include "game/game.h"
#include "game/uobject.h"

#include "sdk/SDK.hpp"
#include <spdlog/spdlog.h>

#include <vector>

namespace {

// The console's "set Engine bEnableColorClear true": the engine clears the frame before drawing the world. Without it,
// wherever nothing is drawn (out of bounds, past the edge of the level) the last frames stay on screen and smear into
// a "hall of mirrors". Like "set", it changes the engine object and its class default; put back when switched off.
class ClearFrame final : public Module {
public:
    ClearFrame() : Module("Clear Frame", "Visual", "Clear the screen every frame: no repeating draws out of bounds", 30) {}

    void OnTick() override
    {
        if (!found_) Find();
        for (Saved& e : engines_) e.engine->bEnableColorClear = 1;
    }

    void OnDisable() override
    {
        // May run on the render thread: put the engine back on the game thread.
        game::RunOnGameThread([this] {
            for (const Saved& e : engines_) e.engine->bEnableColorClear = e.was ? 1 : 0;
            engines_.clear();
            found_ = false;
        });
    }

private:
    struct Saved {
        Classes::UEngine* engine;
        bool was;
    };

    // One walk of the object list: the engine and its class default live as long as the game.
    void Find()
    {
        found_ = true;
        const auto& objects = Classes::UObject::GetGlobalObjects();
        for (std::size_t i = 0; i < objects.Num(); ++i) {
            auto* o = objects.GetByIndex(i);
            if (!o || !game::IsA(o, Classes::UEngine::StaticClass())) continue;
            auto* engine = static_cast<Classes::UEngine*>(o);
            engines_.push_back({engine, engine->bEnableColorClear != 0});
        }
        spdlog::info("Clear Frame: {} engine object(s)", engines_.size());
    }

    // Game thread
    bool found_ = false;
    std::vector<Saved> engines_;
};
MEM_MODULE(ClearFrame)

} // namespace
