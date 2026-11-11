#include "runtime/file_redirect.h"

#include <Windows.h>
#include <MinHook.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace mirage::files
{
	namespace
	{
		using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
		using GetFileAttributesExW_t = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
		CreateFileW_t g_createFile = nullptr;
		GetFileAttributesExW_t g_getAttributes = nullptr;
		void* g_createTarget = nullptr;
		void* g_attributesTarget = nullptr;

		// A redirected package: its copy, or how to make it (once: the first opener builds, the others wait for it)
		struct Entry
		{
			std::wstring copy;
			std::filesystem::path original;
			bool onDemand = false;
			std::mutex building;
			bool tried = false;
		};
		std::shared_mutex g_lock;
		std::unordered_map<std::wstring, std::shared_ptr<Entry>> g_table;	// lowercase full path -> entry
		Builder g_build;
		// Packages the game has opened this session, and the file it got (empty: the original): kept for the session
		std::unordered_map<std::wstring, std::wstring> g_opened;
		std::atomic<unsigned long long> g_redirected{0}, g_built{0};
		thread_local int t_bypass = 0;

		std::wstring Key(const std::filesystem::path& p)
		{
			std::wstring s = std::filesystem::absolute(p).lexically_normal().wstring();
			CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
			return s;
		}

		// Only packages are ever redirected: the cheap test first
		bool IsPackage(LPCWSTR name)
		{
			if (!name) return false;
			const std::size_t n = wcslen(name);
			if (n < 4) return false;
			const wchar_t* ext = name + n - 4;
			return _wcsicmp(ext, L".upk") == 0 || _wcsicmp(ext, L".me1") == 0;
		}

		// The copy to open for `name`, or empty. Builds an on-demand copy (in this thread) the first time.
		std::wstring Lookup(LPCWSTR name)
		{
			// Every package open is remembered (even with no mod content): what it got is what it keeps getting
			if (t_bypass || !IsPackage(name)) return {};
			wchar_t full[MAX_PATH * 2];
			const DWORD len = GetFullPathNameW(name, static_cast<DWORD>(std::size(full)), full, nullptr);
			if (len == 0 || len >= std::size(full)) return {};
			std::wstring key(full, len);
			CharLowerBuffW(key.data(), len);
			std::shared_ptr<Entry> e;
			Builder build;
			{
				std::shared_lock lock(g_lock);
				if (const auto o = g_opened.find(key); o != g_opened.end()) return o->second;	// Same as before
				const auto it = g_table.find(key);
				if (it == g_table.end()) {
					lock.unlock();
					std::unique_lock w(g_lock);
					g_opened.emplace(key, std::wstring());
					return {};
				}
				e = it->second;
				build = g_build;
			}
			std::lock_guard building(e->building);
			if (e->onDemand && !e->tried && build) {
				e->tried = true;
				Bypass self;		// The builder reads the original: not redirected (nor built again)
				try {
					const std::filesystem::path copy = build(e->original);
					if (!copy.empty()) {
						e->copy = std::filesystem::absolute(copy).wstring();
						g_built.fetch_add(1);
					}
				} catch (const std::exception& ex) {
					spdlog::error("Content: the copy of {} can't be made: {}", e->original.filename().string(), ex.what());
				}
			}
			std::unique_lock w(g_lock);
			return g_opened.emplace(key, e->copy).first->second;
		}

		HANDLE WINAPI CreateFileHook(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
									 DWORD flags, HANDLE templ)
		{
			// Reading only: a write (the editor saving) goes to the real file
			if (!(access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) && disposition == OPEN_EXISTING) {
				const std::wstring copy = Lookup(name);
				if (!copy.empty()) {
					const HANDLE h = g_createFile(copy.c_str(), access, share, sa, disposition, flags, templ);
					if (h != INVALID_HANDLE_VALUE) {
						if (g_redirected.fetch_add(1) < 50)
							spdlog::info("Content: {} opened from the mod copy", std::filesystem::path(name).filename().string());
						return h;
					}
					spdlog::warn("Content: the copy of {} can't be opened ({}): the original is used",
								 std::filesystem::path(name).filename().string(), GetLastError());
				}
			}
			return g_createFile(name, access, share, sa, disposition, flags, templ);
		}

		BOOL WINAPI GetAttributesHook(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info)
		{
			const std::wstring copy = Lookup(name);
			return g_getAttributes(copy.empty() ? name : copy.c_str(), level, info);
		}
	}

	Bypass::Bypass() { ++t_bypass; }
	Bypass::~Bypass() { --t_bypass; }

	bool Install()
	{
		if (g_createTarget) return true;
		const HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
		void* create = kb ? reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileW")) : nullptr;
		void* attrs = kb ? reinterpret_cast<void*>(GetProcAddress(kb, "GetFileAttributesExW")) : nullptr;
		if (!create || !attrs) {
			spdlog::error("Content: kernelbase's CreateFileW / GetFileAttributesExW not found: package files can't be redirected");
			return false;
		}
		if (MH_CreateHook(create, reinterpret_cast<void*>(&CreateFileHook), reinterpret_cast<void**>(&g_createFile)) != MH_OK)
			return spdlog::error("Content: CreateFileW can't be hooked"), false;
		if (MH_CreateHook(attrs, reinterpret_cast<void*>(&GetAttributesHook), reinterpret_cast<void**>(&g_getAttributes)) != MH_OK) {
			MH_RemoveHook(create);
			return spdlog::error("Content: GetFileAttributesExW can't be hooked"), false;
		}
		if (MH_EnableHook(create) != MH_OK || MH_EnableHook(attrs) != MH_OK) {
			MH_RemoveHook(create);
			MH_RemoveHook(attrs);
			return spdlog::error("Content: the file hooks can't be enabled"), false;
		}
		g_createTarget = create;
		g_attributesTarget = attrs;
		spdlog::info("Content: package opens are redirected to mods' copies when a mod changes a package");
		return true;
	}

	void Uninstall()
	{
		Set({}, {}, nullptr);
		if (!g_createTarget) return;
		MH_DisableHook(g_createTarget);
		MH_DisableHook(g_attributesTarget);
		MH_RemoveHook(g_createTarget);
		MH_RemoveHook(g_attributesTarget);
		g_createTarget = g_attributesTarget = nullptr;
	}

	void Set(const std::map<std::filesystem::path, std::filesystem::path>& copies, const std::set<std::filesystem::path>& onDemand,
			 Builder build)
	{
		std::unordered_map<std::wstring, std::shared_ptr<Entry>> next;
		for (const auto& [from, to] : copies) {
			auto e = std::make_shared<Entry>();
			e->original = from;
			e->copy = std::filesystem::absolute(to).wstring();
			next[Key(from)] = std::move(e);
		}
		for (const auto& from : onDemand) {
			auto e = std::make_shared<Entry>();
			e->original = from;
			e->onDemand = true;
			next[Key(from)] = std::move(e);
		}
		std::unique_lock lock(g_lock);
		g_table.swap(next);
		g_build = std::move(build);
	}

	bool Redirects(const std::filesystem::path& original)
	{
		const std::wstring key = Key(original);
		std::shared_lock lock(g_lock);
		if (const auto o = g_opened.find(key); o != g_opened.end()) return !o->second.empty();
		return g_table.count(key) != 0;
	}

	int Held()
	{
		std::shared_lock lock(g_lock);
		int held = 0;
		for (const auto& [key, file] : g_opened) {
			const auto it = g_table.find(key);
			const bool wantsCopy = it != g_table.end();
			if (wantsCopy != !file.empty() || (wantsCopy && !it->second->onDemand && it->second->copy != file)) ++held;
		}
		return held;
	}

	unsigned long long Redirected() { return g_redirected.load(); }
	unsigned long long Built() { return g_built.load(); }
}
