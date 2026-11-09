#include "core/hardware.h"
#include "core/cache.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <spdlog/fmt/fmt.h>

namespace hw
{
	namespace
	{
		cache::Stats& g_stats = cache::Register("Hardware info", cache::Scope::Manual);

		Cpu QueryCpu()
		{
			Cpu c;
			int r[4]{};
			__cpuid(r, 0);
			const int maxLeaf = r[0];
			char vendor[13]{};
			std::memcpy(vendor, &r[1], 4);
			std::memcpy(vendor + 4, &r[3], 4);
			std::memcpy(vendor + 8, &r[2], 4);
			c.vendor = vendor;
			if (maxLeaf >= 1)
			{
				__cpuid(r, 1);
				c.sse2 = (r[3] >> 26) & 1;
				c.sse41 = (r[2] >> 19) & 1;
				c.sse42 = (r[2] >> 20) & 1;
				c.hypervisor = (r[2] >> 31) & 1;
				const bool osxsave = (r[2] >> 27) & 1, avxBit = (r[2] >> 28) & 1;
				// AVX also needs the OS to save the YMM registers (XCR0 bits 1 and 2)
				const bool ymm = osxsave && (_xgetbv(0) & 6) == 6;
				c.avx = avxBit && ymm;
				if (maxLeaf >= 7)
				{
					__cpuidex(r, 7, 0);
					c.avx2 = c.avx && ((r[1] >> 5) & 1);
				}
			}
			__cpuid(r, static_cast<int>(0x80000000));
			const auto maxExt = static_cast<unsigned>(r[0]);
			if (maxExt >= 0x80000004)
			{
				char brand[49]{};
				for (unsigned i = 0; i < 3; ++i)
				{
					__cpuid(r, static_cast<int>(0x80000002 + i));
					std::memcpy(brand + 16 * i, r, 16);
				}
				c.brand = brand;
				const auto first = c.brand.find_first_not_of(' ');
				c.brand = first == std::string::npos ? std::string{} : c.brand.substr(first);
				while (!c.brand.empty() && c.brand.back() == ' ') c.brand.pop_back();
			}
			if (maxExt >= 0x80000007)
			{
				__cpuid(r, static_cast<int>(0x80000007));
				c.invariantTsc = (r[3] >> 8) & 1;
			}

			DWORD_PTR processMask = 0, systemMask = 0;
			if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
				for (DWORD_PTR m = processMask; m; m &= m - 1) ++c.logical;
			DWORD len = 0;
			GetLogicalProcessorInformation(nullptr, &len);
			std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> infos(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
			if (!infos.empty() && GetLogicalProcessorInformation(infos.data(), &len))
				for (const auto& i : infos)
					if (i.Relationship == RelationProcessorCore) ++c.cores;
			return c;
		}

		Memory QueryMemory()
		{
			Memory m;
			MEMORYSTATUSEX s{};
			s.dwLength = sizeof s;
			if (GlobalMemoryStatusEx(&s))
			{
				m.totalMb = s.ullTotalPhys / (1024 * 1024);
				m.availableMb = s.ullAvailPhys / (1024 * 1024);
				m.loadPercent = s.dwMemoryLoad;
				m.addressSpaceMb = s.ullTotalVirtual / (1024 * 1024);
			}
			return m;
		}

		Os QueryOs()
		{
			Os o;
			const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
			RTL_OSVERSIONINFOW v{sizeof v};
			if (auto fn = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(ntdll, "RtlGetVersion")); fn && fn(&v) == 0)
			{
				o.major = v.dwMajorVersion;
				o.minor = v.dwMinorVersion;
				o.build = v.dwBuildNumber;
			}
			o.version = fmt::format("Windows {}.{} build {}", o.major, o.minor, o.build);
			using WineVersion_t = const char*(__cdecl*)();
			if (auto wine = reinterpret_cast<WineVersion_t>(GetProcAddress(ntdll, "wine_get_version")))
			{
				o.wine = true;
				o.wineVersion = wine();
				o.version += fmt::format(" (Wine/Proton {})", o.wineVersion);
			}
			BOOL wow64 = FALSE;
			o.wow64 = IsWow64Process(GetCurrentProcess(), &wow64) && wow64;
			return o;
		}

		std::mutex g_mutex;			// SetGpu calls

		Info& Storage()
		{
			static Info info;
			return info;
		}

		std::mutex g_memoryMutex;
		Memory g_memory;
		clk::Ticks g_memoryAt = 0;
	}

	const char* Gpu::Vendor() const
	{
		switch (vendorId)
		{
			case 0x10DE: return "NVIDIA";
			case 0x1002: case 0x1022: return "AMD";
			case 0x8086: return "Intel";
			case 0x15AD: return "VMware";
			case 0x1414: return "Microsoft";
			case 0x80EE: return "VirtualBox";
			default: return vendorId ? "other" : "unknown";
		}
	}

	std::string Info::Summary() const
	{
		std::string s = fmt::format("{} ({} cores / {} threads{}{}), {} MB RAM, {}", cpu.brand.empty() ? cpu.vendor : cpu.brand,
									cpu.cores, cpu.logical, cpu.avx2 ? ", AVX2" : cpu.avx ? ", AVX" : "",
									cpu.hypervisor ? ", VM" : "", memory.totalMb, os.version);
		if (gpu.known)
			s += fmt::format(", {} ({}) {}x{} @ {} Hz", gpu.description, gpu.driver, gpu.width, gpu.height, gpu.refreshHz);
		return s;
	}

	const Info& Get()
	{
		static std::once_flag once;
		bool queried = false;
		std::call_once(once, [&] {
			const clk::Ticks t0 = clk::Now();
			Info& i = Storage();
			i.cpu = QueryCpu();
			i.memory = QueryMemory();
			i.os = QueryOs();
			queried = true;
			g_stats.misses.fetch_add(1, std::memory_order_relaxed);
			g_stats.computeTicks.fetch_add(clk::Now() - t0, std::memory_order_relaxed);
			g_stats.entries.store(1, std::memory_order_relaxed);
		});
		if (!queried) g_stats.hits.fetch_add(1, std::memory_order_relaxed);
		return Storage();
	}

	Memory CurrentMemory()
	{
		std::lock_guard lock(g_memoryMutex);
		const clk::Ticks now = clk::Now();
		if (!opt::kEnabled || !g_memoryAt || now - g_memoryAt >= clk::FromSeconds(0.5))
		{
			g_memory = QueryMemory();
			g_memoryAt = now;
		}
		return g_memory;
	}

	void SetGpu(const Gpu& gpu)
	{
		Get();		// The rest first, so Get() never overwrites this
		std::lock_guard lock(g_mutex);
		Storage().gpu = gpu;
		Storage().gpu.known = true;
	}
}
