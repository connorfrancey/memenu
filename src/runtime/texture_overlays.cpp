#include "runtime/texture_overlays.h"

#include "paths.h"
#include "runtime/file_redirect.h"
#include "runtime/packages.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace mirage::textures
{
	namespace fs = std::filesystem;

	namespace
	{
		constexpr const char* kIndexHeader = "mirage-texture-index 1";
		constexpr const char* kOverlayVersion = "mirage-texture-overlay 2";	// Change when the copies' format changes

		std::mutex g_lock;
		std::condition_variable g_cv;
		std::vector<std::pair<std::string, std::vector<Replacement>>> g_wanted;	// By owner, in activation order
		bool g_dirty = false, g_busy = false, g_quit = false;
		std::thread g_worker;
		Status g_status;
		std::map<std::string, std::vector<std::string>> g_problems;				// By owner

		// ---- Worker only
		struct Entry
		{
			std::uintmax_t size = 0;
			std::int64_t time = 0;
			std::vector<std::string> textures;
		};
		std::map<std::string, Entry> g_index;		// Package path relative to CookedPC (UTF-8, '/') -> its textures
		bool g_indexLoaded = false, g_pruned = false;
		std::set<std::string> g_failedKeys;			// Copies that couldn't be built this session (not tried again)

		std::string Utf8(const fs::path& p)
		{
			const std::u8string s = p.generic_u8string();
			return std::string(s.begin(), s.end());
		}
		fs::path FromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

		fs::path CacheDir() { return paths::Dir() / "cache" / "textures"; }

		std::int64_t TimeOf(const fs::path& p)
		{
			std::error_code ec;
			return static_cast<std::int64_t>(fs::last_write_time(p, ec).time_since_epoch().count());
		}

		void LoadIndex()
		{
			g_indexLoaded = true;
			std::ifstream in(CacheDir() / "index.txt");
			std::string line;
			if (!std::getline(in, line) || line != kIndexHeader) return;
			Entry* current = nullptr;
			while (std::getline(in, line)) {
				if (line.size() < 2) continue;
				if (line[0] == 'P') {	// P <size> <time> <path>
					std::istringstream ls(line.substr(2));
					Entry e;
					std::string path;
					ls >> e.size >> e.time;
					std::getline(ls >> std::ws, path);
					current = &(g_index[path] = std::move(e));
				} else if (line[0] == 'T' && current) {
					current->textures.push_back(line.substr(2));
				}
			}
		}

		void SaveIndex()
		{
			std::error_code ec;
			fs::create_directories(CacheDir(), ec);
			const fs::path tmp = CacheDir() / "index.txt.tmp";
			{
				std::ofstream out(tmp, std::ios::trunc);
				out << kIndexHeader << '\n';
				for (const auto& [path, e] : g_index) {
					out << "P " << e.size << ' ' << e.time << ' ' << path << '\n';
					for (const auto& t : e.textures) out << "T " << t << '\n';
				}
			}
			fs::rename(tmp, CacheDir() / "index.txt", ec);
		}

		// Every package under CookedPC: read again when its size or time changed, gone ones dropped
		void UpdateIndex(const fs::path& cooked)
		{
			if (!g_indexLoaded) LoadIndex();
			const auto start = std::chrono::steady_clock::now();
			std::set<std::string> seen;
			std::size_t read = 0, failed = 0;
			std::error_code ec;
			for (auto it = fs::recursive_directory_iterator(cooked, fs::directory_options::skip_permission_denied, ec);
				 it != fs::recursive_directory_iterator(); it.increment(ec)) {
				if (ec) break;
				if (!it->is_regular_file(ec)) continue;
				std::string ext = it->path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (ext != ".upk" && ext != ".me1") continue;
				const std::string rel = Utf8(fs::relative(it->path(), cooked, ec));
				seen.insert(rel);
				const std::uintmax_t size = it->file_size(ec);
				const std::int64_t time = TimeOf(it->path());
				auto& e = g_index[rel];
				if (e.size == size && e.time == time) continue;		// A new entry has time 0: read
				e.size = size;
				e.time = time;
				try {
					e.textures = ScanTextures(it->path());
				} catch (const std::exception& ex) {
					e.textures.clear();
					if (++failed <= 5) spdlog::warn("Content: {} can't be read for its textures: {}", rel, ex.what());
				}
				++read;
			}
			std::erase_if(g_index, [&](const auto& kv) { return !seen.count(kv.first); });
			if (read) SaveIndex();
			const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			std::lock_guard lock(g_lock);
			g_status.indexedPackages = g_index.size();
			g_status.indexSeconds = s;
			if (read) spdlog::info("Content: texture index updated, {} of {} packages read in {:.1f} s", read, g_index.size(), s);
		}

		std::uint64_t Fnv(std::uint64_t h, const std::string& s)
		{
			for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
			return (h ^ 0xFF) * 1099511628211ull;
		}

		// The copy's file for `rel` with these replacements: changes with the original and with every image
		fs::path OverlayPath(const fs::path& cooked, const std::string& rel, const std::vector<Replacement>& reps)
		{
			std::uint64_t h = Fnv(1469598103934665603ull, kOverlayVersion);
			h = Fnv(h, rel);
			const fs::path original = cooked / FromUtf8(rel);
			std::error_code ec;
			h = Fnv(h, std::to_string(fs::file_size(original, ec)) + "/" + std::to_string(TimeOf(original)));
			for (const Replacement& r : reps)
				h = Fnv(h, r.path + "|" + Utf8(fs::absolute(r.file)) + "|" + std::to_string(fs::file_size(r.file, ec)) + "/" +
							   std::to_string(TimeOf(r.file)));
			char hex[17];
			std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
			return CacheDir() / "overlays" / (FromUtf8(rel).stem().string() + "-" + hex + FromUtf8(rel).extension().string());
		}

		// Copies not used for a week go (a few are kept so that turning a mod off and on doesn't rebuild)
		void Prune()
		{
			g_pruned = true;
			std::error_code ec;
			const auto weekAgo = fs::file_time_type::clock::now() - std::chrono::hours(24 * 7);
			for (const auto& e : fs::directory_iterator(CacheDir() / "overlays", ec))
				if (e.is_regular_file(ec) && e.last_write_time(ec) < weekAgo) fs::remove(e.path(), ec);
		}

		void Update(const std::vector<std::pair<std::string, std::vector<Replacement>>>& wanted)
		{
			std::map<std::string, std::vector<std::string>> problems;
			if (wanted.empty()) {
				files::Set({}, {}, nullptr);
				std::lock_guard lock(g_lock);
				g_status.packages = g_status.textures = 0;
				g_problems.clear();
				return;
			}
			files::Install();
			const fs::path cooked = packages::CookedDir();
			UpdateIndex(cooked);
			if (!g_pruned) Prune();

			// What is wanted, the last owner of a texture winning
			std::map<std::string, std::pair<std::string, Replacement>> merged;
			for (const auto& [owner, reps] : wanted)
				for (const Replacement& r : reps) merged[r.path] = {owner, r};

			std::map<std::string, std::vector<Replacement>> perPackage;	// rel -> replacements it holds
			std::map<std::string, std::string> ownerOf;
			int found = 0;
			for (const auto& [rel, e] : g_index)
				for (const std::string& t : e.textures)
					if (const auto it = merged.find(t); it != merged.end()) perPackage[rel].push_back(it->second.second);
			for (const auto& [path, or_] : merged) {
				ownerOf[path] = or_.first;
				const bool any = std::any_of(perPackage.begin(), perPackage.end(), [&](const auto& kv) {
					return std::any_of(kv.second.begin(), kv.second.end(), [&](const Replacement& r) { return r.path == path; });
				});
				if (any) ++found;
				else problems[or_.first].push_back(path + " isn't in any package of the game (check the file's name: the texture's full path)");
			}

			// Copies already built are used right away; the others are made the first time the game opens the package
			struct Plan
			{
				std::string rel;
				fs::path copy;
				std::vector<Replacement> reps;
			};
			auto plans = std::make_shared<std::map<fs::path, Plan>>();
			std::map<fs::path, fs::path> copies;
			std::set<fs::path> onDemand;
			for (auto& [rel, reps] : perPackage) {
				std::sort(reps.begin(), reps.end(), [](const Replacement& a, const Replacement& b) { return a.path < b.path; });
				const fs::path copy = OverlayPath(cooked, rel, reps);
				const fs::path original = cooked / FromUtf8(rel);
				std::error_code ec;
				if (fs::exists(copy, ec)) {
					copies[original] = copy;
					fs::last_write_time(copy, fs::file_time_type::clock::now(), ec);	// In use: kept by Prune
				} else if (!g_failedKeys.count(Utf8(copy))) {
					onDemand.insert(original);
					(*plans)[original] = {rel, copy, reps};
				}
			}
			files::Set(copies, onDemand, [plans, ownerOf](const fs::path& original) -> fs::path {
				const auto it = plans->find(original);
				if (it == plans->end()) return {};
				const Plan& plan = it->second;
				const auto t0 = std::chrono::steady_clock::now();
				std::vector<std::pair<std::string, std::string>> problems;		// owner, problem
				fs::path result;
				try {
					const Overlay o = BuildOverlay(original, plan.reps);
					for (const std::string& pr : o.problems) {
						const auto owner = ownerOf.find(pr.substr(0, pr.find(':')));
						problems.emplace_back(owner == ownerOf.end() ? std::string() : owner->second, pr);
					}
					if (!o.replaced.empty()) {
						std::error_code ec;
						fs::create_directories(plan.copy.parent_path(), ec);
						const fs::path tmp = fs::path(plan.copy).concat(".tmp");
						{
							std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
							out.write(reinterpret_cast<const char*>(o.bytes.data()), static_cast<std::streamsize>(o.bytes.size()));
							if (!out) throw std::runtime_error("can't write " + Utf8(tmp));
						}
						fs::rename(tmp, plan.copy, ec);
						if (ec) throw std::runtime_error("can't write " + Utf8(plan.copy) + ": " + ec.message());
						result = plan.copy;
						spdlog::info("Content: {} rewritten with {} texture(s) in {:.0f} ms ({:.1f} MB)", plan.rel, o.replaced.size(),
									 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
									 static_cast<double>(o.bytes.size()) / 1048576.0);
					}
				} catch (const std::exception& ex) {
					for (const Replacement& r : plan.reps) {
						const auto owner = ownerOf.find(r.path);
						problems.emplace_back(owner == ownerOf.end() ? std::string() : owner->second,
											  plan.rel + " can't be rewritten: " + ex.what());
					}
				}
				std::lock_guard lock(g_lock);
				g_status.buildSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				for (auto& [owner, pr] : problems) {
					auto& list = g_problems[owner];
					if (std::find(list.begin(), list.end(), pr) != list.end()) continue;	// The same image, another package
					spdlog::warn("[mod {}] content: {}", owner, pr);
					list.push_back(std::move(pr));
				}
				return result;
			});

			for (const auto& [owner, list] : problems)
				for (const auto& pr : list) spdlog::warn("[mod {}] content: {}", owner, pr);
			std::lock_guard lock(g_lock);
			g_status.packages = static_cast<int>(perPackage.size());
			g_status.ready = static_cast<int>(copies.size());
			g_status.textures = found;
			g_problems = std::move(problems);
		}

		void WorkerMain()
		{
			for (;;) {
				std::vector<std::pair<std::string, std::vector<Replacement>>> wanted;
				{
					std::unique_lock lock(g_lock);
					g_busy = false;
					g_status.busy = false;
					g_cv.notify_all();
					g_cv.wait(lock, [] { return g_quit || g_dirty; });
					if (g_quit) return;
					g_dirty = false;
					g_busy = g_status.busy = true;
					wanted = g_wanted;
				}
				try {
					files::Bypass self;		// Reading the originals
					Update(wanted);
				} catch (const std::exception& ex) {
					spdlog::error("Content: replacing textures failed: {}", ex.what());
				}
			}
		}

		void Wake()
		{
			g_dirty = true;
			if (!g_worker.joinable()) {
				g_quit = false;
				g_worker = std::thread(WorkerMain);
			}
			g_cv.notify_all();
		}

		bool SameList(const std::vector<Replacement>& a, const std::vector<Replacement>& b)
		{
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const Replacement& x, const Replacement& y) {
					   return x.path == y.path && x.file == y.file;
				   });
		}
	}

	void Activate(const std::string& owner, std::vector<Replacement> replacements)
	{
		std::lock_guard lock(g_lock);
		const auto it = std::find_if(g_wanted.begin(), g_wanted.end(), [&](const auto& w) { return w.first == owner; });
		if (it != g_wanted.end()) {
			if (SameList(it->second, replacements)) return;		// Re-applied after a level load: nothing changed
			g_wanted.erase(it);
		}
		g_wanted.emplace_back(owner, std::move(replacements));
		Wake();
	}

	void Deactivate(const std::string& owner)
	{
		std::lock_guard lock(g_lock);
		const auto before = g_wanted.size();
		std::erase_if(g_wanted, [&](const auto& w) { return w.first == owner; });
		if (g_wanted.size() != before) Wake();
	}

	Status GetStatus(const std::string& owner)
	{
		std::lock_guard lock(g_lock);
		Status s = g_status;
		s.busy = g_busy || g_dirty;
		for (const auto& [o, list] : g_problems)
			if (owner.empty() || o == owner) s.problems.insert(s.problems.end(), list.begin(), list.end());
		return s;
	}

	bool WaitIdle(double seconds)
	{
		std::unique_lock lock(g_lock);
		return g_cv.wait_for(lock, std::chrono::duration<double>(seconds), [] { return !g_busy && !g_dirty; });
	}

	void Shutdown()
	{
		{
			std::lock_guard lock(g_lock);
			g_quit = true;
			g_wanted.clear();
		}
		g_cv.notify_all();
		if (g_worker.joinable()) g_worker.join();
		files::Uninstall();
	}
}
