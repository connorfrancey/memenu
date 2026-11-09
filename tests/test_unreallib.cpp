// UnrealLib (cmake/Libraries.cmake): checks the library builds and links here, and reads the game's own packages.
// The package test finds Mirror's Edge like the launcher does (Steam, EA registry, ...), or uses
// UNREALLIB_MIRRORSEDGE_DIR (the CookedPC folder); without either it is skipped.

#include "detect.h"

#include <UnrealLib/UnrealLib.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;
using namespace UnrealLib;

namespace
{
    fs::path FindCookedPC()
    {
        if (const char* dir = std::getenv("UNREALLIB_MIRRORSEDGE_DIR"); dir && *dir) return dir;
        for (const auto& install : detect::FindInstalls())
        {
            // ...\Binaries\MirrorsEdge.exe -> ...\TdGame\CookedPC
            const fs::path cooked = install.exe.parent_path().parent_path() / "TdGame" / "CookedPC";
            if (fs::exists(cooked / "Core.u")) return cooked;
        }
        return {};
    }
}

TEST(UnrealLib, SerializationRoundTrip)
{
    BinaryWriter writer;
    writer.write_i32(-123456);
    writer.write_compact_index(1000);
    writer.write_compact_index(-5);
    writer.write_fstring(FString("Faith"));

    const auto bytes = writer.take();
    BinaryReader reader{std::span<const std::byte>(bytes)};
    EXPECT_EQ(reader.read_i32(), -123456);
    EXPECT_EQ(reader.read_compact_index(), 1000);
    EXPECT_EQ(reader.read_compact_index(), -5);
    EXPECT_EQ(reader.read_fstring().text, "Faith");
    EXPECT_TRUE(reader.at_end());
    EXPECT_THROW((void)reader.read_i32(), Error);   // Bounds-checked: reading past the end throws
}

TEST(UnrealLib, RejectsGarbage)
{
    std::vector<std::byte> junk(64, std::byte{0x42});
    Package package;
    EXPECT_THROW(package.load_from_memory(std::move(junk), "Junk"), Error);
    EXPECT_FALSE(package.is_loaded());
}

TEST(UnrealLib, LoadsCoreFromTheGame)
{
    const fs::path cooked = FindCookedPC();
    if (cooked.empty()) GTEST_SKIP() << "Mirror's Edge not found (set UNREALLIB_MIRRORSEDGE_DIR to its CookedPC folder)";

    Package package;
    package.load(cooked / "Core.u");
    ASSERT_TRUE(package.is_loaded());

    EXPECT_EQ(package.summary().file_version, 536);       // Mirror's Edge PC
    EXPECT_EQ(package.summary().licensee_version, 43);
    EXPECT_GT(package.names().size(), 0u);
    EXPECT_GT(package.exports().size(), 0u);

    const auto object = package.find_export("Object");   // Core.Object, the root of every UnrealScript class
    ASSERT_TRUE(object.has_value());
    EXPECT_EQ(package.class_name(*object), "Class");
    EXPECT_EQ(package.object_path(*object), "Object");
}
