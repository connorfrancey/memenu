#pragma once
#include "math.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// CPU-side geometry for the debug renderer: world-space flat-colored triangles and lines. No Direct3D and no game types in here, so it can be tested on its own. This is a hot path (thousands of
// shapes per rebuild): no logging and no per-triangle lighting in here.
namespace rmesh {
    struct Vertex {
        float x, y, z;
        std::uint32_t color;    // 0xAARRGGBB (D3DCOLOR)
    };

    struct MeshData {
        std::vector<Vertex> tris;       // Triangle list
        std::vector<Vertex> lines;      // Line list
    };

    struct Label {
        rmath::Vec3 pos;
        std::string text;
        std::uint32_t color = 0xFFFFFFFF;
    };

    std::uint32_t PackColor(float r, float g, float b, float a = 1.f);

    struct Style {
        std::uint32_t color = 0xFFFFFFFF;
        bool fill = true;                   // Flat-colored faces
        bool outline = true;                // Feature edges (boundaries and creases)
        std::uint32_t outlineColor = 0;     // 0 = a lighter version of `color`
    };


}