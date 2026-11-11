#pragma once
// Texture replacement, the file side (Mirage, step 7): what a mod's image becomes in the game's package format. Pure:
// no engine, no Windows; unit-tested (tests/test_textures.cpp).
//
// How the game stores a texture, measured on the Prologue and menu maps (9,157 textures):
//   - a map holds a *copy* of each texture it uses ("B_C_01.Facade.T_X", outer chain kept). Streamable copies keep
//     their small mips inline and stream the large ones ("S4i7": 4 separate, 7 inline) from the texture's own package
//     (B_C_01.upk) by raw file offset; those storage packages are not compressed;
//   - every texture whose mips are all inline is NeverStream or small enough to stay resident.
// So a replaced texture is written as NeverStream with all its mips inline, into every package that has a copy of it
// (BuildOverlay). Storage packages are never rewritten, and no other package's streamed mips move.

#include <UnrealLib/Assets/Texture.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mirage::textures
{
	using UnrealLib::Assets::PixelFormat;

	struct Mip
	{
		int width = 0, height = 0;
		std::vector<std::byte> data;	// In the texture's format (DXT blocks, BGRA, ...)
	};
	struct Encoded
	{
		PixelFormat format = PixelFormat::Unknown;
		std::vector<Mip> mips;			// Largest first, down to 1x1 for a generated chain
	};

	// Formats the encoder writes: DXT1, DXT3, DXT5, A8R8G8B8, G8 (every format the game's textures use).
	bool CanEncode(PixelFormat format);

	// A mod's replacement image: .dds is taken as stored (DXT1/3/5, 32-bit ARGB or 8-bit luminance, its own mips);
	// .png / .tga / .bmp / .jpg are decoded, given a full mip chain (box filter) and encoded to `target`, the replaced
	// texture's own format. Sides must be powers of two. Throws std::runtime_error saying what is wrong.
	Encoded LoadReplacement(const std::filesystem::path& file, PixelFormat target);

	// One surface from 8-bit RGBA (width x height, rows packed), in `format`. Block formats pad partial blocks.
	std::vector<std::byte> EncodeSurface(const std::uint8_t* rgba, int width, int height, PixelFormat format);
	// A full mip chain from RGBA, encoded (sides powers of two).
	Encoded EncodeChain(const std::uint8_t* rgba, int width, int height, PixelFormat format);
	// A DDS file's surfaces (throws std::runtime_error for anything else).
	Encoded ParseDds(const std::vector<std::byte>& file);

	// The paths of the Texture2D exports (and subclasses) of a package, read from its header only (the chunks that
	// hold the tables, not the whole file), lowercase: "b_c_01.facade.t_x". A copy keeps its original path; a
	// texture of the package itself gets the package's name in front ("edge_p.lightmaps.lm_1").
	std::vector<std::string> ScanTextures(const std::filesystem::path& package);

	// The object path a mod names a texture by, lowercase (see ScanTextures).
	std::string NormalizePath(std::string path);

	struct Replacement
	{
		std::string path;					// Lowercase object path (NormalizePath)
		std::filesystem::path file;			// The mod's image
	};
	struct Overlay
	{
		std::vector<std::byte> bytes;		// The rewritten package (LZO-compressed like the original)
		std::vector<std::string> replaced;	// Paths written
		std::vector<std::string> problems;	// Replacements that couldn't be used, and why
	};
	// `package` with every replacement it holds a copy of rewritten (NeverStream, all mips inline, sizes and format
	// from the image). Throws std::runtime_error if the package itself can't be read or written.
	Overlay BuildOverlay(const std::filesystem::path& package, const std::vector<Replacement>& replacements);
}
