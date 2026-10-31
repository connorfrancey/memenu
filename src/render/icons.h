#pragma once
#include <cstdint>
#include <vector>

// Procedurally drawn icons for the textured volumes (no image files to ship, no image decoder). Every icon is a white
// shape whose alpha is its coverage; the renderer blends it over the volume's color and tiles it over the faces.
// No Direct3D in here, so it can be tested on its own.
namespace ricons {
    enum class Icon : int {
        None = 0,   // plain colored faces
        Skull,      // skull and crossbones: kill zones
        Flag,       // checkpoints
        Bolt,       // triggers
        Arrow,      // movement volumes (ladders, ziplines, ...)
        NoEntry,    // blocking volumes
        Target,     // cylinders
        Box,        // any other volume
        Count
    };

    constexpr int kIconCount = static_cast<int>(Icon::Count);
    constexpr int kDefaultSize = 128;
}