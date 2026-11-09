// Ray hits and containment for the collision view's inspector (render/geometry.h).
#include "render/geometry.h"

#include <gtest/gtest.h>

using namespace rgeom;
using rmath::Vec3;

TEST(Geometry, InverseAffine)
{
    // Scale 2, then move by (10, 0, 5)
    rmath::Mat4 m = rmath::Identity();
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 2.f;
    m.m[3][0] = 10.f;
    m.m[3][2] = 5.f;
    rmath::Mat4 inv;
    ASSERT_TRUE(InverseAffine(m, inv));
    const Vec3 p = rmath::TransformPoint(inv, rmath::TransformPoint(m, {1.f, 2.f, 3.f}));
    EXPECT_NEAR(p.x, 1.f, 1e-5f);
    EXPECT_NEAR(p.y, 2.f, 1e-5f);
    EXPECT_NEAR(p.z, 3.f, 1e-5f);

    rmath::Mat4 flat = rmath::Identity();
    flat.m[2][2] = 0.f;
    EXPECT_FALSE(InverseAffine(flat, inv));
}

TEST(Geometry, RayBoxAndSphere)
{
    EXPECT_FLOAT_EQ(RayBox({-10.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {2.f, 2.f, 2.f}), 8.f);
    EXPECT_LT(RayBox({-10.f, 5.f, 0.f}, {1.f, 0.f, 0.f}, {2.f, 2.f, 2.f}), 0.f);    // Passes above
    EXPECT_LT(RayBox({10.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {2.f, 2.f, 2.f}), 0.f);     // Behind
    EXPECT_FLOAT_EQ(RayBox({0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {2.f, 2.f, 2.f}), 0.f);    // From inside
    EXPECT_FLOAT_EQ(RaySphere({-10.f, 0.f, 0.f}, {2.f, 0.f, 0.f}, {}, 4.f), 3.f);      // t counts the direction's length
}

TEST(Geometry, RayCylinderAndCapsule)
{
    // From the side, and from above onto the cap
    EXPECT_FLOAT_EQ(RayCylinder({-10.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 2.f, 5.f), 8.f);
    EXPECT_FLOAT_EQ(RayCylinder({0.f, 0.f, 10.f}, {0.f, 0.f, -1.f}, 2.f, 5.f), 5.f);
    EXPECT_LT(RayCylinder({-10.f, 0.f, 6.f}, {1.f, 0.f, 0.f}, 2.f, 5.f), 0.f);     // Above it
    // The capsule's round end reaches higher than the cylinder
    EXPECT_FLOAT_EQ(RayCapsule({0.f, 0.f, 10.f}, {0.f, 0.f, -1.f}, 2.f, 5.f), 3.f);
}

TEST(Geometry, Containment)
{
    EXPECT_TRUE(InBox({1.f, -1.f, 1.9f}, {2.f, 2.f, 2.f}));
    EXPECT_FALSE(InBox({2.1f, 0.f, 0.f}, {2.f, 2.f, 2.f}));
    EXPECT_TRUE(InCylinder({1.f, 1.f, 4.f}, 2.f, 5.f));
    EXPECT_FALSE(InCylinder({2.f, 2.f, 0.f}, 2.f, 5.f));
    EXPECT_TRUE(InCapsule({0.f, 0.f, 6.5f}, 2.f, 5.f));
    EXPECT_FALSE(InCapsule({1.5f, 0.f, 6.5f}, 2.f, 5.f));
    EXPECT_TRUE(InSphere({1.f, 1.f, 1.f}, 2.f));
}

TEST(Geometry, ConvexHullOfACube)
{
    // A 2x2x2 cube with mixed winding (as hulls come out of the game)
    std::vector<Vec3> v;
    for (int i = 0; i < 8; ++i) v.push_back({(i & 1) ? 1.f : -1.f, (i & 2) ? 1.f : -1.f, (i & 4) ? 1.f : -1.f});
    const std::vector<int> idx = {0, 2, 6, 0, 6, 4, 1, 3, 7, 1, 7, 5, 0, 4, 5, 0, 5, 1,
                                  2, 3, 7, 2, 7, 6, 0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6};
    EXPECT_TRUE(InConvex({0.5f, -0.5f, 0.9f}, v, idx));
    EXPECT_FALSE(InConvex({1.5f, 0.f, 0.f}, v, idx));
    EXPECT_FALSE(InConvex({0.f, 0.f, -1.2f}, v, idx));
    EXPECT_FLOAT_EQ(RayTriangles({-5.f, 0.2f, 0.3f}, {1.f, 0.f, 0.f}, v, idx), 4.f);
    EXPECT_LT(RayTriangles({-5.f, 3.f, 0.f}, {1.f, 0.f, 0.f}, v, idx), 0.f);
}
