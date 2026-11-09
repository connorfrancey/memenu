// The caches in src/core, part 2: function (memo), negative, string interning, hardware.
#include "core/cache.h"
#include "core/clock.h"
#include "core/function_cache.h"
#include "core/hardware.h"
#include "core/negative_cache.h"
#include "core/string_cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

class Cache2Test : public ::testing::Test {
protected:
    static void SetUpTestSuite() { clk::Init(); }
};

// ---- Memo -----------------------------------------------------------------------------------------------------------
TEST_F(Cache2Test, MemoRemembersByArguments)
{
    int calls = 0;
    cache::Memo<int(int, int)> add{"test memo", [&](int a, int b) { ++calls; return a + b; }};
    EXPECT_EQ(add(2, 3), 5);
    EXPECT_EQ(add(2, 3), 5);
    EXPECT_EQ(add(3, 2), 5);
    EXPECT_EQ(calls, 2);        // (2,3) once, (3,2) once
    add.Forget(2, 3);
    EXPECT_EQ(add(2, 3), 5);
    EXPECT_EQ(calls, 3);
}

TEST_F(Cache2Test, MemoStringArgumentsAndCapacity)
{
    int calls = 0;
    cache::Memo<std::size_t(const std::string&)> length{"test memo strings",
        [&](const std::string& s) { ++calls; return s.size(); }, {2, cache::Scope::Manual}};
    EXPECT_EQ(length("abc"), 3u);
    EXPECT_EQ(length(std::string("abc")), 3u);      // Same contents: same key
    EXPECT_EQ(calls, 1);
    length("de");
    length("fghi");                                 // Capacity 2: "abc" goes
    EXPECT_EQ(length.Size(), 2u);
    length("abc");
    EXPECT_EQ(calls, 4);
}

TEST_F(Cache2Test, MemoTickScope)
{
    int calls = 0;
    cache::Memo<int(int)> twice{"test memo tick", [&](int v) { ++calls; return v * 2; }, {16, cache::Scope::Tick}};
    twice(4);
    twice(4);
    EXPECT_EQ(calls, 1);
    cache::NextTick();
    twice(4);
    EXPECT_EQ(calls, 2);
}

// ---- NegativeCache --------------------------------------------------------------------------------------------------
TEST_F(Cache2Test, NegativeCacheRetriesAfterTicks)
{
    cache::Retry retry;
    retry.ticks = 3;
    retry.untilLevel = false;
    cache::NegativeCache<std::string> missing{"test negative ticks", retry};
    int lookups = 0;
    auto find = [&] { return missing.Lookup("TdBoss", [&] { ++lookups; return static_cast<void*>(nullptr); }); };
    find();
    find();
    EXPECT_EQ(lookups, 1);
    for (int i = 0; i < 3; ++i) cache::NextTick();
    find();
    EXPECT_EQ(lookups, 2);
}

TEST_F(Cache2Test, NegativeCacheLevelLoadAndSuccessClear)
{
    cache::Retry retry;
    retry.ticks = 1000;
    cache::NegativeCache<int> missing{"test negative level", retry};
    missing.Remember(7);
    EXPECT_TRUE(missing.Failed(7));
    cache::NextLevel();
    EXPECT_FALSE(missing.Failed(7));                // A level load: try again
    missing.Remember(7);
    EXPECT_TRUE(missing.Failed(7));
    int value = 5;
    EXPECT_EQ(missing.Lookup(8, [&] { return &value; }), &value);
    EXPECT_FALSE(missing.Failed(8));
    missing.Forget(7);
    EXPECT_FALSE(missing.Failed(7));
}

TEST_F(Cache2Test, NegativeCacheBacksOff)
{
    cache::Retry retry;
    retry.ticks = 2;
    retry.untilLevel = false;
    retry.backoff = true;
    retry.maxBackoff = 4;
    cache::NegativeCache<int> missing{"test negative backoff", retry};
    auto ticksUntilRetry = [&] {
        missing.Remember(1);
        int n = 0;
        while (missing.Failed(1)) { cache::NextTick(); ++n; }
        return n;
    };
    EXPECT_EQ(ticksUntilRetry(), 2);    // 1st failure: base
    EXPECT_EQ(ticksUntilRetry(), 4);    // 2nd in a row: doubled
    EXPECT_EQ(ticksUntilRetry(), 8);    // capped at 4x
    EXPECT_EQ(ticksUntilRetry(), 8);
    missing.Forget(1);                  // Success resets the streak
    EXPECT_EQ(ticksUntilRetry(), 2);
}

TEST_F(Cache2Test, NegativeCacheSeconds)
{
    cache::Retry retry;
    retry.seconds = 0.03;
    retry.untilLevel = false;
    cache::NegativeCache<int> missing{"test negative seconds", retry};
    missing.Remember(1);
    EXPECT_TRUE(missing.Failed(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(missing.Failed(1));
}

// ---- String interning -----------------------------------------------------------------------------------------------
TEST_F(Cache2Test, InternSharesOneCopy)
{
    const std::string built = std::string("TdPlayer") + "Pawn";
    const str::Atom a = str::Intern("TdPlayerPawn");
    const str::Atom b = str::Intern(built);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.c_str(), b.c_str());        // The same memory
    EXPECT_EQ(a.view(), "TdPlayerPawn");
    EXPECT_EQ(a.size(), 12u);
    EXPECT_NE(a, str::Intern("TdPlayerController"));
    EXPECT_TRUE(str::Intern("").empty());
    EXPECT_EQ(str::Intern(""), str::Atom{});
    EXPECT_EQ(str::Find("never interned anywhere 123"), str::Atom{});
    EXPECT_EQ(str::Find("TdPlayerPawn"), a);
}

TEST_F(Cache2Test, InternLongStringsAndEmbeddedZeros)
{
    const std::string longText(50000, 'x');
    const str::Atom l = str::Intern(longText);
    EXPECT_EQ(l.view(), longText);
    const std::string_view withZero("a\0b", 3);
    EXPECT_EQ(str::Intern(withZero).size(), 3u);
    EXPECT_NE(str::Intern(withZero), str::Intern("a"));
    // Small strings interned after the long one still work (the block bookkeeping)
    EXPECT_EQ(str::Intern("after long").view(), "after long");
}

TEST_F(Cache2Test, InternIsThreadSafe)
{
    std::vector<std::vector<const char*>> seen(4);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 2000; ++i) seen[t].push_back(str::Intern("name_" + std::to_string(i)).c_str());
        });
    for (auto& th : threads) th.join();
    for (int t = 1; t < 4; ++t) EXPECT_EQ(seen[t], seen[0]);   // Every thread got the same copies
    std::unordered_set<str::Atom> set;
    for (int i = 0; i < 2000; ++i) set.insert(str::Intern("name_" + std::to_string(i)));
    EXPECT_EQ(set.size(), 2000u);
}

// ---- Hardware -------------------------------------------------------------------------------------------------------
TEST_F(Cache2Test, HardwareIsQueriedOnce)
{
    const hw::Info& a = hw::Get();
    const hw::Info& b = hw::Get();
    EXPECT_EQ(&a, &b);
    EXPECT_FALSE(a.cpu.vendor.empty());
    EXPECT_GE(a.cpu.logical, 1u);
    EXPECT_GE(a.cpu.cores, 1u);
    EXPECT_TRUE(a.cpu.sse2);                // Any x86 CPU this runs on
    EXPECT_GT(a.memory.totalMb, 0u);
    EXPECT_GT(a.os.major, 0u);
    EXPECT_FALSE(a.Summary().empty());
    EXPECT_GT(hw::CurrentMemory().totalMb, 0u);
}

TEST_F(Cache2Test, HardwareGpuFromHooks)
{
    hw::Gpu gpu;
    gpu.description = "Test GPU";
    gpu.vendorId = 0x10DE;
    gpu.width = 1920;
    gpu.height = 1080;
    gpu.refreshHz = 144;
    hw::SetGpu(gpu);
    EXPECT_TRUE(hw::Get().gpu.known);
    EXPECT_STREQ(hw::Get().gpu.Vendor(), "NVIDIA");
    EXPECT_NE(hw::Get().Summary().find("Test GPU"), std::string::npos);
}

} // namespace
