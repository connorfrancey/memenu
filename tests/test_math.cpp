#include "render/math.h"

#include <gtest/gtest.h>

using namespace rmath;

TEST(Math, VectorBasics)
{
    const Vec3 a{1, 2, 3}, b{4, 5, 6};
    EXPECT_FLOAT_EQ(Dot(a, b), 32.f);
    const Vec3 c = Cross({1, 0, 0}, {0, 1, 0});
    EXPECT_FLOAT_EQ(c.x, 0.f);
    EXPECT_FLOAT_EQ(c.y, 0.f);
    EXPECT_FLOAT_EQ(c.z, 1.f);
    EXPECT_FLOAT_EQ(Length(Normalize({3, 4, 0})), 1.f);
    EXPECT_FLOAT_EQ(Length(Normalize({0, 0, 0})), 0.f);   // No NaN for a zero vector
}

TEST(Math, IdentityTranslationTranspose)
{
    const Mat4 t = Translation({10, 20, 30});
    const Vec3 p = TransformPoint(Mul(Identity(), t), {1, 1, 1});
    EXPECT_FLOAT_EQ(p.x, 11.f);
    EXPECT_FLOAT_EQ(p.y, 21.f);
    EXPECT_FLOAT_EQ(p.z, 31.f);

    const Mat4 tt = Transpose(Transpose(t));
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) EXPECT_EQ(tt.m[i][j], t.m[i][j]) << "at [" << i << "][" << j << "]";
}

TEST(Math, RotationMatrixRowsAreOrthonormal)
{
    const Mat4 r = RotationMatrix(4000, 12000, 1500);
    for (int i = 0; i < 3; ++i) {
        const Vec3 row{r.m[i][0], r.m[i][1], r.m[i][2]};
        EXPECT_NEAR(Length(row), 1.f, 1e-4) << "row " << i;
        for (int j = i + 1; j < 3; ++j)
            EXPECT_NEAR(Dot(row, {r.m[j][0], r.m[j][1], r.m[j][2]}), 0.f, 1e-4) << "rows " << i << " and " << j;
    }
}

TEST(Math, ProjectionPointAheadLandsMidScreen)
{
    Camera cam;
    cam.pos = {100, 200, 50};
    cam.yaw = 0;            // Looking down +X (Unreal forward)
    cam.fov = 90.f;
    cam.valid = true;

    const Mat4 vp = Mul(ViewMatrix(cam), ProjectionMatrix(cam.fov, 16.f / 9.f, 10.f));
    float x = 0, y = 0;
    ASSERT_TRUE(Project(vp, {1100, 200, 50}, 1920, 1080, x, y));
    EXPECT_NEAR(x, 960.f, 0.5);
    EXPECT_NEAR(y, 540.f, 0.5);

    // Something to the right (+Y in Unreal) shows up right of the centre, something above (+Z) above it.
    ASSERT_TRUE(Project(vp, {1100, 400, 50}, 1920, 1080, x, y));
    EXPECT_GT(x, 960.f);
    ASSERT_TRUE(Project(vp, {1100, 200, 250}, 1920, 1080, x, y));
    EXPECT_LT(y, 540.f);

    // Behind the camera: not drawn.
    EXPECT_FALSE(Project(vp, {-900, 200, 50}, 1920, 1080, x, y));
}
