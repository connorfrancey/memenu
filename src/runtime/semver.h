#pragma once
// Semantic versions (https://semver.org) and version ranges, for mod manifests (Mirage, steps 4 and 5).
//
// Ranges use the npm / Cargo syntax mod authors know:
//   1.2.3  =1.2.3          exactly
//   >1.2  >=1.2.3  <2  <=2.1
//   ^1.2.3                 >=1.2.3 <2.0.0   (^0.2.3: <0.3.0, ^0.0.3: <0.0.4: the leftmost non-zero part is kept)
//   ~1.2.3  ~1.2           >=1.2.3 <1.3.0
//   1.2.x  1.x  1  *       any 1.2, any 1, anything
//   >=1.2 <2   >=1.2, <2   all of them (spaces or commas)
//   ^1.0 || ^2.0           either
// A pre-release (1.2.0-beta) is ordered before its release and compared like any other version.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mirage::semver
{
	struct Version
	{
		std::uint32_t major = 0, minor = 0, patch = 0;
		std::string pre;		// "beta.2" (without the '-'); empty for a release
		std::string build;		// "+..." metadata: kept, never compared

		// "1.2.3", "1.2.3-rc.1+abc"; nullopt (and `error`) when it isn't one
		static std::optional<Version> Parse(std::string_view text, std::string* error = nullptr);
		std::string ToString() const;
	};
	// -1, 0, 1 (build metadata ignored, pre-releases per semver 2.0.0 rule 11)
	int Compare(const Version& a, const Version& b);
	inline bool operator==(const Version& a, const Version& b) { return Compare(a, b) == 0; }
	inline bool operator<(const Version& a, const Version& b) { return Compare(a, b) < 0; }

	class Range
	{
	public:
		// nullopt (and `error`) when the text isn't a range
		static std::optional<Range> Parse(std::string_view text, std::string* error = nullptr);
		static Range Any();

		bool Matches(const Version& v) const;
		const std::string& Text() const { return text_; }

	private:
		enum class Op : std::uint8_t { Eq, Lt, Le, Gt, Ge };
		struct Comparator
		{
			Op op;
			Version v;
		};
		std::vector<std::vector<Comparator>> sets_;	// Any set whose comparators all hold
		std::string text_;

		static bool AddComparator(std::string_view token, std::vector<Comparator>& out, std::string* error);
	};
}
