#pragma once
#include <cstdint>
#include <string>

// hw: the machine the game runs on, worked out once and kept (the hardware doesn't change while the game runs).
//
//   const hw::Info& i = hw::Get();      // CPU, memory, OS: first call queries them (~1 ms), every later call is free
//   i.cpu.brand, i.cpu.logical, i.cpu.avx2, i.memory.totalMb, i.gpu.description, ...
//
// The GPU and display part comes from Direct3D 9: the hooks fill it in once, while they install (before the menu
// draws anything), so it is empty until then: check gpu.known. Memory in use changes all the time: CurrentMemory()
// reads it again at most twice a second. Thread-safe.
namespace hw
{
	struct Cpu
	{
		std::string vendor;				// "GenuineIntel", "AuthenticAMD"
		std::string brand;				// "AMD Ryzen 7 5800X3D 8-Core Processor"
		unsigned logical = 0;			// Logical processors (threads) the process can use
		unsigned cores = 0;				// Physical cores
		bool sse2 = false, sse41 = false, sse42 = false, avx = false, avx2 = false;
		bool invariantTsc = false;		// Time stamp counter at a constant rate (the menu's clock uses it)
		bool hypervisor = false;		// Running in a VM
	};

	struct Memory
	{
		std::uint64_t totalMb = 0;		// Physical memory
		std::uint64_t availableMb = 0;	// Free now (from Memory(); in Get() it's as of the first call)
		std::uint32_t loadPercent = 0;
		std::uint64_t addressSpaceMb = 0;	// Virtual address space of this 32-bit process (2 GB, or 4 GB large-address-aware)
	};

	struct Os
	{
		std::string version;			// "Windows 10.0 build 26200"
		std::uint32_t major = 0, minor = 0, build = 0;
		bool wine = false;				// Wine / Proton
		std::string wineVersion;
		bool wow64 = false;				// 32-bit game on 64-bit Windows
	};

	struct Gpu
	{
		bool known = false;				// Filled in by the D3D9 hooks
		std::string description;		// "NVIDIA GeForce RTX 4070"
		std::string driver;				// "nvldumd.dll 31.0.15.5244"
		std::uint32_t vendorId = 0, deviceId = 0;
		std::uint32_t adapters = 0;
		std::uint32_t width = 0, height = 0, refreshHz = 0;		// The default adapter's display mode
		const char* Vendor() const;		// "NVIDIA", "AMD", "Intel", ... from vendorId
	};

	struct Info
	{
		Cpu cpu;
		Memory memory;
		Os os;
		Gpu gpu;
		std::string Summary() const;	// One line for the log
	};

	// Everything, queried on the first call. The reference stays valid; gpu changes when SetGpu() is called.
	const Info& Get();
	// Memory in use now (re-read at most every 0.5 s).
	Memory CurrentMemory();

	// From the D3D9 hooks, once, before anything reads gpu (it isn't locked against readers).
	void SetGpu(const Gpu& gpu);
}
