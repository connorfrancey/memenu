#pragma once
#include <cmath>

// Tiny math layer for the debug renderer. Row-vector convention (v' = v * M), the same as Direct3D and
// Unreal Engine 3's FMatrix, so engine matrices can be used as they are.
namespace rmath {
    constexpr float kPi = 3.14159265358979f;

    struct Vec3 {
        float x = 0.f, y = 0.f, z = 0.f;
    };

    inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }

    inline Vec3 Normalize(Vec3 a) {
        const float l = Length(a);
        return l > 1e-8f ? a * (1.f / l) : Vec3{};
    }

    struct Mat4 {
        float m[4][4];
    };

    inline Mat4 Identity() { return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}; }

    // a first, then b
    inline Mat4 Mul(const Mat4 &a, const Mat4 &b) {
        Mat4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][
                                j];
        return r;
    }

    inline Mat4 Transpose(const Mat4 &a) {
        Mat4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[j][i];
        return r;
    }

    inline Mat4 Translation(Vec3 t) {
        Mat4 r = Identity();
        r.m[3][0] = t.x;
        r.m[3][1] = t.y;
        r.m[3][2] = t.z;
        return r;
    }

    inline Vec3 TransformPoint(const Mat4 &M, Vec3 p) {
        return {
            p.x * M.m[0][0] + p.y * M.m[1][0] + p.z * M.m[2][0] + M.m[3][0],
            p.x * M.m[0][1] + p.y * M.m[1][1] + p.z * M.m[2][1] + M.m[3][1],
            p.x * M.m[0][2] + p.y * M.m[1][2] + p.z * M.m[2][2] + M.m[3][2]
        };
    }

    // ---- camera ------------------------------------------------------------------------------------------------
    // Unreal conventions: X forward, Y right, Z up; rotations in 1/65536 of a turn; FOV is the horizontal angle.
    struct Camera {
        Vec3 pos;
        int pitch = 0, yaw = 0, roll = 0;
        float fov = 90.f;
        bool valid = false;
    };

    // FRotationMatrix: the rows are the camera's forward / right / up axes in world space.
    inline Mat4 RotationMatrix(int pitch, int yaw, int roll) {
        const float k = 2.f * kPi / 65536.f;
        const float p = static_cast<float>(pitch & 0xFFFF) * k, y = static_cast<float>(yaw & 0xFFFF) * k,
                r = static_cast<float>(roll & 0xFFFF) * k;
        const float sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr =
                std::cos(r);

        Mat4 m = Identity();
        m.m[0][0] = cp * cy;
        m.m[0][1] = cp * sy;
        m.m[0][2] = sp;
        m.m[1][0] = sr * sp * cy - cr * sy;
        m.m[1][1] = sr * sp * sy + cr * cy;
        m.m[1][2] = -sr * cp;
        m.m[2][0] = -(cr * sp * cy + sr * sy);
        m.m[2][1] = cy * sr - cr * sp * sy;
        m.m[2][2] = cr * cp;
        return m;
    }

    // World -> D3D view space (x right, y up, z forward), like the engine's view matrix.
    inline Mat4 ViewMatrix(const Camera &c) {
        const Mat4 toCamera = Transpose(RotationMatrix(c.pitch, c.yaw, c.roll)); // orthonormal: inverse = transpose
        const Mat4 swizzle = {{{0, 0, 1, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}}};
        // (fwd,right,up) -> (right,up,fwd)
        return Mul(Mul(Translation(c.pos * -1.f), toCamera), swizzle);
    }

    // Perspective with an infinite far plane, like the engine's FPerspectiveMatrix. Depth maps to [0,1].
    inline Mat4 ProjectionMatrix(float horizontalFovDeg, float aspect, float nearZ) {
        const float t = std::tan(horizontalFovDeg * 0.5f * kPi / 180.f);
        return {{{1.f / t, 0, 0, 0}, {0, aspect / t, 0, 0}, {0, 0, 1, 1}, {0, 0, -nearZ, 0}}};
    }

    // World point -> pixel position in a viewport of the given size. False if the point is behind the camera.
    inline bool Project(const Mat4 &viewProj, Vec3 p, float width, float height, float &sx, float &sy) {
        const float cx = p.x * viewProj.m[0][0] + p.y * viewProj.m[1][0] + p.z * viewProj.m[2][0] + viewProj.m[3][0];
        const float cy = p.x * viewProj.m[0][1] + p.y * viewProj.m[1][1] + p.z * viewProj.m[2][1] + viewProj.m[3][1];
        const float cw = p.x * viewProj.m[0][3] + p.y * viewProj.m[1][3] + p.z * viewProj.m[2][3] + viewProj.m[3][3];
        if (cw <= 1e-4f) return false;
        sx = (cx / cw * 0.5f + 0.5f) * width;
        sy = (0.5f - cy / cw * 0.5f) * height;
        return true;
    }
} // namespace rmath
