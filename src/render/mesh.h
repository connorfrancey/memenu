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
        std::uint32_t color;        // 0xAARRGGBB (D3DCOLOR)
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
        bool fill = true;                // Flat-colored faces
        bool outline = true;             // Feature edges (boundaries and creases)
        std::uint32_t outlineColor = 0;  // 0 = a lighter version of `color`
    };

    // Appends shapes to a MeshData in the current style. Primitives take a world matrix (row-vector convention).
    class Builder {
    public:
        explicit Builder(MeshData& out) : out_(out) {}

        void SetStyle(const Style& style) { style_ = style; }
        const Style& GetStyle() const { return style_; }
        std::size_t Shapes() const { return shapes_; }   // Meshes added so far (primitives count as one each)

        struct Counters {
            std::size_t invalidTriangles = 0;       // An index out of range: skipped
            std::size_t degenerateTriangles = 0;    // Zero area: no outline edges
        };
        const Counters& GetCounters() const { return counters_; }

        void Line(rmath::Vec3 a, rmath::Vec3 b, std::uint32_t color);
        // Indexed triangle list. Out-of-range triangles are skipped.
        void Mesh(const std::vector<rmath::Vec3>& v, const std::vector<int>& idx);
        void Box(const rmath::Mat4& world, rmath::Vec3 halfExtent);
        void Sphere(const rmath::Mat4& world, float radius, int segments = 16);
        void Cylinder(const rmath::Mat4& world, float radius, float halfHeight, int segments = 16);
        void Capsule(const rmath::Mat4& world, float radius, float halfCylinder, int segments = 16);

    private:
        std::uint32_t OutlineColor() const;

        MeshData& out_;
        Style style_;
        std::size_t shapes_ = 0;
        Counters counters_;
    };
}