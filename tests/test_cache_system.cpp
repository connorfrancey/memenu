// The cache system's managers: the eviction manager (core/cache_evict.h) and the cache scheduler (core/cache_sched.h).
#include "core/cache_evict.h"
#include "core/cache_sched.h"
#include "core/lru_cache.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
	// A cache that holds `bytes` and has been hit `hits` times; Trim gives back what is asked, up to all it has
	struct FakeCache : cache::Evictable
	{
		std::size_t bytes = 0;
		std::uint64_t hits = 0;
		std::size_t expired = 0, expireCalls = 0;
		std::size_t Bytes() const override { return bytes; }
		std::size_t Trim(std::size_t want) override
		{
			const std::size_t n = std::min(want, bytes);
			bytes -= n;
			return n;
		}
		std::size_t Expire() override { ++expireCalls; return expired; }
		std::uint64_t Hits() const override { return hits; }
	};
}

TEST(CacheEviction, UnderBudgetNothingIsTrimmed)
{
	cache::Evictor e(1000);
	FakeCache a;
	a.bytes = 600;
	e.Add(a, "a");
	const auto pass = e.Enforce();
	EXPECT_EQ(pass.trimmed, 0);
	EXPECT_EQ(a.bytes, 600u);
	EXPECT_EQ(a.expireCalls, 1u);		// Stale entries go at every pass
	EXPECT_EQ(e.Total(), 600u);
}

TEST(CacheEviction, TheColdestCacheGivesBackFirst)
{
	cache::Evictor e(100000);
	FakeCache hot, cold;
	hot.bytes = cold.bytes = 800;
	e.Add(hot, "hot");
	e.Add(cold, "cold");
	e.Enforce();						// Under budget: only sets where the hit counts start
	e.SetBudget(1000);
	hot.hits = 5000;					// Used since; the other one not
	const auto pass = e.Enforce();
	EXPECT_LE(pass.after, 900u);		// Down to 90% of the budget
	EXPECT_EQ(hot.bytes, 800u);			// Kept
	EXPECT_EQ(cold.bytes, 100u);		// Gave back what was over
	ASSERT_EQ(e.Snapshot().size(), 2u);
	EXPECT_EQ(e.Snapshot()[1].trims, 1u);
	EXPECT_EQ(e.Snapshot()[0].trims, 0u);
}

TEST(CacheEviction, AnLruRegistersAndTrimsItsOldestEntries)
{
	auto& global = cache::Evictions();
	const std::size_t budget = global.Budget();
	{
		cache::Lru<int, int> lru{"test lru evicted", 1000};
		for (int i = 0; i < 500; ++i) lru.Put(i, i);
		global.SetBudget(lru.Bytes() / 2);
		global.Enforce();
		EXPECT_LT(lru.Size(), 500u);
		EXPECT_EQ(lru.Peek(0), nullptr);		// The oldest went
		EXPECT_NE(lru.Peek(499), nullptr);	// The newest stayed
	}
	global.SetBudget(budget);
	for (const auto& v : global.Snapshot()) EXPECT_NE(v.name, "test lru evicted");		// Gone with it
}

namespace
{
	// A clock the test moves by hand (seconds)
	struct FakeClock
	{
		double now = 0.0;
		std::function<double()> fn() { return [this] { return now; }; }
	};
}

TEST(CacheScheduler, RunsDueJobsOverdueFirstThenByPriority)
{
	cache::Scheduler s;
	FakeClock c;
	std::vector<std::string> order;
	s.Add({"low", 1.0, 5, [&](double) { order.push_back("low"); return true; }});
	s.Add({"high", 1.0, 0, [&](double) { order.push_back("high"); return true; }});
	EXPECT_EQ(s.RunSlice(10.0, 0.0, c.fn()), 2);
	EXPECT_EQ(order, (std::vector<std::string>{"high", "low"}));
	// Not due again until their interval has passed
	EXPECT_EQ(s.RunSlice(10.0, 0.5, c.fn()), 0);
	EXPECT_EQ(s.RunSlice(10.0, 1.0, c.fn()), 2);
}

TEST(CacheScheduler, ABusyJobContinuesAndTheBudgetIsKept)
{
	cache::Scheduler s;
	FakeClock c;
	int pieces = 0;
	s.Add({"long", 5.0, 9, [&](double budgetMs) {
		EXPECT_GT(budgetMs, 0.0);
		c.now += 0.002;			// Each piece takes 2 ms
		return ++pieces == 3;	// Done after three
	}});
	int others = 0;
	s.Add({"other", 0.0, 0, [&](double) { ++others; c.now += 0.0005; return true; }});
	s.RunSlice(1.0, 0.0, c.fn());		// 1 ms: "other" (priority) then one piece of "long", then over budget
	EXPECT_EQ(others, 1);
	EXPECT_EQ(pieces, 1);
	s.RunSlice(1.0, 0.0, c.fn());		// Turns: "other" again, then the busy job goes on
	EXPECT_EQ(pieces, 2);
	s.RunSlice(1.0, 0.0, c.fn());
	EXPECT_EQ(pieces, 3);
	s.RunSlice(1.0, 0.0, c.fn());		// Done: waits its 5 s
	EXPECT_EQ(pieces, 3);
	const auto view = s.Snapshot(0.0);
	ASSERT_EQ(view.size(), 2u);
	EXPECT_EQ(view[0].completions, 1u);
	EXPECT_FALSE(view[0].busy);
}

TEST(CacheScheduler, WakeMakesAJobDueNow)
{
	cache::Scheduler s;
	FakeClock c;
	int runs = 0;
	s.Add({"index", 60.0, 0, [&](double) { ++runs; return true; }});
	s.RunSlice(10.0, 0.0, c.fn());
	s.RunSlice(10.0, 1.0, c.fn());
	EXPECT_EQ(runs, 1);
	s.Wake("index");					// A level load: start over
	s.RunSlice(10.0, 2.0, c.fn());
	EXPECT_EQ(runs, 2);
}

TEST(CacheScheduler, ALongJobDoesntKeepTheOthersOut)
{
	cache::Scheduler s;
	FakeClock c;
	int quick = 0;
	s.Add({"long", 0.0, 0, [&](double budgetMs) { c.now += budgetMs / 1000.0; return false; }});	// Takes all it gets
	s.Add({"quick", 1.0, 9, [&](double) { ++quick; return true; }});
	s.RunSlice(1.0, 0.0, c.fn());		// "long" first (same turn, better priority): the slice is used up
	s.RunSlice(1.0, 0.0, c.fn());		// "quick" hasn't had a turn: it goes first now
	EXPECT_EQ(quick, 1);
}

