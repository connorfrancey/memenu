#pragma once
#include <cmath>

// Pure movement math for the flight modules: no SDK, no game types, so it can be tested on its own.
namespace flight
{
    struct Vec
    {
        float x = 0.f, y = 0.f, z = 0.f;
    };

    constexpr float kTwoPi = 6.28318530718f;

    // Unreal rotators are unsigned 1/65536ths of a turn.
    inline float YawRadians(unsigned int units)
    {
        return static_cast<float>(units & 0xFFFFu) * (kTwoPi / 65536.f);
    }

    // Pitch as a signed angle: 0..16383 looks up, 49152..65535 looks down.
    inline float PitchRadians(unsigned int units)
    {
        int p = static_cast<int>(units & 0xFFFFu);
        if (p > 32767) p -= 65536;
        return static_cast<float>(p) * (kTwoPi / 65536.f);
    }

    // World-space direction from the player's input. `forward` and `right` are the analog axes (WASD or a stick,
    // -1..1), `vertical` is -1..1. With `followView` forward goes where the camera looks (pitch included), otherwise it
    // stays level. The result is limited to length 1, so diagonals are not faster. Unreal: X forward, Y right, Z up.
    inline Vec Direction(unsigned int yawUnits, unsigned int pitchUnits, float forward, float right, float vertical,
                         bool followView)
    {
        const float yaw = YawRadians(yawUnits);
        const float pitch = followView ? PitchRadians(pitchUnits) : 0.f;
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);

        Vec d{forward * cp * cy - right * sy, forward * cp * sy + right * cy, forward * sp + vertical};
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len > 1.f)
        {
            d.x /= len;
            d.y /= len;
            d.z /= len;
        }
        return d;
    }

    // Frame-rate independent exponential approach of `current` towards `target`.
    inline float Approach(float current, float target, float responsiveness, float dt)
    {
        return current + (target - current) * (1.f - std::exp(-responsiveness * dt));
    }
}
