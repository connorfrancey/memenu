// The debug renderer's CPU side: matrix math and building the collision view's meshes.
#include "bench.h"

#include "render/math.h"
#include "render/mesh.h"

using namespace rmath;
using namespace rmesh;

namespace {

Mat4 ViewProj()
{
    Camera cam{};
    cam.pos = {100.f, 200.f, 300.f};
    cam.pitch = 1000;
    cam.yaw = 16384;
    cam.fov = 90.f;
    return Mul(ViewMatrix(cam), ProjectionMatrix(90.f, 16.f / 9.f, 10.f));
}

void MatMul(benchmark::State& state)
{
    const Mat4 viewProj = ViewProj();
    for (auto _ : state) bench::Keep(Mul(viewProj, viewProj));
}
BENCHMARK(MatMul)->Name("Math/Mat4 Mul");

void Rotation(benchmark::State& state)
{
    for (auto _ : state) bench::Keep(RotationMatrix(1000, 16384, 0));
}
BENCHMARK(Rotation)->Name("Math/RotationMatrix");

void ProjectPoint(benchmark::State& state)
{
    const Mat4 viewProj = ViewProj();
    float sx = 0.f, sy = 0.f;
    Vec3 p{500.f, 250.f, 125.f};
    for (auto _ : state) {
        p.x += 0.001f;
        bench::Keep(Project(viewProj, p, 1920.f, 1080.f, sx, sy));
    }
}
BENCHMARK(ProjectPoint)->Name("Math/Project a point");

// A collision view frame: 100 shapes, the buffers kept between rebuilds like the renderer does
template <class Add> void Shapes(benchmark::State& state, Add add)
{
    Style style;
    style.fill = true;
    style.outline = true;
    const Mat4 world = Translation({10.f, 20.f, 30.f});
    MeshData data;
    for (auto _ : state) {
        data.tris.clear();
        data.lines.clear();
        Builder b(data);
        b.SetStyle(style);
        for (int i = 0; i < 100; ++i) add(b, world);
        bench::Keep(data.tris.size());
    }
    bench::PerItem(state, 100, "shape");
}

void Boxes(benchmark::State& s) { Shapes(s, [](Builder& b, const Mat4& w) { b.Box(w, {50.f, 50.f, 50.f}); }); }
void Spheres(benchmark::State& s) { Shapes(s, [](Builder& b, const Mat4& w) { b.Sphere(w, 50.f); }); }
void Cylinders(benchmark::State& s) { Shapes(s, [](Builder& b, const Mat4& w) { b.Cylinder(w, 30.f, 60.f); }); }
void Capsules(benchmark::State& s) { Shapes(s, [](Builder& b, const Mat4& w) { b.Capsule(w, 34.f, 54.f); }); }
BENCHMARK(Boxes)->Name("Mesh/Box x100")->Unit(benchmark::kMicrosecond);
BENCHMARK(Spheres)->Name("Mesh/Sphere x100 (16 segments)")->Unit(benchmark::kMicrosecond);
BENCHMARK(Cylinders)->Name("Mesh/Cylinder x100 (16 segments)")->Unit(benchmark::kMicrosecond);
BENCHMARK(Capsules)->Name("Mesh/Capsule x100 (16 segments)")->Unit(benchmark::kMicrosecond);

} // namespace
