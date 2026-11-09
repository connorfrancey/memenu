#pragma once
#include "core/cache.h"
#include "core/lru_cache.h"

#include <cstddef>
#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

// cache::Memo: a function whose results are remembered by its arguments (memoization).
//
//   cache::Memo<float(int, int)> cost{"Path cost", [](int from, int to) { return Expensive(from, to); },
//                                     {512, cache::Scope::Level}};        // capacity, scope[, ttl]
//   float c = cost(3, 7);       // computed once per level for (3, 7), then remembered
//
// Results are kept in an LRU of `capacity` entries, each fresh for `scope`: Tick, Level, Timed (`ttl` seconds) or
// Manual (until Clear/Forget). Arguments are copied into the key, so they need std::hash and ==; a std::string
// argument is hashed by its contents. Use it only for functions whose result depends on nothing but their arguments
// (within the scope). One owner thread, like cache::Value. Without optimizations every call runs the function.
namespace cache
{
	struct MemoOptions
	{
		std::size_t capacity = 256;
		Scope scope = Scope::Manual;
		double ttl = 0.0;			// Seconds, for Scope::Timed
	};

	namespace detail
	{
		inline void HashCombine(std::size_t& seed, std::size_t h) { seed ^= h + 0x9e3779b9 + (seed << 6) + (seed >> 2); }

		template <class Tuple>
		struct TupleHash
		{
			std::size_t operator()(const Tuple& t) const
			{
				std::size_t seed = 0;
				std::apply([&seed](const auto&... v) {
					(HashCombine(seed, std::hash<std::decay_t<decltype(v)>>{}(v)), ...);
				}, t);
				return seed;
			}
		};
	}

	template <class Signature>
	class Memo;

	template <class R, class... Args>
	class Memo<R(Args...)>
	{
	public:
		using Key = std::tuple<std::decay_t<Args>...>;

		Memo(std::string name, std::function<R(Args...)> fn, MemoOptions options = {})
			: stats_(&Register(std::move(name), options.scope)), fn_(std::move(fn)), scope_(options.scope),
			  ttl_(clk::FromSeconds(options.ttl)), results_(*stats_, options.capacity) {}

		// The remembered result for these arguments, or the function's (then remembered).
		const R& operator()(const std::decay_t<Args>&... args)
		{
			Key key{args...};
			if (opt::kEnabled)
			{
				if (const Entry* e = results_.Peek(key); e && IsFresh(scope_, e->stamp, ttl_, Now()))
				{
					return results_.Find(key)->value;		// Most recently used now, counted as a hit
				}
			}
			Entry fresh{Compute(*stats_, [&] { return fn_(args...); }), 0};
			fresh.stamp = StampFor(scope_, clk::Now());
			return results_.Put(std::move(key), std::move(fresh)).value;
		}

		// Forget the result for these arguments / every result.
		void Forget(const std::decay_t<Args>&... args) { results_.Erase(Key{args...}); }
		void Clear() { results_.Clear(); }
		std::size_t Size() const { return results_.Size(); }

	private:
		struct Entry
		{
			R value;
			std::uint64_t stamp;
		};

		clk::Ticks Now() const { return scope_ == Scope::Timed ? clk::Now() : 0; }

		Stats* stats_;
		std::function<R(Args...)> fn_;
		Scope scope_;
		clk::Ticks ttl_;
		Lru<Key, Entry, detail::TupleHash<Key>> results_;
	};
}
