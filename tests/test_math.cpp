#include "render/math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using namespace rmath;

TEST_CASE("vector basics", "[math]")
{
    const Vec3 a{1, 2, 3}, b{4, 5, 6};
    CHECK(Dot(a, b) == Approx(32.f));
    const Vec3 c = Cross({1, 0, 0}, {0, 1, 0});
    CHECK(c.x == Approx(0.f));
    CHECK(c.y == Approx(0.f));
    CHECK(c.z == Approx(1.f));
    CHECK(Length(Normalize({3, 4, 0})) == Approx(1.f));
    CHECK(Length(Normalize({0, 0, 0})) == Approx(0.f));   // No NaN for a zero vector
}

TEST_CASE("matrices: identity, translation, transpose", "[math]")
{
    const Mat4 t = Translation({10, 20, 30});
    const Vec3 p = TransformPoint(Mul(Identity(), t), {1, 1, 1});
    CHECK(p.x == Approx(11.f));
    CHECK(p.y == Approx(21.f));
    CHECK(p.z == Approx(31.f));

    const Mat4 tt = Transpose(Transpose(t));
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) CHECK(tt.m[i][j] == t.m[i][j]);
}

TEST_CASE("rotation matrix rows are orthonormal", "[math]")
{
    const Mat4 r = RotationMatrix(4000, 12000, 1500);
    for (int i = 0; i < 3; ++i) {
        const Vec3 row{r.m[i][0], r.m[i][1], r.m[i][2]};
        CHECK(Length(row) == Approx(1.f).margin(1e-4));
        for (int j = i + 1; j < 3; ++j)
            CHECK(Dot(row, {r.m[j][0], r.m[j][1], r.m[j][2]}) == Approx(0.f).margin(1e-4));
    }
}

TEST_CASE("projection: a point straight ahead lands in the middle of the screen", "[math]")
{
    Camera cam;
    cam.pos = {100, 200, 50};
    cam.yaw = 0;            // Looking down +X (Unreal forward)
    cam.fov = 90.f;
    cam.valid = true;

    const Mat4 vp = Mul(ViewMatrix(cam), ProjectionMatrix(cam.fov, 16.f / 9.f, 10.f));
    float x = 0, y = 0;
    REQUIRE(Project(vp, {1100, 200, 50}, 1920, 1080, x, y));
    CHECK(x == Approx(960.f).margin(0.5));
    CHECK(y == Approx(540.f).margin(0.5));

    // Something to the right (+Y in Unreal) shows up right of the centre, something above (+Z) above it.
    REQUIRE(Project(vp, {1100, 400, 50}, 1920, 1080, x, y));
    CHECK(x > 960.f);
    REQUIRE(Project(vp, {1100, 200, 250}, 1920, 1080, x, y));
    CHECK(y < 540.f);

    // Behind the camera: not drawn.
    CHECK_FALSE(Project(vp, {-900, 200, 50}, 1920, 1080, x, y));
}
