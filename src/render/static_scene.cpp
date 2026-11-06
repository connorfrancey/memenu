#include "render/static_scene.h"

#include <algorithm>
#include <cmath>

namespace rstatic {

const char* CategoryName(Category c)
{
    switch (c) {
        case Category::Architecture: return "architecture";
        case Category::Movers:       return "movers";
        case Category::Props:        return "physics props";
        case Category::Hidden:       return "invisible collision";
        case Category::Other:        return "other meshes";
        case Category::Bounds:       return "no simple collision";
        default:                     return "?";
    }
}

bool IsLive(Category c) { return c == Category::Movers || c == Category::Props; }

std::size_t MeshGeom::Vertices() const
{
    std::size_t n = 0;
    for (const LocalShape& s : shapes) n += s.type == ShapeType::Hull ? s.idx.size() * 3 : 200;   // Lines + triangles
    return n;
}

// ---- Grid -------------------------------------------------------------------------------------------------------------
std::uint64_t Grid::Key(int x, int y, int z)
{
    const auto u = [](int v) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(v) & 0x1FFFFFu); };
    return (u(x) << 42) | (u(y) << 21) | u(z);
}

int Grid::Cell(float v) const { return static_cast<int>(std::floor(v / cell_)); }

void Grid::Build(const std::vector<Instance>& instances)
{
    cells_.clear();
    big_.clear();
    maxRadius_ = 0.f;
    for (std::uint32_t i = 0; i < instances.size(); ++i) {
        const Instance& in = instances[i];
        if (in.radius > cell_ * 2.f) {
            big_.push_back(i);
            continue;
        }
        cells_[Key(Cell(in.center.x), Cell(in.center.y), Cell(in.center.z))].push_back(i);
        maxRadius_ = std::max(maxRadius_, in.radius);
    }
}

void Grid::Query(rmath::Vec3 p, float range, std::vector<std::uint32_t>& out) const
{
    // A sphere within `range` of p has its center within range + its radius: the largest radius bounds the search
    // (huge ones, a whole floor, are kept apart so they don't widen every search).
    out.insert(out.end(), big_.begin(), big_.end());
    const float reach = range + maxRadius_;
    const int x0 = Cell(p.x - reach), x1 = Cell(p.x + reach);
    const int y0 = Cell(p.y - reach), y1 = Cell(p.y + reach);
    const int z0 = Cell(p.z - reach), z1 = Cell(p.z + reach);
    for (int x = x0; x <= x1; ++x)
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                if (const auto it = cells_.find(Key(x, y, z)); it != cells_.end())
                    out.insert(out.end(), it->second.begin(), it->second.end());
}

// ---- Build ------------------------------------------------------------------------------------------------------------
namespace {

void FadeFrom(std::vector<rmesh::Vertex>& v, std::size_t from, float alpha)
{
    for (std::size_t i = from; i < v.size(); ++i) {
        const auto a = static_cast<std::uint32_t>(static_cast<float>(v[i].color >> 24) * alpha + 0.5f);
        v[i].color = (v[i].color & 0x00FFFFFFu) | (std::min<std::uint32_t>(a, 255u) << 24);
    }
}

void Emit(rmesh::Builder& b, const World& world, const Instance& in, std::vector<rmath::Vec3>& scratch)
{
    if (in.mesh == kNoMesh || in.mesh >= world.meshes.size() || !world.meshes[in.mesh]) {
        b.Box(rmath::Translation(in.center), in.extent);
        return;
    }
    for (const LocalShape& s : world.meshes[in.mesh]->shapes) {
        switch (s.type) {
            case ShapeType::Box:     b.Box(rmath::Mul(s.local, in.world), {s.a, s.b, s.c}); break;
            case ShapeType::Sphere:  b.Sphere(rmath::Mul(s.local, in.world), s.a, 12); break;
            case ShapeType::Capsule: b.Capsule(rmath::Mul(s.local, in.world), s.a, s.b, 12); break;
            case ShapeType::Hull:
                scratch.clear();
                scratch.reserve(s.verts.size());
                for (const rmath::Vec3& v : s.verts) scratch.push_back(rmath::TransformPoint(in.world, v));
                b.Mesh(scratch, s.idx);
                break;
        }
    }
}

} // namespace

BuildResult Build(const World& world, const Grid& grid, const std::vector<Instance>& live, const BuildConfig& cfg)
{
    BuildResult r;
    struct Candidate {
        const Instance* in;
        float distance;     // From the camera to its bounding sphere (0 inside)
    };
    std::vector<Candidate> candidates;

    const auto consider = [&](const Instance& in) {
        if (!cfg.enabled[static_cast<std::size_t>(in.category)]) return;
        if (!in.colliding && !cfg.nonColliding) return;
        if (!cfg.filter.empty() && (in.name >= world.names.size() ||
                                    world.names[in.name].find(cfg.filter) == std::string::npos))
            return;
        const float d = std::max(0.f, rmath::Length(in.center - cfg.camera) - in.radius);
        if (d > cfg.maxDistance) return;
        candidates.push_back({&in, d});
    };

    std::vector<std::uint32_t> near;
    grid.Query(cfg.camera, cfg.maxDistance, near);
    for (std::uint32_t i : near)
        if (i < world.instances.size()) consider(world.instances[i]);
    for (const Instance& in : live) consider(in);
    r.inRange = candidates.size();

    // Nearest first: when the budget runs out, what is left out is the farthest
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });

    rmesh::Builder b(r.mesh);
    std::vector<rmath::Vec3> scratch;
    const float fadeFrom = cfg.maxDistance * 0.75f;
    for (const Candidate& c : candidates) {
        if (r.mesh.tris.size() + r.mesh.lines.size() >= cfg.vertexBudget) {
            r.budgetHit = true;
            break;
        }
        rmesh::Style style;
        style.color = cfg.colors[static_cast<std::size_t>(c.in->category)];
        style.fill = cfg.fill;
        style.outline = cfg.outline;
        b.SetStyle(style);
        const std::size_t tris = r.mesh.tris.size(), lines = r.mesh.lines.size();
        Emit(b, world, *c.in, scratch);
        if (cfg.fade && c.distance > fadeFrom) {
            const float alpha = std::clamp((cfg.maxDistance - c.distance) / (cfg.maxDistance - fadeFrom), 0.f, 1.f);
            FadeFrom(r.mesh.tris, tris, alpha);
            FadeFrom(r.mesh.lines, lines, alpha);
        }
        ++r.drawn[static_cast<std::size_t>(c.in->category)];
    }
    return r;
}

} // namespace rstatic
