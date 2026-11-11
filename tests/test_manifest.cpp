// mod.toml (src/runtime/manifest.h)
#include "runtime/manifest.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace mirage::mods;

namespace
{
	bool HasMessage(const std::vector<std::string>& list, const std::string& part)
	{
		for (const auto& s : list)
			if (s.find(part) != std::string::npos) return true;
		return false;
	}
}

TEST(Manifest, ReadsEverything)
{
	const Parsed p = Parse(R"(
id = "hello-world"
name = "Hello World"
version = "1.2.3-beta"
description = "Says hello"
authors = ["A", "B"]
mirage = ">=0.1, <1"
game = "1.0"
entry = "bin/hello.dll"
lua = "main.lua"
phase = "world"
content = ["content"]

[dependencies]
core-lib = "^1.2"

[optional]
extras = "*"

[incompatible]
old_hello = "<2"
)");
	ASSERT_TRUE(p.manifest) << (p.errors.empty() ? "" : p.errors[0]);
	EXPECT_TRUE(p.errors.empty());
	EXPECT_TRUE(p.warnings.empty());
	const Manifest& m = *p.manifest;
	EXPECT_EQ(m.id, "hello-world");
	EXPECT_EQ(m.name, "Hello World");
	EXPECT_EQ(m.version.ToString(), "1.2.3-beta");
	EXPECT_EQ(m.authors.size(), 2u);
	EXPECT_TRUE(m.mirage.Matches(*mirage::semver::Version::Parse("0.5.0")));
	EXPECT_FALSE(m.mirage.Matches(*mirage::semver::Version::Parse("1.0.0")));
	EXPECT_EQ(m.entry, "bin/hello.dll");
	EXPECT_EQ(m.phase, Phase::World);
	ASSERT_EQ(m.required.size(), 1u);
	EXPECT_EQ(m.required[0].id, "core-lib");
	EXPECT_TRUE(m.required[0].range.Matches(*mirage::semver::Version::Parse("1.9.0")));
	ASSERT_EQ(m.optional.size(), 1u);
	ASSERT_EQ(m.incompatible.size(), 1u);
	EXPECT_EQ(m.incompatible[0].id, "old_hello");
}

TEST(Manifest, Defaults)
{
	const Parsed p = Parse("id = \"tiny\"\nversion = \"0.1.0\"\n");
	ASSERT_TRUE(p.manifest);
	EXPECT_EQ(p.manifest->name, "tiny");
	EXPECT_EQ(p.manifest->phase, Phase::Engine);
	EXPECT_TRUE(p.manifest->mirage.Matches(*mirage::semver::Version::Parse("9.9.9")));
}

TEST(Manifest, ReportsMistakes)
{
	struct Case
	{
		const char* toml;
		const char* message;
	};
	const Case cases[] = {
		{"version = \"1.0.0\"", "id is required"},
		{"id = \"Hello\"\nversion = \"1.0.0\"", "id 'Hello'"},
		{"id = \"x\"\nversion = \"1.0.0\"", "id 'x'"},
		{"id = \"hello\"", "version is required"},
		{"id = \"hello\"\nversion = \"1.0\"", "version:"},
		{"id = \"hello\"\nversion = 1", "version must be a string"},
		{"id = \"hello\"\nversion = \"1.0.0\"\nentry = \"../evil.dll\"", "entry"},
		{"id = \"hello\"\nversion = \"1.0.0\"\nentry = \"C:/evil.dll\"", "entry"},
		{"id = \"hello\"\nversion = \"1.0.0\"\nentry = \"hello.exe\"", "entry"},
		{"id = \"hello\"\nversion = \"1.0.0\"\nlua = \"main.txt\"", "lua"},
		{"id = \"hello\"\nversion = \"1.0.0\"\nphase = \"later\"", "phase"},
		{"id = \"hello\"\nversion = \"1.0.0\"\nmirage = \">=\"", "mirage:"},
		{"id = \"hello\"\nversion = \"1.0.0\"\n[dependencies]\nhello = \"*\"", "names the mod itself"},
		{"id = \"hello\"\nversion = \"1.0.0\"\n[dependencies]\nother = 2", "version range string"},
		{"id = \"hello\"\nversion = \"1.0.0\"\n[dependencies]\nother = \"*\"\n[optional]\nother = \"*\"", "listed twice"},
		{"id = \"hello\"\nversion = \"1.0.0\"\ndependencies = \"other\"", "[dependencies] must be a table"},
		{"id = \"hello\"\nversion = \"1.0.0\"\nauthors = \"me\"", "authors must be a list"},
		{"id = \"hello\nversion = \"1.0.0\"", "mod.toml"},		// TOML syntax: toml11's message names the file
	};
	for (const Case& c : cases) {
		const Parsed p = Parse(c.toml);
		EXPECT_FALSE(p.manifest) << c.toml;
		EXPECT_TRUE(HasMessage(p.errors, c.message)) << c.toml << "\nexpected: " << c.message
													 << "\ngot: " << (p.errors.empty() ? "nothing" : p.errors[0]);
	}
}

TEST(Manifest, WarnsAboutUnknownKeys)
{
	const Parsed p = Parse("id = \"hello\"\nversion = \"1.0.0\"\nentri = \"hello.dll\"\n");
	ASSERT_TRUE(p.manifest);
	EXPECT_TRUE(HasMessage(p.warnings, "unknown key 'entri'"));
}

TEST(Manifest, LoadChecksTheFolder)
{
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "memenu_test_manifest";
	fs::remove_all(root);
	const fs::path dir = root / "hello";
	fs::create_directories(dir);
	std::ofstream(dir / "mod.toml") << "id = \"hello\"\nversion = \"1.0.0\"\n";
	std::ofstream(dir / "main.lua") << "-- hi\n";

	Parsed p = Load(dir);
	ASSERT_TRUE(p.manifest) << (p.errors.empty() ? "" : p.errors[0]);
	EXPECT_EQ(p.manifest->lua, "main.lua");		// Found by itself
	EXPECT_EQ(p.manifest->dir, dir);

	std::ofstream(dir / "mod.toml") << "id = \"other\"\nversion = \"1.0.0\"\n";
	p = Load(dir);
	EXPECT_FALSE(p.manifest);
	EXPECT_TRUE(HasMessage(p.errors, "must match"));

	std::ofstream(dir / "mod.toml") << "id = \"hello\"\nversion = \"1.0.0\"\nentry = \"hello.dll\"\n";
	p = Load(dir);
	EXPECT_FALSE(p.manifest);
	EXPECT_TRUE(HasMessage(p.errors, "not in the folder"));

	EXPECT_FALSE(Load(root / "missing").manifest);
	fs::remove_all(root);
}
