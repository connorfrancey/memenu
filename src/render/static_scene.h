#pragma once
#include "math.h"
#include "mesh.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Static world geometry for the collision view: the collision of static meshes (level architecture, movers, physics
// props, invisible walls...) as an immutable snapshot, and the code that turns the part of it near the camera into
// triangles and lines. No game and no Direct3D here: the game thread fills a World (game/static_world.cpp), a worker
// thread calls Build() on it, so a big level never costs the game a frame. Unit tested.
namespace rstatic {
    // Shown and colored separately. The order is the collision view's.
    enum class Category : std::uint8_t {
        Architecture,   // StaticMeshActor: walls, floors, roofs, rails
        Movers,         // InterpActor: doors, elevators, moving platforms (drawn where they are now)
        Props,          // KActor and other dynamic static-mesh actors: physics objects (drawn where they are now)
        Hidden,         // Collision you can't see: hidden meshes that still block (invisible walls)
        Other,          // Static meshes of any other actor
        Bounds,         // Meshes without simple collision (they collide per polygon): their bounding box only
        Count
    };
    constexpr int kCategoryCount = static_cast<int>(Category::Count);
    const char* CategoryName(Category c);
    bool IsLive(Category c);    // Movers and props move: the game thread sends where they are with every build

    enum class ShapeType : std::uint8_t { Box, Sphere, Capsule, Hull };

    // One element of a mesh's simple collision, in the mesh's own space.
    struct LocalShape {
        ShapeType type = ShapeType::Box;
        rmath::Mat4 local = rmath::Identity();  // Box/Sphere/Capsule: element -> mesh
        float a = 0.f, b = 0.f, c = 0.f;        // Box: half extents. Sphere: a=radius. Capsule: a=radius, b=half length
        std::vector<rmath::Vec3> verts;         // Hull: mesh space
        std::vector<int> idx;
    };

    // A mesh's collision, shared by every place it is used.
    struct MeshGeom {
        std::string name;
        std::vector<LocalShape> shapes;
        std::size_t Vertices() const;           // Of its hulls, for the budget estimate
    };

    constexpr std::uint32_t kNoMesh = 0xFFFFFFFFu;

    // A placed mesh.
    struct Instance {
        std::uint32_t mesh = kNoMesh;           // Into World::meshes; kNoMesh: draw the bounding box (Category::Bounds)
        Category category = Category::Other;
        bool colliding = true;                  // Blocks the player (component and owner collide and block)
        rmath::Mat4 world = rmath::Identity();  // Mesh -> world
        rmath::Vec3 center;                     // Bounding sphere (world)
        float radius = 0.f;
        rmath::Vec3 extent;                     // Bounding box half extents around `center` (world axes)
        std::uint32_t name = 0;                 // Into World::names (lower case: owner and mesh name)
    };

    struct World {
        std::vector<std::shared_ptr<const MeshGeom>> meshes;
        std::vector<Instance> instances;        // Static ones; live ones come with each build
        std::vector<std::string> names;
    };

    // Uniform grid over instance centers: the instances near a point without looking at all of them.
    class Grid {
    public:
        explicit Grid(float cell = 2048.f) : cell_(cell) {}
        void Build(const std::vector<Instance>& instances);
        // Instances whose bounding sphere may come within `range` of `p` (a superset: test them after).
        void Query(rmath::Vec3 p, float range, std::vector<std::uint32_t>& out) const;
        float MaxRadius() const { return maxRadius_; }

    private:
        static std::uint64_t Key(int x, int y, int z);
        int Cell(float v) const;
        float cell_;
        float maxRadius_ = 0.f;
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells_;
        std::vector<std::uint32_t> big_;        // Bounding spheres larger than a few cells: always returned
    };

    struct BuildConfig {
        rmath::Vec3 camera;
        float maxDistance = 3000.f;
        bool fade = true;                       // The last quarter of the range fades out
        bool fill = false, outline = true;
        bool nonColliding = false;              // Also meshes the player passes through
        std::string filter;                     // Lower case; only instances whose name contains it
        std::size_t vertexBudget = 400000;      // Triangle + line vertices; the nearest instances first
        std::array<bool, kCategoryCount> enabled{};
        std::array<std::uint32_t, kCategoryCount> colors{};    // 0xAARRGGBB
    };

    struct BuildResult {
        rmesh::MeshData mesh;
        std::array<std::size_t, kCategoryCount> drawn{};       // Instances drawn per category
        std::size_t inRange = 0;                // Passed the filters and the range test
        bool budgetHit = false;                 // Some in range were left out (the farthest)
    };

    // The geometry near the camera: static instances found through `grid` (built from world.instances) and `live` ones.
    BuildResult Build(const World& world, const Grid& grid, const std::vector<Instance>& live, const BuildConfig& cfg);
}
