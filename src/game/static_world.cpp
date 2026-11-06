#include "game/static_world.h"
#include "game/game.h"
#include "game/uobject.h"
#include "render/debug_renderer.h"

#include <ME_SDK.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace staticworld {
namespace {

using Clock = std::chrono::steady_clock;
using rstatic::Category;

// Each tick reads the level for at most this long, wherever it gets to (a slice by count could still be slow when it
// meets many new classes or big meshes at once).
constexpr double kSliceMs = 1.0;
// Another pass when the object list grew or shrank by a streamed-in part's worth (particles and the like change the count
// all the time), or after a while if it changed at all.
constexpr double kRescanAfter = 3.0;
constexpr std::size_t kRescanObjects = 200;
constexpr double kRescanAnyway = 15.0;
constexpr std::size_t kMaxElems = 512, kMaxHullVerts = 4096, kMaxHullIndices = 3 * 8192;

// ---- What a class is (by its class chain's names: the SDK's StaticClass() lookups can fail early, and its IsA() only
// matches the exact class) ------------------------------------------------------------------------------------------------
enum class ObjKind : std::uint8_t { None, StaticMesh, BodySetup, MeshComponent };

// The classes a class is or derives from, among the ones that matter here, as bits. Built from the parent's bits plus
// the class's own name: every class name is read once (a name read is a string conversion: thousands per pass add up).
enum ClassBit : std::uint32_t {
    kStaticMeshComponent = 1u << 0, kStaticMesh = 1u << 1, kBodySetup = 1u << 2,
    kInterpActor = 1u << 3, kDynamicSMActor = 1u << 4, kStaticMeshActorBase = 1u << 5,
    kSkipOwner = 1u << 6,       // Pawns, weapons, effects: not world geometry
};

std::unordered_map<const Classes::UStruct*, std::uint32_t> g_classBits;

std::uint32_t BitsOf(const Classes::UStruct* cls, int depth = 0)
{
    if (!cls || depth > 64) return 0;
    if (const auto it = g_classBits.find(cls); it != g_classBits.end()) return it->second;
    std::uint32_t bits = BitsOf(game::SuperOf(cls), depth + 1);
    const std::string name = cls->GetName();
    static const std::pair<const char*, std::uint32_t> kNames[] = {
        {"StaticMeshComponent", kStaticMeshComponent}, {"StaticMesh", kStaticMesh}, {"RB_BodySetup", kBodySetup},
        {"InterpActor", kInterpActor}, {"DynamicSMActor", kDynamicSMActor}, {"StaticMeshActorBase", kStaticMeshActorBase},
        {"Pawn", kSkipOwner}, {"Inventory", kSkipOwner}, {"Projectile", kSkipOwner}, {"Emitter", kSkipOwner},
        {"DroppedPickup", kSkipOwner}, {"HUD", kSkipOwner}, {"Controller", kSkipOwner},
    };
    for (const auto& [n, b] : kNames)
        if (name == n) bits |= b;
    g_classBits.emplace(cls, bits);
    return bits;
}

ObjKind KindOf(const Classes::UObject* o)
{
    const std::uint32_t bits = BitsOf(o->Class);
    if (bits & kStaticMeshComponent) return ObjKind::MeshComponent;
    if (bits & kStaticMesh) return ObjKind::StaticMesh;
    if (bits & kBodySetup) return ObjKind::BodySetup;
    return ObjKind::None;
}

struct OwnerKind {
    bool skip = false;
    Category category = Category::Other;
};

OwnerKind OwnerOf(const Classes::AActor* a)
{
    const std::uint32_t bits = BitsOf(a->Class);
    OwnerKind k;
    k.skip = (bits & kSkipOwner) != 0;
    if (bits & kInterpActor) k.category = Category::Movers;
    else if (bits & kDynamicSMActor) k.category = Category::Props;     // KActor and the like
    else if (bits & kStaticMeshActorBase) k.category = Category::Architecture;
    return k;
}

// ---- Conversion ----------------------------------------------------------------------------------------------------------
rmath::Mat4 ToMat4(const Classes::FMatrix& m)
{
    return {{{m.XPlane.X, m.XPlane.Y, m.XPlane.Z, m.XPlane.W},
             {m.YPlane.X, m.YPlane.Y, m.YPlane.Z, m.YPlane.W},
             {m.ZPlane.X, m.ZPlane.Y, m.ZPlane.Z, m.ZPlane.W},
             {m.WPlane.X, m.WPlane.Y, m.WPlane.Z, m.WPlane.W}}};
}

bool Finite(const rmath::Mat4& m)
{
    for (const auto& row : m.m)
        for (float f : row)
            if (!std::isfinite(f)) return false;
    return true;
}

rmath::Vec3 ToVec3(const Classes::FVector& v) { return {v.X, v.Y, v.Z}; }

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- State (game thread) ----------------------------------------------------------------------------------------------------
struct LiveRef {
    Classes::UPrimitiveComponent* component = nullptr;
    std::size_t index = 0;          // In the object list (game::AliveAt before every read)
    rstatic::Instance instance;     // Its mesh, category, name: the placement is read fresh
};

struct Pending {
    Classes::UObject* component;
    std::size_t index;
};

enum class Phase { Objects, Components, Idle };

std::string g_map;
Phase g_phase = Phase::Idle;
std::size_t g_at = 0;                                       // Next object (pass 1) or pending component (pass 2)
std::size_t g_objectsAtPass = 0;                            // Object list size when the last pass started
Clock::time_point g_passEnded{};
std::unordered_set<const void*> g_meshObjects, g_bodyObjects;   // Real StaticMesh / RB_BodySetup objects (this pass)
std::vector<Pending> g_pending;                             // Mesh components not read yet
std::unordered_set<std::uint64_t> g_seen;                   // Components already read (address and object slot)

std::uint64_t SeenKey(const void* component, std::size_t index)
{
    return (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(component)) << 32) ^ index;
}
std::unordered_map<const void*, std::uint32_t> g_meshIds;   // StaticMesh -> World::meshes index (or kNoMesh)
rstatic::World g_world;                                     // Grows during passes; published as snapshots
std::vector<LiveRef> g_live;
std::size_t g_publishedInstances = ~std::size_t{0};
std::shared_ptr<const rstatic::World> g_snapshot;
std::array<std::size_t, rstatic::kCategoryCount> g_found{};

// What the last build was asked for
std::shared_ptr<const rstatic::World> g_sentSnapshot;
rmath::Vec3 g_sentCamera;
bool g_sentAny = false;
Config g_sentConfig;
Clock::time_point g_sentAt{};

std::mutex g_infoMutex;
Info g_info;

// ---- Worker -----------------------------------------------------------------------------------------------------------------
struct Job {
    std::shared_ptr<const rstatic::World> world;
    std::vector<rstatic::Instance> live;
    rstatic::BuildConfig config;
    std::uint64_t generation = 0;
};

std::mutex g_workerMutex;           // Starting and stopping the thread
// Never destroyed: a joinable std::thread destroyed at process exit (the game closing without unloading the menu) would
// end the process with std::terminate. Windows ends the thread itself then.
std::thread& g_worker = *new std::thread();
std::mutex g_jobMutex;
std::condition_variable g_jobCv;
std::optional<Job> g_job;
bool g_busy = false, g_quit = false;
std::atomic<std::uint64_t> g_generation{1};     // Bumped when the drawing must go (new level, stopped): late results dropped

void WorkerMain()
{
    std::shared_ptr<const rstatic::World> gridFor;
    rstatic::Grid grid;
    for (;;) {
        Job job;
        {
            std::unique_lock lock(g_jobMutex);
            g_jobCv.wait(lock, [] { return g_quit || g_job.has_value(); });
            if (g_quit) return;
            job = std::move(*g_job);
            g_job.reset();
            g_busy = true;
        }
        const auto started = Clock::now();
        if (job.world != gridFor) {
            grid.Build(job.world->instances);
            gridFor = job.world;
        }
        rstatic::BuildResult r = rstatic::Build(*job.world, grid, job.live, job.config);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        const std::size_t vertices = r.mesh.tris.size() + r.mesh.lines.size();
        if (job.generation == g_generation.load()) {
            debugrender::SetStatic(std::make_shared<const rmesh::MeshData>(std::move(r.mesh)));
            std::lock_guard lock(g_infoMutex);
            g_info.drawn = r.drawn;
            g_info.vertices = vertices;
            g_info.buildMs = ms;
            g_info.budgetHit = r.budgetHit;
        }
        {
            std::lock_guard lock(g_jobMutex);
            g_busy = false;
        }
    }
}

void EnsureWorker()
{
    std::lock_guard lock(g_workerMutex);
    if (g_worker.joinable()) return;
    {
        std::lock_guard jl(g_jobMutex);
        g_quit = false;
        g_busy = false;
        g_job.reset();
    }
    g_worker = std::thread(WorkerMain);
}

void StopWorker()
{
    std::lock_guard lock(g_workerMutex);
    if (!g_worker.joinable()) return;
    {
        std::lock_guard jl(g_jobMutex);
        g_quit = true;
        g_job.reset();
    }
    g_jobCv.notify_all();
    g_worker.join();
}

// ---- Reading the game ----------------------------------------------------------------------------------------------------------
// A mesh's simple collision (once per mesh). kNoMesh when it has none we can read.
std::uint32_t MeshId(Classes::UStaticMesh* mesh)
{
    if (const auto it = g_meshIds.find(mesh); it != g_meshIds.end()) return it->second;
    std::uint32_t id = rstatic::kNoMesh;
    Classes::URB_BodySetup* body = mesh->BodySetup;
    if (body && g_bodyObjects.count(body)) {
        const Classes::FKAggregateGeom& g = body->AggGeom;
        auto geom = std::make_shared<rstatic::MeshGeom>();
        geom->name = mesh->GetName();
        const auto sane = [](std::size_t n) { return n <= kMaxElems; };
        if (sane(g.BoxElems.Num()) && sane(g.SphereElems.Num()) && sane(g.SphylElems.Num()) && sane(g.ConvexElems.Num())) {
            for (std::size_t i = 0; i < g.BoxElems.Num(); ++i) {
                const auto& e = g.BoxElems[i];
                rstatic::LocalShape s;
                s.type = rstatic::ShapeType::Box;
                s.local = ToMat4(e.TM);
                s.a = e.X * 0.5f; s.b = e.Y * 0.5f; s.c = e.Z * 0.5f;
                if (Finite(s.local)) geom->shapes.push_back(std::move(s));
            }
            for (std::size_t i = 0; i < g.SphereElems.Num(); ++i) {
                const auto& e = g.SphereElems[i];
                rstatic::LocalShape s;
                s.type = rstatic::ShapeType::Sphere;
                s.local = ToMat4(e.TM);
                s.a = e.Radius;
                if (Finite(s.local)) geom->shapes.push_back(std::move(s));
            }
            for (std::size_t i = 0; i < g.SphylElems.Num(); ++i) {
                const auto& e = g.SphylElems[i];
                rstatic::LocalShape s;
                s.type = rstatic::ShapeType::Capsule;
                s.local = ToMat4(e.TM);
                s.a = e.Radius; s.b = e.Length * 0.5f;
                if (Finite(s.local)) geom->shapes.push_back(std::move(s));
            }
            for (std::size_t i = 0; i < g.ConvexElems.Num(); ++i) {
                const auto& e = g.ConvexElems[i];
                const std::size_t nv = e.VertexData.Num(), ni = e.FaceTriData.Num();
                if (nv < 4 || nv > kMaxHullVerts || ni < 3 || ni > kMaxHullIndices || ni % 3) continue;
                rstatic::LocalShape s;
                s.type = rstatic::ShapeType::Hull;
                s.verts.reserve(nv);
                for (std::size_t v = 0; v < nv; ++v) s.verts.push_back(ToVec3(e.VertexData[v]));
                s.idx.reserve(ni);
                for (std::size_t t = 0; t < ni; ++t) s.idx.push_back(e.FaceTriData[t]);
                geom->shapes.push_back(std::move(s));
            }
        }
        if (!geom->shapes.empty()) {
            id = static_cast<std::uint32_t>(g_world.meshes.size());
            g_world.meshes.push_back(std::move(geom));
        }
    }
    g_meshIds.emplace(mesh, id);
    return id;
}

void ReadComponent(const Pending& p)
{
    if (!game::AliveAt(p.component, p.index)) return;
    auto* pc = static_cast<Classes::UStaticMeshComponent*>(p.component);
    Classes::AActor* owner = pc->Owner;
    if (!owner || !owner->Class || owner->bDeleteMe) return;     // Templates and class defaults have no owner
    const OwnerKind ok = OwnerOf(owner);
    if (ok.skip) return;

    const rmath::Mat4 world = ToMat4(pc->LocalToWorld);
    const rmath::Vec3 center = ToVec3(pc->Bounds.Origin);
    const float radius = pc->Bounds.SphereRadius;
    if (!Finite(world) || !std::isfinite(radius) || radius <= 0.f || radius > 200000.f) return;   // Not placed

    const bool colliding = pc->CollideActors && pc->BlockActors && owner->bCollideActors;
    const bool hidden = pc->HiddenGame || owner->bHidden;
    if (hidden && !colliding) return;       // Neither seen nor touched

    rstatic::Instance in;
    in.world = world;
    in.center = center;
    in.radius = radius;
    in.extent = ToVec3(pc->Bounds.BoxExtent);
    in.colliding = colliding;
    Classes::UStaticMesh* mesh = pc->StaticMesh;
    in.mesh = mesh && g_meshObjects.count(mesh) ? MeshId(mesh) : rstatic::kNoMesh;
    in.category = in.mesh == rstatic::kNoMesh ? Category::Bounds : hidden ? Category::Hidden : ok.category;
    const bool live = rstatic::IsLive(ok.category);     // Moves: placed fresh for every build, whatever its layer
    const std::string meshName = in.mesh != rstatic::kNoMesh ? g_world.meshes[in.mesh]->name : std::string();
    in.name = static_cast<std::uint32_t>(g_world.names.size());
    g_world.names.push_back(Lower(owner->GetName() + " " + meshName));
    ++g_found[static_cast<std::size_t>(in.category)];

    if (live) g_live.push_back({static_cast<Classes::UPrimitiveComponent*>(p.component), p.index, in});
    else g_world.instances.push_back(in);
}

void Publish()
{
    if (g_world.instances.size() == g_publishedInstances && g_snapshot) return;
    g_snapshot = std::make_shared<const rstatic::World>(g_world);
    g_publishedInstances = g_world.instances.size();
}

// A slice of the current pass.
void ScanTick()
{
    const auto& objects = Classes::UObject::GetGlobalObjects();
    const std::size_t n = objects.Num();
    if (g_phase == Phase::Idle) {
        const std::size_t diff = n > g_objectsAtPass ? n - g_objectsAtPass : g_objectsAtPass - n;
        const double since = std::chrono::duration<double>(Clock::now() - g_passEnded).count();
        if (diff == 0 || since < kRescanAfter || (diff < kRescanObjects && since < kRescanAnyway)) return;
        g_phase = Phase::Objects;
        g_at = 0;
        g_objectsAtPass = n;
        g_meshObjects.clear();
        g_bodyObjects.clear();
        g_pending.clear();
    }
    const auto started = Clock::now();
    const auto overBudget = [&] {
        return std::chrono::duration<double, std::milli>(Clock::now() - started).count() > kSliceMs;
    };
    if (g_phase == Phase::Objects) {
        std::size_t i = g_at;
        for (; i < n; ++i) {
            if ((i & 255) == 0 && i != g_at && overBudget()) break;
            Classes::UObject* o = objects.GetByIndex(i);
            if (!o || !o->Class) continue;
            switch (KindOf(o)) {
                case ObjKind::StaticMesh:    g_meshObjects.insert(o); break;
                case ObjKind::BodySetup:     g_bodyObjects.insert(o); break;
                case ObjKind::MeshComponent: if (!g_seen.count(SeenKey(o, i))) g_pending.push_back({o, i}); break;
                default: break;
            }
        }
        g_at = i;
        if (g_at >= n) {
            g_phase = Phase::Components;
            g_at = 0;
        }
        return;
    }
    // Components: the meshes and body setups they point to are known by now, so a pointer can be checked before use
    std::size_t i = g_at;
    for (; i < g_pending.size(); ++i) {
        if ((i & 15) == 0 && i != g_at && overBudget()) break;
        g_seen.insert(SeenKey(g_pending[i].component, g_pending[i].index));
        ReadComponent(g_pending[i]);
    }
    g_at = i;
    if (g_at >= g_pending.size()) {
        spdlog::info("Static world: pass over '{}' done: {} meshes, {} placed, {} moving ({} components read)", g_map,
                     g_world.meshes.size(), g_world.instances.size(), g_live.size(), g_pending.size());
        g_pending.clear();
        g_phase = Phase::Idle;
        g_passEnded = Clock::now();
        Publish();
    } else if (g_world.instances.size() >= g_publishedInstances + 3000 || !g_snapshot) {
        Publish();      // Show progress on a big level
    }
}

void Reset(const std::string& map)
{
    g_generation.fetch_add(1);
    debugrender::SetStatic(nullptr);
    g_map = map;
    g_phase = Phase::Idle;
    g_at = 0;
    g_objectsAtPass = ~std::size_t{0};      // Starts a pass at once
    g_passEnded = {};
    g_meshObjects.clear();
    g_bodyObjects.clear();
    g_pending.clear();
    g_seen.clear();
    g_meshIds.clear();
    g_world = {};
    g_live.clear();
    g_snapshot.reset();
    g_publishedInstances = ~std::size_t{0};
    g_found = {};
    g_sentSnapshot.reset();
    g_sentAny = false;
    std::lock_guard lock(g_infoMutex);
    g_info.tickMaxMs = 0.0;
}

bool SameBuild(const Config& a, const Config& b)
{
    return a.enabled == b.enabled && a.colors == b.colors && a.nonColliding == b.nonColliding && a.fill == b.fill &&
           a.outline == b.outline && a.maxDistance == b.maxDistance && a.fade == b.fade && a.filter == b.filter &&
           a.vertexBudget == b.vertexBudget;
}

void PublishInfo()
{
    std::lock_guard lock(g_infoMutex);
    g_info.map = g_map;
    g_info.scanning = g_phase != Phase::Idle;
    const auto n = Classes::UObject::GetGlobalObjects().Num();
    g_info.scanProgress = g_phase == Phase::Objects ? 0.5f * static_cast<float>(g_at) / static_cast<float>(std::max<std::size_t>(n, 1))
                        : g_phase == Phase::Components
                            ? 0.5f + 0.5f * static_cast<float>(g_at) / static_cast<float>(std::max<std::size_t>(g_pending.size(), 1))
                            : 1.f;
    g_info.meshes = g_world.meshes.size();
    g_info.found = g_found;
}

} // namespace

namespace { void UpdateNow(const Config& cfg); }

void Update(const Config& cfg)
{
    const auto started = Clock::now();
    UpdateNow(cfg);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    std::lock_guard lock(g_infoMutex);
    g_info.tickMs = ms;
    g_info.tickMaxMs = std::max(g_info.tickMaxMs, ms);
    g_info.tickAvgMs = g_info.tickAvgMs * 0.97 + ms * 0.03;
}

namespace {
void UpdateNow(const Config& cfg)
{
    if (!game::Ready() || game::IsLoading()) return;
    const std::string map = Lower(game::MapName());
    if (map.empty()) return;
    if (map != g_map) {
        spdlog::info("Static world: level '{}'", map);
        Reset(map);
    }
    EnsureWorker();
    const auto t0 = Clock::now();
    const Phase phaseWas = g_phase;
    ScanTick();
    const auto t1 = Clock::now();
    PublishInfo();
    struct SlowLog {
        Clock::time_point t0, t1;
        Phase phase;
        ~SlowLog()
        {
            const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            const double scan = ms(t0, t1), rest = ms(t1, Clock::now());
            if (scan + rest > 2.0)
                spdlog::debug("Static world: slow tick: scan {:.2f} ms (phase {}), the rest {:.2f} ms", scan,
                              static_cast<int>(phase), rest);
        }
    } slowLog{t0, t1, phaseWas};

    const rmath::Camera cam = game::GetCamera();
    if (!cam.valid || !g_snapshot) return;

    // Rebuild only when it would look different: a setting, new geometry, the camera far enough from the last build,
    // or movers/props in view that may have moved.
    bool liveShown = false;     // A moving one is in a layer that is shown
    for (const LiveRef& r : g_live)
        if (cfg.enabled[static_cast<std::size_t>(r.instance.category)]) { liveShown = true; break; }
    const auto now = Clock::now();
    const float moveStep = std::max(150.f, cfg.maxDistance * 0.1f);
    const bool due = !g_sentAny || !SameBuild(cfg, g_sentConfig) || g_snapshot != g_sentSnapshot ||
                     rmath::Length(cam.pos - g_sentCamera) > moveStep ||
                     (liveShown &&
                      std::chrono::duration<double>(now - g_sentAt).count() >= 1.0 / std::clamp(cfg.liveHz, 1.f, 30.f));
    if (!due) return;
    {
        std::lock_guard lock(g_jobMutex);
        if (g_busy || g_job) return;        // One at a time: the next tick asks again with the newest state
    }

    Job job;
    job.world = g_snapshot;
    job.generation = g_generation.load();
    job.live.reserve(g_live.size());
    for (const LiveRef& r : g_live) {
        if (!game::AliveAt(r.component, r.index)) continue;
        rstatic::Instance in = r.instance;
        in.world = ToMat4(r.component->LocalToWorld);
        in.center = ToVec3(r.component->Bounds.Origin);
        in.radius = r.component->Bounds.SphereRadius;
        in.extent = ToVec3(r.component->Bounds.BoxExtent);
        if (!Finite(in.world) || !std::isfinite(in.radius)) continue;
        job.live.push_back(in);
    }
    rstatic::BuildConfig& b = job.config;
    b.camera = cam.pos;
    b.maxDistance = cfg.maxDistance;
    b.fade = cfg.fade;
    b.fill = cfg.fill;
    b.outline = cfg.outline;
    b.nonColliding = cfg.nonColliding;
    b.filter = cfg.filter;
    b.vertexBudget = cfg.vertexBudget;
    b.enabled = cfg.enabled;
    b.colors = cfg.colors;
    {
        std::lock_guard lock(g_jobMutex);
        g_job = std::move(job);
    }
    g_jobCv.notify_one();
    g_sentAny = true;
    g_sentConfig = cfg;
    g_sentSnapshot = g_snapshot;
    g_sentCamera = cam.pos;
    g_sentAt = now;
}
} // namespace

void Stop()
{
    g_generation.fetch_add(1);      // A build in flight is dropped
    StopWorker();
    debugrender::SetStatic(nullptr);
    g_sentAny = false;              // Shown again: build at once (the scanned level is kept)
    std::lock_guard lock(g_infoMutex);
    g_info.drawn = {};
    g_info.vertices = 0;
}

Info GetInfo()
{
    std::lock_guard lock(g_infoMutex);
    return g_info;
}

} // namespace staticworld
