// Texture replacement, the file side (src/runtime/texture_files.cpp): encoding a mod's image into the game's formats,
// and rewriting a real map package with a replaced texture. The package tests use the game's files (like
// test_unreallib.cpp) and are skipped without them.

#include "detect.h"
#include "runtime/texture_files.h"

#include <UnrealLib/UnrealLib.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;
using namespace UnrealLib;
using namespace mirage::textures;
using Assets::PixelFormat;

namespace
{
	fs::path FindCookedPC()
	{
		if (const char* dir = std::getenv("UNREALLIB_MIRRORSEDGE_DIR"); dir && *dir) return dir;
		for (const auto& install : detect::FindInstalls()) {
			const fs::path cooked = install.exe.parent_path().parent_path() / "TdGame" / "CookedPC";
			if (fs::exists(cooked / "Core.u")) return cooked;
		}
		return {};
	}

	// A picture with smooth gradients, hard edges and an alpha ramp: what DXT is built for, and easy to tell apart
	Assets::Image TestImage(std::uint32_t w, std::uint32_t h)
	{
		Assets::Image img = Assets::Image::create(w, h);
		for (std::uint32_t y = 0; y < h; ++y)
			for (std::uint32_t x = 0; x < w; ++x) {
				std::uint8_t* p = img.at(x, y);
				p[0] = static_cast<std::uint8_t>(x * 255 / std::max(1u, w - 1));
				p[1] = static_cast<std::uint8_t>(y * 255 / std::max(1u, h - 1));
				p[2] = (x / 8 + y / 8) % 2 ? 200 : 40;
				p[3] = static_cast<std::uint8_t>(255 - x * 255 / std::max(1u, w - 1));
			}
		return img;
	}

	// Largest difference of one channel between two images (alpha only if `alpha`)
	int MaxError(const Assets::Image& a, const Assets::Image& b, bool alpha)
	{
		int worst = 0;
		for (std::size_t i = 0; i < a.rgba.size(); ++i) {
			if (!alpha && i % 4 == 3) continue;
			worst = std::max(worst, std::abs(int(a.rgba[i]) - int(b.rgba[i])));
		}
		return worst;
	}

	Assets::Image Decode(const Mip& m, PixelFormat f)
	{
		return Assets::decode_pixels(m.data, f, static_cast<std::uint32_t>(m.width), static_cast<std::uint32_t>(m.height));
	}
}

TEST(Textures, EncodesEveryGameFormat)
{
	const Assets::Image img = TestImage(64, 32);
	struct Case { PixelFormat format; int colourError, alphaError; };
	// DXT: 5:6:5 endpoints with 2 interpolated colours; DXT3 alpha is 4 bits; ARGB is exact
	for (const Case& c : {Case{PixelFormat::DXT1, 40, -1}, Case{PixelFormat::DXT3, 40, 17}, Case{PixelFormat::DXT5, 40, 12},
						  Case{PixelFormat::A8R8G8B8, 0, 0}}) {
		SCOPED_TRACE(std::string(Assets::to_string(c.format)));
		const auto data = EncodeSurface(img.rgba.data(), 64, 32, c.format);
		EXPECT_EQ(data.size(), *Assets::surface_size(c.format, 64, 32));
		const Assets::Image back = Assets::decode_pixels(data, c.format, 64, 32);
		EXPECT_LE(MaxError(img, back, false), c.colourError);
		if (c.alphaError >= 0) {
			int alpha = 0;
			for (std::size_t i = 3; i < img.rgba.size(); i += 4) alpha = std::max(alpha, std::abs(int(img.rgba[i]) - int(back.rgba[i])));
			EXPECT_LE(alpha, c.alphaError);
		}
	}
	// G8: luminance
	const auto g8 = EncodeSurface(img.rgba.data(), 64, 32, PixelFormat::G8);
	ASSERT_EQ(g8.size(), 64u * 32u);
	EXPECT_EQ(static_cast<int>(g8[0]), (img.rgba[0] * 77 + img.rgba[1] * 150 + img.rgba[2] * 29) >> 8);

	EXPECT_THROW((void)EncodeSurface(img.rgba.data(), 64, 32, PixelFormat::BC5), std::runtime_error);
}

TEST(Textures, BuildsAFullMipChain)
{
	const Assets::Image img = TestImage(16, 4);
	const Encoded enc = EncodeChain(img.rgba.data(), 16, 4, PixelFormat::DXT1);
	ASSERT_EQ(enc.mips.size(), 5u);	// 16x4, 8x2, 4x1, 2x1, 1x1
	const int sizes[][2] = {{16, 4}, {8, 2}, {4, 1}, {2, 1}, {1, 1}};
	for (std::size_t i = 0; i < enc.mips.size(); ++i) {
		EXPECT_EQ(enc.mips[i].width, sizes[i][0]);
		EXPECT_EQ(enc.mips[i].height, sizes[i][1]);
		EXPECT_EQ(enc.mips[i].data.size(), 8u * std::max(1, sizes[i][0] / 4) * std::max(1, sizes[i][1] / 4));
	}
	// A flat colour stays that colour down the chain
	Assets::Image flat = Assets::Image::create(8, 8);
	for (std::size_t i = 0; i < flat.rgba.size(); i += 4) flat.rgba[i] = 255, flat.rgba[i + 3] = 255;
	const Encoded red = EncodeChain(flat.rgba.data(), 8, 8, PixelFormat::A8R8G8B8);
	const Assets::Image last = Decode(red.mips.back(), PixelFormat::A8R8G8B8);
	EXPECT_EQ(last.rgba, (std::vector<std::uint8_t>{255, 0, 0, 255}));

	EXPECT_THROW((void)EncodeChain(img.rgba.data(), 12, 4, PixelFormat::DXT1), std::runtime_error);	// Not a power of two
}

TEST(Textures, ReadsDds)
{
	const Assets::Image img = TestImage(32, 16);
	const Encoded enc = EncodeChain(img.rgba.data(), 32, 16, PixelFormat::DXT5);
	std::vector<std::vector<std::byte>> surfaces;
	for (const Mip& m : enc.mips) surfaces.push_back(m.data);
	const auto dds = Assets::encode_dds(Assets::DdsFormat::DXT5, 32, 16, surfaces);
	const Encoded back = ParseDds(dds);
	EXPECT_EQ(back.format, PixelFormat::DXT5);
	ASSERT_EQ(back.mips.size(), enc.mips.size());
	for (std::size_t i = 0; i < enc.mips.size(); ++i) EXPECT_EQ(back.mips[i].data, enc.mips[i].data);

	std::vector<std::byte> junk(200, std::byte{1});
	EXPECT_THROW((void)ParseDds(junk), std::runtime_error);
}

TEST(Textures, PathsAreLowercaseWithDots)
{
	EXPECT_EQ(NormalizePath("B_C_01/Facade.T_X"), "b_c_01.facade.t_x");
}

// A real map: the header-only scan finds the same textures as a full load, and a rewritten copy holds the new texture
// (NeverStream, every mip inline, the image's size) while every other export is byte for byte the same.
TEST(Textures, RewritesAMapPackage)
{
	const fs::path cooked = FindCookedPC();
	if (cooked.empty()) GTEST_SKIP() << "Mirror's Edge not found";

	// The smallest Prologue package with a texture copied in from another package
	std::vector<fs::path> maps;
	for (const auto& e : fs::directory_iterator(cooked / "Maps" / "SP01")) maps.push_back(e.path());
	std::sort(maps.begin(), maps.end(), [](const fs::path& a, const fs::path& b) { return fs::file_size(a) < fs::file_size(b); });
	fs::path file;
	std::string target;
	for (const fs::path& m : maps) {
		for (const std::string& t : ScanTextures(m))
			if (t.rfind(mirage::textures::NormalizePath(m.stem().string()) + ".", 0) != 0) { target = t; break; }
		if (!target.empty()) { file = m; break; }
	}
	ASSERT_FALSE(target.empty()) << "no Prologue package has a copied texture";
	SCOPED_TRACE(file.filename().string() + ": " + target);

	// The scan agrees with a full load
	Package original = Package::open(file);
	std::set<std::string> full;
	for (std::size_t i = 0; i < original.exports().size(); ++i)
		if (Assets::is_texture2d_class(original.class_name(ObjectIndex::from_export(i)))) full.insert(original.object_path(ObjectIndex::from_export(i)));
	const auto scanned = ScanTextures(file);
	EXPECT_EQ(scanned.size(), full.size());

	// Replace it with a 64x32 picture
	const fs::path png = fs::temp_directory_path() / "memenu_test_replacement.png";
	const Assets::Image img = TestImage(64, 32);
	img.save_png(png);
	const Overlay overlay = BuildOverlay(file, {{target, png}});
	fs::remove(png);
	ASSERT_TRUE(overlay.problems.empty()) << overlay.problems.front();
	ASSERT_EQ(overlay.replaced, std::vector<std::string>{target});

	Linker linker;
	linker.add_search_path(cooked);
	Package rewritten;
	rewritten.load_from_memory(overlay.bytes, original.name());
	EXPECT_FALSE(rewritten.summary().is_compressed());		// Uncompressed: quick to write while the game waits
	EXPECT_EQ(rewritten.summary().package_flags & PackageFlag::Compressed, 0u) << "the flag must say so too";
	EXPECT_EQ(rewritten.summary().package_flags | PackageFlag::Compressed,
			  original.summary().package_flags | PackageFlag::Compressed);	// Nothing else changed
	ASSERT_EQ(rewritten.exports().size(), original.exports().size());
	const Package& added = linker.add(std::move(rewritten));

	// The replaced export: a texture named like the target's last part, whose outer chain ends the target's path
	const std::string leaf = target.substr(target.rfind('.') + 1);
	std::size_t replacedIndex = SIZE_MAX;
	for (std::size_t i = 0; i < added.exports().size(); ++i) {
		const ObjectIndex idx = ObjectIndex::from_export(i);
		if (Assets::is_texture2d_class(added.class_name(idx)) && NormalizePath(added.object_name(idx)) == leaf &&
			target.ends_with(NormalizePath(added.object_path(idx)))) {
			replacedIndex = i;
			continue;
		}
		const auto a = added.export_data(i), b = original.export_data(i);
		EXPECT_TRUE(std::equal(a.begin(), a.end(), b.begin(), b.end())) << "export " << i << " changed";
	}
	ASSERT_NE(replacedIndex, SIZE_MAX);

	const Assets::Texture2D tex = Assets::read_texture2d(linker, ObjectRef{&added, ObjectIndex::from_export(replacedIndex)});
	EXPECT_EQ(tex.size_x, 64);
	EXPECT_EQ(tex.size_y, 32);
	EXPECT_EQ(tex.properties.get_bool("NeverStream"), std::optional<bool>(true));
	EXPECT_EQ(tex.properties.get_int("MipTailBaseIdx"), std::optional<std::int32_t>(6));
	ASSERT_EQ(tex.mips.size(), 7u);
	for (const auto& m : tex.mips) {
		EXPECT_TRUE(m.data.is_inline());
		EXPECT_FALSE(m.data.offset_mismatch()) << "bulk data offset " << m.data.offset_in_file << " vs " << m.data.payload_offset;
	}
	const Assets::Image back = Assets::decode_mip(added, tex, 0);
	EXPECT_EQ(back.width, 64u);
	EXPECT_LE(MaxError(img, back, false), 40);
}
