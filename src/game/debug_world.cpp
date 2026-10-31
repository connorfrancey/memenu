#include "debug_world.h"
#include "game.h"
#include "levels.h"
#include "paths.h"
#include "render/debug_renderer.h"
#include "render/mesh.h"

#include <Windows.h>

#include <mirrorsedge_sdk.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace debugworld
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        namespace fs = std::filesystem;

        enum class Kind : std::uint8_t { None, Cylinder, Brush };
        enum class ShapeType : std::uint8_t { Box, Sphere, Capsule, Cylinder, Mesh };

        constexpr int kNoCategory = -1;      // Not something this view shows (static meshes, world geometry, ...)
        const char* const kCategoryNames[kCategoryCount] = {
            "triggers", "kill zones", "checkpoints", "blocking", "movement", "volumes", "cylinders"
        };

        // A volume, remembered in world space: everything needed to draw it without touching the game again.
        struct Shape
        {
            std::uint8_t category = 0;
            ShapeType type = ShapeType::Box;
            rmath::Mat4 world = rmath::Identity();  // Box/Sphere/Capsule/Cylinder: local -> world
            float a = 0.f, b = 0.f, c = 0.f;        // Box: half extents. Sphere: a=radius. Capsule: a=radius b=half length.
                                                    // Cylinder: a=radius b=half height
            rmath::Vec3 center;                     // Bounding sphere, for the distance test
            float radius = 0.f;
            std::string name;                       // Set on the first shape of a component: its label
            std::vector<rmath::Vec3> verts;         // Mesh: world-space
            std::vector<int> idx;
        };

        struct ClassInfo
        {
            bool skip = false;      // e.g. the world-sized default physics volume
            int category = kNoCategory;
            Kind kind = Kind::None;
        };

        // ---- State (game thread, except g_info) ---------------------------------------------------------------------
        std::unordered_map<Classes::UClass*, ClassInfo> g_classes; // Computed once per UClass, not per object
        std::vector<Shape> g_shapes;                                // The remembered volumes of g_map
        std::unordered_set<std::string> g_keys;                     // "<actor>/<component>" of everything in g_shapes
        std::string g_map;
        bool g_fromDisk = false;
        bool g_cacheDirty = false;
        Clock::time_point g_dirtySince{};
        std::size_t g_lastObjectCount = 0;
        Clock::time_point g_lastScan{};
        Clock::time_point g_lastBuild{};
        bool g_forceScan = true;
        bool g_dirty = true;
        bool g_waitingForGame = false;
        std::uint64_t g_rebuildCount = 0;
        std::uint64_t g_scanCount = 0;
        double g_lastScanMs = 0.0;

        std::mutex g_infoMutex;
        Info g_info;

        constexpr std::uint32_t kCacheMagic = 0x4356454Du; // "MEVC"
        constexpr std::uint32_t kCacheVersion = 1;
        constexpr double kSlowMs = 50.0;

        bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

        bool EndsWith(const std::string& s, const char* suffix)
        {
            const std::size_t n = std::char_traits<char>::length(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        }

        int Classify(const std::string& name)
        {
            if (Has(name, "KillVolume") || Has(name, "KillZone")) return static_cast<int>(Category::Kill);
            if (Has(name, "Trigger")) return static_cast<int>(Category::Trigger);
            if (Has(name, "Checkpoint")) return static_cast<int>(Category::Checkpoint);
            if (Has(name, "BlockingVolume")) return static_cast<int>(Category::Blocking);
            if (Has(name, "Volume"))
            {
                static const char* const movement[] = {
                    "Movement", "Ladder", "Zipline", "Swing", "BalanceWalk", "LedgeWalk",
                    "BarbedWire", "FallHeight", "PhysicsVolume", "WaterVolume", "GravityVolume",
                    "AIKeepMoving"
                };
                for (const char* m : movement) if (Has(name, m)) return static_cast<int>(Category::Movement);
                return static_cast<int>(Category::Volume);
            }
            return kNoCategory;
        }

        // Class information is computed once per UClass (from the first object seen), then looked up by class pointer.
        // Everything is decided from the class *name*, not from IsA(): in this build IsA() only matches the exact class.
        const ClassInfo& ClassOf(Classes::UObject* o)
        {
            const auto it = g_classes.find(o->Class);
            if (it != g_classes.end()) return it->second;

            ClassInfo info;
            const std::string className = o->Class ? o->Class->GetName() : std::string();
            info.category = Classify(className);
            info.skip = Has(className, "DefaultPhysicsVolume");
            if (EndsWith(className, "CylinderComponent")) info.kind = Kind::Cylinder;
            else if (EndsWith(className, "BrushComponent")) info.kind = Kind::Brush;

            if (info.kind != Kind::None || info.category != kNoCategory || info.skip)
                spdlog::debug("Debug world: classified '{}' (skip={}, category={}, kind={})", className, info.skip,
                              info.category == kNoCategory ? "-" : kCategoryNames[info.category],
                              info.kind == Kind::Cylinder ? "cylinder" : info.kind == Kind::Brush ? "brush" : "-");
            return g_classes.emplace(o->Class, info).first->second;
        }

        // ---- Conversion ---------------------------------------------------------------------------------------------
        rmath::Mat4 ToMat4(const Classes::FMatrix& m)
        {
            return {
                {
                    {m.XPlane.X, m.XPlane.Y, m.XPlane.Z, m.XPlane.W},
                    {m.YPlane.X, m.YPlane.Y, m.YPlane.Z, m.YPlane.W},
                    {m.ZPlane.X, m.ZPlane.Y, m.ZPlane.Z, m.ZPlane.W},
                    {m.WPlane.X, m.WPlane.Y, m.WPlane.Z, m.WPlane.W}
                }
            };
        }

        bool Finite(const rmath::Mat4& m)
        {
            for (const auto& row : m.m)
                for (float f : row) if (!std::isfinite(f)) return false;
            return true;
        }

        rmath::Vec3 ToVec3(const Classes::FVector& v) { return {v.X, v.Y, v.Z}; }

        bool FiniteVec(rmath::Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        // ---- Extracting shapes from a component -----------------------------------------------------------------------
        // Boxes, spheres, capsules and convex hulls of an aggregate geometry, placed by the component's LocalToWorld.
        bool AddAggGeom(std::vector<Shape>& out, const Classes::FKAggregateGeom& g, const rmath::Mat4& l2w)
        {
            const std::size_t before = out.size();

            for (std::size_t i = 0; i < g.BoxElems.Num(); ++i)
            {
                const auto& e = g.BoxElems[i];
                Shape s;
                s.type = ShapeType::Box;
                s.world = rmath::Mul(ToMat4(e.TM), l2w);
                s.a = e.X * 0.5f; s.b = e.Y * 0.5f; s.c = e.Z * 0.5f;
                out.push_back(std::move(s));
            }
            for (std::size_t i = 0; i < g.SphereElems.Num(); ++i)
            {
                const auto& e = g.SphereElems[i];
                Shape s;
                s.type = ShapeType::Sphere;
                s.world = rmath::Mul(ToMat4(e.TM), l2w);
                s.a = e.Radius;
                out.push_back(std::move(s));
            }
            for (std::size_t i = 0; i < g.SphylElems.Num(); ++i)
            {
                const auto& e = g.SphylElems[i];
                Shape s;
                s.type = ShapeType::Capsule;
                s.world = rmath::Mul(ToMat4(e.TM), l2w);
                s.a = e.Radius; s.b = e.Length * 0.5f;
                out.push_back(std::move(s));
            }
            for (std::size_t i = 0; i < g.ConvexElems.Num(); ++i)
            {
                const auto& e = g.ConvexElems[i];
                if (e.VertexData.Num() < 4 || e.VertexData.Num() > 2048 || e.FaceTriData.Num() < 3) continue;
                Shape s;
                s.type = ShapeType::Mesh;
                s.verts.reserve(e.VertexData.Num());
                for (std::size_t v = 0; v < e.VertexData.Num(); ++v)
                    s.verts.push_back(rmath::TransformPoint(l2w, ToVec3(e.VertexData[v])));
                s.idx.reserve(e.FaceTriData.Num());
                for (std::size_t t = 0; t < e.FaceTriData.Num(); ++t) s.idx.push_back(e.FaceTriData[t]);
                out.push_back(std::move(s));
            }
            return out.size() > before;
        }

        // Appends the shapes of one component. Returns how many were added.
        std::size_t Extract(Classes::UPrimitiveComponent* pc, Kind kind, int category, const std::string& label,
                            rmath::Vec3 ownerLocation, std::vector<Shape>& out)
        {
            const rmath::Mat4 l2w = ToMat4(pc->LocalToWorld);
            if (!Finite(l2w)) return 0;

            rmath::Vec3 center = ToVec3(pc->Bounds.Origin);
            float radius = pc->Bounds.SphereRadius;
            if (radius <= 0.f && center.x == 0.f && center.y == 0.f && center.z == 0.f) center = ownerLocation;
            if (!FiniteVec(center) || !std::isfinite(radius) || radius > 100000.f) return 0; // Unregistered / world-sized

            const std::size_t first = out.size();
            if (kind == Kind::Cylinder)
            {
                auto* cyl = static_cast<Classes::UCylinderComponent*>(pc);
                Shape s;
                s.type = ShapeType::Cylinder;
                s.world = l2w;
                s.a = cyl->CollisionRadius;
                s.b = cyl->CollisionHeight; // CollisionHeight is a half height
                out.push_back(std::move(s));
            }
            else if (kind == Kind::Brush)
            {
                auto* brush = static_cast<Classes::UBrushComponent*>(pc);
                if (!AddAggGeom(out, brush->BrushAggGeom, l2w))
                {
                    // Cooked brushes sometimes keep only PhysX data: fall back to the bounding box.
                    Shape s;
                    s.type = ShapeType::Box;
                    s.world = rmath::Translation(ToVec3(pc->Bounds.Origin));
                    const rmath::Vec3 e = ToVec3(pc->Bounds.BoxExtent);
                    s.a = e.x; s.b = e.y; s.c = e.z;
                    out.push_back(std::move(s));
                }
            }

            for (std::size_t i = first; i < out.size(); ++i)
            {
                out[i].category = static_cast<std::uint8_t>(category);
                out[i].center = center;
                out[i].radius = radius;
            }
            if (out.size() > first) out[first].name = label;
            return out.size() - first;
        }

        void Build(rmesh::Builder& b, const Shape& s)
        {
            switch (s.type)
            {
                case ShapeType::Box:      b.Box(s.world, {s.a, s.b, s.c}); break;
                case ShapeType::Sphere:   b.Sphere(s.world, s.a); break;
                case ShapeType::Capsule:  b.Capsule(s.world, s.a, s.b); break;
                case ShapeType::Cylinder: b.Cylinder(s.world, s.a, s.b); break;
                case ShapeType::Mesh:     b.Mesh(s.verts, s.idx); break;
            }
        }

        // ---- Level identity and the cache file -----------------------------------------------------------------------
        // The persistent level's package name: follow Outer links up from the WorldInfo.
        std::string MapKey()
        {
            Classes::UObject* o = game::World();
            if (!o) return {};
            for (int guard = 0; o->Outer && guard < 16; ++guard) o = o->Outer;
            std::string name = o->GetName();
            for (char& c : name)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
            return name;
        }

        fs::path CachePath(const std::string& map) { return paths::Dir() / "cache" / (map + ".mevc"); }

        template <class T>
        void Put(std::ofstream& f, const T& v) { f.write(reinterpret_cast<const char*>(&v), sizeof v); }

        template <class T>
        bool Get(std::ifstream& f, T& v) { return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof v)); }

        void SaveCache(const std::string& map)
        {
            if (map.empty()) return;
            std::error_code ec;
            fs::create_directories(paths::Dir() / "cache", ec);
            const fs::path path = CachePath(map);
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                spdlog::warn("Debug world: could not write the cache file {}", path.string());
                return;
            }
            Put(f, kCacheMagic);
            Put(f, kCacheVersion);
            Put(f, static_cast<std::uint32_t>(g_shapes.size()));
            for (const Shape& s : g_shapes)
            {
                Put(f, s.category);
                Put(f, s.type);
                Put(f, s.world);
                Put(f, s.a); Put(f, s.b); Put(f, s.c);
                Put(f, s.center);
                Put(f, s.radius);
                const std::size_t nameLen = std::min<std::size_t>(s.name.size(), 0xFFFF);
                Put(f, static_cast<std::uint16_t>(nameLen));
                f.write(s.name.data(), static_cast<std::streamsize>(nameLen));
                Put(f, static_cast<std::uint32_t>(s.verts.size()));
                if (!s.verts.empty()) f.write(reinterpret_cast<const char*>(s.verts.data()), static_cast<std::streamsize>(s.verts.size() * sizeof(rmath::Vec3)));
                Put(f, static_cast<std::uint32_t>(s.idx.size()));
                if (!s.idx.empty()) f.write(reinterpret_cast<const char*>(s.idx.data()), static_cast<std::streamsize>(s.idx.size() * sizeof(int)));
            }
            // The keys go last: they stop a later scan from adding the same volumes again.
            Put(f, static_cast<std::uint32_t>(g_keys.size()));
            for (const std::string& k : g_keys)
            {
                const std::size_t len = std::min<std::size_t>(k.size(), 0xFFFF);
                Put(f, static_cast<std::uint16_t>(len));
                f.write(k.data(), static_cast<std::streamsize>(len));
            }
            spdlog::info("Debug world: saved {} volumes of '{}' to {}", g_shapes.size(), map, path.string());
        }

        bool LoadCache(const std::string& map)
        {
            const fs::path path = CachePath(map);
            std::ifstream f(path, std::ios::binary);
            if (!f) return false;

            std::uint32_t magic = 0, version = 0, count = 0;
            if (!Get(f, magic) || !Get(f, version) || !Get(f, count) || magic != kCacheMagic || version != kCacheVersion ||
                count > 2000000)
            {
                spdlog::warn("Debug world: ignoring the cache file {} (unknown format)", path.string());
                return false;
            }

            std::vector<Shape> shapes;
            shapes.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                Shape s;
                std::uint16_t nameLen = 0;
                std::uint32_t nv = 0, ni = 0;
                if (!Get(f, s.category) || !Get(f, s.type) || !Get(f, s.world) || !Get(f, s.a) || !Get(f, s.b) ||
                    !Get(f, s.c) || !Get(f, s.center) || !Get(f, s.radius) || !Get(f, nameLen))
                    return false;
                s.name.resize(nameLen);
                if (nameLen && !f.read(s.name.data(), nameLen)) return false;
                if (!Get(f, nv) || nv > 100000) return false;
                s.verts.resize(nv);
                if (nv && !f.read(reinterpret_cast<char*>(s.verts.data()), static_cast<std::streamsize>(nv * sizeof(rmath::Vec3)))) return false;
                if (!Get(f, ni) || ni > 400000) return false;
                s.idx.resize(ni);
                if (ni && !f.read(reinterpret_cast<char*>(s.idx.data()), static_cast<std::streamsize>(ni * sizeof(int)))) return false;
                if (s.category >= kCategoryCount) return false;
                shapes.push_back(std::move(s));
            }

            std::unordered_set<std::string> keys;
            std::uint32_t keyCount = 0;
            if (!Get(f, keyCount) || keyCount > 4000000) return false;
            for (std::uint32_t i = 0; i < keyCount; ++i)
            {
                std::uint16_t len = 0;
                if (!Get(f, len)) return false;
                std::string k(len, '\0');
                if (len && !f.read(k.data(), len)) return false;
                keys.insert(std::move(k));
            }

            g_shapes = std::move(shapes);
            g_keys = std::move(keys);
            spdlog::info("Debug world: loaded {} volumes of '{}' from the cache ({})", g_shapes.size(), map, path.string());
            return true;
        }

        void PublishInfo()
        {
            std::lock_guard lock(g_infoMutex);
            g_info.map = g_map;
            g_info.shapes = g_shapes.size();
            g_info.fromDisk = g_fromDisk;
            g_info.cacheDirty = g_cacheDirty;
            g_info.lastScanMs = g_lastScanMs;
            g_info.scans = static_cast<std::size_t>(g_scanCount);
        }

        // ---- Scanning --------------------------------------------------------------------------------------------------
        // Component-first: walk every object once and pick out the components we know how to draw, then use their Owner
        // for classification. Volumes already remembered are skipped by key. Static meshes are not looked at.
        void ScanStatic()
        {
            const auto started = Clock::now();
            const auto& objects = Classes::UObject::GetGlobalObjects();
            std::size_t components = 0, added = 0, addedShapes = 0;
            std::vector<Shape> found;

            for (std::size_t i = 0; i < objects.Num(); ++i)
            {
                Classes::UObject* o = objects.GetByIndex(i);
                if (!o || !o->Class) continue;

                const ClassInfo& compInfo = ClassOf(o);
                if (compInfo.kind == Kind::None) continue;
                ++components;

                auto* pc = static_cast<Classes::UPrimitiveComponent*>(o);
                Classes::AActor* owner = pc->Owner;
                if (!owner || !owner->Class || owner->bDeleteMe) continue;   // class default objects / templates

                const ClassInfo& ownerInfo = ClassOf(owner);
                if (ownerInfo.skip || ownerInfo.category == kNoCategory) continue;   // Live cylinders are not remembered

                const std::string ownerName = owner->GetName();
                std::string key = ownerName + "/" + pc->GetName();
                if (g_keys.count(key)) continue;

                found.clear();
                const std::size_t n = Extract(pc, compInfo.kind, ownerInfo.category, ownerName, ToVec3(owner->Location), found);
                g_keys.insert(std::move(key)); // Also when nothing came out: it would not next time either
                if (n == 0) continue;
                ++added;
                addedShapes += n;
                for (Shape& s : found) g_shapes.push_back(std::move(s));
            }

            ++g_scanCount;
            g_lastScanMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            if (addedShapes > 0)
            {
                g_cacheDirty = true;
                g_dirtySince = Clock::now();
                g_dirty = true;
            }
            spdlog::debug("Debug world: scan #{} of '{}': {} components, {} new ({} shapes), {} remembered, {:.2f} ms",
                          g_scanCount, g_map, components, added, addedShapes, g_shapes.size(), g_lastScanMs);
            if (g_lastScanMs > kSlowMs)
                spdlog::warn("Debug world: slow scan took {:.1f} ms ({} objects)", g_lastScanMs, objects.Num());
        }

        // ---- Baking all levels -----------------------------------------------------------------------------------------
        struct Bake
        {
            enum class Phase { Idle, Start, WaitLoad, Settle } phase = Phase::Idle;
            bool running = false;
            std::vector<levels::MapView> maps;
            std::size_t map = 0, checkpoint = 0;   // checkpoint 0 = start of the level
            bool sawLoading = false;
            bool forcedScan = false;
            Clock::time_point since{};
        };
        Bake g_bake;

        void SetBakeStatus(const std::string& status)
        {
            std::lock_guard lock(g_infoMutex);
            g_info.baking = g_bake.running;
            g_info.bakeStatus = status;
        }

        void FinishBake(const char* why)
        {
            spdlog::info("Debug world: baking {}", why);
            g_bake.running = false;
            g_bake.phase = Bake::Phase::Idle;
            SetBakeStatus(why);
        }

        void TickBake()
        {
            if (!g_bake.running) return;
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - g_bake.since).count();

            switch (g_bake.phase)
            {
                case Bake::Phase::Idle:
                    break;
                case Bake::Phase::Start:
                {
                    if (g_bake.map >= g_bake.maps.size()) { FinishBake("finished: all levels are in the cache"); return; }
                    const auto& m = g_bake.maps[g_bake.map];
                    const std::string where = g_bake.checkpoint == 0 ? std::string("start of level") : m.checkpoints[g_bake.checkpoint - 1];
                    spdlog::info("Debug world: baking {} / {}: '{}' {}", g_bake.map + 1, g_bake.maps.size(), m.label, where);
                    SetBakeStatus("Baking " + std::to_string(g_bake.map + 1) + "/" + std::to_string(g_bake.maps.size()) + ": " +
                                  m.label + " - " + where);
                    levels::Load(static_cast<int>(g_bake.map), static_cast<int>(g_bake.checkpoint), levels::Method::Game, false);
                    g_bake.phase = Bake::Phase::WaitLoad;
                    g_bake.since = now;
                    g_bake.sawLoading = false;
                    g_bake.forcedScan = false;
                    break;
                }
                case Bake::Phase::WaitLoad:
                    if (game::IsLoading()) g_bake.sawLoading = true;
                    if ((g_bake.sawLoading && !game::IsLoading()) || elapsed > 30.0)
                    {
                        if (!g_bake.sawLoading) spdlog::warn("Debug world: baking did not see the level load start; continuing anyway");
                        g_bake.phase = Bake::Phase::Settle;
                        g_bake.since = now;
                    }
                    break;
                case Bake::Phase::Settle:
                    // Streaming sublevels need a moment to come in: scan again a few seconds after the load.
                    if (!g_bake.forcedScan && elapsed > 4.0) { g_forceScan = true; g_bake.forcedScan = true; }
                    if (elapsed > 7.0)
                    {
                        g_forceScan = true;
                        if (g_cacheDirty) { SaveCache(g_map); g_cacheDirty = false; g_fromDisk = true; }
                        const auto& m = g_bake.maps[g_bake.map];
                        if (++g_bake.checkpoint > m.checkpoints.size()) { g_bake.checkpoint = 0; ++g_bake.map; }
                        g_bake.phase = Bake::Phase::Start;
                        g_bake.since = now;
                    }
                    break;
            }
        }
    }

    // =================================================================================================================
    void Update(const Config& cfg)
    {
        TickBake();

        if (!game::Ready() || game::IsLoading())
        {
            if (!g_waitingForGame)
            {
                spdlog::info("Debug world: paused ({})", !game::Ready() ? "game not ready" : "game is loading");
                g_waitingForGame = true;
            }
            return;
        }
        if (g_waitingForGame)
        {
            spdlog::info("Debug world: resumed, game is ready");
            g_waitingForGame = false;
            g_dirty = true;
        }

        const auto now = Clock::now();

        // ---- Which level is this? Remember its volumes, or read them from disk -----------------------------------------
        const std::string map = MapKey();
        if (map.empty()) return;
        if (map != g_map)
        {
            if (g_cacheDirty && cfg.useCache) SaveCache(g_map);
            spdlog::info("Debug world: level '{}' (was '{}')", map, g_map.empty() ? "none" : g_map);
            g_map = map;
            g_shapes.clear();
            g_keys.clear();
            g_cacheDirty = false;
            g_fromDisk = cfg.useCache && LoadCache(map);
            g_lastObjectCount = 0;
            g_forceScan = !g_fromDisk || cfg.scanCached;
            g_dirty = true;
            debugrender::SetScene(nullptr);
        }

        // ---- Look for volumes that are not remembered yet --------------------------------------------------------------
        // Only when the engine's object list changed (something streamed in or was spawned), at most every 2 seconds.
        const std::size_t objectCount = Classes::UObject::GetGlobalObjects().Num();
        const bool listChanged = objectCount != g_lastObjectCount;
        const bool mayScan = !g_fromDisk || cfg.scanCached;
        if (g_forceScan || (mayScan && listChanged && std::chrono::duration<double>(now - g_lastScan).count() > 2.0))
        {
            g_forceScan = false;
            g_lastObjectCount = objectCount;
            g_lastScan = now;
            ScanStatic();
        }

        // Write the cache once nothing new has turned up for a few seconds.
        if (g_cacheDirty && cfg.useCache && std::chrono::duration<double>(now - g_dirtySince).count() > 4.0)
        {
            SaveCache(g_map);
            g_cacheDirty = false;
            g_fromDisk = true;
        }
        PublishInfo();

        // ---- Rebuild the scene ------------------------------------------------------------------------------------------
        const double interval = 1.0 / std::clamp(cfg.updateHz, 0.5f, 30.f);
        if (!g_dirty && std::chrono::duration<double>(now - g_lastBuild).count() < interval) return;

        const rmath::Camera cam = game::GetCamera();
        if (!cam.valid) return;
        const auto started = Clock::now();
        ++g_rebuildCount;

        auto scene = std::make_shared<debugrender::Scene>();
        rmesh::Builder builder(scene->mesh);
        constexpr std::size_t kMaxVertices = 3000000;   // A hard stop for dense levels
        constexpr std::size_t kMaxLabels = 150;

        const auto styleFor = [&](int category)
        {
            rmesh::Style style;
            style.color = cfg.colors[static_cast<std::size_t>(category)];
            style.fill = cfg.style != DrawStyle::Wireframe;
            style.outline = cfg.style != DrawStyle::Solid;
            return style;
        };
        const auto addLabel = [&](const Shape& s, std::uint32_t color)
        {
            if (cfg.labels && !s.name.empty() && scene->labels.size() < kMaxLabels &&
                s.category != static_cast<int>(Category::Cylinder))
                scene->labels.push_back({s.center, s.name, color});
        };

        std::array<std::size_t, kCategoryCount> perCategory{};
        std::size_t culled = 0;
        bool capHit = false;

        // Remembered volumes: just a distance test, then geometry. No contact with the game.
        for (const Shape& s : g_shapes)
        {
            if (!cfg.enabled[s.category]) continue;
            if (rmath::Length(s.center - cam.pos) - s.radius > cfg.maxDistance) { ++culled; continue; }
            if (scene->mesh.tris.size() + scene->mesh.lines.size() > kMaxVertices) { capHit = true; break; }

            builder.SetStyle(styleFor(s.category));
            Build(builder, s);
            ++perCategory[s.category];
            addLabel(s, cfg.colors[s.category]);
        }

        // Live cylinders (pawns and other moving actors) are looked up every rebuild: they are not static.
        constexpr auto kCyl = static_cast<std::size_t>(Category::Cylinder);
        if (cfg.enabled[kCyl])
        {
            const auto& objects = Classes::UObject::GetGlobalObjects();
            std::vector<Shape> tmp;
            for (std::size_t i = 0; i < objects.Num(); ++i)
            {
                Classes::UObject* o = objects.GetByIndex(i);
                if (!o || !o->Class) continue;
                const ClassInfo& compInfo = ClassOf(o);
                if (compInfo.kind != Kind::Cylinder) continue;

                auto* pc = static_cast<Classes::UPrimitiveComponent*>(o);
                Classes::AActor* owner = pc->Owner;
                if (!owner || !owner->Class || owner->bDeleteMe) continue;
                const ClassInfo& ownerInfo = ClassOf(owner);
                if (ownerInfo.skip || ownerInfo.category != kNoCategory) continue;   // Those are remembered above

                tmp.clear();
                if (Extract(pc, Kind::Cylinder, static_cast<int>(kCyl), std::string(), ToVec3(owner->Location), tmp) == 0) continue;
                if (rmath::Length(tmp[0].center - cam.pos) - tmp[0].radius > cfg.maxDistance) { ++culled; continue; }
                builder.SetStyle(styleFor(static_cast<int>(kCyl)));
                Build(builder, tmp[0]);
                ++perCategory[kCyl];
            }
        }

        scene->shapes = builder.Shapes();
        scene->buildMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        if (scene->buildMs > kSlowMs)
            spdlog::warn("Debug world: slow rebuild #{} took {:.1f} ms ({} shapes)", g_rebuildCount, scene->buildMs, scene->shapes);
        if (capHit) spdlog::warn("Debug world: vertex cap ({}) reached; farther volumes omitted", kMaxVertices);

        spdlog::debug("Debug world: rebuild #{} in {:.2f} ms: {} shapes, {} tri verts, {} line verts, {} labels, {} out of range",
                      g_rebuildCount, scene->buildMs, scene->shapes, scene->mesh.tris.size(), scene->mesh.lines.size(),
                      scene->labels.size(), culled);
        if (g_rebuildCount == 1 || g_rebuildCount % 120 == 0)
        {
            std::string cats;
            for (int i = 0; i < kCategoryCount; ++i)
                cats += fmt::format("{}{} {}", cats.empty() ? "" : ", ", perCategory[static_cast<std::size_t>(i)], kCategoryNames[i]);
            spdlog::info("Debug world: rebuild #{}: {} shapes in range ({}) of {} remembered", g_rebuildCount, scene->shapes, cats,
                         g_shapes.size());
        }

        debugrender::SetScene(std::move(scene));
        g_lastBuild = Clock::now();
        g_dirty = false;
    }

    void RequestRebuild()
    {
        spdlog::debug("Debug world: rebuild requested");
        g_dirty = true;
    }

    void Clear()
    {
        spdlog::info("Debug world: scene cleared ({} volumes stay remembered)", g_shapes.size());
        debugrender::SetScene(nullptr);
        g_dirty = true;
    }

    void RescanNow()
    {
        g_forceScan = true;
        g_dirty = true;
    }

    void ClearCache()
    {
        game::RunOnGameThread([]
        {
            spdlog::info("Debug world: clearing the remembered volumes of '{}'", g_map);
            std::error_code ec;
            if (!g_map.empty()) fs::remove(CachePath(g_map), ec);
            g_shapes.clear();
            g_keys.clear();
            g_cacheDirty = false;
            g_fromDisk = false;
            g_forceScan = true;
            g_dirty = true;
            PublishInfo();
        });
    }

    void BakeAllLevels()
    {
        game::RunOnGameThread([]
        {
            if (g_bake.running) return;
            g_bake = Bake{};
            g_bake.maps = levels::Maps();
            if (g_bake.maps.empty())
            {
                SetBakeStatus("no levels known yet");
                return;
            }
            g_bake.running = true;
            g_bake.phase = Bake::Phase::Start;
            g_bake.since = Clock::now();
            spdlog::info("Debug world: baking {} levels", g_bake.maps.size());
            SetBakeStatus("starting");
        });
    }

    void CancelBake()
    {
        game::RunOnGameThread([]
        {
            if (g_bake.running) FinishBake("cancelled");
        });
    }

    Info GetInfo()
    {
        std::lock_guard lock(g_infoMutex);
        return g_info;
    }
}
