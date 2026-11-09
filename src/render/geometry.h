#pragma once
#include "math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// Ray hits and "is this point inside" for the collision view's shapes: boxes, spheres, cylinders and capsules in their
// own local space (placed by a world matrix that may scale, rotate and move them), and convex hulls given as world-space
// triangles. Rays are origin + t * dir; the functions return the nearest t >= 0 of a hit, or a negative number for none.
namespace rgeom {
    using rmath::Vec3;
    using rmath::Mat4;

    constexpr float kNoHit = -1.f;

    // Inverse of an affine matrix (row vectors: rotation/scale in the upper 3x3, translation in the last row).
    // False when it can't be inverted (a zero scale).
    inline bool InverseAffine(const Mat4& m, Mat4& out)
    {
        const float a = m.m[0][0], b = m.m[0][1], c = m.m[0][2];
        const float d = m.m[1][0], e = m.m[1][1], f = m.m[1][2];
        const float g = m.m[2][0], h = m.m[2][1], i = m.m[2][2];
        const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        if (!(std::fabs(det) > 1e-12f)) return false;
        const float s = 1.f / det;
        out = rmath::Identity();
        out.m[0][0] = (e * i - f * h) * s; out.m[0][1] = (c * h - b * i) * s; out.m[0][2] = (b * f - c * e) * s;
        out.m[1][0] = (f * g - d * i) * s; out.m[1][1] = (a * i - c * g) * s; out.m[1][2] = (c * d - a * f) * s;
        out.m[2][0] = (d * h - e * g) * s; out.m[2][1] = (b * g - a * h) * s; out.m[2][2] = (a * e - b * d) * s;
        const Vec3 t{m.m[3][0], m.m[3][1], m.m[3][2]};
        for (int j = 0; j < 3; ++j)
            out.m[3][j] = -(t.x * out.m[0][j] + t.y * out.m[1][j] + t.z * out.m[2][j]);
        return true;
    }

    // A direction through the upper 3x3 only (no translation).
    inline Vec3 TransformDir(const Mat4& m, Vec3 v)
    {
        return {v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0], v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1],
                v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2]};
    }

    // ---- Local space shapes (t is the same in world space: the direction is transformed, not normalized) ------------
    inline float RayBox(Vec3 o, Vec3 d, Vec3 half)
    {
        float t0 = 0.f, t1 = std::numeric_limits<float>::max();
        const float os[3] = {o.x, o.y, o.z}, ds[3] = {d.x, d.y, d.z}, hs[3] = {half.x, half.y, half.z};
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(ds[k]) < 1e-12f) {
                if (os[k] < -hs[k] || os[k] > hs[k]) return kNoHit;
                continue;
            }
            float a = (-hs[k] - os[k]) / ds[k], b = (hs[k] - os[k]) / ds[k];
            if (a > b) std::swap(a, b);
            t0 = std::max(t0, a);
            t1 = std::min(t1, b);
            if (t0 > t1) return kNoHit;
        }
        return t0;
    }

    inline float RaySphere(Vec3 o, Vec3 d, Vec3 c, float r)
    {
        const Vec3 oc = o - c;
        const float A = rmath::Dot(d, d), B = rmath::Dot(oc, d), C = rmath::Dot(oc, oc) - r * r;
        if (C <= 0.f) return 0.f;                       // Inside
        const float disc = B * B - A * C;
        if (disc < 0.f || A <= 0.f) return kNoHit;
        const float t = (-B - std::sqrt(disc)) / A;
        return t >= 0.f ? t : kNoHit;
    }

    // Upright cylinder around local Z: radius r, from z = -h to h.
    inline float RayCylinder(Vec3 o, Vec3 d, float r, float h)
    {
        if (o.x * o.x + o.y * o.y <= r * r && std::fabs(o.z) <= h) return 0.f;
        float best = kNoHit;
        const auto take = [&](float t) { if (t >= 0.f && (best < 0.f || t < best)) best = t; };
        const float A = d.x * d.x + d.y * d.y, B = o.x * d.x + o.y * d.y, C = o.x * o.x + o.y * o.y - r * r;
        if (A > 1e-12f) {
            const float disc = B * B - A * C;
            if (disc >= 0.f) {
                const float s = std::sqrt(disc);
                for (float t : {(-B - s) / A, (-B + s) / A})
                    if (std::fabs(o.z + t * d.z) <= h) take(t);
            }
        }
        if (std::fabs(d.z) > 1e-12f)
            for (float z : {-h, h}) {
                const float t = (z - o.z) / d.z;
                const float x = o.x + t * d.x, y = o.y + t * d.y;
                if (x * x + y * y <= r * r) take(t);
            }
        return best;
    }

    // Capsule around local Z: a cylinder of half length h with spheres of radius r on its ends.
    inline float RayCapsule(Vec3 o, Vec3 d, float r, float h)
    {
        float best = RayCylinder(o, d, r, h);
        for (float z : {-h, h}) {
            const float t = RaySphere(o, d, {0.f, 0.f, z}, r);
            if (t >= 0.f && (best < 0.f || t < best)) best = t;
        }
        return best;
    }

    inline bool InBox(Vec3 p, Vec3 half) { return std::fabs(p.x) <= half.x && std::fabs(p.y) <= half.y && std::fabs(p.z) <= half.z; }
    inline bool InSphere(Vec3 p, float r) { return rmath::Dot(p, p) <= r * r; }
    inline bool InCylinder(Vec3 p, float r, float h) { return p.x * p.x + p.y * p.y <= r * r && std::fabs(p.z) <= h; }
    inline bool InCapsule(Vec3 p, float r, float h)
    {
        const float z = std::clamp(p.z, -h, h);
        return p.x * p.x + p.y * p.y + (p.z - z) * (p.z - z) <= r * r;
    }

    // ---- World-space triangles ---------------------------------------------------------------------------------------
    // Nearest hit on an indexed triangle list (Moller-Trumbore, both sides).
    inline float RayTriangles(Vec3 o, Vec3 d, const std::vector<Vec3>& v, const std::vector<int>& idx)
    {
        float best = kNoHit;
        for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
            const auto ia = static_cast<std::size_t>(idx[i]), ib = static_cast<std::size_t>(idx[i + 1]),
                       ic = static_cast<std::size_t>(idx[i + 2]);
            if (ia >= v.size() || ib >= v.size() || ic >= v.size()) continue;
            const Vec3 e1 = v[ib] - v[ia], e2 = v[ic] - v[ia];
            const Vec3 p = rmath::Cross(d, e2);
            const float det = rmath::Dot(e1, p);
            if (std::fabs(det) < 1e-12f) continue;
            const float inv = 1.f / det;
            const Vec3 s = o - v[ia];
            const float u = rmath::Dot(s, p) * inv;
            if (u < 0.f || u > 1.f) continue;
            const Vec3 q = rmath::Cross(s, e1);
            const float w = rmath::Dot(d, q) * inv;
            if (w < 0.f || u + w > 1.f) continue;
            const float t = rmath::Dot(e2, q) * inv;
            if (t >= 0.f && (best < 0.f || t < best)) best = t;
        }
        return best;
    }

    // Inside a convex hull: on the inner side of every face. The faces' winding isn't trusted: "inner" is the side the
    // hull's centroid is on.
    inline bool InConvex(Vec3 p, const std::vector<Vec3>& v, const std::vector<int>& idx)
    {
        if (v.size() < 4 || idx.size() < 12) return false;
        Vec3 c{};
        for (const Vec3& x : v) c = c + x;
        c = c * (1.f / static_cast<float>(v.size()));
        bool any = false;
        for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
            const auto ia = static_cast<std::size_t>(idx[i]), ib = static_cast<std::size_t>(idx[i + 1]),
                       ic = static_cast<std::size_t>(idx[i + 2]);
            if (ia >= v.size() || ib >= v.size() || ic >= v.size()) continue;
            const Vec3 n = rmath::Cross(v[ib] - v[ia], v[ic] - v[ia]);
            if (rmath::Dot(n, n) < 1e-12f) continue;    // Degenerate
            const float side = rmath::Dot(n, p - v[ia]), centroid = rmath::Dot(n, c - v[ia]);
            if (side * centroid < 0.f && std::fabs(side) > 1e-3f * rmath::Length(n)) return false;
            any = true;
        }
        return any;
    }
}
