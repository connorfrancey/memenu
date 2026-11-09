#pragma once
#include "core/cache.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// sig::Cache: remembers where byte patterns were found in an executable image, in memory and on disk.
//
// A pattern scan over MirrorsEdge.exe costs milliseconds (over a hundred in a debug build), and the menu runs a dozen
// at startup. The same exe always gives the same answers, so the results are saved next to the menu
// (cache/signatures.json) with the exe's identity (PE timestamp, image size, checksum) and loaded on the next start.
// A file written for another exe (a game update, another release) is ignored and replaced.
//
// Nothing is taken on trust: before a remembered address is returned, the bytes there are compared with the pattern
// again (a few bytes, not a scan). If they don't match, the image is scanned again. Patterns that weren't found are
// remembered for this session only: never on disk, so a failed scan can't stick.
//
// Thread-safe. Without optimizations (MEMENU_OPTIMIZATIONS off) every Find() scans and nothing is loaded or saved.
namespace sig
{
	// Pattern syntax as everywhere in the menu: `mask[i]` 'x' = pattern[i] must match, anything else = any byte.
	bool Matches(const std::uint8_t* at, const char* pattern, const char* mask);

	// Identifies an executable image: if any of these differ, it's another build of the game.
	struct ImageId
	{
		std::uint32_t timeDateStamp = 0, sizeOfImage = 0, checksum = 0;
		bool operator==(const ImageId& o) const
		{
			return timeDateStamp == o.timeDateStamp && sizeOfImage == o.sizeOfImage && checksum == o.checksum;
		}
	};
	// The identity of a module loaded in this process (its PE headers).
	ImageId IdentifyImage(const void* moduleBase);

	// Scans [from, to) (offsets from the image base, clamped to the image) and returns the first match or nullptr.
	using Scanner = void* (*)(std::size_t from, std::size_t to, const char* pattern, const char* mask);

	class Cache
	{
	public:
		explicit Cache(std::string name);

		// The image the patterns are found in, and where its results are kept (empty: memory only). Loads the file
		// when it was written for the same image. Call again for another image: forgets everything.
		void Bind(std::uintptr_t imageBase, std::size_t imageSize, const ImageId& id, std::filesystem::path file);

		// The first match of the pattern in [from, to) of the image, from the cache or from `scan`.
		void* Find(const char* pattern, const char* mask, std::size_t from, std::size_t to, Scanner scan);

		// Writes the file when something was found since it was loaded/saved. False on a write error.
		bool Save();
		void Clear();			// Forgets everything (the file stays until the next Save)

		std::size_t Size();
		std::size_t Loaded();	// Entries that came from the file (0 when it was missing or for another image)

		// The key a pattern is stored under (masked pattern bytes, mask and range). Exposed for tests.
		static std::uint64_t Key(const char* pattern, const char* mask, std::size_t from, std::size_t to);

	private:
		bool Load();

		cache::Stats* stats_;
		std::mutex mutex_;
		std::uintptr_t base_ = 0;
		std::size_t size_ = 0;
		ImageId id_;
		std::filesystem::path file_;
		std::unordered_map<std::uint64_t, std::optional<std::uint32_t>> found_;		// Key -> image offset (nullopt: not found)
		std::size_t loaded_ = 0;
		bool dirty_ = false;
	};

	// The game's signature cache (bound by the game layer at startup).
	Cache& Game();
}
