#include "runtime/manifest.h"

#include <toml.hpp>

#include <algorithm>
#include <cctype>
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

	const char* SettingTypeName(SettingDecl::Type t)
	{
		switch (t) {
			case SettingDecl::Type::Bool: return "bool";
			case SettingDecl::Type::Int: return "int";
			case SettingDecl::Type::Float: return "float";
			case SettingDecl::Type::String: return "string";
			case SettingDecl::Type::Choice: return "choice";
			case SettingDecl::Type::Key: return "key";
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
		auto parsed = toml::try_parse<toml::ordered_type_config>(in, source);	// Ordered: settings keep the file's order
		if (parsed.is_err()) {
			for (const auto& e : parsed.unwrap_err()) out.errors.push_back(toml::format_error(e));
			return out;
		}
		const toml::ordered_value root = parsed.unwrap();
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
			const toml::ordered_value& v = root.at(key);
			if (!v.is_string()) {
				error(std::string(key) + " must be a string");
				return fallback;
			}
			return v.as_string();
		};
		const auto strings = [&](const char* key) {
			std::vector<std::string> list;
			if (!root.contains(key)) return list;
			const toml::ordered_value& v = root.at(key);
			if (!v.is_array()) {
				error(std::string(key) + " must be a list of strings");
				return list;
			}
			for (const toml::ordered_value& e : v.as_array()) {
				if (e.is_string()) list.push_back(e.as_string());
				else error(std::string(key) + " must be a list of strings");
			}
			return list;
		};

		static const std::set<std::string> known = {"id", "name", "version", "description", "authors", "mirage", "game",
			"entry", "lua", "phase", "content", "dependencies", "optional", "incompatible", "settings"};
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
			const toml::ordered_value& t = root.at(table);
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

		// [settings.<key>]: declared settings
		if (root.contains("settings")) {
			const toml::ordered_value& table = root.at("settings");
			if (!table.is_table()) error("settings must be tables: [settings.<key>]");
			else
				for (const auto& [key, v] : table.as_table()) {
					const std::string where = "settings." + key;
					if (!ValidId(key) && !(key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0])))) {
						error(where + ": a setting key is a-z, 0-9, '-' and '_'");
						continue;
					}
					if (!v.is_table()) {
						error(where + " must be a table ([" + where + "])");
						continue;
					}
					static const std::set<std::string> settingKeys = {"type", "name", "description", "default", "min",
						"max", "format", "options", "module"};
					for (const auto& [k, unused] : v.as_table())
						if (!settingKeys.count(k)) warn(where + ": unknown key '" + k + "' (ignored)");
					const auto textField = [&](const char* k) -> std::string {
						if (!v.contains(k)) return {};
						if (!v.at(k).is_string()) {
							error(where + "." + k + " must be a string");
							return {};
						}
						return v.at(k).as_string();
					};
					const auto number = [&](const char* k, double& out) {
						if (!v.contains(k)) return false;
						const toml::ordered_value& n = v.at(k);
						if (n.is_integer()) out = static_cast<double>(n.as_integer());
						else if (n.is_floating()) out = n.as_floating();
						else {
							error(where + "." + k + " must be a number");
							return false;
						}
						return true;
					};

					SettingDecl d;
					d.key = key;
					d.name = textField("name");
					if (d.name.empty()) d.name = key;
					d.description = textField("description");
					d.module = textField("module");
					d.format = textField("format");
					const std::string type = textField("type");
					using T = SettingDecl::Type;
					if (type == "bool") d.type = T::Bool;
					else if (type == "int") d.type = T::Int;
					else if (type == "float") d.type = T::Float;
					else if (type == "string") d.type = T::String;
					else if (type == "choice") d.type = T::Choice;
					else if (type == "key") d.type = T::Key;
					else {
						error(where + ".type must be bool, int, float, string, choice or key" +
							  (type.empty() ? std::string(" (it is missing)") : ", not '" + type + "'"));
						continue;
					}
					if ((d.type == T::Int || d.type == T::Float)) {
						const bool hasMin = number("min", d.min), hasMax = number("max", d.max);
						if (hasMin != hasMax) error(where + ": min and max go together");
						else if (!hasMin) error(where + ": an " + type + " setting needs min and max");
						else if (!(d.min < d.max)) error(where + ": min must be below max");
						d.hasRange = hasMin && hasMax;
					} else if (v.contains("min") || v.contains("max")) {
						warn(where + ": min and max are for int and float settings (ignored)");
					}
					if (d.type == T::Choice) {
						if (!v.contains("options") || !v.at("options").is_array() || v.at("options").as_array().empty()) {
							error(where + ": a choice setting needs options = [\"...\", ...]");
							continue;
						}
						for (const auto& o : v.at("options").as_array()) {
							if (o.is_string()) d.options.push_back(o.as_string());
							else error(where + ".options must be strings");
						}
					}
					if (!v.contains("default")) {
						error(where + ".default is required");
						continue;
					}
					const toml::ordered_value& def = v.at("default");
					switch (d.type) {
						case T::Bool:
							if (def.is_boolean()) d.boolDefault = def.as_boolean();
							else error(where + ".default must be true or false");
							break;
						case T::Int: case T::Key:
							if (def.is_integer()) d.intDefault = def.as_integer();
							else error(where + ".default must be an integer");
							if (d.type == T::Int && d.hasRange && (d.intDefault < d.min || d.intDefault > d.max))
								error(where + ".default is outside min..max");
							if (d.type == T::Key && (d.intDefault < 0 || d.intDefault > 0xFFFF))
								error(where + ".default must be a key code (0 = none)");
							break;
						case T::Float:
							if (def.is_floating()) d.floatDefault = def.as_floating();
							else if (def.is_integer()) d.floatDefault = static_cast<double>(def.as_integer());
							else error(where + ".default must be a number");
							if (d.hasRange && (d.floatDefault < d.min || d.floatDefault > d.max))
								error(where + ".default is outside min..max");
							break;
						case T::String:
							if (def.is_string()) d.stringDefault = def.as_string();
							else error(where + ".default must be a string");
							break;
						case T::Choice: {
							if (!def.is_string()) {
								error(where + ".default must be one of the options");
								break;
							}
							const auto it = std::find(d.options.begin(), d.options.end(), def.as_string());
							if (it == d.options.end()) error(where + ".default '" + def.as_string() + "' is not one of the options");
							else d.intDefault = it - d.options.begin();
							break;
						}
					}
					if (std::any_of(m.settings.begin(), m.settings.end(), [&](const SettingDecl& o) { return o.name == d.name; }))
						error(where + ": another setting is also named '" + d.name + "'");
					m.settings.push_back(std::move(d));
				}
		}

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
