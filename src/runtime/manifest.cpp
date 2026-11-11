#include "runtime/manifest.h"

#include <toml.hpp>

#include <fstream>
#include <set>
#include <sstream>

namespace mirage::mods
{
	const char* PhaseName(Phase p)
	{
		switch (p) {
			case Phase::Early: return "early";
			case Phase::Engine: return "engine";
			case Phase::World: return "world";
		}
		return "?";
	}

	namespace
	{
		// Paths and UTF-8 text (C++20's u8string is std::u8string)
		std::string Utf8(const std::filesystem::path& p)
		{
			const std::u8string s = p.u8string();
			return std::string(s.begin(), s.end());
		}
		std::filesystem::path FromUtf8(const std::string& s)
		{
			return std::filesystem::path(std::u8string(s.begin(), s.end()));
		}

		bool ValidId(std::string_view id)
		{
			if (id.size() < 2 || id.size() > 64) return false;
			for (std::size_t i = 0; i < id.size(); ++i) {
				const char c = id[i];
				const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (i > 0 && (c == '-' || c == '_'));
				if (!ok) return false;
			}
			return true;
		}

		// A file name inside the mod's folder: relative, no "..", no drive or root
		bool InsideFolder(const std::string& path)
		{
			if (path.empty()) return false;
			const std::filesystem::path p = FromUtf8(path);
			if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
			for (const auto& part : p)
				if (part == "..") return false;
			return true;
		}

		bool EndsWith(const std::string& s, std::string_view suffix)
		{
			if (s.size() < suffix.size()) return false;
			for (std::size_t i = 0; i < suffix.size(); ++i)
				if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != suffix[i]) return false;
			return true;
		}
	}

	Parsed Parse(std::string_view text, const std::string& source)
	{
		Parsed out;
		std::istringstream in{std::string(text)};
		auto parsed = toml::try_parse(in, source);
		if (parsed.is_err()) {
			for (const auto& e : parsed.unwrap_err()) out.errors.push_back(toml::format_error(e));
			return out;
		}
		const toml::value root = parsed.unwrap();
		if (!root.is_table()) {
			out.errors.push_back(source + ": not a table");
			return out;
		}

		Manifest m;
		const auto error = [&](const std::string& what) { out.errors.push_back(source + ": " + what); };
		const auto warn = [&](const std::string& what) { out.warnings.push_back(source + ": " + what); };
		// A string value, or `fallback` when missing; a wrong type is an error
		const auto str = [&](const char* key, std::string fallback = {}) -> std::string {
			if (!root.contains(key)) return fallback;
			const toml::value& v = root.at(key);
			if (!v.is_string()) {
				error(std::string(key) + " must be a string");
				return fallback;
			}
			return v.as_string();
		};
		const auto strings = [&](const char* key) {
			std::vector<std::string> list;
			if (!root.contains(key)) return list;
			const toml::value& v = root.at(key);
			if (!v.is_array()) {
				error(std::string(key) + " must be a list of strings");
				return list;
			}
			for (const toml::value& e : v.as_array()) {
				if (e.is_string()) list.push_back(e.as_string());
				else error(std::string(key) + " must be a list of strings");
			}
			return list;
		};

		static const std::set<std::string> known = {"id", "name", "version", "description", "authors", "mirage", "game",
			"entry", "lua", "phase", "content", "dependencies", "optional", "incompatible"};
		for (const auto& [key, value] : root.as_table())
			if (!known.count(key)) warn("unknown key '" + key + "' (ignored)");

		m.id = str("id");
		if (m.id.empty()) error("id is required");
		else if (!ValidId(m.id)) error("id '" + m.id + "' must be 2-64 characters of a-z, 0-9, '-' and '_', starting with a letter or digit");

		const std::string version = str("version");
		if (version.empty()) {
			error("version is required (like \"1.0.0\")");
		} else {
			std::string why;
			if (auto v = semver::Version::Parse(version, &why)) m.version = *v;
			else error("version: " + why);
		}
		m.name = str("name", m.id);
		m.description = str("description");
		m.game = str("game");
		m.authors = strings("authors");
		if (root.contains("mirage")) {
			std::string why;
			if (auto r = semver::Range::Parse(str("mirage"), &why)) m.mirage = *r;
			else error("mirage: " + why);
		}

		m.entry = str("entry");
		if (!m.entry.empty() && (!InsideFolder(m.entry) || !EndsWith(m.entry, ".dll")))
			error("entry '" + m.entry + "' must be a .dll inside the mod's folder");
		m.lua = str("lua");
		if (!m.lua.empty() && (!InsideFolder(m.lua) || !EndsWith(m.lua, ".lua")))
			error("lua '" + m.lua + "' must be a .lua file inside the mod's folder");

		const std::string phase = str("phase", "engine");
		if (phase == "early") m.phase = Phase::Early;
		else if (phase == "engine") m.phase = Phase::Engine;
		else if (phase == "world") m.phase = Phase::World;
		else error("phase '" + phase + "' must be early, engine or world");

		m.content = strings("content");
		for (const std::string& c : m.content)
			if (!InsideFolder(c)) error("content '" + c + "' must be a folder inside the mod's folder");

		std::set<std::string> named;
		const auto deps = [&](const char* table, std::vector<Dependency>& list) {
			if (!root.contains(table)) return;
			const toml::value& t = root.at(table);
			if (!t.is_table()) {
				error(std::string("[") + table + "] must be a table of id = \"version range\"");
				return;
			}
			for (const auto& [id, value] : t.as_table()) {
				if (!ValidId(id)) error(std::string("[") + table + "] '" + id + "' is not a mod id");
				if (id == m.id) error(std::string("[") + table + "] names the mod itself");
				if (!named.insert(id).second) error("'" + id + "' is listed twice in the dependencies");
				if (!value.is_string()) {
					error(std::string("[") + table + "] " + id + " must be a version range string (\"*\" for any)");
					continue;
				}
				std::string why;
				if (auto r = semver::Range::Parse(value.as_string(), &why)) list.push_back({id, *r});
				else error(std::string("[") + table + "] " + id + ": " + why);
			}
		};
		deps("dependencies", m.required);
		deps("optional", m.optional);
		deps("incompatible", m.incompatible);

		if (out.errors.empty()) out.manifest = std::move(m);
		return out;
	}

	Parsed Load(const std::filesystem::path& dir)
	{
		const std::filesystem::path file = dir / "mod.toml";
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			Parsed out;
			out.errors.push_back(Utf8(file) + ": can't be read");
			return out;
		}
		std::stringstream text;
		text << in.rdbuf();
		Parsed out = Parse(text.str(), Utf8(file));
		if (!out.manifest) return out;

		Manifest& m = *out.manifest;
		m.dir = dir;
		const auto fail = [&](const std::string& what) {
			out.errors.push_back(Utf8(file) + ": " + what);
			out.manifest.reset();
		};
		if (m.lua.empty() && std::filesystem::is_regular_file(dir / "main.lua")) m.lua = "main.lua";
		if (Utf8(dir.filename()) != m.id) fail("the folder is '" + Utf8(dir.filename()) + "' but the id is '" + m.id + "' (they must match)");
		else if (!m.entry.empty() && !std::filesystem::is_regular_file(dir / FromUtf8(m.entry)))
			fail("entry " + m.entry + " is not in the folder");
		else if (!m.lua.empty() && !std::filesystem::is_regular_file(dir / FromUtf8(m.lua)))
			fail("lua " + m.lua + " is not in the folder");
		else if (m.entry.empty() && m.lua.empty() && m.content.empty())
			out.warnings.push_back(Utf8(file) + ": no entry, no Lua and no content: the mod does nothing");
		return out;
	}
}
