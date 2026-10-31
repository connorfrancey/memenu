#include "features/player/fly_module.h"

namespace {

class NoClip final : public FlyModule {
public:
    NoClip() : FlyModule("No Clip", "Fly through walls, floors and everything else", 20, flight::Mode::NoClip)
    {
        key = VK_F5;
    }
};
MEM_MODULE(NoClip)

} // namespace
