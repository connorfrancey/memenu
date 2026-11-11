// Managed native detours (src/runtime/detours.h): several detours on one function, in priority order, relinked as
// they come and go. On real code in this test executable, through MinHook like in the game.
#include "runtime/detours.h"

#include <MinHook.h>
#include <gtest/gtest.h>

namespace
{
	// The function detoured: noinline, and called through a volatile pointer so the call isn't folded away
	extern "C" __declspec(noinline) int __cdecl MirageDetourTarget(int x) { return x + 1; }
	int (*volatile g_call)(int) = &MirageDetourTarget;

	int(__cdecl* g_origHigh)(int) = nullptr;
	int(__cdecl* g_origLow)(int) = nullptr;
	int(__cdecl* g_origMid)(int) = nullptr;
	int __cdecl High(int x) { return g_origHigh(x) * 2; }		// Priority 20
	int __cdecl Low(int x) { return g_origLow(x) + 100; }		// Priority 10
	int __cdecl Mid(int x) { return g_origMid(x) - 7; }			// Priority 15, added last

	struct MinHookUp
	{
		MinHookUp() { MH_Initialize(); }
		~MinHookUp() { MH_Uninitialize(); }
	};
}

TEST(Detours, ChainRunsByPriorityAndRelinks)
{
	MinHookUp mh;
	void* target = reinterpret_cast<void*>(&MirageDetourTarget);
	ASSERT_EQ(g_call(1), 2);

	std::string error;
	const int low = mirage::detours::Add(target, reinterpret_cast<void*>(&Low), 10, reinterpret_cast<void**>(&g_origLow), "low", &error);
	ASSERT_GT(low, 0) << error;
	EXPECT_EQ(g_call(1), 102);			// Low -> real

	const int high = mirage::detours::Add(target, reinterpret_cast<void*>(&High), 20, reinterpret_cast<void**>(&g_origHigh), "high", &error);
	ASSERT_GT(high, 0) << error;
	EXPECT_EQ(g_call(1), 204);			// High -> Low -> real: (1 + 1 + 100) * 2
	EXPECT_EQ(mirage::detours::Chain(target), (std::vector<std::string>{"high (20)", "low (10)"}));

	const int mid = mirage::detours::Add(target, reinterpret_cast<void*>(&Mid), 15, reinterpret_cast<void**>(&g_origMid), "mid", &error);
	ASSERT_GT(mid, 0) << error;
	EXPECT_EQ(g_call(1), 190);			// High -> Mid -> Low -> real: (102 - 7) * 2

	EXPECT_EQ(mirage::detours::Add(target, reinterpret_cast<void*>(&Mid), 1, reinterpret_cast<void**>(&g_origMid), "mid", &error), 0);

	mirage::detours::Remove(high);
	EXPECT_EQ(g_call(1), 95);			// Mid -> Low -> real
	EXPECT_EQ(g_origHigh, nullptr);
	mirage::detours::Remove(low);
	EXPECT_EQ(g_call(1), -5);			// Mid -> real: 2 - 7
	mirage::detours::Remove(mid);
	EXPECT_EQ(g_call(1), 2);			// The function as it was
	EXPECT_TRUE(mirage::detours::Chain(target).empty());
}

TEST(Detours, RemoveAllPutsTheFunctionBack)
{
	MinHookUp mh;
	void* target = reinterpret_cast<void*>(&MirageDetourTarget);
	ASSERT_GT(mirage::detours::Add(target, reinterpret_cast<void*>(&Low), 10, reinterpret_cast<void**>(&g_origLow), "low"), 0);
	ASSERT_GT(mirage::detours::Add(target, reinterpret_cast<void*>(&High), 20, reinterpret_cast<void**>(&g_origHigh), "high"), 0);
	EXPECT_EQ(g_call(3), 208);
	mirage::detours::RemoveAll();
	EXPECT_EQ(g_call(3), 4);
}

TEST(Detours, RejectsBadArguments)
{
	MinHookUp mh;
	std::string error;
	EXPECT_EQ(mirage::detours::Add(nullptr, reinterpret_cast<void*>(&Low), 0, reinterpret_cast<void**>(&g_origLow), "x", &error), 0);
	EXPECT_FALSE(error.empty());
	EXPECT_EQ(mirage::detours::Add(reinterpret_cast<void*>(&MirageDetourTarget), reinterpret_cast<void*>(&Low), 0, nullptr, "x"), 0);
}
