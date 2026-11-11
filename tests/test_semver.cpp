// Versions and ranges for mod manifests (src/runtime/semver.h)
#include "runtime/semver.h"

#include <gtest/gtest.h>

using mirage::semver::Compare;
using mirage::semver::Range;
using mirage::semver::Version;

namespace
{
	Version V(const char* s)
	{
		const auto v = Version::Parse(s);
		EXPECT_TRUE(v.has_value()) << s;
		return v.value_or(Version{});
	}
	bool In(const char* range, const char* version)
	{
		std::string error;
		const auto r = Range::Parse(range, &error);
		EXPECT_TRUE(r.has_value()) << range << ": " << error;
		return r && r->Matches(V(version));
	}
}

TEST(Semver, ParsesVersions)
{
	const Version v = V("1.22.333-rc.1+build.5");
	EXPECT_EQ(v.major, 1u);
	EXPECT_EQ(v.minor, 22u);
	EXPECT_EQ(v.patch, 333u);
	EXPECT_EQ(v.pre, "rc.1");
	EXPECT_EQ(v.build, "build.5");
	EXPECT_EQ(v.ToString(), "1.22.333-rc.1+build.5");
	for (const char* bad : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.2.x", "a.b.c", "1.2.3-", "1.2.3-01", "1.2.3+", "-1.2.3"})
		EXPECT_FALSE(Version::Parse(bad).has_value()) << bad;
}

TEST(Semver, OrdersLikeTheSpec)
{
	// semver.org rule 11's example, in order
	const char* order[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
						   "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0", "10.0.0"};
	for (std::size_t i = 0; i + 1 < std::size(order); ++i) {
		EXPECT_LT(Compare(V(order[i]), V(order[i + 1])), 0) << order[i] << " < " << order[i + 1];
		EXPECT_GT(Compare(V(order[i + 1]), V(order[i])), 0);
	}
	EXPECT_EQ(Compare(V("1.0.0+a"), V("1.0.0+b")), 0);	// Build metadata doesn't count
}

TEST(Semver, Ranges)
{
	EXPECT_TRUE(In("*", "0.0.1"));
	EXPECT_TRUE(In("1.2.3", "1.2.3"));
	EXPECT_FALSE(In("=1.2.3", "1.2.4"));

	EXPECT_TRUE(In("^1.2.3", "1.9.0"));
	EXPECT_FALSE(In("^1.2.3", "2.0.0"));
	EXPECT_FALSE(In("^1.2.3", "1.2.2"));
	EXPECT_TRUE(In("^0.2.3", "0.2.9"));
	EXPECT_FALSE(In("^0.2.3", "0.3.0"));
	EXPECT_TRUE(In("^0.0.3", "0.0.3"));
	EXPECT_FALSE(In("^0.0.3", "0.0.4"));
	EXPECT_TRUE(In("^1", "1.99.0"));

	EXPECT_TRUE(In("~1.2.3", "1.2.9"));
	EXPECT_FALSE(In("~1.2.3", "1.3.0"));
	EXPECT_TRUE(In("~1", "1.5.0"));

	EXPECT_TRUE(In("1.2.x", "1.2.7"));
	EXPECT_FALSE(In("1.2", "1.3.0"));
	EXPECT_TRUE(In("1.x", "1.3.0"));

	EXPECT_TRUE(In(">=1.2 <2", "1.2.0"));
	EXPECT_TRUE(In(">=1.2, <2", "1.9.9"));
	EXPECT_FALSE(In(">= 1.2 < 2", "2.0.0"));
	EXPECT_TRUE(In(">1.2", "1.3.0"));
	EXPECT_FALSE(In(">1.2", "1.2.5"));		// >1.2 means past every 1.2.x
	EXPECT_TRUE(In("<=1.2", "1.2.9"));
	EXPECT_FALSE(In("<1.2", "1.2.0"));

	EXPECT_TRUE(In("^1.0 || ^3.0", "3.1.0"));
	EXPECT_FALSE(In("^1.0 || ^3.0", "2.0.0"));

	EXPECT_FALSE(In(">=1.0.0", "1.0.0-beta"));	// A pre-release is before its release
	EXPECT_TRUE(In(">=1.0.0-beta", "1.0.0-rc.1"));
}

TEST(Semver, RejectsBadRanges)
{
	for (const char* bad : {"", "  ", ">=", "abc", "^1.2.3.4", "1.2.3 ||", "|| 1.0", ">*", "1.0-beta"}) {
		std::string error;
		EXPECT_FALSE(Range::Parse(bad, &error).has_value()) << "'" << bad << "'";
		EXPECT_FALSE(error.empty()) << "'" << bad << "'";
	}
}
