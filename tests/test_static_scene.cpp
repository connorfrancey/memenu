// Static world geometry (render/static_scene.h): the grid, and what Build() draws for the camera.
#include "render/static_scene.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace rstatic;

namespace {

// A unit box mesh (half extent 50) and `n` instances of it in a row along X, `spacing` apart.
World Row(int n, float spacing, Category category = Category::Architecture)
{
    World w;
    auto box = std::make_shared<MeshGeom>();
    LocalShape s;
    s.type = ShapeType::Box;
    s.a = s.b = s.c = 50.f;
    box->shapes.push_back(s);
    box->name = "box";
    w.meshes.push_back(box);
    for (int i = 0; i < n; ++i) {
        Instance in;
        in.mesh = 0;
        in.category = category;
        in.world = rmath::Translation({i * spacing, 0.f, 0.f});
        in.center = {i * spacing, 0.f, 0.f};
        in.radius = 87.f;
        in.extent = {50.f, 50.f, 50.f};
        in.name = static_cast<std::uint32_t>(w.names.size());
        w.names.push_back(i % 2 ? "wall_glass" : "wall_concrete");
        w.instances.push_back(in);
    }
    return w;
}

BuildConfig AllOn()
{
    BuildConfig c;
    c.enabled.fill(true);
    c.colors.fill(0xFFFFFFFFu);
    c.fade = false;
    return c;
}

std::size_t Drawn(const BuildResult& r)
{
    std::size_t n = 0;
    for (std::size_t d : r.drawn) n += d;
    return n;
}

} // namespace

TEST(StaticScene, GridFindsWhatIsInRange)
{
    const World w = Row(50, 1000.f);
    Grid g(2048.f);
    g.Build(w.instances);
    std::vector<std::uint32_t> near;
    g.Query({0.f, 0.f, 0.f}, 3000.f, near);
    // Every instance within range is returned (a superset is fine)
    for (std::uint32_t i = 0; i < w.instances.size(); ++i)
        if (w.instances[i].center.x - w.instances[i].radius <= 3000.f)
            EXPECT_NE(std::find(near.begin(), near.end(), i), near.end()) << i;
    EXPECT_LT(near.size(), w.instances.size());     // ... and not everything
}

TEST(StaticScene, HugeInstancesAreAlwaysFound)
{
    World w = Row(1, 0.f);
    w.instances[0].center = {50000.f, 0.f, 0.f};
    w.instances[0].radius = 49500.f;                // Reaches back to the camera
    Grid g(2048.f);
    g.Build(w.instances);
    std::vector<std::uint32_t> near;
    g.Query({0.f, 0.f, 0.f}, 1000.f, near);
    EXPECT_EQ(near.size(), 1u);
}

TEST(StaticScene, RangeLayersAndFilters)
{
    World w = Row(20, 1000.f);
    w.instances[3].colliding = false;
    Grid g;
    g.Build(w.instances);

    BuildConfig c = AllOn();
    c.maxDistance = 4500.f;                         // Instances 0..4 (5 reaches 5000 - 87)
    BuildResult r = Build(w, g, {}, c);
    EXPECT_EQ(Drawn(r), 4u);                        // 0..4, without the non-colliding one
    EXPECT_FALSE(r.mesh.lines.empty());

    c.nonColliding = true;
    EXPECT_EQ(Drawn(Build(w, g, {}, c)), 5u);

    c.filter = "glass";                             // Odd ones: 1 and 3
    EXPECT_EQ(Drawn(Build(w, g, {}, c)), 2u);
    c.filter.clear();

    c.enabled[static_cast<std::size_t>(Category::Architecture)] = false;
    EXPECT_EQ(Drawn(Build(w, g, {}, c)), 0u);
}

TEST(StaticScene, LiveInstancesAndBounds)
{
    const World w = Row(0, 0.f);
    Grid g;
    g.Build(w.instances);
    Instance door;                                  // A mover with no simple collision: its bounding box
    door.mesh = kNoMesh;
    door.category = Category::Movers;
    door.center = {100.f, 0.f, 0.f};
    door.radius = 100.f;
    door.extent = {10.f, 80.f, 100.f};
    BuildConfig c = AllOn();
    const BuildResult r = Build(w, g, {door}, c);
    EXPECT_EQ(r.drawn[static_cast<std::size_t>(Category::Movers)], 1u);
    EXPECT_FALSE(r.mesh.lines.empty());
}

TEST(StaticScene, BudgetKeepsTheNearest)
{
    const World w = Row(40, 200.f);
    Grid g;
    g.Build(w.instances);
    BuildConfig c = AllOn();
    c.maxDistance = 100000.f;
    const BuildResult all = Build(w, g, {}, c);
    ASSERT_FALSE(all.budgetHit);
    const std::size_t perBox = (all.mesh.tris.size() + all.mesh.lines.size()) / 40;

    c.vertexBudget = perBox * 10;
    c.camera = {0.f, 0.f, 0.f};
    const BuildResult some = Build(w, g, {}, c);
    EXPECT_TRUE(some.budgetHit);
    EXPECT_EQ(Drawn(some), 10u);
    // The ones drawn are the nearest: every line vertex lies within the first ten boxes
    for (const auto& v : some.mesh.lines) EXPECT_LT(v.x, 10 * 200.f);
}

TEST(StaticScene, FadeLowersAlphaFarAway)
{
    const World w = Row(2, 2900.f);                  // At 0 and 2900; range 3000: the second is in the faded quarter
    Grid g;
    g.Build(w.instances);
    BuildConfig c = AllOn();
    c.fade = true;
    c.maxDistance = 3000.f;
    const BuildResult r = Build(w, g, {}, c);
    bool nearOpaque = false, farFaded = false;
    for (const auto& v : r.mesh.lines) {
        if (v.x < 100.f && (v.color >> 24) == 255) nearOpaque = true;
        if (v.x > 2700.f && (v.color >> 24) < 255) farFaded = true;
    }
    EXPECT_TRUE(nearOpaque);
    EXPECT_TRUE(farFaded);
}
