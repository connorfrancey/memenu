#pragma once
#include "render/static_scene.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Static world geometry for the collision view: the simple collision of every static mesh in the level (walls, floors,
// movers, physics props, invisible walls), sorted into rstatic::Category layers that are shown and colored separately.
//
// Made not to cost the game frames:
//   - the object list is walked a slice per tick, never all at once;
//   - a mesh's collision is read once and shared by every place it is used;
//   - the triangles are built on a worker thread from an immutable snapshot, only when the camera has moved far enough
//     or a setting changed, nearest first and within a vertex budget;
//   - the renderer uploads them only when they change.
// Every pointer read from the game is checked against the objects actually in the engine's object list first, so a wrong
// SDK offset finds nothing instead of crashing.
//
// Game thread, except GetInfo.
namespace staticworld {
    struct Config {
        std::array<bool, rstatic::kCategoryCount> enabled{};
        std::array<std::uint32_t, rstatic::kCategoryCount> colors{};   // 0xAARRGGBB
        bool nonColliding = false;      // Also meshes the player passes through
        bool fill = false, outline = true;
        float maxDistance = 3000.f;
        bool fade = true;
        std::string filter;             // Lower case, owner or mesh name
        std::size_t vertexBudget = 400000;
        float liveHz = 10.f;            // How often movers and props are redrawn where they are
    };

    struct Info {
        std::string map;
        bool scanning = false;
        float scanProgress = 0.f;       // 0..1 of the current pass
        std::size_t meshes = 0;         // Different meshes read
        std::array<std::size_t, rstatic::kCategoryCount> found{}, drawn{};
        std::size_t vertices = 0;       // In the last build
        double buildMs = 0.0;           // Of the last build (worker thread)
        bool budgetHit = false;
        double tickMs = 0.0, tickMaxMs = 0.0;   // Game thread: the last Update() and the slowest one in this level
        double tickAvgMs = 0.0;                 // ... and on average (recent ticks)
    };

    void Update(const Config& config);  // Every tick while shown
    void Stop();                        // Hidden or the view is off: forget the drawing, stop the worker
    Info GetInfo();                     // Any thread
}
