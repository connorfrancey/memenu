#include "debug_world.h"
#include "game.h"
#include "game/uobject.h"
#include "levels.h"
#include "paths.h"
#include "render/debug_renderer.h"
#include "render/geometry.h"
#include "render/mesh.h"

#include <Windows.h>

#include <ME_SDK.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
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

        // Components this view can draw: cylinders, brushes (volumes), and the box/sphere shapes some actors use to show
        // an area (AI grenade areas, radial impulses, ...).
        enum class Kind : std::uint8_t { None, Cylinder, Brush, DrawBox, DrawSphere };
        enum class ShapeType : std::uint8_t { Box, Sphere, Capsule, Cylinder, Mesh };

        constexpr int kNoCategory = -1;      // Not something this view shows (static meshes, world geometry, ...)
        const char* const kCategoryNames[kCategoryCount] = {
            "triggers", "kill zones", "checkpoints", "blocking", "movement", "volumes", "cylinders",
            "spawns", "pickups", "AI", "navigation", "post process", "audio", "fog", "streaming"
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

        // Live cylinders (pawns and other moving actors). They move, so they are drawn every tick from a list of their
        // components, and not with the rest at the update rate (a pawn running past would lag behind its cylinder).
        // The list is found by walking the object list a slice per tick, never all at once (a full walk is a hitch).
        struct LiveCylinder
        {
            Classes::UPrimitiveComponent* component = nullptr;
            std::size_t index = 0;          // In the object list: checked before every use (game::AliveAt)
        };
        constexpr std::size_t kCylinderScanPerTick = 2500;     // ~100k objects: the whole list in about a second
        std::vector<LiveCylinder> g_cylinders;          // The last complete walk
        std::vector<LiveCylinder> g_cylindersNext;      // The walk in progress
        std::size_t g_cylinderScanAt = 0;
        bool g_cylindersShown = false;                  // Something was published to debugrender::SetDynamic
        std::vector<std::string> g_shapeNames;          // Lower-case name of each g_shapes entry's component
        std::size_t g_shapeNamesFor = ~std::size_t{0};  // g_shapes.size() they were made for
        int g_lookShape = -1;                           // g_shapes index under the crosshair, -1: none


        std::mutex g_infoMutex;
        Info g_info;

        constexpr std::uint32_t kCacheMagic = 0x4356454Du; // "MEVC"
        // 2: AI-only volumes have their own category (were blocking/movement); spawns, pickups, navigation points and
        // box/sphere components are remembered.
        // 3: the other volumes are split into post process, audio, fog, streaming and the rest. Version 2 files are still
        // read: their other volumes are sorted by actor name (OtherVolumeKind). Older ones are ignored, the level read again.
        constexpr std::uint32_t kCacheVersion = 3;
        constexpr std::uint32_t kOldestCacheVersion = 2;
        constexpr double kSlowMs = 50.0;

        bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

        bool EndsWith(const std::string& s, const char* suffix)
        {
            const std::size_t n = std::char_traits<char>::length(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        }

        // Volumes and areas only AI cares about: they don't block or affect the player (an AI blocking volume is not a
        // wall for Faith), so they are kept apart from the blocking and movement volumes.
        bool IsAiOnly(const std::string& name)
        {
            static const char* const ai[] = {
                "TdAIBlockingVolume", "TdAIPawnBlockingVolume", "TdAIKeepMovingVolume", "PathBlockingVolume",
                "PathLimitsVolume", "CoverGroupVolume", "GrenadeArea", "GrenadeTargetArea", "ExplosiveTargetArea"
            };
            for (const char* a : ai) if (Has(name, a)) return true;
            return false;
        }

        // Which of the other volumes (or navigation, for path nodes named like volumes), from a class name ("TdReverbVolume") or an actor's ("PostProcessVolume_3": level
        // actors are named after their class).
        int OtherVolumeKind(const std::string& name)
        {
            // Path nodes of the AI's network that have "Volume" in their name (TdConfinedVolumePathNode, VolumePathNode)
            if (Has(name, "PathNode")) return static_cast<int>(Category::Navigation);
            if (Has(name, "Fog")) return static_cast<int>(Category::Fog);
            if (Has(name, "Reverb") || Has(name, "AudioVolume") || Has(name, "AmbientSound")) return static_cast<int>(Category::Audio);
            if (Has(name, "PostProcess") || Has(name, "ColorScale") || Has(name, "LightVolume") || Has(name, "Reflection") ||
                Has(name, "Lighting"))
                return static_cast<int>(Category::PostProcess);
            if (Has(name, "LevelStreaming") || Has(name, "CullDistance") || Has(name, "Portal") || Has(name, "Visibility"))
                return static_cast<int>(Category::Streaming);
            return static_cast<int>(Category::Volume);
        }

        int Classify(const std::string& name)
        {
            if (Has(name, "KillVolume") || Has(name, "KillZone")) return static_cast<int>(Category::Kill);
            if (IsAiOnly(name)) return static_cast<int>(Category::AI);
            if (Has(name, "Trigger")) return static_cast<int>(Category::Trigger);
            if (Has(name, "Checkpoint")) return static_cast<int>(Category::Checkpoint);
            if (Has(name, "BlockingVolume")) return static_cast<int>(Category::Blocking);
            if (Has(name, "Volume"))
            {
                static const char* const movement[] = {
                    "Movement", "Ladder", "Zipline", "Swing", "BalanceWalk", "LedgeWalk",
                    "BarbedWire", "FallHeight", "PhysicsVolume", "WaterVolume", "GravityVolume"
                };
                for (const char* m : movement) if (Has(name, m)) return static_cast<int>(Category::Movement);
                return OtherVolumeKind(name);
            }
            return kNoCategory;
        }

        // Actors that aren't volumes, by what they derive from (a level's subclasses have all sorts of names). Only asked
        // when the name didn't place them. Everything left is drawn live, as a cylinder (pawns, dropped weapons, ...).
        int ClassifyActor(Classes::UObject* o)
        {
            using namespace Classes;
            if (game::IsA(o, APlayerStart::StaticClass())) return static_cast<int>(Category::Spawn);
            if (game::IsA(o, APickupFactory::StaticClass()) || game::IsA(o, ATdStashpoint::StaticClass()))
                return static_cast<int>(Category::Pickup);
            if (game::IsA(o, ALadder::StaticClass()) || game::IsA(o, ATeleporter::StaticClass()))
                return static_cast<int>(Category::Movement);
            if (game::IsA(o, ANavigationPoint::StaticClass())) return static_cast<int>(Category::Navigation);
            return kNoCategory;
        }

        // Class information is computed once per UClass (from the first object seen), then looked up by class pointer.
        // Everything is decided from the class *name* (the SDK's IsA() only matches the exact class; game::IsA() in
        // game/uobject.h is the correct one).
        const ClassInfo& ClassOf(Classes::UObject* o)
        {
            const auto it = g_classes.find(o->Class);
            if (it != g_classes.end()) return it->second;

            ClassInfo info;
            const std::string className = o->Class ? o->Class->GetName() : std::string();
            info.category = Classify(className);
            info.skip = Has(className, "DefaultPhysicsVolume");
            if (EndsWith(className, "DrawCylinderComponent")) info.kind = Kind::None;     // A cone: no shape for it
            else if (EndsWith(className, "CylinderComponent")) info.kind = Kind::Cylinder;
            else if (EndsWith(className, "BrushComponent")) info.kind = Kind::Brush;
            else if (EndsWith(className, "DrawBoxComponent")) info.kind = Kind::DrawBox;
            else if (EndsWith(className, "DrawSphereComponent")) info.kind = Kind::DrawSphere;
            if (info.kind == Kind::None && info.category == kNoCategory && !EndsWith(className, "Component"))
                info.category = ClassifyActor(o);

            if (info.kind != Kind::None || info.category != kNoCategory || info.skip)
                spdlog::debug("Debug world: classified '{}' (skip={}, category={}, kind={})", className, info.skip,
                              info.category == kNoCategory ? "-" : kCategoryNames[info.category],
                              info.kind == Kind::Cylinder ? "cylinder" : info.kind == Kind::Brush ? "brush" :
                              info.kind == Kind::DrawBox ? "box" : info.kind == Kind::DrawSphere ? "sphere" : "-");
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

            // Sizes the shape components report must be real ones (they can be left at 0)
            const auto sane = [](float f) { return std::isfinite(f) && f > 0.5f && f < 100000.f; };

            const std::size_t first = out.size();
            if (kind == Kind::DrawBox)
            {
                const auto& e = static_cast<Classes::UDrawBoxComponent*>(pc)->BoxExtent;
                if (!sane(e.X) || !sane(e.Y) || !sane(e.Z)) return 0;
                Shape s;
                s.type = ShapeType::Box;
                s.world = l2w;
                s.a = e.X; s.b = e.Y; s.c = e.Z;
                out.push_back(std::move(s));
            }
            else if (kind == Kind::DrawSphere)
            {
                const float r = static_cast<Classes::UDrawSphereComponent*>(pc)->SphereRadius;
                if (!sane(r)) return 0;
                Shape s;
                s.type = ShapeType::Sphere;
                s.world = l2w;
                s.a = r;
                out.push_back(std::move(s));
            }
            else if (kind == Kind::Cylinder)
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

        // Where the ray first hits the shape (t >= 0), < 0 for a miss. A ray starting inside hits at 0.
        float RayShape(const Shape& s, rmath::Vec3 o, rmath::Vec3 d)
        {
            // A hull's triangles would report where the ray leaves it: starting inside is a hit at 0, like the others
            if (s.type == ShapeType::Mesh)
                return rgeom::InConvex(o, s.verts, s.idx) ? 0.f : rgeom::RayTriangles(o, d, s.verts, s.idx);
            rmath::Mat4 inv;
            if (!rgeom::InverseAffine(s.world, inv)) return rgeom::kNoHit;
            const rmath::Vec3 lo = rmath::TransformPoint(inv, o), ld = rgeom::TransformDir(inv, d);
            switch (s.type)
            {
                case ShapeType::Box:      return rgeom::RayBox(lo, ld, {s.a, s.b, s.c});
                case ShapeType::Sphere:   return rgeom::RaySphere(lo, ld, {}, s.a);
                case ShapeType::Capsule:  return rgeom::RayCapsule(lo, ld, s.a, s.b);
                case ShapeType::Cylinder: return rgeom::RayCylinder(lo, ld, s.a, s.b);
                default:                  return rgeom::kNoHit;
            }
        }

        bool Contains(const Shape& s, rmath::Vec3 p)
        {
            if (rmath::Length(p - s.center) > s.radius + 1.f) return false;
            if (s.type == ShapeType::Mesh) return rgeom::InConvex(p, s.verts, s.idx);
            rmath::Mat4 inv;
            if (!rgeom::InverseAffine(s.world, inv)) return false;
            const rmath::Vec3 l = rmath::TransformPoint(inv, p);
            switch (s.type)
            {
                case ShapeType::Box:      return rgeom::InBox(l, {s.a, s.b, s.c});
                case ShapeType::Sphere:   return rgeom::InSphere(l, s.a);
                case ShapeType::Capsule:  return rgeom::InCapsule(l, s.a, s.b);
                case ShapeType::Cylinder: return rgeom::InCylinder(l, s.a, s.b);
                default:                  return false;
            }
        }

        std::string LowerCase(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // Scales the alpha of the vertices from `from` on (fading a shape out with distance).
        void FadeFrom(std::vector<rmesh::Vertex>& v, std::size_t from, float alpha)
        {
            for (std::size_t i = from; i < v.size(); ++i)
            {
                const auto a = static_cast<std::uint32_t>(static_cast<float>(v[i].color >> 24) * alpha + 0.5f);
                v[i].color = (v[i].color & 0x00FFFFFFu) | (std::min<std::uint32_t>(a, 255u) << 24);
            }
        }

        // Names follow the component's first shape; the rest of its shapes have none.
        void RefreshShapeNames()
        {
            if (g_shapeNamesFor == g_shapes.size()) return;
            g_shapeNames.assign(g_shapes.size(), std::string());
            std::string current;
            for (std::size_t i = 0; i < g_shapes.size(); ++i)
            {
                if (!g_shapes[i].name.empty()) current = g_shapes[i].name;
                g_shapeNames[i] = current;
            }
            g_shapeNamesFor = g_shapes.size();
        }

        bool Passes(const Config& cfg, std::size_t i)
        {
            if (!cfg.enabled[g_shapes[i].category]) return false;
            return cfg.filter.empty() || LowerCase(g_shapeNames[i]).find(cfg.filter) != std::string::npos;
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
            if (!Get(f, magic) || !Get(f, version) || !Get(f, count) || magic != kCacheMagic || version > kCacheVersion ||
                version < kOldestCacheVersion ||
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

            if (version < 3)
            {
                // Before the other volumes had sub-categories: sort them by their actor's name (names are on the first
                // shape of a component; the rest of its shapes follow it)
                std::string current;
                for (Shape& s : shapes)
                {
                    if (!s.name.empty()) current = s.name;
                    if (s.category == static_cast<std::uint8_t>(Category::Volume))
                        s.category = static_cast<std::uint8_t>(OtherVolumeKind(current));
                }
            }
            g_shapes = std::move(shapes);
            g_keys = std::move(keys);
            spdlog::info("Debug world: loaded {} volumes of '{}' from the cache ({}, version {})", g_shapes.size(), map,
                         path.string(), version);
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
        // A scan is spread over ticks, about kScanSliceMs of work each: walking ~100k objects at once was a 30-40 ms
        // hitch every time something streamed in.
        constexpr double kScanSliceMs = 1.0;
        constexpr std::size_t kNotScanning = ~std::size_t{0};
        struct Scan
        {
            std::size_t at = kNotScanning;      // Next object
            std::size_t components = 0, added = 0, addedShapes = 0;
            double ms = 0.0;                    // Work so far (not the wall time across ticks)
        };
        Scan g_scan;

        void StartScan()
        {
            if (g_scan.at != kNotScanning) return;      // One is going: it will see what is new too
            g_scan = Scan{};
            g_scan.at = 0;
        }

        bool Scanning() { return g_scan.at != kNotScanning; }

        void ScanSlice()
        {
            if (!Scanning()) return;
            const auto started = Clock::now();
            const auto& objects = Classes::UObject::GetGlobalObjects();
            std::vector<Shape> found;

            std::size_t i = g_scan.at;
            for (; i < objects.Num(); ++i)
            {
                if ((i & 255) == 0 && i != g_scan.at &&
                    std::chrono::duration<double, std::milli>(Clock::now() - started).count() > kScanSliceMs)
                    break;
                Classes::UObject* o = objects.GetByIndex(i);
                if (!o || !o->Class) continue;

                const ClassInfo& compInfo = ClassOf(o);
                if (compInfo.kind == Kind::None) continue;
                ++g_scan.components;

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
                ++g_scan.added;
                g_scan.addedShapes += n;
                for (Shape& s : found) g_shapes.push_back(std::move(s));
                g_cacheDirty = true;                // Shown as they come (a big level takes a few ticks)
                g_dirtySince = Clock::now();
                g_dirty = true;
            }
            g_scan.ms += std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            g_scan.at = i;
            if (i < objects.Num()) return;

            ++g_scanCount;
            g_lastScanMs = g_scan.ms;
            spdlog::debug("Debug world: scan #{} of '{}': {} components, {} new ({} shapes), {} remembered, {:.2f} ms of work",
                          g_scanCount, g_map, g_scan.components, g_scan.added, g_scan.addedShapes, g_shapes.size(),
                          g_lastScanMs);
            g_scan.at = kNotScanning;
        }

        // ---- Baking all levels -----------------------------------------------------------------------------------------
        // One job per level start and checkpoint, in story order. Each one loads, waits for the engine's level-load hook
        // to report the load finished and the player to exist, then rescans every second until nothing new turns up
        // (streamed parts come in a little after the load). A level whose jobs all worked gets cache/<map>.baked, so
        // "bake missing" can skip it. At the end the player goes back to the level they started in.
        constexpr double kLoadTimeout = 45.0;     // Seconds for a load to finish before the job is skipped
        constexpr double kSettleMin = 2.0;        // Seconds after the load before a job may finish
        constexpr double kSettleQuiet = 1.5;      // ...and this long without new volumes
        constexpr double kSettleMax = 12.0;
        constexpr double kScanEvery = 1.0;

        struct BakeJob
        {
            int map = 0;                // Index into levels::Maps()
            int checkpoint = 0;         // 0 = start of the level
            std::string label;          // For the status line
        };

        struct Bake
        {
            enum class Phase { Idle, Start, WaitLoad, Settle } phase = Phase::Idle;
            bool running = false;
            std::vector<BakeJob> jobs;
            std::size_t job = 0;
            std::size_t failed = 0;
            bool mapOk = true;          // Every job of the current map worked so far
            std::size_t loadsSeen = 0;  // g_loadsFinished when this job's load was asked for
            std::size_t shapesAtStart = 0, shapesSeen = 0;
            std::string startMap;       // Lower-case file name of the level the bake started in
            Clock::time_point since{}, lastScan{}, lastNew{};
        };
        Bake g_bake;
        std::atomic<std::size_t> g_loadsFinished{0};   // Level loads the engine finished (game::OnLevelLoad)

        void OnLevelLoaded(bool finished)
        {
            if (finished) g_loadsFinished.fetch_add(1, std::memory_order_relaxed);
        }

        std::string Lower(std::string s)
        {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // "CHAPTER 9 - THE SHARD (Scraper_p)" -> "scraper_p"
        std::string MapFile(const std::string& label)
        {
            const std::size_t open = label.rfind('('), close = label.rfind(')');
            if (open == std::string::npos || close == std::string::npos || close < open) return Lower(label);
            return Lower(label.substr(open + 1, close - open - 1));
        }

        // Story order; maps not in it come after, in the game's order.
        int StoryRank(const std::string& file)
        {
            static const char* const order[] = {"edge_p", "escape_p", "stromdrain_p", "cranes_p", "subway_p",
                                                "mall_p", "factory_p", "boat_p", "convoy_p", "scraper_p"};
            for (int i = 0; i < static_cast<int>(std::size(order)); ++i)
                if (file == order[i]) return i;
            return 1000;
        }

        fs::path BakedMarker(const std::string& map) { return paths::Dir() / "cache" / (map + ".baked"); }
        // What a .baked file holds: the cache version it was baked with. Older bakes (just "baked") don't count: their
        // cache files are no longer read.
        const std::string kBakedMarker = "baked " + std::to_string(kCacheVersion);

        // Lower-case names of the maps that have been baked completely, with this cache version.
        std::unordered_set<std::string> BakedMaps()
        {
            std::unordered_set<std::string> out;
            std::error_code ec;
            for (fs::directory_iterator it(paths::Dir() / "cache", ec), end; !ec && it != end; it.increment(ec))
            {
                if (it->path().extension() != ".baked") continue;
                std::ifstream in(it->path());
                std::string text;
                std::getline(in, text);
                // A bake of an older cache version still counts when that version is still read
                if (text == kBakedMarker || text == "baked " + std::to_string(kOldestCacheVersion))
                    out.insert(Lower(it->path().stem().string()));
            }
            return out;
        }

        void SetBakeStatus(const std::string& status)
        {
            std::lock_guard lock(g_infoMutex);
            g_info.baking = g_bake.running;
            g_info.bakeStatus = status;
            g_info.bakeDone = g_bake.job;
            g_info.bakeTotal = g_bake.jobs.size();
            g_info.bakeFailed = g_bake.failed;
        }

        // Back to the start of the level the bake began in (if it was one it can load).
        bool ReturnToStart()
        {
            if (g_bake.startMap.empty()) return false;
            const auto maps = levels::Maps();
            for (std::size_t m = 0; m < maps.size(); ++m)
            {
                if (MapFile(maps[m].label) != g_bake.startMap) continue;
                spdlog::info("Debug world: baking: back to '{}'", maps[m].label);
                levels::Load(static_cast<int>(m), 0, levels::Method::Game, false);
                return true;
            }
            return false;
        }

        void FinishBake(const std::string& why, bool goBack)
        {
            spdlog::info("Debug world: baking {}", why);
            g_bake.running = false;
            g_bake.phase = Bake::Phase::Idle;
            if (goBack && ReturnToStart()) SetBakeStatus(why + ", back to the level you were in");
            else SetBakeStatus(why);
        }

        // The current job is done (or gave up): save what it found, mark its level when that was the last of it.
        void EndJob(bool ok)
        {
            if (!ok)
            {
                ++g_bake.failed;
                g_bake.mapOk = false;
            }
            if (g_cacheDirty) { SaveCache(g_map); g_cacheDirty = false; g_fromDisk = true; }

            const BakeJob& job = g_bake.jobs[g_bake.job];
            const bool lastOfMap = g_bake.job + 1 >= g_bake.jobs.size() || g_bake.jobs[g_bake.job + 1].map != job.map;
            if (lastOfMap)
            {
                if (g_bake.mapOk && !g_map.empty())
                {
                    std::ofstream(BakedMarker(g_map)) << kBakedMarker << '\n';
                    spdlog::info("Debug world: '{}' baked ({} volumes)", g_map, g_shapes.size());
                }
                g_bake.mapOk = true;
            }
            ++g_bake.job;
            g_bake.phase = Bake::Phase::Start;
        }

        void TickBake()
        {
            if (!g_bake.running) return;
            const auto now = Clock::now();
            const auto secs = [&](Clock::time_point t) { return std::chrono::duration<double>(now - t).count(); };

            switch (g_bake.phase)
            {
                case Bake::Phase::Idle:
                    break;

                case Bake::Phase::Start:
                {
                    if (g_bake.job >= g_bake.jobs.size())
                    {
                        FinishBake(g_bake.failed ? "finished, " + std::to_string(g_bake.failed) + " checkpoint(s) failed"
                                                 : std::string("finished: every level is in the cache"), true);
                        return;
                    }
                    const BakeJob& job = g_bake.jobs[g_bake.job];
                    spdlog::info("Debug world: baking {} / {}: {}", g_bake.job + 1, g_bake.jobs.size(), job.label);
                    SetBakeStatus(job.label);
                    g_bake.loadsSeen = g_loadsFinished.load(std::memory_order_relaxed);
                    levels::Load(job.map, job.checkpoint, levels::Method::Game, false);
                    g_bake.phase = Bake::Phase::WaitLoad;
                    g_bake.since = now;
                    break;
                }

                case Bake::Phase::WaitLoad:
                    // The engine doesn't tick while it loads, so the load is seen through its hook, not IsLoading().
                    if (g_loadsFinished.load(std::memory_order_relaxed) > g_bake.loadsSeen && !game::IsLoading() &&
                        game::Pawn())
                    {
                        g_bake.phase = Bake::Phase::Settle;
                        g_bake.since = g_bake.lastNew = now;
                        g_bake.lastScan = Clock::time_point{};
                        g_bake.shapesAtStart = g_bake.shapesSeen = g_shapes.size();
                    }
                    else if (secs(g_bake.since) > kLoadTimeout)
                    {
                        spdlog::warn("Debug world: baking: {} did not load in {:.0f} s, skipped",
                                     g_bake.jobs[g_bake.job].label, kLoadTimeout);
                        EndJob(false);
                    }
                    break;

                case Bake::Phase::Settle:
                {
                    if (g_shapes.size() != g_bake.shapesSeen)
                    {
                        g_bake.shapesSeen = g_shapes.size();
                        g_bake.lastNew = now;
                    }
                    if (secs(g_bake.lastScan) >= kScanEvery)
                    {
                        g_forceScan = true;         // Update() scans right after this
                        g_bake.lastScan = now;
                    }
                    const double t = secs(g_bake.since);
                    if ((t >= kSettleMin && secs(g_bake.lastNew) >= kSettleQuiet) || t >= kSettleMax)
                    {
                        spdlog::info("Debug world: baking: {} done, {} new volumes ({} in '{}')",
                                     g_bake.jobs[g_bake.job].label, g_shapes.size() - g_bake.shapesAtStart,
                                     g_shapes.size(), g_map);
                        EndJob(true);
                    }
                    break;
                }
            }
        }
    }

    // =================================================================================================================
    namespace
    {
        void ClearCylinders()
        {
            g_cylinders.clear();
            g_cylindersNext.clear();
            g_cylinderScanAt = 0;
            if (g_cylindersShown) debugrender::SetDynamic({});
            g_cylindersShown = false;
        }

        // A slice of the object list: cylinder components of actors that aren't remembered volumes.
        void ScanCylinders()
        {
            const auto& objects = Classes::UObject::GetGlobalObjects();
            const std::size_t n = objects.Num();
            const std::size_t end = std::min(n, g_cylinderScanAt + kCylinderScanPerTick);
            // Objects of one class tend to sit together: remember the last class instead of a map lookup for each
            Classes::UClass* lastClass = nullptr;
            bool lastIsCylinder = false;
            for (std::size_t i = g_cylinderScanAt; i < end; ++i)
            {
                Classes::UObject* o = objects.GetByIndex(i);
                if (!o || !o->Class) continue;
                if (o->Class != lastClass)
                {
                    lastClass = o->Class;
                    lastIsCylinder = ClassOf(o).kind == Kind::Cylinder;
                }
                if (!lastIsCylinder) continue;
                auto* pc = static_cast<Classes::UPrimitiveComponent*>(o);
                Classes::AActor* owner = pc->Owner;
                if (!owner || !owner->Class || owner->bDeleteMe) continue;
                const ClassInfo& ownerInfo = ClassOf(owner);
                if (ownerInfo.skip || ownerInfo.category != kNoCategory) continue;   // Those are remembered volumes
                g_cylindersNext.push_back({pc, i});
            }
            g_cylinderScanAt = end;
            if (end >= n)
            {
                g_cylinders.swap(g_cylindersNext);
                g_cylindersNext.clear();
                g_cylinderScanAt = 0;
            }
        }

        // Every tick: the cylinders where they are now. The local player's own is left out: the camera is inside it, and
        // its walls would sweep across the screen as you move.
        std::size_t DrawCylinders(const Config& cfg, const rmath::Camera& cam, rmesh::MeshData& mesh)
        {
            constexpr auto kCyl = static_cast<std::size_t>(Category::Cylinder);
            if (!cfg.enabled[kCyl] || !cam.valid) return 0;
            ScanCylinders();

            const Classes::AActor* self = game::Pawn();
            std::size_t drawn = 0;
            rmesh::Builder builder(mesh);
            rmesh::Style style;
            style.color = cfg.colors[kCyl];
            style.fill = cfg.style != DrawStyle::Wireframe;
            style.outline = cfg.style != DrawStyle::Solid;
            builder.SetStyle(style);
            std::vector<Shape> tmp;
            for (const LiveCylinder& c : g_cylinders)
            {
                if (!game::AliveAt(c.component, c.index)) continue;
                Classes::AActor* owner = c.component->Owner;
                if (!owner || owner->bDeleteMe || (owner == self && !cfg.ownCylinder)) continue;
                tmp.clear();
                if (Extract(c.component, Kind::Cylinder, static_cast<int>(kCyl), std::string(), ToVec3(owner->Location), tmp) == 0)
                    continue;
                if (rmath::Length(tmp[0].center - cam.pos) - tmp[0].radius > cfg.maxDistance) continue;
                if (owner == self)
                {
                    // The camera is inside it: only its outline, or its walls would cover the screen
                    rmesh::Style own = style;
                    own.fill = false;
                    own.outline = true;
                    builder.SetStyle(own);
                    Build(builder, tmp[0]);
                    builder.SetStyle(style);
                }
                else Build(builder, tmp[0]);
                ++drawn;
            }
            return drawn;
        }

        // The volume under the crosshair, and the ones the player is in. Every tick: a bounding sphere test first, so
        // only the few volumes near the view ray or the player are looked at closely.
        void Inspect(const Config& cfg, const rmath::Camera& cam, rmesh::MeshData& dynamic)
        {
            Info::Look look;
            std::vector<std::pair<std::string, int>> inside;
            g_lookShape = -1;
            if (cfg.inspect && cam.valid)
            {
                RefreshShapeNames();
                const rmath::Mat4 r = rmath::RotationMatrix(cam.pitch, cam.yaw, cam.roll);
                const rmath::Vec3 dir{r.m[0][0], r.m[0][1], r.m[0][2]};
                float best = cfg.maxDistance;
                std::vector<std::pair<std::size_t, float>> hits;     // Every volume the view ray hits: shape, t
                const auto* pawn = game::Pawn();
                const rmath::Vec3 at = pawn ? ToVec3(pawn->Location) : cam.pos;
                std::string lastInside;
                for (std::size_t i = 0; i < g_shapes.size(); ++i)
                {
                    const Shape& s = g_shapes[i];
                    if (!cfg.enabled[s.category]) continue;
                    if (Contains(s, at) && g_shapeNames[i] != lastInside && inside.size() < 12)
                    {
                        lastInside = g_shapeNames[i];
                        inside.emplace_back(lastInside, s.category);
                    }
                    if (!Passes(cfg, i)) continue;
                    const float sphere = rgeom::RaySphere(cam.pos, dir, s.center, s.radius);
                    if (sphere < 0.f || sphere > best) continue;
                    const float t = RayShape(s, cam.pos, dir);
                    if (t <= 0.f || t >= cfg.maxDistance) continue;  // 0: the camera is inside it (that's the inside list)
                    hits.push_back({i, t});
                    if (t < best) { best = t; g_lookShape = static_cast<int>(i); }
                }
                if (cfg.nearby)
                {
                    std::vector<Info::Near> all;
                    for (std::size_t i = 0; i < g_shapes.size(); ++i)
                    {
                        const Shape& s = g_shapes[i];
                        if (s.name.empty() || !Passes(cfg, i)) continue;     // One per component: its first shape
                        // 0 only when really inside (any of its shapes); else to its center
                        const float toCenter = rmath::Length(s.center - at);
                        if (toCenter - s.radius > cfg.maxDistance) continue;
                        bool in = false;
                        for (std::size_t j = i; j < g_shapes.size() && !in && g_shapeNames[j] == g_shapeNames[i]; ++j)
                            in = (j == i || g_shapes[j].name.empty()) && Contains(g_shapes[j], at);
                        const float d = in ? 0.f : toCenter;
                        all.push_back({g_shapeNames[i], s.category, d, s.center.x, s.center.y, s.center.z});
                    }
                    const std::size_t n = std::min<std::size_t>(all.size(), 8);
                    std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n), all.end(),
                                      [](const Info::Near& a, const Info::Near& b) { return a.distance < b.distance; });
                    all.resize(n);
                    std::lock_guard lock(g_infoMutex);
                    g_info.nearby = std::move(all);
                }
                if (g_lookShape >= 0)
                {
                    const Shape& s = g_shapes[static_cast<std::size_t>(g_lookShape)];
                    look.valid = true;
                    look.name = g_shapeNames[static_cast<std::size_t>(g_lookShape)];
                    look.category = s.category;
                    look.distance = best;
                    look.size = s.radius * 2.f;
                    const rmath::Vec3 hit = cam.pos + dir * best;
                    look.x = hit.x; look.y = hit.y; look.z = hit.z;
                    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
                    for (const auto& [j, tj] : hits)
                    {
                        if (look.behind.size() >= 4) break;
                        const std::string& name = g_shapeNames[j];
                        const bool seen = name == look.name || std::any_of(look.behind.begin(), look.behind.end(),
                                                                           [&](const auto& b) { return b.first == name; });
                        if (!seen) look.behind.emplace_back(name, tj);
                    }

                    if (cfg.highlight)
                    {
                        // Every shape of the component, as a white outline
                        rmesh::Builder builder(dynamic);
                        rmesh::Style hl;
                        hl.color = 0xFFFFFFFFu;
                        hl.fill = false;
                        hl.outline = true;
                        builder.SetStyle(hl);
                        for (std::size_t i = 0; i < g_shapes.size(); ++i)
                            if (g_shapeNames[i] == look.name && rmath::Length(g_shapes[i].center - s.center) < 1.f + s.radius + g_shapes[i].radius)
                                Build(builder, g_shapes[i]);
                    }
                }
            }
            std::lock_guard lock(g_infoMutex);
            g_info.look = std::move(look);
            g_info.inside = std::move(inside);
        }

        // Every tick: the moving and inspected geometry, published together.
        void UpdateDynamic(const Config& cfg, const rmath::Camera& cam)
        {
            rmesh::MeshData mesh;
            const std::size_t cylinders = DrawCylinders(cfg, cam, mesh);
            Inspect(cfg, cam, mesh);
            {
                std::lock_guard lock(g_infoMutex);
                g_info.drawn[static_cast<std::size_t>(Category::Cylinder)] = cylinders;
                g_info.remembered[static_cast<std::size_t>(Category::Cylinder)] = g_cylinders.size();
            }
            const bool any = !mesh.tris.empty() || !mesh.lines.empty();
            if (any || g_cylindersShown) debugrender::SetDynamic(std::move(mesh));
            g_cylindersShown = any;
        }
    }

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
            ClearCylinders();       // Their objects go with the level
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
            g_shapes.clear(); g_shapeNamesFor = ~std::size_t{0};
            g_keys.clear();
            g_cacheDirty = false;
            g_fromDisk = cfg.useCache && LoadCache(map);
            g_lastObjectCount = 0;
            g_scan.at = kNotScanning;           // A scan of the old level stops here
            g_forceScan = !g_fromDisk || cfg.scanCached;
            g_dirty = true;
            debugrender::SetScene(nullptr);
            ClearCylinders();
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
            StartScan();
        }
        ScanSlice();

        // Write the cache once nothing new has turned up for a few seconds.
        if (g_cacheDirty && cfg.useCache && std::chrono::duration<double>(now - g_dirtySince).count() > 4.0)
        {
            SaveCache(g_map);
            g_cacheDirty = false;
            g_fromDisk = true;
        }
        PublishInfo();
        UpdateDynamic(cfg, game::GetCamera());

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
        constexpr std::size_t kMaxLabels = 300;          // The nearest ones: the overlay lays them out without overlaps

        const auto styleFor = [&](int category)
        {
            rmesh::Style style;
            style.color = cfg.colors[static_cast<std::size_t>(category)];
            style.fill = cfg.style != DrawStyle::Wireframe;
            style.outline = cfg.style != DrawStyle::Solid;
            return style;
        };
        struct LabelCandidate { const Shape* shape; std::uint32_t color; float distance; };
        std::vector<LabelCandidate> labelCandidates;
        const auto addLabel = [&](const Shape& s, std::uint32_t color, float distance)
        {
            if (cfg.labels && !s.name.empty() && distance <= cfg.labelDistance &&
                s.category != static_cast<int>(Category::Cylinder))
                labelCandidates.push_back({&s, color, distance});
        };

        std::array<std::size_t, kCategoryCount> perCategory{};
        std::size_t culled = 0;
        bool capHit = false;

        // Remembered volumes: just a distance test, then geometry. No contact with the game.
        RefreshShapeNames();
        std::array<std::size_t, kCategoryCount> remembered{};
        const float fadeFrom = cfg.maxDistance * 0.75f;     // The last quarter of the range fades out
        for (std::size_t i = 0; i < g_shapes.size(); ++i)
        {
            const Shape& s = g_shapes[i];
            if (!s.name.empty()) ++remembered[s.category];      // Components, not shapes
            if (!Passes(cfg, i)) continue;
            const float distance = rmath::Length(s.center - cam.pos) - s.radius;
            if (distance > cfg.maxDistance) { ++culled; continue; }
            if (scene->mesh.tris.size() + scene->mesh.lines.size() > kMaxVertices) { capHit = true; break; }

            const std::size_t tris = scene->mesh.tris.size(), lines = scene->mesh.lines.size();
            builder.SetStyle(styleFor(s.category));
            Build(builder, s);
            if (cfg.fade && distance > fadeFrom)
            {
                const float alpha = std::clamp((cfg.maxDistance - distance) / (cfg.maxDistance - fadeFrom), 0.f, 1.f);
                FadeFrom(scene->mesh.tris, tris, alpha);
                FadeFrom(scene->mesh.lines, lines, alpha);
            }
            if (!s.name.empty()) ++perCategory[s.category];
            addLabel(s, cfg.colors[s.category], distance);
        }
        // The nearest labels (scan order would leave out the ones right next to you in a busy level)
        if (labelCandidates.size() > kMaxLabels)
        {
            std::nth_element(labelCandidates.begin(), labelCandidates.begin() + kMaxLabels, labelCandidates.end(),
                             [](const LabelCandidate& a, const LabelCandidate& b) { return a.distance < b.distance; });
            labelCandidates.resize(kMaxLabels);
        }
        for (const LabelCandidate& c : labelCandidates) scene->labels.push_back({c.shape->center, c.shape->name, c.color});
        {
            std::lock_guard lock(g_infoMutex);
            for (int c = 0; c < kCategoryCount; ++c)
                if (c != static_cast<int>(Category::Cylinder))
                {
                    g_info.drawn[static_cast<std::size_t>(c)] = perCategory[static_cast<std::size_t>(c)];
                    g_info.remembered[static_cast<std::size_t>(c)] = remembered[static_cast<std::size_t>(c)];
                }
        }

        // Live cylinders are drawn every tick on their own (DrawCylinders).
        perCategory[static_cast<std::size_t>(Category::Cylinder)] = g_cylinders.size();

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
        ClearCylinders();
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
            if (!g_map.empty())
            {
                fs::remove(CachePath(g_map), ec);
                fs::remove(BakedMarker(g_map), ec);
            }
            g_shapes.clear(); g_shapeNamesFor = ~std::size_t{0};
            g_keys.clear();
            g_cacheDirty = false;
            g_fromDisk = false;
            g_scan.at = kNotScanning;   // Start over: what it found so far is gone
            g_forceScan = true;
            g_dirty = true;
            PublishInfo();
        });
    }

    void BakeAllLevels(bool onlyMissing)
    {
        game::RunOnGameThread([onlyMissing]
        {
            if (g_bake.running) return;
            static bool hooked = false;
            if (!hooked)
            {
                game::OnLevelLoad(&OnLevelLoaded);
                hooked = true;
            }

            g_bake = Bake{};
            const auto maps = levels::Maps();
            const auto baked = onlyMissing ? BakedMaps() : std::unordered_set<std::string>{};
            std::vector<int> order;
            for (int m = 0; m < static_cast<int>(maps.size()); ++m)
            {
                const std::string file = MapFile(maps[m].label);
                if (file == "tdmainmenu" || baked.count(file)) continue;
                order.push_back(m);
            }
            std::stable_sort(order.begin(), order.end(), [&](int x, int y)
            {
                return StoryRank(MapFile(maps[x].label)) < StoryRank(MapFile(maps[y].label));
            });
            for (int m : order)
            {
                // Checkpoint A is the level start: loading the start as well would load the same place twice
                const auto& cps = maps[m].checkpoints;
                for (int c = cps.empty() ? 0 : 1; c <= static_cast<int>(cps.size()); ++c)
                    g_bake.jobs.push_back({m, c, maps[m].label + " - " + (c == 0 ? std::string("start") : cps[c - 1])});
            }
            if (g_bake.jobs.empty())
            {
                SetBakeStatus(maps.empty() ? "no levels known yet" : "nothing to do: every level is baked already");
                return;
            }

            g_bake.startMap = Lower(g_map);
            g_bake.running = true;
            g_bake.phase = Bake::Phase::Start;
            spdlog::info("Debug world: baking {} levels ({} checkpoints){}", order.size(), g_bake.jobs.size(),
                         onlyMissing ? ", the ones not baked yet" : "");
            SetBakeStatus("starting");
        });
    }

    void CancelBake()
    {
        game::RunOnGameThread([]
        {
            if (!g_bake.running) return;
            if (g_cacheDirty) { SaveCache(g_map); g_cacheDirty = false; g_fromDisk = true; }   // What this one found
            FinishBake("cancelled after " + std::to_string(g_bake.job) + " of " + std::to_string(g_bake.jobs.size()) +
                       " checkpoints", true);
        });
    }

    void FaceTowards(float x, float y, float z)
    {
        game::RunOnGameThread([x, y, z] {
            auto* pc = game::Controller();
            const rmath::Camera cam = game::GetCamera();
            if (!pc || !cam.valid) return;
            const rmath::Vec3 d = rmath::Vec3{x, y, z} - cam.pos;
            const float k = 32768.f / rmath::kPi;
            const float yaw = std::atan2(d.y, d.x) * k;
            const float pitch = std::clamp(std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)) * k, -16000.f, 16000.f);
            pc->Rotation.Yaw = static_cast<unsigned>(static_cast<int>(std::lround(yaw)));
            pc->Rotation.Pitch = static_cast<unsigned>(static_cast<int>(std::lround(pitch))) & 0xFFFFu;
        });
    }

    const char* CategoryName(int category)
    {
        static const char* const names[kCategoryCount] = {"trigger", "kill zone", "checkpoint", "blocking volume",
                                                          "movement", "volume", "cylinder", "spawn point", "pickup",
                                                          "AI only", "navigation point", "post process volume",
                                                          "audio volume", "fog volume", "streaming volume"};
        return category >= 0 && category < kCategoryCount ? names[category] : "?";
    }

    Info GetInfo()
    {
        std::lock_guard lock(g_infoMutex);
        return g_info;
    }
}
