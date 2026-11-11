#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Collects trigger, kill, checkpoint, blocking and other volumes from the game (through the Mirage SDK, sdk/), and the
// actors placed with them (player starts, pickups, AI-only volumes and areas, navigation points), and publishes them to
// the debug renderer as solid, optionally textured shapes. Everything here runs on the game thread.
//
// The volumes of a level never change, so they are scanned once and remembered: per map in memory, and on disk in
// <dll folder>/cache/<map>.mevc ("pregenerated"). The next time the level is loaded its volumes are there at once and
// the engine's object list does not have to be walked for them. Static meshes are not collected at all.
namespace debugworld {
    // The order is stored in the cache files: new categories go at the end.
    // Volume is what is left of the "other volumes" once these are taken out: PostProcess (post process, color, light
    // and reflection volumes), Audio (reverb), Fog, Streaming (level streaming, cull distance, portals).
    enum class Category { Trigger, Kill, Checkpoint, Blocking, Movement, Volume, Cylinder, Spawn, Pickup, AI, Navigation,
                          PostProcess, Audio, Fog, Streaming, Count };
    constexpr int kCategoryCount = static_cast<int>(Category::Count);
    // The "other volumes" group: Volume and its sub-categories.
    constexpr bool IsOtherVolume(int category)
    {
        return category == static_cast<int>(Category::Volume) ||
               (category >= static_cast<int>(Category::PostProcess) && category <= static_cast<int>(Category::Streaming));
    }

    enum class DrawStyle { Solid, SolidOutline, Wireframe };

    struct Config {
        DrawStyle style = DrawStyle::SolidOutline;
        float maxDistance = 6000.f;                 // World units from the camera
        float updateHz = 3.f;                       // How often the scene is rebuilt from the remembered shapes
        bool labels = false;                        // Class names of triggers/volumes
        float labelDistance = 3000.f;               // Labels only for volumes this close (the nearest first)
        bool textures = true;                       // Tiled icon on the face (skull for kill zones, flag for checkpoints, ...)
        float tileSize = 160.f;                     // World units covered by one icon
        bool useCache = true;                       // Read and write the per-level cache on disk
        bool scanCached = true;                     // Keep looking for volumes that streamed in after the cache was written
        bool fade = true;                           // Fade volumes out toward maxDistance instead of popping
        std::string filter;                         // Only volumes whose name contains this (lower case); empty: all
        bool ownCylinder = false;                   // The local player's own cylinder (as an outline)
        bool inspect = true;                        // Look-at and inside tests (Info::look, Info::inside)
        bool highlight = true;                      // Outline the looked-at volume
        bool nearby = false;                        // Fill Info::nearby (the panel is open)
        std::array<bool, kCategoryCount> enabled{};
        std::array<std::uint32_t, kCategoryCount> colors{};     // 0xAARRGGBB
    };

    struct Info {
        std::string map;                            // Level currently remembered
        std::size_t shapes = 0;                     // Remembered volumes (all categories)
        bool fromDisk = false;                      // The cache file of this level was used
        bool cacheDirty = false;                    // Volumes found that are not on disk yet
        double lastScanMs = 0.0;
        std::size_t scans = 0;
        bool baking = false;                        // A bake is running
        std::string bakeStatus;                     // The checkpoint being baked, or the result of the last bake
        std::size_t bakeDone = 0, bakeTotal = 0;    // Checkpoints (a level without any: its start)
        std::size_t bakeFailed = 0;                 // Ones that never finished loading (skipped)

        // Volumes (components) per category: drawn now, and remembered for this level (cylinders: found live)
        std::array<std::size_t, kCategoryCount> drawn{}, remembered{};

        // The volume under the crosshair (Config::inspect)
        struct Look {
            bool valid = false;
            std::string name;
            int category = 0;
            float distance = 0.f;                   // To where the view ray hits it
            float size = 0.f;                       // Bounding sphere diameter
            float x = 0.f, y = 0.f, z = 0.f;        // The hit
            std::vector<std::pair<std::string, float>> behind;     // Other volumes further along the ray, and where
        } look;
        // Volumes the player is inside now: name and category (Config::inspect)
        std::vector<std::pair<std::string, int>> inside;
        // The closest volumes to the player, nearest first (Config::nearby)
        struct Near {
            std::string name;
            int category = 0;
            float distance = 0.f;                   // From the player to its center (0: inside it)
            float x = 0.f, y = 0.f, z = 0.f;        // Its center
        };
        std::vector<Near> nearby;
    };

    // Turns the player's view toward a point (game thread, next tick). For looking at a volume from the Nearby list.
    void FaceTowards(float x, float y, float z);

    const char* CategoryName(int category);         // "trigger", "kill zone", ...

    void Update(const Config& config);              // Call every tick: rebuilds the scene when it is due
    void RequestRebuild();                          // Rebuild on the next Update
    void Clear();                                   // Drop the published scene (the remembered volumes stay)

    void RescanNow();                               // Walk the object list for volumes on the next Update
    void ClearCache();                              // Forget the remembered volumes of this level, delete its file, rescan

    // Loads every checkpoint (A is the level start) in story order, waits for each to load and for its volumes to stop turning
    // up, and writes them to the cache. A level whose checkpoints all worked gets cache/<map>.baked; `onlyMissing`
    // skips those. Afterwards (or when cancelled) the player is taken back to the level they were in. Needs the
    // collision view to be on (it drives the scan).
    void BakeAllLevels(bool onlyMissing);
    void CancelBake();

    Info GetInfo();                                 // Any thread
}