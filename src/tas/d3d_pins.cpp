#include "tas/d3d_pins.h"

#include <Windows.h>
#include <d3d9.h>

#include <MinHook.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <intrin.h>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dxguid.lib")   // IID_IDirect3DSurface9 and friends

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace tas::d3dpins {

namespace {

using Ref_t = ULONG(STDMETHODCALLTYPE*)(IUnknown*);

// One hooked AddRef/Release pair per distinct function (several object types share them).
struct Hooked {
    void* target = nullptr;
    Ref_t original = nullptr;
};
constexpr int kMaxHooks = 32;
Hooked g_addRefs[kMaxHooks];
Hooked g_releases[kMaxHooks];
int g_addRefCount = 0, g_releaseCount = 0;

struct Object {
    Ref_t addRef = nullptr, release = nullptr;   // the real ones for this object's type
    std::int64_t created = -1;                   // session frame it was created in, -1: before the session
    std::uint32_t discovered = 0;                // the game's count when first seen (objects from before the session)
    std::map<std::uint32_t, std::uint32_t> counts;   // keyframe -> the game's count then
};

std::mutex g_mutex;
std::unordered_map<IUnknown*, Object> g_objects;
// The object whose reference count an object's AddRef/Release changes: a texture's surfaces (mip levels, cube faces)
// and volumes count with their texture; the swap chain's back buffers aren't ours to keep (nullptr).
std::unordered_map<IUnknown*, IUnknown*> g_owner;
std::atomic<bool> g_active{false};
std::atomic<bool> g_installed{false};
std::uint32_t g_frame = 0;
Stats g_stats;
thread_local int t_inside = 0;                   // our own AddRef/Release calls aren't seen

bool FromMenu(const void* returnAddress)
{
    static const auto base = reinterpret_cast<std::uintptr_t>(&__ImageBase);
    static const auto size = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew)->OptionalHeader.SizeOfImage;
    return reinterpret_cast<std::uintptr_t>(returnAddress) - base < size;
}

Ref_t OriginalFor(const Hooked* table, int count, void* fn)
{
    for (int i = 0; i < count; ++i)
        if (table[i].target == fn) return table[i].original;
    return nullptr;
}

// The real AddRef/Release of an object (its vtable may point at our hooks)
Ref_t RealAddRef(IUnknown* o)
{
    void* fn = (*reinterpret_cast<void***>(o))[1];
    if (Ref_t r = OriginalFor(g_addRefs, g_addRefCount, fn)) return r;
    return reinterpret_cast<Ref_t>(fn);
}
Ref_t RealRelease(IUnknown* o)
{
    void* fn = (*reinterpret_cast<void***>(o))[2];
    if (Ref_t r = OriginalFor(g_releases, g_releaseCount, fn)) return r;
    return reinterpret_cast<Ref_t>(fn);
}

// The object's whole reference count (ours included)
std::uint32_t Peek(Object& o, IUnknown* p)
{
    o.addRef(p);
    return o.release(p);
}

// Under g_mutex, inside our own calls (t_inside): see g_owner.
IUnknown* Owner(IUnknown* p)
{
    if (auto it = g_owner.find(p); it != g_owner.end()) return it->second;
    IUnknown* owner = p;
    IDirect3DSurface9* surface = nullptr;
    IDirect3DVolume9* volume = nullptr;
    if (SUCCEEDED(p->QueryInterface(IID_IDirect3DSurface9, reinterpret_cast<void**>(&surface))) && surface) {
        IDirect3DBaseTexture9* texture = nullptr;
        IDirect3DSwapChain9* chain = nullptr;
        if (SUCCEEDED(surface->GetContainer(IID_IDirect3DBaseTexture9, reinterpret_cast<void**>(&texture))) && texture) {
            owner = texture;
            RealRelease(texture)(texture);
        } else if (SUCCEEDED(surface->GetContainer(IID_IDirect3DSwapChain9, reinterpret_cast<void**>(&chain))) && chain) {
            owner = nullptr;
            chain->Release();
        }
        RealRelease(surface)(surface);
    } else if (SUCCEEDED(p->QueryInterface(IID_IDirect3DVolume9, reinterpret_cast<void**>(&volume))) && volume) {
        IDirect3DBaseTexture9* texture = nullptr;
        if (SUCCEEDED(volume->GetContainer(IID_IDirect3DBaseTexture9, reinterpret_cast<void**>(&texture))) && texture) {
            owner = texture;
            RealRelease(texture)(texture);
        }
        RealRelease(volume)(volume);
    }
    g_owner.emplace(p, owner);
    return owner;
}

// Under g_mutex: the object, pinned, added if new (with the game's count before the call that found it).
Object& Discover(IUnknown* p, bool createdNow)
{
    auto [it, inserted] = g_objects.try_emplace(p);
    Object& o = it->second;
    if (inserted) {
        o.addRef = RealAddRef(p);
        o.release = RealRelease(p);
        if (createdNow) {
            o.created = g_frame;
            o.discovered = 0;
            ++g_stats.created;
        } else {
            o.discovered = Peek(o, p);
        }
        o.addRef(p);                              // the pin
        ++g_stats.tracked;
    }
    return o;
}

template <int N>
ULONG STDMETHODCALLTYPE AddRefHook(IUnknown* self)
{
    const Ref_t original = g_addRefs[N].original;
    if (!g_active.load(std::memory_order_relaxed) || t_inside || FromMenu(_ReturnAddress())) return original(self);
    std::lock_guard lock(g_mutex);
    ++t_inside;
    IUnknown* owner = Owner(self);
    if (owner) Discover(owner, false);
    const ULONG n = original(self);
    --t_inside;
    return owner ? n - 1 : n;                     // without the pin
}

template <int N>
ULONG STDMETHODCALLTYPE ReleaseHook(IUnknown* self)
{
    const Ref_t original = g_releases[N].original;
    // Outside a session, our own calls, and the menu's objects: as is
    if (!g_active.load(std::memory_order_relaxed) || t_inside || FromMenu(_ReturnAddress())) return original(self);
    std::lock_guard lock(g_mutex);
    ++t_inside;
    IUnknown* owner = Owner(self);
    if (owner) Discover(owner, false);            // pinned: the count never reaches 0
    const ULONG n = original(self);
    --t_inside;
    return owner ? n - 1 : n;
}

template <int N>
constexpr void* AddRefHookAt() { return reinterpret_cast<void*>(&AddRefHook<N>); }
template <int N>
constexpr void* ReleaseHookAt() { return reinterpret_cast<void*>(&ReleaseHook<N>); }

void* AddRefHookFor(int i)
{
    static void* const table[kMaxHooks] = {
        AddRefHookAt<0>(), AddRefHookAt<1>(), AddRefHookAt<2>(), AddRefHookAt<3>(), AddRefHookAt<4>(), AddRefHookAt<5>(),
        AddRefHookAt<6>(), AddRefHookAt<7>(), AddRefHookAt<8>(), AddRefHookAt<9>(), AddRefHookAt<10>(), AddRefHookAt<11>(),
        AddRefHookAt<12>(), AddRefHookAt<13>(), AddRefHookAt<14>(), AddRefHookAt<15>(), AddRefHookAt<16>(), AddRefHookAt<17>(),
        AddRefHookAt<18>(), AddRefHookAt<19>(), AddRefHookAt<20>(), AddRefHookAt<21>(), AddRefHookAt<22>(), AddRefHookAt<23>(),
        AddRefHookAt<24>(), AddRefHookAt<25>(), AddRefHookAt<26>(), AddRefHookAt<27>(), AddRefHookAt<28>(), AddRefHookAt<29>(),
        AddRefHookAt<30>(), AddRefHookAt<31>()};
    return table[i];
}
void* ReleaseHookFor(int i)
{
    static void* const table[kMaxHooks] = {
        ReleaseHookAt<0>(), ReleaseHookAt<1>(), ReleaseHookAt<2>(), ReleaseHookAt<3>(), ReleaseHookAt<4>(), ReleaseHookAt<5>(),
        ReleaseHookAt<6>(), ReleaseHookAt<7>(), ReleaseHookAt<8>(), ReleaseHookAt<9>(), ReleaseHookAt<10>(), ReleaseHookAt<11>(),
        ReleaseHookAt<12>(), ReleaseHookAt<13>(), ReleaseHookAt<14>(), ReleaseHookAt<15>(), ReleaseHookAt<16>(), ReleaseHookAt<17>(),
        ReleaseHookAt<18>(), ReleaseHookAt<19>(), ReleaseHookAt<20>(), ReleaseHookAt<21>(), ReleaseHookAt<22>(), ReleaseHookAt<23>(),
        ReleaseHookAt<24>(), ReleaseHookAt<25>(), ReleaseHookAt<26>(), ReleaseHookAt<27>(), ReleaseHookAt<28>(), ReleaseHookAt<29>(),
        ReleaseHookAt<30>(), ReleaseHookAt<31>()};
    return table[i];
}

// Hooks the AddRef and Release of this object's type (once per distinct function).
void HookType(IUnknown* sample, const char* what)
{
    if (!sample) return;
    void** vt = *reinterpret_cast<void***>(sample);
    const auto hook = [&](void* fn, Hooked* table, int& count, void* (*detourFor)(int), const char* which) {
        for (int i = 0; i < count; ++i)
            if (table[i].target == fn) return;
        if (count >= kMaxHooks) {
            spdlog::warn("TAS snapshots: too many Direct3D {} functions, {} not hooked", which, what);
            return;
        }
        Hooked& h = table[count];
        if (MH_CreateHook(fn, detourFor(count), reinterpret_cast<void**>(&h.original)) != MH_OK ||
            MH_EnableHook(fn) != MH_OK) {
            spdlog::warn("TAS snapshots: could not hook {}::{}", what, which);
            return;
        }
        h.target = fn;
        ++count;
    };
    hook(vt[1], g_addRefs, g_addRefCount, &AddRefHookFor, "AddRef");
    hook(vt[2], g_releases, g_releaseCount, &ReleaseHookFor, "Release");
}

// ---- Create* on the device ----------------------------------------------------------------------------------------------
void Created(void* returnAddress, HRESULT hr, void* object)
{
    if (FAILED(hr) || !object || !g_active.load(std::memory_order_relaxed) || FromMenu(returnAddress)) return;
    std::lock_guard lock(g_mutex);
    ++t_inside;
    auto* p = static_cast<IUnknown*>(object);
    g_owner[p] = p;                               // made on its own: its own count
    Discover(p, true);
    --t_inside;
}

#define MEMENU_CREATE_HOOK(Name, Index, Params, Args, Out)                                               \
    using Name##_t = HRESULT(STDMETHODCALLTYPE*) Params;                                                 \
    Name##_t o##Name = nullptr;                                                                          \
    HRESULT STDMETHODCALLTYPE Name##Hook Params                                                          \
    {                                                                                                    \
        const HRESULT hr = o##Name Args;                                                                 \
        Created(_ReturnAddress(), hr, (Out) ? *(Out) : nullptr);                                         \
        return hr;                                                                                       \
    }                                                                                                    \
    constexpr int k##Name = Index;

MEMENU_CREATE_HOOK(CreateTexture, 23,
    (IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** o, HANDLE* s),
    (d, w, h, l, u, f, p, o, s), o)
MEMENU_CREATE_HOOK(CreateVolumeTexture, 24,
    (IDirect3DDevice9* d, UINT w, UINT h, UINT dp, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** o, HANDLE* s),
    (d, w, h, dp, l, u, f, p, o, s), o)
MEMENU_CREATE_HOOK(CreateCubeTexture, 25,
    (IDirect3DDevice9* d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** o, HANDLE* s),
    (d, e, l, u, f, p, o, s), o)
MEMENU_CREATE_HOOK(CreateVertexBuffer, 26,
    (IDirect3DDevice9* d, UINT n, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** o, HANDLE* s),
    (d, n, u, fvf, p, o, s), o)
MEMENU_CREATE_HOOK(CreateIndexBuffer, 27,
    (IDirect3DDevice9* d, UINT n, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** o, HANDLE* s),
    (d, n, u, f, p, o, s), o)
MEMENU_CREATE_HOOK(CreateRenderTarget, 28,
    (IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL lk, IDirect3DSurface9** o, HANDLE* s),
    (d, w, h, f, m, q, lk, o, s), o)
MEMENU_CREATE_HOOK(CreateDepthStencilSurface, 29,
    (IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL dc, IDirect3DSurface9** o, HANDLE* s),
    (d, w, h, f, m, q, dc, o, s), o)
MEMENU_CREATE_HOOK(CreateOffscreenPlainSurface, 36,
    (IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** o, HANDLE* s),
    (d, w, h, f, p, o, s), o)
MEMENU_CREATE_HOOK(CreateStateBlock, 59,
    (IDirect3DDevice9* d, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** o),
    (d, t, o), o)
MEMENU_CREATE_HOOK(CreateVertexDeclaration, 86,
    (IDirect3DDevice9* d, const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** o),
    (d, e, o), o)
MEMENU_CREATE_HOOK(CreateVertexShader, 91,
    (IDirect3DDevice9* d, const DWORD* f, IDirect3DVertexShader9** o),
    (d, f, o), o)
MEMENU_CREATE_HOOK(CreatePixelShader, 106,
    (IDirect3DDevice9* d, const DWORD* f, IDirect3DPixelShader9** o),
    (d, f, o), o)
MEMENU_CREATE_HOOK(CreateQuery, 118,
    (IDirect3DDevice9* d, D3DQUERYTYPE t, IDirect3DQuery9** o),
    (d, t, o), o)
#undef MEMENU_CREATE_HOOK

bool HookDevice(void** vt, int index, void* detour, void** original, const char* name)
{
    if (MH_CreateHook(vt[index], detour, original) != MH_OK || MH_EnableHook(vt[index]) != MH_OK) {
        spdlog::error("TAS snapshots: could not hook IDirect3DDevice9::{}", name);
        return false;
    }
    return true;
}

// The game's count of an object: everything but our pin.
std::uint32_t GameCount(Object& o, IUnknown* p)
{
    const std::uint32_t n = Peek(o, p);
    return n > 0 ? n - 1 : 0;
}

} // namespace

bool Install(IDirect3DDevice9* dev)
{
    if (g_installed) return true;
    if (!dev) return false;
    void** vt = *reinterpret_cast<void***>(dev);

    // A sample of every object type for its AddRef/Release
    std::vector<IUnknown*> samples;
    const auto keep = [&](IUnknown* o, const char* what) {
        if (!o) {
            spdlog::warn("TAS snapshots: no sample {} to hook", what);
            return;
        }
        HookType(o, what);
        samples.push_back(o);
    };
    IDirect3DTexture9* tex = nullptr;
    dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    keep(tex, "texture");
    if (tex) {
        IDirect3DSurface9* level = nullptr;
        tex->GetSurfaceLevel(0, &level);
        keep(level, "texture surface");
    }
    IDirect3DCubeTexture9* cube = nullptr;
    dev->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr);
    keep(cube, "cube texture");
    IDirect3DVolumeTexture9* volume = nullptr;
    dev->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume, nullptr);
    keep(volume, "volume texture");
    IDirect3DVertexBuffer9* vb = nullptr;
    dev->CreateVertexBuffer(64, 0, 0, D3DPOOL_MANAGED, &vb, nullptr);
    keep(vb, "vertex buffer");
    IDirect3DIndexBuffer9* ib = nullptr;
    dev->CreateIndexBuffer(64, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
    keep(ib, "index buffer");
    IDirect3DSurface9* plain = nullptr;
    dev->CreateOffscreenPlainSurface(4, 4, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &plain, nullptr);
    keep(plain, "surface");
    IDirect3DSurface9* rt = nullptr;
    dev->CreateRenderTarget(4, 4, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
    keep(rt, "render target");
    IDirect3DSurface9* backBuffer = nullptr;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    keep(backBuffer, "back buffer");
    IDirect3DQuery9* query = nullptr;
    dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query);
    keep(query, "query");
    IDirect3DQuery9* event = nullptr;
    dev->CreateQuery(D3DQUERYTYPE_EVENT, &event);
    keep(event, "event query");
    const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                          D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = nullptr;
    dev->CreateVertexDeclaration(elements, &decl);
    keep(decl, "vertex declaration");
    // vs_2_0: dcl_position v0; mov oPos, v0       ps_2_0: mov oC0, c0
    static const DWORD vsCode[] = {0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000, 0x02000001, 0xC00F0000, 0x90E40000,
                                   0x0000FFFF};
    static const DWORD psCode[] = {0xFFFF0200, 0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF};
    IDirect3DVertexShader9* vs = nullptr;
    dev->CreateVertexShader(vsCode, &vs);
    keep(vs, "vertex shader");
    IDirect3DPixelShader9* ps = nullptr;
    dev->CreatePixelShader(psCode, &ps);
    keep(ps, "pixel shader");
    IDirect3DStateBlock9* sb = nullptr;
    dev->CreateStateBlock(D3DSBT_ALL, &sb);
    keep(sb, "state block");

    for (IUnknown* o : samples) o->Release();     // Not active yet: plain releases

    bool ok = HookDevice(vt, kCreateTexture, reinterpret_cast<void*>(&CreateTextureHook), reinterpret_cast<void**>(&oCreateTexture), "CreateTexture") &&
              HookDevice(vt, kCreateVolumeTexture, reinterpret_cast<void*>(&CreateVolumeTextureHook), reinterpret_cast<void**>(&oCreateVolumeTexture), "CreateVolumeTexture") &&
              HookDevice(vt, kCreateCubeTexture, reinterpret_cast<void*>(&CreateCubeTextureHook), reinterpret_cast<void**>(&oCreateCubeTexture), "CreateCubeTexture") &&
              HookDevice(vt, kCreateVertexBuffer, reinterpret_cast<void*>(&CreateVertexBufferHook), reinterpret_cast<void**>(&oCreateVertexBuffer), "CreateVertexBuffer") &&
              HookDevice(vt, kCreateIndexBuffer, reinterpret_cast<void*>(&CreateIndexBufferHook), reinterpret_cast<void**>(&oCreateIndexBuffer), "CreateIndexBuffer") &&
              HookDevice(vt, kCreateRenderTarget, reinterpret_cast<void*>(&CreateRenderTargetHook), reinterpret_cast<void**>(&oCreateRenderTarget), "CreateRenderTarget") &&
              HookDevice(vt, kCreateDepthStencilSurface, reinterpret_cast<void*>(&CreateDepthStencilSurfaceHook), reinterpret_cast<void**>(&oCreateDepthStencilSurface), "CreateDepthStencilSurface") &&
              HookDevice(vt, kCreateOffscreenPlainSurface, reinterpret_cast<void*>(&CreateOffscreenPlainSurfaceHook), reinterpret_cast<void**>(&oCreateOffscreenPlainSurface), "CreateOffscreenPlainSurface") &&
              HookDevice(vt, kCreateStateBlock, reinterpret_cast<void*>(&CreateStateBlockHook), reinterpret_cast<void**>(&oCreateStateBlock), "CreateStateBlock") &&
              HookDevice(vt, kCreateVertexDeclaration, reinterpret_cast<void*>(&CreateVertexDeclarationHook), reinterpret_cast<void**>(&oCreateVertexDeclaration), "CreateVertexDeclaration") &&
              HookDevice(vt, kCreateVertexShader, reinterpret_cast<void*>(&CreateVertexShaderHook), reinterpret_cast<void**>(&oCreateVertexShader), "CreateVertexShader") &&
              HookDevice(vt, kCreatePixelShader, reinterpret_cast<void*>(&CreatePixelShaderHook), reinterpret_cast<void**>(&oCreatePixelShader), "CreatePixelShader") &&
              HookDevice(vt, kCreateQuery, reinterpret_cast<void*>(&CreateQueryHook), reinterpret_cast<void**>(&oCreateQuery), "CreateQuery");
    g_stats.hookedTypes = static_cast<std::uint32_t>(g_releaseCount);
    spdlog::info("TAS snapshots: Direct3D lifetimes {} ({} AddRef / {} Release functions)", ok ? "hooked" : "NOT hooked",
                 g_addRefCount, g_releaseCount);
    g_installed = ok;
    return ok;
}

bool Installed() { return g_installed; }

void Begin()
{
    std::lock_guard lock(g_mutex);
    g_objects.clear();
    g_owner.clear();
    const std::uint32_t types = g_stats.hookedTypes;
    g_stats = {};
    g_stats.hookedTypes = types;
    g_frame = 0;
    g_active = true;
}

void SetFrame(std::uint32_t frame) { g_frame = frame; }

void Capture(std::uint32_t keyframe)
{
    std::lock_guard lock(g_mutex);
    ++t_inside;
    // Only changes: a restore takes the newest count at or before its keyframe
    for (auto& [p, o] : g_objects) {
        const std::uint32_t n = GameCount(o, p);
        const std::uint32_t before = o.counts.empty() ? (o.created >= 0 ? 0u : o.discovered) : o.counts.rbegin()->second;
        if (n != before || o.counts.empty()) o.counts[keyframe] = n;
    }
    --t_inside;
}

void Restore(std::uint32_t keyframe)
{
    std::lock_guard lock(g_mutex);
    ++t_inside;
    for (auto& [p, o] : g_objects) {
        std::uint32_t want;
        if (o.created >= static_cast<std::int64_t>(keyframe)) {
            want = 0;                                       // made in or after the keyframe's frame: not there yet
        } else if (auto it = o.counts.upper_bound(keyframe); it != o.counts.begin()) {
            want = std::prev(it)->second;
        } else {
            want = o.discovered;                            // unchanged from before it was first seen
        }
        std::uint32_t now = GameCount(o, p);
        if (now != want) ++g_stats.adjusted;
        for (; now < want; ++now) o.addRef(p);
        for (; now > want; --now) o.release(p);             // the pin keeps it alive
    }
    --t_inside;
}

void DropAfter(std::uint32_t frame)
{
    std::lock_guard lock(g_mutex);
    for (auto& [p, o] : g_objects) o.counts.erase(o.counts.upper_bound(frame), o.counts.end());
}

void Forget(std::uint32_t /*keyframe*/)
{
    // Nothing: counts are kept only where they changed, so a count stored with a thinned keyframe is still the one of
    // the keyframes after it until the next change.
}

void End()
{
    if (!g_active.exchange(false)) return;
    std::lock_guard lock(g_mutex);
    ++t_inside;
    for (auto& [p, o] : g_objects) o.release(p);           // the pin: destroys what the game no longer holds
    --t_inside;
    spdlog::info("TAS snapshots: let go of {} Direct3D objects ({} made in the session, {} counts put back)",
                 g_objects.size(), g_stats.created, g_stats.adjusted);
    g_objects.clear();
    g_owner.clear();
}

bool Active() { return g_active; }

Stats GetStats()
{
    std::lock_guard lock(g_mutex);
    Stats s = g_stats;
    s.tracked = static_cast<std::uint32_t>(g_objects.size());
    return s;
}

} // namespace tas::d3dpins
