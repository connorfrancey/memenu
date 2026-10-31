#pragma once
#include "math.h"
#include "mesh.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DSurface9;

// Draws world-space debug geometry (collision view) with Direct3D 9.
//
// The game thread publishes an immutable Scene plus the camera; the render thread draws it. Preferably inside the
// game's frame, when the engine resolves its scene color (StretchRect) while the world's depth buffer is still bound, so
// the world hides the volumes behind it. When no such resolve is seen it falls back to an overlay without depth.
namespace debugrender {
    enum class Depth { Scene, Overlay };            // Tested against the world's depth / always on top
    enum class Path { None, InScene, Overlay };     // How the last frame was drawn

    struct Style {
        float opacity = 0.55f;          // Faces; outlines are always opaque
        Depth depth = Depth::Scene;
        float fovOffset = 0.f;          // Degrees added to the camera FOV
        float aspect = 0.f;             // 0 = from the viewport
        float nearPlane = 10.f;         // Used when the engine's near plane is unknown
    };

    struct Scene {
        rmesh::MeshData mesh;
        std::vector<rmesh::Label> labels;
        std::size_t shapes = 0;
        double buildMs = 0.0;
        std::uint64_t version = 0;      // Stamped by SetScene
    };

    struct Stats {
        bool drawn = false;
        Path path = Path::None;
        bool viewFromDevice = false;
        float nearUsed = 0.f;
        std::size_t triangles = 0, lines = 0, shapes = 0, labels = 0;
        double buildMs = 0.0;
        bool sceneHookSeen = false;
        bool overlayFallback = false;
    };

    // ---- Any thread -------------------------------------------------------------------------------------------------
    void SetActive(bool active);
    void SetStyle(const Style& style);
    void SetCamera(const rmath::Camera& camera);
    void SetEngineNear(float nearPlane);            // 0 = unknown
    void SetScene(std::shared_ptr<const Scene> scene);  // nullptr clears
    std::shared_ptr<const Scene> CurrentScene();

    // ---- Render thread ----------------------------------------------------------------------------------------------
    void OnStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, IDirect3DSurface9* dst);  // Before the real call
    void BeginFrame(IDirect3DDevice9* dev);         // EndScene, before Render
    void Render(IDirect3DDevice9* dev);             // EndScene: overlay path
    void EndFrame();                                // After the frame was presented
    bool WorldToScreen(rmath::Vec3 world, float& x, float& y);  // With the last BeginFrame's matrices
    Stats GetStats();
    void OnLostDevice();                            // Before IDirect3DDevice9::Reset
    void Shutdown();
}
