#include "features/player/fly_module.h"

namespace {

class Flight final : public FlyModule {
public:
    Flight() : FlyModule("Flight", "Fly freely but still collide with the world", 30, flight::Mode::Flight) {}
};
MEM_MODULE(Flight)

} // namespace
