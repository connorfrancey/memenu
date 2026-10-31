#include "debug_renderer.h"

#include <d3d9.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace debugrender
{
    namespace
    {
        constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
        static_assert(sizeof(rmesh::Vertex) == 16, "vertex layout must match D3DFVF_XYZ | D3DFVF_DIFFUSE");
        constexpr std::size_t kMaxPrimsPerDraw = 60000; // Stay below the smallest MaxPrimitiveCount on old hardware
        constexpr float kDepthScale = 0.9999f;          // Pulls the volumes a hair toward the camera: coplanar faces win
        constexpr int kFallbackAfterFrames = 120;       // Frames without a scene resolve before the overlay takes over
        constexpr int kMaxDiagnostics = 48;             // StretchRect calls described in the log

        // ---- Shared between threads --------------------------------------------------------------------------------
        std::mutex g_mutex;
        bool g_active = false;
        Style g_style;
        rmath::Camera g_camera;
        float g_engineNear = 0.f;
        std::shared_ptr<const Scene> g_scene;
        std::uint64_t g_nextVersion = 1;

        // ---- Render thread only ------------------------------------------------------------------------------------
        struct FrameSetup
        {
            bool valid = false;
            rmath::Mat4 view{}, proj{}, viewProj{};
            float vpW = 0.f, vpH = 0.f;
            bool viewFromDevice = false;
            float nearUsed = 0.f;
        };
        FrameSetup g_endSetup; // For labels: the setup of the last EndScene

        struct DynBuffer
        {
            IDirect3DVertexBuffer9* vb = nullptr;
            std::size_t capacity = 0; // In vertices
            std::size_t count = 0;    // Uploaded vertices
            const char* name = "";
        };
        DynBuffer g_tris{nullptr, 0, 0, "triangle"};
        DynBuffer g_lines{nullptr, 0, 0, "line"};
        std::uint64_t g_uploadedVersion = 0;

        IDirect3DStateBlock9* g_stateBlock = nullptr;
        bool g_loggedFailure = false;

        Stats g_stats;
        bool g_drewInScene = false;         // This frame
        int g_framesWithoutScenePass = 0;
        bool g_fallbackActive = false;
        bool g_sceneHookSeen = false;
        int g_diagnosticsLeft = kMaxDiagnostics;
        std::uint64_t g_frameCounter = 0;
        UINT g_backW = 0, g_backH = 0;

        // ---- Logging state (render thread only): log transitions, not every frame -----------------------------------
        bool g_lastViewFromDevice = false;
        bool g_viewStateKnown = false;
        float g_lastNear = -1.f;
        UINT g_lastVpW = 0, g_lastVpH = 0;
        bool g_lastDrawn = false;
        Path g_lastPath = Path::None;

        template <class T>
        void Release(T*& p)
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }

        DWORD FloatBits(float f)
        {
            DWORD d;
            std::memcpy(&d, &f, sizeof d);
            return d;
        }

        unsigned Hr(HRESULT hr) { return static_cast<unsigned>(hr); }

        void LogOnce(const char* what, HRESULT hr)
        {
            // Every failure is visible at debug level; the error level is only emitted once until the next device reset.
            spdlog::debug("Debug renderer: {} failed (0x{:08X})", what, Hr(hr));
            if (g_loggedFailure) return;
            g_loggedFailure = true;
            spdlog::error("Debug renderer: {} failed (0x{:08X}); it will keep retrying silently", what, Hr(hr));
        }

        void ReleaseBuffer(DynBuffer& b)
        {
            Release(b.vb);
            b.capacity = b.count = 0;
        }

        bool EnsureBuffer(IDirect3DDevice9* dev, DynBuffer& b, std::size_t needed)
        {
            if (b.vb && b.capacity >= needed) return true;

            const bool hadBuffer = b.vb != nullptr;
            Release(b.vb);
            b.capacity = std::max<std::size_t>(needed + needed / 4, 4096);
            const HRESULT hr = dev->CreateVertexBuffer(static_cast<UINT>(b.capacity * sizeof(rmesh::Vertex)),
                                                       D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_DEFAULT,
                                                       &b.vb, nullptr);
            if (FAILED(hr))
            {
                spdlog::error("Debug renderer: failed to create {} vertex buffer ({} vertices, {} bytes): 0x{:08X}",
                              b.name, b.capacity, b.capacity * sizeof(rmesh::Vertex), Hr(hr));
                LogOnce("CreateVertexBuffer", hr);
                b.capacity = 0;
                return false;
            }
            spdlog::info("Debug renderer: {} {} vertex buffer ({} vertices, {} KiB)", hadBuffer ? "grew" : "created",
                         b.name, b.capacity, b.capacity * sizeof(rmesh::Vertex) / 1024);
            return true;
        }

        bool Upload(IDirect3DDevice9* dev, DynBuffer& b, const std::vector<rmesh::Vertex>& data)
        {
            b.count = 0;
            if (data.empty()) return true;
            if (!EnsureBuffer(dev, b, data.size())) return false;

            void* dst = nullptr;
            const UINT bytes = static_cast<UINT>(data.size() * sizeof(rmesh::Vertex));
            const HRESULT hr = b.vb->Lock(0, bytes, &dst, D3DLOCK_DISCARD);
            if (FAILED(hr))
            {
                spdlog::error("Debug renderer: failed to lock {} vertex buffer ({} bytes): 0x{:08X}", b.name, bytes, Hr(hr));
                LogOnce("VertexBuffer::Lock", hr);
                return false;
            }
            std::memcpy(dst, data.data(), bytes);
            b.vb->Unlock();
            b.count = data.size();
            return true;
        }

        void DrawRange(IDirect3DDevice9* dev, const DynBuffer& b, D3DPRIMITIVETYPE type, UINT perPrim, std::size_t first,
                       std::size_t vertexCount)
        {
            if (!b.vb || vertexCount == 0) return;
            if (const HRESULT hr = dev->SetStreamSource(0, b.vb, 0, sizeof(rmesh::Vertex)); FAILED(hr))
            {
                LogOnce("SetStreamSource", hr);
                return;
            }
            const std::size_t prims = vertexCount / perPrim;
            for (std::size_t start = 0; start < prims; start += kMaxPrimsPerDraw)
            {
                const std::size_t n = std::min(kMaxPrimsPerDraw, prims - start);
                const HRESULT hr = dev->DrawPrimitive(type, static_cast<UINT>(first + start * perPrim), static_cast<UINT>(n));
                if (FAILED(hr))
                {
                    spdlog::warn("Debug renderer: DrawPrimitive failed ({} buffer, start={}, count={}): 0x{:08X}", b.name,
                                 first + start * perPrim, n, Hr(hr));
                    LogOnce("DrawPrimitive", hr);
                    return;
                }
            }
        }

        // ---- Matrices ----------------------------------------------------------------------------------------------
        // The engine's own view matrix, if the device has one that is plausible: a rigid world->view transform that agrees
        // with the camera the game reported. It belongs to the frame being rendered, which the game camera (copied after
        // the last tick) may not.
        bool PlausibleDeviceView(const rmath::Mat4& d, const rmath::Camera& cam)
        {
            for (const auto& row : d.m)
                for (float f : row)
                    if (!std::isfinite(f)) return false;
            if (std::fabs(d.m[0][3]) > 1e-3f || std::fabs(d.m[1][3]) > 1e-3f || std::fabs(d.m[2][3]) > 1e-3f ||
                std::fabs(d.m[3][3] - 1.f) > 1e-3f)
                return false;
            // Orthonormal rotation
            for (int i = 0; i < 3; ++i)
                for (int j = i; j < 3; ++j)
                {
                    const float dot = d.m[i][0] * d.m[j][0] + d.m[i][1] * d.m[j][1] + d.m[i][2] * d.m[j][2];
                    if (std::fabs(dot - (i == j ? 1.f : 0.f)) > 0.02f) return false;
                }
            // Same orientation as the game camera (generous: the camera may be a tick away)
            const rmath::Mat4 an = rmath::ViewMatrix(cam);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    if (std::fabs(d.m[i][j] - an.m[i][j]) > 0.35f) return false;
            // Same position: p = -t * R^T
            const rmath::Vec3 pos = {
                -(d.m[3][0] * d.m[0][0] + d.m[3][1] * d.m[0][1] + d.m[3][2] * d.m[0][2]),
                -(d.m[3][0] * d.m[1][0] + d.m[3][1] * d.m[1][1] + d.m[3][2] * d.m[1][2]),
                -(d.m[3][0] * d.m[2][0] + d.m[3][1] * d.m[2][1] + d.m[3][2] * d.m[2][2])
            };
            return rmath::Length(pos - cam.pos) < 300.f;
        }

        // View/projection for the device's current viewport. Returns false when there is nothing to draw with.
        bool Prepare(IDirect3DDevice9* dev, const Style& style, const rmath::Camera& camera, float engineNear, FrameSetup& out)
        {
            out.valid = false;
            if (!camera.valid) return false;

            D3DVIEWPORT9 vp{};
            if (FAILED(dev->GetViewport(&vp)) || vp.Width == 0 || vp.Height == 0) return false;
            if (vp.Width != g_lastVpW || vp.Height != g_lastVpH)
            {
                spdlog::info("Debug renderer: viewport {}x{} (was {}x{})", vp.Width, vp.Height, g_lastVpW, g_lastVpH);
                g_lastVpW = vp.Width;
                g_lastVpH = vp.Height;
            }
            out.vpW = static_cast<float>(vp.Width);
            out.vpH = static_cast<float>(vp.Height);

            const float aspect = style.aspect > 0.f ? style.aspect : out.vpW / out.vpH;
            const float fov = std::clamp(camera.fov + style.fovOffset, 10.f, 170.f);
            const bool engineNearOk = engineNear >= 1.f && engineNear <= 200.f;
            out.nearUsed = engineNearOk ? engineNear : style.nearPlane;

            out.view = rmath::ViewMatrix(camera);
            out.viewFromDevice = false;
            D3DMATRIX dv{};
            if (SUCCEEDED(dev->GetTransform(D3DTS_VIEW, &dv)))
            {
                rmath::Mat4 deviceView;
                std::memcpy(&deviceView, &dv, sizeof deviceView);
                if (PlausibleDeviceView(deviceView, camera))
                {
                    out.view = deviceView;
                    out.viewFromDevice = true;
                }
            }
            if (!g_viewStateKnown || out.viewFromDevice != g_lastViewFromDevice)
            {
                spdlog::info("Debug renderer: view matrix from {}", out.viewFromDevice ? "the engine (device transform)" : "the game camera");
                g_lastViewFromDevice = out.viewFromDevice;
                g_viewStateKnown = true;
            }
            if (out.nearUsed != g_lastNear)
            {
                spdlog::info("Debug renderer: near plane {:.1f} ({})", out.nearUsed, engineNearOk ? "engine" : "setting");
                g_lastNear = out.nearUsed;
            }

            out.proj = rmath::ProjectionMatrix(fov, aspect, out.nearUsed);
            // Depth = scale * (z - near) / z: the engine's infinite-far depth, nudged toward the camera.
            out.proj.m[2][2] *= kDepthScale;
            out.proj.m[3][2] *= kDepthScale;
            out.viewProj = rmath::Mul(out.view, out.proj);
            out.valid = true;
            return true;
        }

        // Draws the scene with the device's current render target. With `useDepth` the currently bound depth surface is
        // tested against and never written. Everything is restored through a state block.
        bool DrawScene(IDirect3DDevice9* dev, const FrameSetup& setup, const Style& style, const Scene& scene, bool useDepth)
        {
            // Upload when a new scene was published.
            if (scene.version != g_uploadedVersion)
            {
                const bool ok = Upload(dev, g_tris, scene.mesh.tris) && Upload(dev, g_lines, scene.mesh.lines);
                if (!ok) return false; // Retried next frame (version stays old)
                g_uploadedVersion = scene.version;
            }
            if (g_tris.count == 0 && g_lines.count == 0) return false;

            if (!g_stateBlock)
            {
                const HRESULT hr = dev->CreateStateBlock(D3DSBT_ALL, &g_stateBlock);
                if (FAILED(hr))
                {
                    LogOnce("CreateStateBlock", hr);
                    return false;
                }
                spdlog::info("Debug renderer: created state block");
            }
            if (const HRESULT hr = g_stateBlock->Capture(); FAILED(hr))
            {
                LogOnce("StateBlock::Capture", hr);
                return false;
            }

            // Fixed-function pipeline, unlit, vertex colors.
            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
            dev->SetFVF(kFvf);

            D3DMATRIX world{}; // D3DMATRIX is a struct inside a union: no nested-brace init
            world._11 = world._22 = world._33 = world._44 = 1.f;
            dev->SetTransform(D3DTS_WORLD, &world);
            dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(&setup.view));
            dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(&setup.proj));

            const bool blend = style.opacity < 0.999f;
            dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
            dev->SetRenderState(D3DRS_ZENABLE, useDepth ? D3DZB_TRUE : D3DZB_FALSE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
            dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
            dev->SetRenderState(D3DRS_CLIPPING, TRUE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_DEPTHBIAS, FloatBits(0.f));
            dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, FloatBits(0.f));

            // Alpha = vertex alpha * texture factor (the opacity); color = vertex color. No texture is bound.
            dev->SetTexture(0, nullptr);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
            dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

            // 1. Faces, at the requested opacity.
            const DWORD faceAlpha = static_cast<DWORD>(std::clamp(style.opacity, 0.f, 1.f) * 255.f + 0.5f);
            dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(faceAlpha, 255, 255, 255));
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, blend ? TRUE : FALSE);
            DrawRange(dev, g_tris, D3DPT_TRIANGLELIST, 3, 0, g_tris.count);

            // 2. Outlines: always opaque, slightly toward the camera so they do not z-fight with the faces they sit on.
            dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, 255, 255, 255));
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_DEPTHBIAS, FloatBits(-1.5e-5f));
            DrawRange(dev, g_lines, D3DPT_LINELIST, 2, 0, g_lines.count);

            if (const HRESULT hr = g_stateBlock->Apply(); FAILED(hr))
            {
                spdlog::error("Debug renderer: failed to restore device state (0x{:08X}); game rendering may be affected", Hr(hr));
                LogOnce("StateBlock::Apply", hr);
            }
            return true;
        }

        void Describe(const char* what, IDirect3DDevice9* dev, IDirect3DSurface9* src, IDirect3DSurface9* dst, bool rtMatch,
                      const D3DSURFACE_DESC& sd, IDirect3DSurface9* depth, bool accepted)
        {
            D3DSURFACE_DESC dd{};
            if (dst) dst->GetDesc(&dd);
            D3DSURFACE_DESC zd{};
            if (depth) depth->GetDesc(&zd);
            (void)dev;
            (void)src;
            spdlog::info("Debug renderer: {} src {}x{} fmt={} ms={} rt={} | dst {}x{} | current RT0 {} | depth {}x{} fmt={} -> {}",
                         what, sd.Width, sd.Height, static_cast<int>(sd.Format), static_cast<int>(sd.MultiSampleType),
                         (sd.Usage & D3DUSAGE_RENDERTARGET) ? "yes" : "no", dst ? dd.Width : 0, dst ? dd.Height : 0,
                         rtMatch ? "is the source" : "differs", depth ? zd.Width : 0, depth ? zd.Height : 0,
                         depth ? static_cast<int>(zd.Format) : 0, accepted ? "DRAW HERE" : "skipped");
        }

        void NoteDrawn(Path path, const Scene& scene, bool viewFromDevice, float nearUsed)
        {
            g_stats.drawn = true;
            g_stats.path = path;
            g_stats.viewFromDevice = viewFromDevice;
            g_stats.nearUsed = nearUsed;
            g_stats.triangles = g_tris.count / 3;
            g_stats.lines = g_lines.count / 2;
            g_stats.shapes = scene.shapes;
            g_stats.labels = scene.labels.size();
            g_stats.buildMs = scene.buildMs;
            if (!g_lastDrawn || path != g_lastPath)
                spdlog::info("Debug renderer: drawing {} ({} triangles, {} lines)",
                             path == Path::InScene ? "inside the game's frame with its depth buffer" : "as an overlay (no depth)",
                             g_stats.triangles, g_stats.lines);
            g_lastDrawn = true;
            g_lastPath = path;
        }
    }

    // =================================================================================================================
    // Any thread
    void SetActive(bool active)
    {
        bool changed;
        {
            std::lock_guard lock(g_mutex);
            changed = g_active != active;
            g_active = active;
        }
        if (changed) spdlog::info("Debug renderer: {}", active ? "activated" : "deactivated");
    }

    void SetStyle(const Style& style)
    {
        bool changed;
        {
            std::lock_guard lock(g_mutex);
            changed = g_style.opacity != style.opacity || g_style.depth != style.depth || g_style.fovOffset != style.fovOffset || g_style.aspect != style.aspect || g_style.nearPlane != style.nearPlane;
            g_style = style;
        }
        // Called every tick: only log when something actually changed.
        if (changed)
            spdlog::debug("Debug renderer: style updated (opacity={:.2f}, depth={}, fovOffset={:.1f}, nearPlane={:.1f}, aspect={:.3f})",
                          style.opacity, style.depth == Depth::Scene ? "scene" : "overlay", style.fovOffset,
                          style.nearPlane, style.aspect);
    }

    void SetCamera(const rmath::Camera& camera)
    {
        std::lock_guard lock(g_mutex);
        g_camera = camera;
    }

    void SetEngineNear(float nearPlane)
    {
        std::lock_guard lock(g_mutex);
        g_engineNear = nearPlane;
    }

    void SetScene(std::shared_ptr<const Scene> scene)
    {
        if (scene)
        {
            // Scenes are published immutable: stamp the version through a const_cast on our own, still unshared object.
            auto* mutableScene = const_cast<Scene*>(scene.get());
            std::uint64_t version;
            {
                std::lock_guard lock(g_mutex);
                version = g_nextVersion++;
                mutableScene->version = version;
                g_scene = std::move(scene);
            }
            spdlog::trace("Debug renderer: scene v{} published ({} shapes, {} tri verts, {} line verts, {} labels)", version,
                          mutableScene->shapes, mutableScene->mesh.tris.size(), mutableScene->mesh.lines.size(),
                          mutableScene->labels.size());
        }
        else
        {
            {
                std::lock_guard lock(g_mutex);
                g_scene.reset();
            }
            spdlog::debug("Debug renderer: scene cleared");
        }
    }

    std::shared_ptr<const Scene> CurrentScene()
    {
        std::lock_guard lock(g_mutex);
        return g_scene;
    }

    // =================================================================================================================
    // Render thread
    void OnStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, IDirect3DSurface9* dst)
    {
        if (!src) return;

        bool active;
        Style style;
        rmath::Camera camera;
        float engineNear;
        std::shared_ptr<const Scene> scene;
        {
            std::lock_guard lock(g_mutex);
            active = g_active;
            style = g_style;
            camera = g_camera;
            engineNear = g_engineNear;
            scene = g_scene;
        }
        if (!active || !scene || g_drewInScene || style.depth != Depth::Scene) return;

        IDirect3DSurface9* rt0 = nullptr;
        if (FAILED(dev->GetRenderTarget(0, &rt0)) || !rt0) return;
        const bool rtMatch = rt0 == src;
        rt0->Release();

        D3DSURFACE_DESC sd{};
        src->GetDesc(&sd);

        if (g_backW == 0)
        {
            IDirect3DSurface9* back = nullptr;
            if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back)
            {
                D3DSURFACE_DESC bd{};
                back->GetDesc(&bd);
                g_backW = bd.Width;
                g_backH = bd.Height;
                back->Release();
            }
        }

        // The engine resolves its scene color: the source is the render target that is bound right now, it is about as
        // large as the screen, and the depth buffer that was used to render the world is still bound.
        IDirect3DSurface9* depth = nullptr;
        dev->GetDepthStencilSurface(&depth);
        const bool bigEnough = g_backW == 0 || (sd.Width * 2 >= g_backW && sd.Height * 2 >= g_backH);
        const bool accepted = rtMatch && depth && (sd.Usage & D3DUSAGE_RENDERTARGET) && bigEnough;

        if (g_diagnosticsLeft > 0)
        {
            --g_diagnosticsLeft;
            Describe("StretchRect", dev, src, dst, rtMatch, sd, depth, accepted);
        }
        if (!accepted)
        {
            Release(depth);
            return;
        }
        Release(depth);

        FrameSetup setup;
        if (!Prepare(dev, style, camera, engineNear, setup)) return;
        g_sceneHookSeen = true;
        if (DrawScene(dev, setup, style, *scene, true))
        {
            g_drewInScene = true;
            NoteDrawn(Path::InScene, *scene, setup.viewFromDevice, setup.nearUsed);
        }
    }

    void BeginFrame(IDirect3DDevice9* dev)
    {
        bool active;
        Style style;
        rmath::Camera camera;
        float engineNear;
        {
            std::lock_guard lock(g_mutex);
            active = g_active;
            style = g_style;
            camera = g_camera;
            engineNear = g_engineNear;
        }
        g_endSetup.valid = false;
        if (!active) return;
        Prepare(dev, style, camera, engineNear, g_endSetup);
    }

    bool WorldToScreen(rmath::Vec3 world, float& x, float& y)
    {
        return g_endSetup.valid && rmath::Project(g_endSetup.viewProj, world, g_endSetup.vpW, g_endSetup.vpH, x, y);
    }

    Stats GetStats()
    {
        Stats s = g_stats;
        s.sceneHookSeen = g_sceneHookSeen;
        s.overlayFallback = g_fallbackActive;
        return s;
    }

    void Render(IDirect3DDevice9* dev)
    {
        if (!g_endSetup.valid)
        {
            g_stats.drawn = false;
            return;
        }

        Style style;
        std::shared_ptr<const Scene> scene;
        {
            std::lock_guard lock(g_mutex);
            style = g_style;
            scene = g_scene;
        }
        if (!scene) { g_stats.drawn = false; return; }

        if (g_drewInScene)
        {
            g_framesWithoutScenePass = 0;
            if (g_fallbackActive)
            {
                spdlog::info("Debug renderer: the scene resolve was found again; leaving the overlay fallback");
                g_fallbackActive = false;
            }
            return; // Already drawn this frame
        }

        bool overlay = style.depth == Depth::Overlay;
        if (!overlay)
        {
            if (++g_framesWithoutScenePass >= kFallbackAfterFrames)
            {
                if (!g_fallbackActive)
                {
                    spdlog::warn("Debug renderer: no suitable scene resolve (StretchRect) seen for {} frames; drawing as an "
                                 "overlay without depth. See the 'StretchRect' lines above for what the game does.",
                                 kFallbackAfterFrames);
                    g_fallbackActive = true;
                }
                overlay = true;
            }
        }
        if (!overlay)
        {
            g_stats.drawn = false; // Waiting for the scene pass of the next frame
            return;
        }

        if (DrawScene(dev, g_endSetup, style, *scene, false))
            NoteDrawn(Path::Overlay, *scene, g_endSetup.viewFromDevice, g_endSetup.nearUsed);
        else
            g_stats.drawn = false;
    }

    void EndFrame()
    {
        if (!g_drewInScene && g_stats.path == Path::InScene) g_stats.drawn = false;
        g_drewInScene = false;
        ++g_frameCounter;
    }

    void OnLostDevice()
    {
        spdlog::info("Debug renderer: device lost/reset; releasing resources (vb tris={}, lines={}, state block={})",
                     g_tris.vb != nullptr, g_lines.vb != nullptr, g_stateBlock != nullptr);
        ReleaseBuffer(g_tris);
        ReleaseBuffer(g_lines);
        g_uploadedVersion = 0;
        Release(g_stateBlock);
        g_loggedFailure = false;
        g_lastDrawn = false;
        g_backW = g_backH = 0;
        g_drewInScene = false;
        g_framesWithoutScenePass = 0;
        g_diagnosticsLeft = kMaxDiagnostics / 2; // The pipeline may change after a resolution change: look again
    }

    void Shutdown()
    {
        spdlog::info("Debug renderer: shutting down ({} frames)", g_frameCounter);
        OnLostDevice();
        std::lock_guard lock(g_mutex);
        g_active = false;
        g_scene.reset();
    }
}
