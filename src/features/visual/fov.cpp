#include "core/module.h"

namespace {

class Fov final : public Module {
public:
    Fov() : Module("FOV", "Visual", "Override the field of view", 10) {}

    void OnTick() override
    {
        // TODO: write degrees to the player camera (PlayerCamera DefaultFOV / the POV the game renders from).
        (void)degrees.Get();
    }

private:
    ctl::Slider degrees{this, "Degrees", 90.f, 60.f, 130.f, "%.0f", 5.f};
    ctl::Label note{this, "Not implemented yet."};
};
MEM_MODULE(Fov)

} // namespace
