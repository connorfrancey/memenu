#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Collects trigger, kill, checkpoint, blocking and other volumes from the game (through the mmultiplayer SDK) and
// publishes them to the debug renderer as solid, optionally textured shapes. Everything here runs on the game thread.
//
// The volumes of a level never change, so they are scanned once and remembered: per map in memory, and on disk in
// <dll folder>/cache/<map>.mevc ("pregenerated"). The next time the level is loaded its volumes are there at once and
// the engine's object list does not have to be walked for them. Static meshes are not collected at all.
namespace debugworld {
    enum class Category { Trigger, Kill, Checkpoint, Blocking, Movement, Volume, Cylinder, Count };
    constexpr int kCategoryCount = static_cast<int>(Category::Count);

    enum class DrawStyle { Solid, SolidOutline, Wireframe };

    struct Config {
        DrawStyle style = DrawStyle::SolidOutline;
        float maxDistance = 6000.f;                 // World units from the camera
        float updateHz = 3.f;                       // How often the scene is rebuilt from the remembered shapes
        bool labels = false;                        // Class names of triggers/volumes
        bool textures = true;                       // Tiled icon on the face (skull for kill zones, flag for checkpoints, ...)
        float tileSize = 160.f;                     // World units covered by one icon
        bool useCache = true;                       // Read and write the per-level cache on disk
        bool scanCached = true;                     // Keep looking for volumes that streamed in after the cache was written
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
        bool baking = false;                        // "Bake all levels" is running
    };

    void Update(const Config& config);              // Call every tick: rebuilds the scene when it is due
    void RequestRebuild();                          // Rebuild on the next Update
    void Clear();                                   // Drop the published scene (the remembered volumes stay)

    void RescanNow();                               // Walk the object list for volumes on the next Update
    void ClearCache();                              // Forget the remembered volumes of this level, delete its file, rescan

    // Loads every level and checkpoint in turn so that their volumes are scanned and written to the cache. Takes a few
    // minutes; the game ends up in the last level. Needs the collision view to be on (it drives the scan).
    void BakeAllLevels();
    void CancelBake();

    Info GetInfo();                                 // Any thread
}