// The caches in src/core: TTL map, LRU, memory cache, signature cache, address cache.
#include "core/address_cache.h"
#include "core/cache.h"
#include "core/clock.h"
#include "core/lru_cache.h"
#include "core/memory_cache.h"
#include "core/signature_cache.h"
#include "core/ttl_cache.h"

#include <gtest/gtest.h>
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

class CacheTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { clk::Init(); }
};

// ---- TtlMap ---------------------------------------------------------------------------------------------------------
TEST_F(CacheTest, TtlMapComputesOnceUntilExpired)
{
    cache::TtlMap<int, int> m{"test ttl", 0.05};
    int computed = 0;
    EXPECT_EQ(m.Get(1, [&] { return ++computed; }), 1);
    EXPECT_EQ(m.Get(1, [&] { return ++computed; }), 1);
    EXPECT_EQ(computed, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(m.Get(1, [&] { return ++computed; }), 2);
    EXPECT_EQ(computed, 2);
}

TEST_F(CacheTest, TtlMapFindDropsExpiredAndPerEntryTtl)
{
    cache::TtlMap<std::string, int> m{"test ttl find", 10.0};
    m.Put("short", 1, 0.02);
    m.Put("long", 2);
    ASSERT_NE(m.Find("short"), nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(m.Find("short"), nullptr);
    ASSERT_NE(m.Find("long"), nullptr);
    EXPECT_EQ(*m.Find("long"), 2);
    EXPECT_EQ(m.Size(), 1u);
}

TEST_F(CacheTest, TtlMapBoundedEvictsSoonestToExpire)
{
    cache::TtlMap<int, int> m{"test ttl bounded", 10.0, 2};
    m.Put(1, 1, 1.0);   // Expires first
    m.Put(2, 2, 5.0);
    m.Put(3, 3, 5.0);
    EXPECT_EQ(m.Size(), 2u);
    EXPECT_EQ(m.Find(1), nullptr);
    EXPECT_NE(m.Find(2), nullptr);
    EXPECT_NE(m.Find(3), nullptr);
}

// ---- Lru ------------------------------------------------------------------------------------------------------------
TEST_F(CacheTest, LruDropsLeastRecentlyUsed)
{
    cache::Lru<int, std::string> lru{"test lru", 2};
    lru.Put(1, "one");
    lru.Put(2, "two");
    ASSERT_NE(lru.Find(1), nullptr);    // 1 is now the most recent: 2 goes first
    lru.Put(3, "three");
    EXPECT_EQ(lru.Size(), 2u);
    EXPECT_EQ(lru.Find(2), nullptr);
    EXPECT_EQ(*lru.Find(1), "one");
    EXPECT_EQ(*lru.Find(3), "three");
}

TEST_F(CacheTest, LruGetComputesOnMissOnly)
{
    cache::Lru<int, int> lru{"test lru get", 8};
    int computed = 0;
    for (int i = 0; i < 5; ++i) lru.Get(7, [&] { ++computed; return 49; });
    EXPECT_EQ(computed, 1);
    EXPECT_EQ(lru.Get(7, [] { return 0; }), 49);
}

TEST_F(CacheTest, LruShrinkingAndStatistics)
{
    cache::Lru<int, int> lru{"test lru stats", 4};
    for (int i = 0; i < 4; ++i) lru.Put(i, i);
    lru.SetCapacity(2);
    EXPECT_EQ(lru.Size(), 2u);
    EXPECT_EQ(lru.Peek(0), nullptr);
    EXPECT_NE(lru.Peek(3), nullptr);
    for (const auto& s : cache::Snapshot()) {
        if (s.name != "test lru stats") continue;
        EXPECT_EQ(s.entries, 2u);
        EXPECT_EQ(s.evictions, 2u);
        EXPECT_EQ(s.scope, cache::Scope::Lru);
    }
}

// ---- Memory cache ---------------------------------------------------------------------------------------------------
TEST_F(CacheTest, MemoryReadIsCrashSafe)
{
    int value = 1234;
    EXPECT_EQ(mem::Read<int>(reinterpret_cast<std::uintptr_t>(&value)), 1234);
    EXPECT_FALSE(mem::Read<int>(0).has_value());
    EXPECT_FALSE(mem::Read<int>(0x10).has_value());     // The first 64 KB are never mapped
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_NE(page, nullptr);
    EXPECT_FALSE(mem::Read<int>(reinterpret_cast<std::uintptr_t>(page)).has_value());
    EXPECT_FALSE(mem::Readable(reinterpret_cast<std::uintptr_t>(page), 4));
    VirtualFree(page, 0, MEM_RELEASE);
}

TEST_F(CacheTest, MemoryCacheKeepsValueForTheTick)
{
    mem::Cache c{"test memory tick", cache::Scope::Tick};
    volatile float speed = 1.5f;
    const auto at = reinterpret_cast<std::uintptr_t>(&speed);
    EXPECT_EQ(c.Get<float>(at), 1.5f);
    speed = 9.f;
    EXPECT_EQ(c.Get<float>(at), 1.5f);     // Same tick: the cached copy
    cache::NextTick();
    EXPECT_EQ(c.Get<float>(at), 9.f);
}

TEST_F(CacheTest, MemoryCacheInvalidateAndFailedReads)
{
    mem::Cache c{"test memory manual", cache::Scope::Manual};
    struct { int a, b; } pair{1, 2};
    const auto at = reinterpret_cast<std::uintptr_t>(&pair);
    EXPECT_EQ(c.Get<int>(at), 1);
    EXPECT_EQ(c.Get<int>(at + 4), 2);
    pair.a = 10;
    pair.b = 20;
    c.Invalidate(at + 4, 4);                // Only b
    EXPECT_EQ(c.Get<int>(at), 1);
    EXPECT_EQ(c.Get<int>(at + 4), 20);
    EXPECT_FALSE(c.Get<int>(0x20).has_value());
    EXPECT_FALSE(c.Get<int>(0x20).has_value());
}

TEST_F(CacheTest, MemoryCacheIsThreadSafe)
{
    mem::Cache c{"test memory threads", cache::Scope::Manual, 0.0, 64};
    std::vector<int> values(256);
    for (int i = 0; i < 256; ++i) values[i] = i;
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int round = 0; round < 200; ++round)
                for (int i = 0; i < 256; ++i)
                    if (c.Get<int>(reinterpret_cast<std::uintptr_t>(&values[i])) != i) ++wrong;
        });
    for (auto& t : threads) t.join();
    EXPECT_EQ(wrong.load(), 0);
    EXPECT_LE(c.Size(), 64u);
}

// ---- Signature cache ------------------------------------------------------------------------------------------------
// A fake "image": a buffer with a pattern in it, and a scanner over it that counts its scans.
struct FakeImage {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(4096, 0x90);
    std::uintptr_t Base() const { return reinterpret_cast<std::uintptr_t>(bytes.data()); }
};
FakeImage* g_image = nullptr;
int g_scans = 0;

void* ScanFake(std::size_t from, std::size_t to, const char* pattern, const char* mask)
{
    ++g_scans;
    const std::size_t len = std::strlen(mask);
    const std::size_t end = std::min(to, g_image->bytes.size());
    for (std::size_t i = from; i + len <= end; ++i)
        if (sig::Matches(&g_image->bytes[i], pattern, mask)) return &g_image->bytes[i];
    return nullptr;
}

class SignatureCacheTest : public CacheTest {
protected:
    void SetUp() override
    {
        g_image = &image;
        g_scans = 0;
        const std::uint8_t code[] = {0x8B, 0x0D, 0x44, 0x33, 0x22, 0x11, 0x8B, 0x84};
        std::memcpy(&image.bytes[0x300], code, sizeof code);
        file = std::filesystem::temp_directory_path() /
               ("memenu_sigtest_" + std::to_string(GetCurrentProcessId()) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".json");
        std::filesystem::remove(file);
    }
    void TearDown() override { std::filesystem::remove(file); }

    FakeImage image;
    std::filesystem::path file;
    const sig::ImageId id{0x5000, 4096, 7};
    static constexpr const char* kPattern = "\x8B\x0D\x00\x00\x00\x00\x8B\x84";
    static constexpr const char* kMask = "xx????xx";
};

TEST_F(SignatureCacheTest, ScansOncePerSession)
{
    sig::Cache c{"test sig session"};
    c.Bind(image.Base(), image.bytes.size(), id, {});
    void* a = c.Find(kPattern, kMask, 0, ~std::size_t{0}, &ScanFake);
    void* b = c.Find(kPattern, kMask, 0, ~std::size_t{0}, &ScanFake);
    EXPECT_EQ(a, &image.bytes[0x300]);
    EXPECT_EQ(a, b);
    EXPECT_EQ(g_scans, 1);
    // A missing pattern is remembered too (this session)
    EXPECT_EQ(c.Find("\xCC\xCC", "xx", 0, ~std::size_t{0}, &ScanFake), nullptr);
    EXPECT_EQ(c.Find("\xCC\xCC", "xx", 0, ~std::size_t{0}, &ScanFake), nullptr);
    EXPECT_EQ(g_scans, 2);
}

TEST_F(SignatureCacheTest, PersistsForTheSameImage)
{
    {
        sig::Cache c{"test sig save"};
        c.Bind(image.Base(), image.bytes.size(), id, file);
        c.Find(kPattern, kMask, 0, ~std::size_t{0}, &ScanFake);
        c.Find("\xCC\xCC", "xx", 0, ~std::size_t{0}, &ScanFake);      // Not found: never saved
        ASSERT_TRUE(c.Save());
    }
    ASSERT_TRUE(std::filesystem::exists(file));
    g_scans = 0;
    sig::Cache c{"test sig load"};
    c.Bind(image.Base(), image.bytes.size(), id, file);
    EXPECT_EQ(c.Loaded(), 1u);
    EXPECT_EQ(c.Find(kPattern, kMask, 0, ~std::size_t{0}, &ScanFake), &image.bytes[0x300]);
    EXPECT_EQ(g_scans, 0);          // From the file, checked against the bytes
}

TEST_F(SignatureCacheTest, IgnoresFileOfAnotherImage)
{
    {
        sig::Cache c{"test sig other"};
        c.Bind(image.Base(), image.bytes.size(), id, file);
        c.Find(kPattern, kMask, 0, ~std::size_t{0}, &ScanFake);
        ASSERT_TRUE(c.Save());
    }
    g_scans = 0;
    sig::Cache c{"test sig other 2"};
    c.Bind(image.Base(), image.bytes.size(), sig::ImageId{0x6000, 4096, 7}, file);   // A game update
    EXPECT_EQ(c.Loaded(), 0u);
    c.Find(kPattern, kMask, 0, ~std::size_t{0}, &ScanFake);
    EXPECT_EQ(g_scans, 1);
}

TEST_F(SignatureCacheTest, RescansWhenRememberedBytesChanged)
{
    sig::Cache c{"test sig verify"};
    c.Bind(image.Base(), image.bytes.size(), id, {});
    c.Find(kPattern, kMask, 0, ~std::size_t{0}, &ScanFake);
    // The code moved (patched by something else): the old place no longer matches
    std::memcpy(&image.bytes[0x500], &image.bytes[0x300], 8);
    image.bytes[0x300] = 0xCC;
    EXPECT_EQ(c.Find(kPattern, kMask, 0, ~std::size_t{0}, &ScanFake), &image.bytes[0x500]);
    EXPECT_EQ(g_scans, 2);
}

TEST_F(SignatureCacheTest, CorruptFileIsIgnored)
{
    { std::ofstream(file) << "{ not json"; }
    sig::Cache c{"test sig corrupt"};
    c.Bind(image.Base(), image.bytes.size(), id, file);
    EXPECT_EQ(c.Loaded(), 0u);
    EXPECT_EQ(c.Find(kPattern, kMask, 0, ~std::size_t{0}, &ScanFake), &image.bytes[0x300]);
}

TEST_F(SignatureCacheTest, KeyDependsOnMaskedBytesAndRange)
{
    // Wildcard bytes don't matter, exact ones and the range do
    EXPECT_EQ(sig::Cache::Key("\x8B\x0D\x11", "xx?", 0, 100), sig::Cache::Key("\x8B\x0D\x99", "xx?", 0, 100));
    EXPECT_NE(sig::Cache::Key("\x8B\x0D\x11", "xxx", 0, 100), sig::Cache::Key("\x8B\x0D\x99", "xxx", 0, 100));
    EXPECT_NE(sig::Cache::Key("\x8B\x0D", "xx", 0, 100), sig::Cache::Key("\x8B\x0D", "xx", 0, 200));
}

// ---- Address cache --------------------------------------------------------------------------------------------------
TEST_F(CacheTest, AddressResolveOnce)
{
    addr::Cache c{"test addresses"};
    int resolved = 0;
    auto resolve = [&] { ++resolved; return std::uintptr_t{0x401000}; };
    EXPECT_EQ(c.Resolve("Tick", resolve), 0x401000u);
    EXPECT_EQ(c.Resolve("Tick", resolve), 0x401000u);
    EXPECT_EQ(resolved, 1);
    c.Invalidate("Tick");
    EXPECT_EQ(c.Peek("Tick"), 0u);
    EXPECT_EQ(c.Resolve("Tick", resolve), 0x401000u);
    EXPECT_EQ(resolved, 2);
}

TEST_F(CacheTest, AddressZeroIsNeverCached)
{
    addr::Cache c{"test addresses zero"};
    int tries = 0;
    c.Resolve("Missing", [&] { ++tries; return std::uintptr_t{0}; });
    c.Resolve("Missing", [&] { ++tries; return std::uintptr_t{0}; });
    EXPECT_EQ(tries, 2);
}

TEST_F(CacheTest, AddressPointerChainPerTick)
{
    struct Pawn { int pad[3]; float speed; };
    struct Controller { int pad; Pawn* pawn; };
    Pawn p1{{}, 1.f}, p2{{}, 2.f};
    Controller ctl{0, &p1};
    Controller* global = &ctl;      // The "engine global" holding the controller

    const auto base = reinterpret_cast<std::uintptr_t>(&global);
    // [[base] + 4] + 12: the pawn's speed
    EXPECT_EQ(addr::Follow(base, {offsetof(Controller, pawn), offsetof(Pawn, speed)}),
              reinterpret_cast<std::uintptr_t>(&p1.speed));

    addr::Cache c{"test addresses chain"};
    const auto speed = [&] { return c.Chain("Speed", base, {offsetof(Controller, pawn), offsetof(Pawn, speed)}); };
    EXPECT_EQ(speed(), reinterpret_cast<std::uintptr_t>(&p1.speed));
    ctl.pawn = &p2;                                         // Respawned
    EXPECT_EQ(speed(), reinterpret_cast<std::uintptr_t>(&p1.speed));   // Same tick: cached
    cache::NextTick();
    EXPECT_EQ(speed(), reinterpret_cast<std::uintptr_t>(&p2.speed));
    ctl.pawn = nullptr;                                     // Dead: the chain breaks, nothing cached
    cache::NextTick();
    EXPECT_EQ(speed(), 0u);
}

} // namespace
