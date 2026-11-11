#include "runtime/firstparty.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace mirage::firstparty
{
	namespace
	{
		bool Same(const std::filesystem::path& file, const File& f)
		{
			std::error_code ec;
			if (std::filesystem::file_size(file, ec) != f.size || ec) return false;
			std::ifstream in(file, std::ios::binary);
			std::vector<char> bytes(f.size);
			return in.read(bytes.data(), static_cast<std::streamsize>(f.size)) && std::memcmp(bytes.data(), f.data, f.size) == 0;
		}
	}

	int Install(const std::filesystem::path& modsDir)
	{
		int written = 0;
		std::set<std::string> mods;
		for (const File& f : Files()) {
			const std::string path = f.path;
			mods.insert(path.substr(0, path.find('/')));
			const auto target = modsDir / std::filesystem::path(std::u8string(path.begin(), path.end()));
			if (Same(target, f)) continue;
			std::error_code ec;
			std::filesystem::create_directories(target.parent_path(), ec);
			std::ofstream out(target, std::ios::binary | std::ios::trunc);
			if (!out.write(reinterpret_cast<const char*>(f.data), static_cast<std::streamsize>(f.size))) {
				spdlog::error("Mods: the first-party file {} can't be written", target.string());
				continue;
			}
			++written;
		}
		if (written) spdlog::info("Mods: {} first-party mod file(s) written ({} mods)", written, mods.size());
		return written;
	}
}
