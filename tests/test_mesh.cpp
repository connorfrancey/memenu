#include "render/mesh.h"

#include <catch2/catch_test_macros.hpp>

using namespace rmesh;

TEST_CASE("PackColor packs ARGB and clamps", "[mesh]")
{
    CHECK(PackColor(1.f, 0.f, 0.f) == 0xFFFF0000u);
    CHECK(PackColor(0.f, 1.f, 0.f, 0.f) == 0x0000FF00u);
    CHECK(PackColor(2.f, -1.f, 0.f, 1.f) == 0xFFFF0000u);
}

TEST_CASE("a box is 12 triangles and 12 outline edges", "[mesh]")
{
    MeshData data;
    Builder b(data);
    Style style;
    style.fill = true;
    style.outline = true;
    b.SetStyle(style);
    b.Box(rmath::Identity(), {10, 20, 30});

    CHECK(b.Shapes() == 1);
    CHECK(data.tris.size() == 12 * 3);
    CHECK(data.lines.size() == 12 * 2);    // Only the cube's edges: the diagonals of its faces are not creases
    for (const Vertex& v : data.tris) {
        CHECK((v.x == 10.f || v.x == -10.f));
        CHECK((v.y == 20.f || v.y == -20.f));
        CHECK((v.z == 30.f || v.z == -30.f));
    }
}

TEST_CASE("wireframe style has no faces, fill-only style has no lines", "[mesh]")
{
    MeshData wire, solid;
    Builder bw(wire), bs(solid);
    Style s;
    s.fill = false;
    bw.SetStyle(s);
    bw.Sphere(rmath::Identity(), 50.f);
    s.fill = true;
    s.outline = false;
    bs.SetStyle(s);
    bs.Sphere(rmath::Identity(), 50.f);

    CHECK(wire.tris.empty());
    CHECK_FALSE(wire.lines.empty());
    CHECK_FALSE(solid.tris.empty());
    CHECK(solid.lines.empty());
}

TEST_CASE("out-of-range indices are skipped and counted", "[mesh]")
{
    MeshData data;
    Builder b(data);
    b.Mesh({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 1, 2, 0, 1, 7});
    CHECK(data.tris.size() == 3);
    CHECK(b.GetCounters().invalidTriangles == 1);
}

TEST_CASE("lines keep their color", "[mesh]")
{
    MeshData data;
    Builder b(data);
    b.Line({0, 0, 0}, {1, 2, 3}, 0xFF123456u);
    REQUIRE(data.lines.size() == 2);
    CHECK(data.lines[1].x == 1.f);
    CHECK(data.lines[0].color == 0xFF123456u);
}
