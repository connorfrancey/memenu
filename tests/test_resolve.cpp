// Which mods load and in what order (src/runtime/resolve.h), on synthetic manifests
#include "runtime/resolve.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace mirage::mods;
using mirage::semver::Range;
using mirage::semver::Version;

namespace
{
	const Version kRuntime = *Version::Parse("0.1.0");

	struct Mod
	{
		Manifest m;
		explicit Mod(const char* id, const char* version = "1.0.0")
		{
			m.id = id;
			m.version = *Version::Parse(version);
		}
		Mod& Needs(const char* id, const char* range = "*")
		{
			m.required.push_back({id, *Range::Parse(range)});
			return *this;
		}
		Mod& Wants(const char* id, const char* range = "*")
		{
			m.optional.push_back({id, *Range::Parse(range)});
			return *this;
		}
		Mod& Breaks(const char* id, const char* range = "*")
		{
			m.incompatible.push_back({id, *Range::Parse(range)});
			return *this;
		}
		Mod& In(Phase p)
		{
			m.phase = p;
			return *this;
		}
		Mod& Mirage(const char* range)
		{
			m.mirage = *Range::Parse(range);
			return *this;
		}
		operator Manifest() const { return m; }
	};

	Resolution Solve(std::vector<Manifest> mods, std::map<std::string, std::string> off = {})
	{
		return Resolve(mods, kRuntime, off);
	}
	bool Before(const Resolution& r, const std::string& a, const std::string& b)
	{
		const auto ia = std::find(r.order.begin(), r.order.end(), a), ib = std::find(r.order.begin(), r.order.end(), b);
		return ia != r.order.end() && ib != r.order.end() && ia < ib;
	}
	bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}

TEST(Resolve, ChainLoadsDependenciesFirst)
{
	const Resolution r = Solve({Mod("aa").Needs("bb"), Mod("bb").Needs("cc"), Mod("cc"), Mod("zz")});
	EXPECT_TRUE(r.excluded.empty());
	ASSERT_EQ(r.order.size(), 4u);
	EXPECT_EQ(r.order, (std::vector<std::string>{"cc", "bb", "aa", "zz"}));
}

TEST(Resolve, IndependentModsGoByPhaseThenId)
{
	const Resolution r = Solve({Mod("bb"), Mod("aa").In(Phase::World), Mod("cc").In(Phase::Early)});
	EXPECT_EQ(r.order, (std::vector<std::string>{"cc", "bb", "aa"}));
}

TEST(Resolve, MissingAndOutOfRangeDependencies)
{
	const Resolution r = Solve({Mod("aa").Needs("missing"), Mod("bb").Needs("cc", "^2"), Mod("cc", "1.5.0"), Mod("dd").Needs("cc", ">=1.2")});
	EXPECT_TRUE(Has(r.excluded.at("aa"), "missing"));
	EXPECT_TRUE(Has(r.excluded.at("bb"), "1.5.0 is installed"));
	EXPECT_FALSE(r.excluded.count("dd"));
	EXPECT_TRUE(Before(r, "cc", "dd"));
}

TEST(Resolve, ExclusionCascades)
{
	// cc needs a newer runtime; bb needs cc; aa needs bb: all three are out, ee is fine
	const Resolution r = Solve({Mod("aa").Needs("bb"), Mod("bb").Needs("cc"), Mod("cc").Mirage(">=9"), Mod("ee")});
	EXPECT_TRUE(Has(r.excluded.at("cc"), "Mirage >=9"));
	EXPECT_TRUE(Has(r.excluded.at("bb"), "needs cc"));
	EXPECT_TRUE(Has(r.excluded.at("aa"), "needs bb"));
	EXPECT_EQ(r.order, (std::vector<std::string>{"ee"}));
}

TEST(Resolve, RequiredCyclesAreLeftOut)
{
	const Resolution r = Solve({Mod("aa").Needs("bb"), Mod("bb").Needs("cc"), Mod("cc").Needs("aa"), Mod("dd").Needs("aa"), Mod("ee")});
	for (const char* id : {"aa", "bb", "cc"}) EXPECT_TRUE(Has(r.excluded.at(id), "cycle")) << id;
	EXPECT_TRUE(Has(r.excluded.at("dd"), "needs aa"));
	EXPECT_EQ(r.order, (std::vector<std::string>{"ee"}));
}

TEST(Resolve, SelfCycleThroughTwo)
{
	const Resolution r = Solve({Mod("aa").Needs("bb"), Mod("bb").Needs("aa")});
	EXPECT_TRUE(r.order.empty());
	EXPECT_TRUE(Has(r.excluded.at("aa"), "aa -> bb -> aa") || Has(r.excluded.at("aa"), "bb -> aa -> bb"));
}

TEST(Resolve, OptionalDependencies)
{
	// Present and in range: ordered first. Missing: fine. Out of range or left out: a warning, no order.
	const Resolution r = Solve({Mod("aa").Wants("zz"), Mod("zz"), Mod("bb").Wants("missing"), Mod("cc").Wants("zz", "^2"),
							  Mod("dd").Wants("gone"), Mod("gone").Mirage(">=9")});
	EXPECT_TRUE(Before(r, "zz", "aa"));
	EXPECT_FALSE(r.excluded.count("bb"));
	EXPECT_FALSE(r.excluded.count("cc"));
	EXPECT_FALSE(r.excluded.count("dd"));
	EXPECT_TRUE(r.excluded.count("gone"));
	const auto warned = [&](const char* part) {
		return std::any_of(r.warnings.begin(), r.warnings.end(), [&](const std::string& w) { return Has(w, part); });
	};
	EXPECT_TRUE(warned("cc: optional zz"));
	EXPECT_TRUE(warned("dd: optional gone"));
}

TEST(Resolve, OptionalCycleIsBroken)
{
	const Resolution r = Solve({Mod("aa").Wants("bb"), Mod("bb").Wants("aa")});
	EXPECT_TRUE(r.excluded.empty());
	EXPECT_EQ(r.order.size(), 2u);
	EXPECT_FALSE(r.warnings.empty());
}

TEST(Resolve, MixedCycleKeepsRequiredOrder)
{
	// bb requires aa; aa optionally wants bb: the optional edge gives way
	const Resolution r = Solve({Mod("aa").Wants("bb"), Mod("bb").Needs("aa")});
	EXPECT_TRUE(r.excluded.empty());
	EXPECT_EQ(r.order, (std::vector<std::string>{"aa", "bb"}));
}

TEST(Resolve, Phases)
{
	// An early mod can't need an engine one; a world mod can need an early one
	const Resolution r = Solve({Mod("aa").In(Phase::Early).Needs("bb"), Mod("bb"), Mod("cc").In(Phase::World).Needs("dd"),
							  Mod("dd").In(Phase::Early)});
	EXPECT_TRUE(Has(r.excluded.at("aa"), "later"));
	EXPECT_EQ(r.order, (std::vector<std::string>{"dd", "bb", "cc"}));
}

TEST(Resolve, Incompatible)
{
	const Resolution r = Solve({Mod("aa").Breaks("bb", "<2"), Mod("bb", "1.0.0"), Mod("cc").Breaks("dd", "<2"), Mod("dd", "2.0.0"),
							  Mod("ee").Breaks("missing")});
	EXPECT_TRUE(Has(r.excluded.at("aa"), "doesn't work with bb 1.0.0"));
	EXPECT_FALSE(r.excluded.count("bb"));
	EXPECT_FALSE(r.excluded.count("cc"));
	EXPECT_FALSE(r.excluded.count("ee"));
}

TEST(Resolve, OffListAndDuplicates)
{
	const Resolution r = Solve({Mod("aa"), Mod("bb").Needs("aa"), Mod("cc"), Mod("cc")},
							 {{"aa", "it crashed the last time"}});
	EXPECT_EQ(r.excluded.at("aa"), "it crashed the last time");
	EXPECT_TRUE(Has(r.excluded.at("bb"), "crashed"));
	EXPECT_TRUE(Has(r.excluded.at("cc"), "two mods"));
	EXPECT_TRUE(r.order.empty());
}

TEST(Resolve, AnIncompatibleModThatIsOffDoesntCount)
{
	const Resolution r = Solve({Mod("aa").Breaks("bb"), Mod("bb")}, {{"bb", "turned off"}});
	EXPECT_EQ(r.order, (std::vector<std::string>{"aa"}));
}
