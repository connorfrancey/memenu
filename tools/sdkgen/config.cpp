#include "config.h"

#include <toml.hpp>

#include <exception>
#include <vector>

namespace sdkgen
{
	namespace
	{
		std::filesystem::path Resolve(const std::filesystem::path& base, const std::string& value)
		{
			if (value.empty()) return {};
			const std::filesystem::path p(value);
			return (p.is_absolute() ? p : base / p).lexically_normal();
		}
	}

	bool LoadConfig(const std::filesystem::path& file, Options& o, std::string& error)
	{
		try {
			const toml::value root = toml::parse(file);
			const std::filesystem::path base = std::filesystem::absolute(file).parent_path();

			// cooked: one folder or a list, the first that exists
			if (root.contains("game") && root.at("game").contains("cooked")) {
				const toml::value& cooked = root.at("game").at("cooked");
				std::vector<std::string> candidates;
				if (cooked.is_string()) candidates.push_back(cooked.as_string());
				else candidates = toml::get<std::vector<std::string>>(cooked);
				for (const std::string& c : candidates)
					if (std::filesystem::is_directory(Resolve(base, c))) {
						o.cooked = Resolve(base, c);
						break;
					}
			}
			const auto path = [&](const char* table, const char* key, std::filesystem::path& out) {
				out = Resolve(base, toml::find_or<std::string>(root, table, key, std::string()));
			};
			path("input", "engine_layouts", o.engine);
			path("input", "static", o.statics);
			path("output", "sdk", o.out);
			path("output", "reference", o.reference);
			path("report", "old_sdk", o.oldSdk);
			path("report", "file", o.report);
			return true;
		} catch (const std::exception& e) {
			// toml11's messages name the file, line and column, with the offending line quoted
			error = e.what();
			return false;
		}
	}
}
