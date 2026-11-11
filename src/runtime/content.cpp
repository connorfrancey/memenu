#include "runtime/content.h"

#include "game/object_cache.h"
#include "game/object_query.h"
#include "game/uobject.h"
#include "runtime/reflect.h"
#include "runtime/texture_overlays.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>
#include <toml.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace mirage::content
{
	namespace
	{
		struct Value
		{
			enum class Kind : std::uint8_t { Bool, Int, Float, Text } kind = Kind::Int;
			bool b = false;
			std::int64_t i = 0;
			double f = 0.0;
			std::string text;
		};
		struct Override
		{
			std::string path;			// "MaxStepHeight", "Location.Z"
			Value value;
		};
		struct Target
		{
			std::string key;			// As written: "TdGame.Default__TdPawn" or "Class Package.Name"
			bool instances = false;
			std::vector<Override> overrides;
		};
		// A value as it was before: the object (with its index, to know it is still the same object) and the bytes
		struct Saved
		{
			Classes::UObject* object;
			std::size_t index;
			std::uint8_t* address;
			std::vector<std::uint8_t> bytes;
		};
	}

	class Set
	{
	public:
		std::string mod;
		std::vector<Target> targets;
		std::vector<textures::Replacement> textures;
		bool applied = false;
		std::vector<Saved> saved;
		Stats stats;
	};

	namespace
	{
		std::mutex g_lock;
		std::vector<std::shared_ptr<Set>> g_sets;

		void Flatten(const toml::value& v, const std::string& path, std::vector<Override>& out, const std::string& where,
					 std::vector<std::string>& problems)
		{
			Value x;
			if (v.is_boolean()) {
				x.kind = Value::Kind::Bool;
				x.b = v.as_boolean();
			} else if (v.is_integer()) {
				x.kind = Value::Kind::Int;
				x.i = v.as_integer();
			} else if (v.is_floating()) {
				x.kind = Value::Kind::Float;
				x.f = v.as_floating();
			} else if (v.is_string()) {
				x.kind = Value::Kind::Text;
				x.text = v.as_string();
			} else if (v.is_table()) {
				for (const auto& [k, sub] : v.as_table()) Flatten(sub, path + "." + k, out, where, problems);
				return;
			} else {
				problems.push_back(where + ": " + path + " is neither a boolean, a number, a string nor a table");
				return;
			}
			out.push_back({path, x});
		}

		// The object a key names: a full name ("Class Package.Name"), or a path, for a default object found by its
		// class ("Default__TdPawn" is a TdPawn), else looked for in the object list
		Classes::UObject* FindTarget(const std::string& key)
		{
			if (key.find(' ') != std::string::npos) return game::FindObject<Classes::UObject>(key);
			const std::string name = key.substr(key.find_last_of('.') + 1);
			if (name.rfind("Default__", 0) == 0)
				if (auto* o = game::FindObject<Classes::UObject>(name.substr(9) + " " + key)) return o;
			return game::FindByPath(key);		// The object index: by name, no string built per object
		}

		bool IsDefaultObject(const Classes::UObject* o) { return o->GetName().rfind("Default__", 0) == 0; }

		// `v` written to the member, its bytes before saved (once per object and member). An instance of a class whose
		// default object the set changes is saved with the default object's original: one made while the set was on
		// already holds the set's value.
		bool Write(Set& set, Classes::UObject* object, const Override& o, Classes::UObject* defaults = nullptr)
		{
			const reflect::Value at = reflect::Resolve(object->Class, object, o.path);
			if (!at) return false;
			const std::size_t size = at.member->elementSize ? at.member->elementSize : 4;
			const bool seen = std::any_of(set.saved.begin(), set.saved.end(), [&](const Saved& s) { return s.address == at.address; });
			if (!seen) {
				std::vector<std::uint8_t> bytes(at.address, at.address + size);
				if (defaults) {
					const reflect::Value d = reflect::Resolve(defaults->Class, defaults, o.path);
					const auto it = std::find_if(set.saved.begin(), set.saved.end(), [&](const Saved& s) { return d && s.address == d.address; });
					if (it != set.saved.end() && it->bytes.size() == size) bytes = it->bytes;
				}
				set.saved.push_back({object, game::IndexOf(object), at.address, std::move(bytes)});
			}
			switch (o.value.kind) {
				case Value::Kind::Bool: return reflect::SetInt(at, o.value.b ? 1 : 0);
				case Value::Kind::Int: return reflect::SetInt(at, static_cast<std::int32_t>(o.value.i));
				case Value::Kind::Float: return reflect::SetFloat(at, static_cast<float>(o.value.f));
				case Value::Kind::Text: return reflect::SetText(at, o.value.text);
			}
			return false;
		}

		void Note(Set& set, std::string what)
		{
			++set.stats.missing;
			if (set.stats.notes.size() < 8) set.stats.notes.push_back(what);
			spdlog::warn("[mod {}] content: {}", set.mod, set.stats.notes.back());
		}
	}

	std::shared_ptr<Set> Load(const std::string& modId, const std::vector<std::filesystem::path>& folders,
							  std::vector<std::string>& problems)
	{
		auto set = std::make_shared<Set>();
		set->mod = modId;
		for (const auto& folder : folders) {
			// textures/<full path of the texture>.<image>
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(folder / "textures", ec)) {
				if (!e.is_regular_file(ec)) continue;
				std::string ext = e.path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (ext != ".png" && ext != ".tga" && ext != ".bmp" && ext != ".jpg" && ext != ".jpeg" && ext != ".dds") {
					problems.push_back("textures/" + e.path().filename().string() + " isn't an image (png, tga, bmp, jpg or dds)");
					continue;
				}
				const std::string path = textures::NormalizePath(e.path().stem().string());
				if (path.find('.') == std::string::npos) {
					problems.push_back("textures/" + e.path().filename().string() +
									   ": name it by the texture's full path, like M_Rooftops.Textures.T_Roof_02_D.png");
					continue;
				}
				std::erase_if(set->textures, [&](const textures::Replacement& r) { return r.path == path; });
				set->textures.push_back({path, e.path()});
			}
			const auto file = folder / "defaults.toml";
			if (!std::filesystem::exists(file)) continue;
			try {
				const toml::value root = toml::parse(file);
				for (const auto& [key, table] : root.as_table()) {
					const std::string where = file.filename().string() + " [" + key + "]";
					if (!table.is_table()) {
						problems.push_back(where + " must be a table of properties");
						continue;
					}
					Target t;
					t.key = key;
					for (const auto& [prop, v] : table.as_table()) {
						if (prop == "instances") {
							if (v.is_boolean()) t.instances = v.as_boolean();
							else problems.push_back(where + ": instances must be true or false");
							continue;
						}
						Flatten(v, prop, t.overrides, where, problems);
					}
					set->targets.push_back(std::move(t));
				}
			} catch (const std::exception& e) {
				problems.push_back(e.what());
			}
		}
		return set;
	}

	bool Empty(const Set& set) { return set.targets.empty() && set.textures.empty(); }
	bool Applied(const Set& set) { return set.applied; }
	Stats GetStats(const Set& set) { return set.stats; }

	void Apply(Set& set)
	{
		set.applied = true;
		set.stats = {};
		set.stats.textures = static_cast<int>(set.textures.size());
		if (!set.textures.empty()) textures::Activate(set.mod, set.textures);	// Again after a level load: a no-op
		for (const Target& t : set.targets) {
			Classes::UObject* object = FindTarget(t.key);
			if (!object) {
				Note(set, t.key + " isn't loaded (yet: tried again after each level load)");
				continue;
			}
			std::vector<Classes::UObject*> objects{object};
			if (t.instances && IsDefaultObject(object)) {
				// Every object of the default object's class, as it is now
				for (Classes::UObject* o : game::ObjectsOf(object->Class, game::Freshness::Exact))
					if (o != object && o->Class == object->Class && !IsDefaultObject(o)) objects.push_back(o);
			}
			for (Classes::UObject* o : objects) {		// The default object first: the instances' originals come from it
				++set.stats.objects;
				for (const Override& ov : t.overrides) {
					if (Write(set, o, ov, o != object ? object : nullptr)) ++set.stats.values;
					else if (o == object) Note(set, t.key + ": " + ov.path + " can't be set (no such property, or another type)");
				}
			}
		}
		spdlog::info("[mod {}] content on: {} values on {} objects{}{}", set.mod, set.stats.values, set.stats.objects,
					 set.stats.missing ? " (" + std::to_string(set.stats.missing) + " not found)" : std::string(),
					 set.textures.empty() ? std::string() : ", " + std::to_string(set.textures.size()) + " texture(s)");
	}

	void Restore(Set& set)
	{
		std::size_t restored = 0;
		for (auto it = set.saved.rbegin(); it != set.saved.rend(); ++it)
			if (game::AliveAt(it->object, it->index)) {	// Gone with its level: nothing to put back
				std::memcpy(it->address, it->bytes.data(), it->bytes.size());
				++restored;
			}
		set.saved.clear();
		// Instances made while the set was on (a respawned pawn, between level loads) were never saved: they get the
		// default object's value, which is the original again now
		for (const Target& t : set.targets) {
			if (!t.instances) continue;
			Classes::UObject* defaults = FindTarget(t.key);
			if (!defaults || !IsDefaultObject(defaults)) continue;
			const auto& all = Classes::UObject::GetGlobalObjects();
			for (std::size_t i = 0; i < all.Num(); ++i) {
				auto* o = all.GetByIndex(i);
				if (!o || o == defaults || o->Class != defaults->Class || IsDefaultObject(o)) continue;
				for (const Override& ov : t.overrides) {
					const reflect::Value from = reflect::Resolve(defaults->Class, defaults, ov.path);
					const reflect::Value to = reflect::Resolve(o->Class, o, ov.path);
					if (!from || !to) continue;
					std::memcpy(to.address, from.address, from.member->elementSize ? from.member->elementSize : 4);
					++restored;
				}
			}
		}
		if (!set.textures.empty()) textures::Deactivate(set.mod);
		set.applied = false;
		spdlog::info("[mod {}] content off: {} values put back{}", set.mod, restored,
					 set.textures.empty() ? std::string() : ", textures original from the next level load");
	}

	void Register(std::shared_ptr<Set> set)
	{
		std::lock_guard lock(g_lock);
		g_sets.push_back(std::move(set));
	}

	void Unregister(const Set& set)
	{
		std::lock_guard lock(g_lock);
		std::erase_if(g_sets, [&](const auto& s) { return s.get() == &set; });
	}

	void ReapplyAll()
	{
		std::vector<std::shared_ptr<Set>> sets;
		{
			std::lock_guard lock(g_lock);
			sets = g_sets;
		}
		for (auto& s : sets) {
			if (!s->applied) continue;
			// Values of objects gone with the old level are forgotten; the rest stay as saved (not saved again over
			// the mod's own values)
			std::erase_if(s->saved, [](const Saved& v) { return !game::AliveAt(v.object, v.index); });
			Apply(*s);
		}
	}
}
