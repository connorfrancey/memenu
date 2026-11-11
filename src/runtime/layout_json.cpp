// mirage::layout::ToJson / FromJson: layouts saved to a file and read back. The in-game self-test saves what only the
// engine knows (the intrinsic classes no package declares), and the SDK generator reads it (Mirage, step 3).

#include "runtime/layout.h"

#include <nlohmann/json.hpp>

namespace mirage::layout
{
	namespace
	{
		Kind KindFromName(const std::string& name)
		{
			for (int k = 0; k <= static_cast<int>(Kind::Unknown); ++k)
				if (name == KindName(static_cast<Kind>(k))) return static_cast<Kind>(k);
			return Kind::Unknown;
		}
	}

	const std::vector<std::string>& SdkPackages()
	{
		// The packages the game loads (the old generated SDK's set, without Editor, UnrealEd and TdEditor: the game
		// loads their classes with the properties stripped, so they hold nothing a mod could use). WinDrv, ALAudio and
		// OnlineSubsystemPC are native only (no .u file: everything about them comes from the engine).
		static const std::vector<std::string> packages{"Core", "Engine", "GameFramework", "IpDrv", "WinDrv", "ALAudio",
			"OnlineSubsystemPC", "Ts", "Tp", "Fp", "TdGame", "TdSharedContent", "TdTTContent", "TdTuContent"};
		return packages;
	}

	std::string ToJson(const Layouts& layouts)
	{
		nlohmann::ordered_json root = nlohmann::ordered_json::array();
		for (const auto& [path, s] : layouts) {
			nlohmann::ordered_json j;
			j["path"] = s.path;
			j["kind"] = s.kind;
			j["super"] = s.super;
			j["size"] = s.size;
			if (s.functionFlags) j["functionFlags"] = s.functionFlags;
			nlohmann::ordered_json props = nlohmann::ordered_json::array();
			for (const Property& p : s.properties) {
				nlohmann::ordered_json q;
				q["name"] = p.name;
				q["kind"] = KindName(p.kind);
				q["offset"] = p.offset;
				q["size"] = p.elementSize;
				if (p.arrayDim != 1) q["dim"] = p.arrayDim;
				if (p.boolMask) q["mask"] = p.boolMask;
				q["flags"] = p.flags;
				if (!p.typePath.empty()) q["type"] = p.typePath;
				if (p.innerKind != Kind::Unknown) q["inner"] = KindName(p.innerKind);
				if (!p.innerPath.empty()) q["innerType"] = p.innerPath;
				props.push_back(std::move(q));
			}
			j["properties"] = std::move(props);
			root.push_back(std::move(j));
		}
		return root.dump(1, '\t');
	}

	Layouts FromJson(const std::string& text, std::string* error)
	{
		Layouts out;
		try {
			const auto root = nlohmann::json::parse(text);
			for (const auto& j : root) {
				Struct s;
				s.path = j.at("path").get<std::string>();
				s.kind = j.at("kind").get<std::string>();
				s.super = j.value("super", "");
				s.size = j.at("size").get<std::uint32_t>();
				s.functionFlags = j.value("functionFlags", 0u);
				for (const auto& q : j.at("properties")) {
					Property p;
					p.name = q.at("name").get<std::string>();
					p.kind = KindFromName(q.at("kind").get<std::string>());
					p.offset = q.at("offset").get<std::uint32_t>();
					p.elementSize = q.at("size").get<std::uint32_t>();
					p.arrayDim = q.value("dim", 1u);
					p.boolMask = q.value("mask", 0u);
					p.flags = q.value("flags", std::uint64_t{0});
					p.typePath = q.value("type", "");
					p.type = p.typePath.substr(p.typePath.find_last_of('.') + 1);
					p.innerKind = KindFromName(q.value("inner", "Unknown"));
					p.innerPath = q.value("innerType", "");
					p.innerType = p.innerPath.substr(p.innerPath.find_last_of('.') + 1);
					s.properties.push_back(std::move(p));
				}
				out.emplace(s.path, std::move(s));
			}
		} catch (const std::exception& e) {
			if (error) *error = e.what();
			out.clear();
		}
		return out;
	}
}
