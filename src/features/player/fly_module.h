#pragma once
#include <Windows.h>

#include "core/module.h"
#include "game/flight.h"

// What No Clip and Flight have in common: the same controls and the same movement, a different mode.
class FlyModule : public Module {
public:
    FlyModule(std::string name, std::string description, int order, flight::Mode mode);

    void OnTick() override;
    void OnDisable() override;

private:
    flight::Settings Current() const;

    flight::Mode mode_;

    ctl::Slider speed{this, "Speed", 900.f, 100.f, 10000.f, "%.0f", 100.f,
                      "Units per second. Hold the boost key for more"};
    ctl::Slider boost{this, "Boost", 3.f, 1.f, 10.f, "%.1fx", 0.5f};
    ctl::Slider responsiveness{this, "Responsiveness", 10.f, 1.f, 40.f, "%.0f", 1.f,
                               "How fast you reach full speed and stop. Higher = snappier, lower = floatier"};
    ctl::Checkbox followView{this, "Move along view", true,
                             "Forward goes where you look, up and down included. Off: forward stays level"};
    ctl::Checkbox invulnerable{this, "Invulnerable", true,
                               "Keeps health full and prevents fall damage while active (and after you land)"};
    ctl::Checkbox blockActions{this, "Block jump/actions", true,
                               "Ignore the game's own jump/crouch/etc. while flying (the up/down keys are often those)"};
    ctl::KeyHold upKey{this, "Up key", VK_SPACE};
    ctl::KeyHold downKey{this, "Down key", VK_LCONTROL};
    ctl::KeyHold boostKey{this, "Boost key", VK_LSHIFT};
};
