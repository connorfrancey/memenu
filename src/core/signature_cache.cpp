#include "core/signature_cache.h"
#include "core/memory_cache.h"

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

namespace sig
{
	namespace
	{
		constexpr int kFormat = 1;		// Bump when the key or the file layout changes: older files are then ignored

		std::string Hex(std::uint64_t v)
		{
			char buf[17];
			std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
			return buf;
		}
	}

	bool Matches(const std::uint8_t* at, const char* pattern, const char* mask)
	{
		for (std::size_t j = 0; mask[j]; ++j)
			if (mask[j] == 'x' && at[j] != static_cast<std::uint8_t>(pattern[j])) return false;
		return true;
	}

	ImageId IdentifyImage(const void* moduleBase)
	{
		const auto* base = static_cast<const std::uint8_t*>(moduleBase);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		return {nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.CheckSum};
	}

	std::uint64_t Cache::Key(const char* pattern, const char* mask, std::size_t from, std::size_t to)
	{
		std::uint64_t h = 0xcbf29ce484222325ull;		// FNV-1a
		auto mix = [&h](std::uint8_t b) { h = (h ^ b) * 0x100000001b3ull; };
		auto mixValue = [&mix](std::uint64_t v) { for (int i = 0; i < 8; ++i) mix(static_cast<std::uint8_t>(v >> (8 * i))); };
		const std::size_t len = std::strlen(mask);
		mixValue(len);
		mixValue(from);
		mixValue(to == ~std::size_t{0} ? ~std::uint64_t{0} : to);
		for (std::size_t i = 0; i < len; ++i)
		{
			const bool exact = mask[i] == 'x';
			mix(exact ? 'x' : '?');
			mix(exact ? static_cast<std::uint8_t>(pattern[i]) : 0);
		}
		return h;
	}

	Cache::Cache(std::string name) : stats_(&cache::Register(std::move(name), cache::Scope::Persistent)) {}

	void Cache::Bind(std::uintptr_t imageBase, std::size_t imageSize, const ImageId& id, std::filesystem::path file)
	{
		std::lock_guard lock(mutex_);
		base_ = imageBase;
		size_ = imageSize;
		id_ = id;
		file_ = std::move(file);
		found_.clear();
		loaded_ = 0;
		dirty_ = false;
		if (opt::kEnabled && !file_.empty()) Load();
		stats_->entries.store(found_.size(), std::memory_order_relaxed);
	}

	// Under mutex_.
	bool Cache::Load()
	{
		std::ifstream in(file_, std::ios::binary);
		if (!in) return false;
		try
		{
			const nlohmann::json j = nlohmann::json::parse(in);
			if (j.value("format", 0) != kFormat) return false;
			const auto& img = j.at("image");
			const ImageId fileId{img.at("timestamp").get<std::uint32_t>(), img.at("size").get<std::uint32_t>(),
								 img.at("checksum").get<std::uint32_t>()};
			if (!(fileId == id_))
			{
				spdlog::info("Signature cache: {} is for another game executable, scanning again", file_.filename().string());
				return false;
			}
			for (const auto& [key, offset] : j.at("signatures").items())
			{
				const std::uint64_t k = std::stoull(key, nullptr, 16);
				const auto off = offset.get<std::uint32_t>();
				if (off < size_) found_[k] = off;
			}
			loaded_ = found_.size();
			spdlog::info("Signature cache: {} pattern(s) remembered from the last start", loaded_);
			return true;
		}
		catch (const std::exception& e)
		{
			spdlog::warn("Signature cache: {} unreadable ({}), scanning again", file_.string(), e.what());
			found_.clear();
			return false;
		}
	}

	void* Cache::Find(const char* pattern, const char* mask, std::size_t from, std::size_t to, Scanner scan)
	{
		const std::uint64_t key = Key(pattern, mask, from, to);
		const std::size_t len = std::strlen(mask);
		if (opt::kEnabled)
		{
			std::lock_guard lock(mutex_);
			if (auto it = found_.find(key); it != found_.end())
			{
				if (!it->second)
				{
					stats_->hits.fetch_add(1, std::memory_order_relaxed);
					return nullptr;		// Not found earlier this session
				}
				// Check the remembered address still holds the pattern (read safely: it's only a hint until then).
				const std::uintptr_t at = base_ + *it->second;
				std::vector<std::uint8_t> bytes(len);
				if (*it->second + len <= size_ && mem::ReadRaw(at, bytes.data(), len) && Matches(bytes.data(), pattern, mask))
				{
					stats_->hits.fetch_add(1, std::memory_order_relaxed);
					return reinterpret_cast<void*>(at);
				}
				spdlog::warn("Signature cache: remembered address {:#x} no longer matches its pattern, scanning again", at);
				found_.erase(it);
				stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void* result = cache::Compute(*stats_, [&] { return scan(from, to, pattern, mask); });
		if (!opt::kEnabled) return result;

		std::lock_guard lock(mutex_);
		const auto p = reinterpret_cast<std::uintptr_t>(result);
		if (result && p >= base_ && p - base_ < size_)
		{
			found_[key] = static_cast<std::uint32_t>(p - base_);
			dirty_ = true;
		}
		else if (!result)
		{
			found_[key] = std::nullopt;		// This session only (not saved)
		}
		stats_->entries.store(found_.size(), std::memory_order_relaxed);
		return result;
	}

	bool Cache::Save()
	{
		std::lock_guard lock(mutex_);
		if (!opt::kEnabled || !dirty_ || file_.empty()) return true;
		nlohmann::json sigs = nlohmann::json::object();
		for (const auto& [key, offset] : found_)
			if (offset) sigs[Hex(key)] = *offset;
		const nlohmann::json j = {
			{"format", kFormat},
			{"image", {{"timestamp", id_.timeDateStamp}, {"size", id_.sizeOfImage}, {"checksum", id_.checksum}}},
			{"signatures", sigs},
		};

		std::error_code ec;
		std::filesystem::create_directories(file_.parent_path(), ec);
		const std::filesystem::path tmp = file_.string() + ".tmp";
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			out << j.dump(1, '\t') << '\n';
			if (!out)
			{
				spdlog::warn("Signature cache: could not write {}", tmp.string());
				return false;
			}
		}
		std::filesystem::rename(tmp, file_, ec);		// Replaces the old file in one step: never half written
		if (ec)
		{
			spdlog::warn("Signature cache: could not replace {}: {}", file_.string(), ec.message());
			std::filesystem::remove(tmp, ec);
			return false;
		}
		dirty_ = false;
		spdlog::info("Signature cache: {} pattern(s) saved to {}", sigs.size(), file_.filename().string());
		return true;
	}

	void Cache::Clear()
	{
		std::lock_guard lock(mutex_);
		if (!found_.empty()) stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
		found_.clear();
		dirty_ = true;
		stats_->entries.store(0, std::memory_order_relaxed);
	}

	std::size_t Cache::Size()
	{
		std::lock_guard lock(mutex_);
		return found_.size();
	}

	std::size_t Cache::Loaded()
	{
		std::lock_guard lock(mutex_);
		return loaded_;
	}

	Cache& Game()
	{
		static Cache c{"Signatures"};
		return c;
	}
}
