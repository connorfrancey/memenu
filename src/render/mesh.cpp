#include "mesh.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace rmesh
{
    using rmath::Mat4;
    using rmath::Vec3;

    namespace
    {
        constexpr float kFeatureCos = 0.9659f; // cos(15 deg)
        constexpr int kMinSegments = 6;

        // Mix the color towards white, keeping alpha.
        std::uint32_t Lighten(std::uint32_t color, float t)
        {
            const auto channel = [&](int shift)
            {
                const float c = static_cast<float>((color >> shift) & 0xFF);
                return static_cast<std::uint32_t>(std::clamp(c + (255.f - c) * t, 0.f, 255.f));
            };
            return (color & 0xFF000000u) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
        }

        struct Edge
        {
            int a, b;
            Vec3 normal;
            int count;
            bool crease;
        };
    }

    std::uint32_t PackColor(float r, float g, float b, float a)
    {
        const auto c = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
        return (c(a) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
    }

    std::uint32_t Builder::OutlineColor() const
    {
        if (style_.outlineColor) return style_.outlineColor;
        return style_.fill ? Lighten(style_.color, 0.4f) : style_.color;
    }

    void Builder::Line(Vec3 a, Vec3 b, std::uint32_t color)
    {
        out_.lines.push_back({a.x, a.y, a.z, color});
        out_.lines.push_back({b.x, b.y, b.z, color});
    }

    void Builder::Mesh(const std::vector<Vec3>& v, const std::vector<int>& idx)
    {
        ++shapes_;
        const int vertexCount = static_cast<int>(v.size());
        const std::size_t triCount = idx.size() / 3;

        std::vector<Vertex>& faces = out_.tris;
        if (style_.fill) faces.reserve(faces.size() + triCount * 3);

        // Edge bookkeeping is only needed for outlines.
        std::unordered_map<std::uint64_t, Edge> edges;
        if (style_.outline) edges.reserve(idx.size());

        for (std::size_t t = 0; t + 2 < idx.size(); t += 3)
        {
            const int i[3] = {idx[t], idx[t + 1], idx[t + 2]};
            if (i[0] < 0 || i[1] < 0 || i[2] < 0 || i[0] >= vertexCount || i[1] >= vertexCount || i[2] >= vertexCount)
            {
                ++counters_.invalidTriangles;
                continue;
            }

            // Faces are flat-colored: no lighting. Degenerate triangles cost the GPU nothing.
            if (style_.fill)
                for (int k = 0; k < 3; ++k)
                {
                    const Vec3& p = v[static_cast<std::size_t>(i[k])];
                    faces.push_back({p.x, p.y, p.z, style_.color});
                }

            if (style_.outline)
            {
                // Normals are only used to find creases between faces.
                const Vec3 normal = rmath::Cross(v[static_cast<std::size_t>(i[1])] - v[static_cast<std::size_t>(i[0])],
                                                 v[static_cast<std::size_t>(i[2])] - v[static_cast<std::size_t>(i[0])]);
                const float len = rmath::Length(normal);
                if (len < 1e-6f)
                {
                    ++counters_.degenerateTriangles;
                    continue;
                }
                const Vec3 n = normal * (1.f / len);

                for (int e = 0; e < 3; ++e)
                {
                    const int a = i[e], b = i[(e + 1) % 3];
                    const std::uint64_t key = (static_cast<std::uint64_t>(std::min(a, b)) << 32) |
                        static_cast<std::uint32_t>(std::max(a, b));
                    const auto [it, inserted] = edges.try_emplace(key, Edge{a, b, n, 1, false});
                    if (!inserted)
                    {
                        ++it->second.count;
                        if (rmath::Dot(it->second.normal, n) < kFeatureCos) it->second.crease = true;
                    }
                }
            }
        }

        if (style_.outline)
        {
            const std::uint32_t color = OutlineColor();
            for (const auto& [key, e] : edges)
                if (e.count == 1 || e.count > 2 || e.crease)
                    Line(v[static_cast<std::size_t>(e.a)], v[static_cast<std::size_t>(e.b)], color);
        }
    }

    void Builder::Box(const Mat4& world, Vec3 h)
    {
        std::vector<Vec3> v(8);
        for (int i = 0; i < 8; ++i)
            v[static_cast<std::size_t>(i)] = rmath::TransformPoint(world, {
                                                                       (i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y,
                                                                       (i & 4) ? h.z : -h.z
                                                                   });
        static const std::vector<int> idx = {
            0, 2, 6, 0, 6, 4, 1, 3, 7, 1, 7, 5, 0, 4, 5, 0, 5, 1,
            2, 3, 7, 2, 7, 6, 0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6
        };

        Mesh(v, idx);
    }

    void Builder::Sphere(const Mat4& world, float radius, int segments)
    {
        const int segs = std::max(kMinSegments, segments), rings = std::max(4, segments / 2);
        std::vector<Vec3> v;
        std::vector<int> idx;
        v.reserve(static_cast<std::size_t>((rings + 1) * (segs + 1)));
        for (int i = 0; i <= rings; ++i)
        {
            const float phi = -rmath::kPi * 0.5f + rmath::kPi * static_cast<float>(i) / static_cast<float>(rings);
            for (int j = 0; j <= segs; ++j)
            {
                const float theta = 2.f * rmath::kPi * static_cast<float>(j) / static_cast<float>(segs);
                v.push_back(rmath::TransformPoint(world, {
                                                      radius * std::cos(phi) * std::cos(theta),
                                                      radius * std::cos(phi) * std::sin(theta), radius * std::sin(phi)
                                                  }));
            }
        }
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < segs; ++j)
            {
                const int a = i * (segs + 1) + j, b = a + 1, c = a + segs + 1, d = c + 1;
                idx.insert(idx.end(), {a, b, d, a, d, c});
            }
        Mesh(v, idx);
    }

    void Builder::Cylinder(const Mat4& world, float radius, float halfHeight, int segments)
    {
        const int n = std::max(kMinSegments, segments);
        std::vector<Vec3> v;
        std::vector<int> idx;
        v.reserve(static_cast<std::size_t>(2 * n + 2));
        for (int k = 0; k < 2; ++k) // Bottom ring, then top ring
            for (int j = 0; j < n; ++j)
            {
                const float a = 2.f * rmath::kPi * static_cast<float>(j) / static_cast<float>(n);
                v.push_back(rmath::TransformPoint(world, {
                                                      radius * std::cos(a), radius * std::sin(a),
                                                      k ? halfHeight : -halfHeight
                                                  }));
            }
        v.push_back(rmath::TransformPoint(world, {0.f, 0.f, -halfHeight})); // 2n: bottom center
        v.push_back(rmath::TransformPoint(world, {0.f, 0.f, halfHeight}));  // 2n+1: top center
        for (int j = 0; j < n; ++j)
        {
            const int a = j, b = (j + 1) % n;
            idx.insert(idx.end(), {a, b, n + b, a, n + b, n + a}); // Side
            idx.insert(idx.end(), {2 * n, b, a});                  // Bottom cap
            idx.insert(idx.end(), {2 * n + 1, n + a, n + b});      // Top cap
        }
        Mesh(v, idx);
    }

    void Builder::Capsule(const Mat4& world, float radius, float halfCylinder, int segments)
    {
        const int n = std::max(kMinSegments, segments), rings = std::max(2, segments / 3);

        // Profile (radius, z) from the bottom pole to the top pole.
        std::vector<std::pair<float, float>> profile;
        for (int k = 0; k <= rings; ++k)
        {
            const float a = -rmath::kPi * 0.5f + rmath::kPi * 0.5f * static_cast<float>(k) / static_cast<float>(rings);
            profile.emplace_back(radius * std::cos(a), -halfCylinder + radius * std::sin(a));
        }
        for (int k = 0; k <= rings; ++k)
        {
            const float a = rmath::kPi * 0.5f * static_cast<float>(k) / static_cast<float>(rings);
            profile.emplace_back(radius * std::cos(a), halfCylinder + radius * std::sin(a));
        }

        std::vector<Vec3> v;
        std::vector<int> idx;
        for (const auto& [r, z] : profile)
            for (int j = 0; j <= n; ++j)
            {
                const float theta = 2.f * rmath::kPi * static_cast<float>(j) / static_cast<float>(n);
                v.push_back(rmath::TransformPoint(world, {r * std::cos(theta), r * std::sin(theta), z}));
            }
        for (std::size_t i = 0; i + 1 < profile.size(); ++i)
            for (int j = 0; j < n; ++j)
            {
                const int a = static_cast<int>(i) * (n + 1) + j, b = a + 1, c = a + n + 1, d = c + 1;
                idx.insert(idx.end(), {a, b, d, a, d, c});
            }
        Mesh(v, idx);
    }
}
