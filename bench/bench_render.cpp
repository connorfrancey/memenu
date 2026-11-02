// The debug renderer's CPU side: matrix math and building the collision view's meshes.
#include "bench.h"

#include "render/math.h"
#include "render/mesh.h"

using namespace rmath;
using namespace rmesh;
namespace nb = ankerl::nanobench;

MEMENU_BENCH(Math)
{
    Camera cam{};
    cam.pos = {100.f, 200.f, 300.f};
    cam.pitch = 1000;
    cam.yaw = 16384;
    cam.fov = 90.f;
    const Mat4 viewProj = Mul(ViewMatrix(cam), ProjectionMatrix(90.f, 16.f / 9.f, 10.f));

    bench.unit("op");
    bench.run("Mat4 Mul", [&] { nb::doNotOptimizeAway(Mul(viewProj, viewProj)); });
    bench.run("RotationMatrix", [&] { nb::doNotOptimizeAway(RotationMatrix(1000, 16384, 0)); });
    float sx = 0.f, sy = 0.f;
    Vec3 p{500.f, 250.f, 125.f};
    bench.run("Project a point", [&] {
        p.x += 0.001f;
        nb::doNotOptimizeAway(Project(viewProj, p, 1920.f, 1080.f, sx, sy));
    });
}

MEMENU_BENCH(Mesh)
{
    Style style;
    style.fill = true;
    style.outline = true;
    const Mat4 world = Translation({10.f, 20.f, 30.f});
    MeshData data;

    // A collision view frame: many shapes, the buffers kept between rebuilds like the renderer does
    auto shapes = [&](int count, auto&& add) {
        data.tris.clear();
        data.lines.clear();
        Builder b(data);
        b.SetStyle(style);
        for (int i = 0; i < count; ++i) add(b);
        nb::doNotOptimizeAway(data.tris.size());
    };

    bench.batch(100).unit("shape");
    bench.run("Box x100", [&] { shapes(100, [&](Builder& b) { b.Box(world, {50.f, 50.f, 50.f}); }); });
    bench.run("Sphere x100 (16 segments)", [&] { shapes(100, [&](Builder& b) { b.Sphere(world, 50.f); }); });
    bench.run("Cylinder x100 (16 segments)", [&] { shapes(100, [&](Builder& b) { b.Cylinder(world, 30.f, 60.f); }); });
    bench.run("Capsule x100 (16 segments)", [&] { shapes(100, [&](Builder& b) { b.Capsule(world, 34.f, 54.f); }); });
}
