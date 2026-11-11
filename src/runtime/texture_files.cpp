#include "runtime/texture_files.h"

#include <UnrealLib/Assets/BulkData.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Package/CompressedChunk.hpp>
#include <UnrealLib/Package/ObjectTables.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Package/PackageSummary.hpp>
#include <UnrealLib/Properties/PropertyTag.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Writer/PackageWriter.hpp>

#include <stb_dxt.h>
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>

namespace mirage::textures
{
	namespace fs = std::filesystem;
	using namespace UnrealLib;
	using Assets::PixelFormat;

	namespace
	{
		std::runtime_error Fail(const std::string& what) { return std::runtime_error(what); }

		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		std::string Utf8(const fs::path& p)
		{
			const std::u8string s = p.filename().u8string();
			return std::string(s.begin(), s.end());
		}

		std::vector<std::byte> ReadFile(const fs::path& file, std::uint64_t offset = 0, std::uint64_t size = UINT64_MAX)
		{
			std::ifstream in(file, std::ios::binary);
			if (!in) throw Fail("can't open " + Utf8(file));
			in.seekg(0, std::ios::end);
			const std::uint64_t total = static_cast<std::uint64_t>(in.tellg());
			if (offset > total) throw Fail(Utf8(file) + " is shorter than expected");
			size = std::min(size, total - offset);
			std::vector<std::byte> out(static_cast<std::size_t>(size));
			in.seekg(static_cast<std::streamoff>(offset));
			if (size && !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)))
				throw Fail("can't read " + Utf8(file));
			return out;
		}

		bool PowerOfTwo(int v) { return v > 0 && (v & (v - 1)) == 0; }

		// One 4x4 block of RGBA from (bx, by), edge pixels repeated past the surface
		void Block(const std::uint8_t* rgba, int w, int h, int bx, int by, std::uint8_t out[64])
		{
			for (int y = 0; y < 4; ++y)
				for (int x = 0; x < 4; ++x) {
					const int sx = std::min(bx + x, w - 1), sy = std::min(by + y, h - 1);
					std::memcpy(out + (y * 4 + x) * 4, rgba + (static_cast<std::size_t>(sy) * w + sx) * 4, 4);
				}
		}

		// A path from per-object accessors: "Outer.Name", with the package name in front unless the outermost object
		// is a package itself (a copy brought in from another package)
		std::string ObjectPath(ObjectIndex index, const std::string& packageName,
							   const std::function<std::string(ObjectIndex)>& name,
							   const std::function<ObjectIndex(ObjectIndex)>& outer,
							   const std::function<std::string(ObjectIndex)>& className)
		{
			std::vector<std::string> parts;
			ObjectIndex top = index;
			for (ObjectIndex o = index; !o.is_null() && parts.size() < 64; o = outer(o)) {
				parts.push_back(name(o));
				top = o;
			}
			std::string path;
			for (auto it = parts.rbegin(); it != parts.rend(); ++it) path += (path.empty() ? "" : ".") + *it;
			if (Lower(className(top)) != "package") path = packageName + "." + path;
			return Lower(path);
		}

		Property* FindProperty(std::vector<Property>& props, std::string_view name)
		{
			for (Property& p : props)
				if (p.array_index == 0 && p.name.size() == name.size() &&
					std::equal(p.name.begin(), p.name.end(), name.begin(),
							   [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
					return &p;
			return nullptr;
		}

		Property& AddProperty(std::vector<Property>& props, PackageModel& model, const std::string& name, PropertyType type,
							  const char* typeName, std::int32_t size)
		{
			Property p;
			p.name = name;
			PropertyTag tag;
			tag.name = model.make_name(name);
			tag.type_name = model.make_name(typeName);
			tag.type = type;
			tag.size = size;
			p.tag = tag;
			props.push_back(std::move(p));
			return props.back();
		}

		void SetInt(std::vector<Property>& props, PackageModel& model, const std::string& name, std::int32_t value)
		{
			Property* p = FindProperty(props, name);
			if (!p) p = &AddProperty(props, model, name, PropertyType::Int, "IntProperty", 4);
			p->value.value = value;
		}

		void SetBool(std::vector<Property>& props, PackageModel& model, const std::string& name, bool value)
		{
			Property* p = FindProperty(props, name);
			if (!p) p = &AddProperty(props, model, name, PropertyType::Bool, "BoolProperty", 0);
			p->value.value = value;
			if (p->tag) p->tag->bool_value = value;
		}

		// The texture's native data (version 536 layout, see UnrealLib read_texture2d): source art, then the mips,
		// every payload inline and uncompressed. `base`: the absolute offset of the first byte in the package image,
		// for the bulk data offsets.
		std::vector<std::byte> NativeData(const SerializationContext& ctx, BulkData sourceArt, const Encoded& enc,
										  std::uint64_t base)
		{
			BinaryWriter w(ctx);
			write_bulk_data(w, sourceArt, {}, base);
			w.write_i32(static_cast<std::int32_t>(enc.mips.size()));
			for (const Mip& m : enc.mips) {
				BulkData rec;
				rec.flags = 0;
				rec.element_count = static_cast<std::int32_t>(m.data.size());
				write_bulk_data(w, rec, m.data, base);
				w.write_i32(m.width);
				w.write_i32(m.height);
			}
			return w.take();
		}
	}

	bool CanEncode(PixelFormat f)
	{
		return f == PixelFormat::DXT1 || f == PixelFormat::DXT3 || f == PixelFormat::DXT5 || f == PixelFormat::A8R8G8B8 ||
			   f == PixelFormat::G8;
	}

	std::string NormalizePath(std::string path)
	{
		path = Lower(std::move(path));
		std::replace(path.begin(), path.end(), '/', '.');
		return path;
	}

	std::vector<std::byte> EncodeSurface(const std::uint8_t* rgba, int w, int h, PixelFormat format)
	{
		std::vector<std::byte> out;
		switch (format) {
			case PixelFormat::DXT1:
			case PixelFormat::DXT3:
			case PixelFormat::DXT5: {
				const std::size_t blockBytes = format == PixelFormat::DXT1 ? 8 : 16;
				const int bw = (w + 3) / 4, bh = (h + 3) / 4;
				out.resize(static_cast<std::size_t>(bw) * bh * blockBytes);
				std::uint8_t block[64];
				auto* dst = reinterpret_cast<unsigned char*>(out.data());
				for (int by = 0; by < bh; ++by)
					for (int bx = 0; bx < bw; ++bx, dst += blockBytes) {
						Block(rgba, w, h, bx * 4, by * 4, block);
						if (format == PixelFormat::DXT1) {
							stb_compress_dxt_block(dst, block, 0, STB_DXT_HIGHQUAL);
						} else if (format == PixelFormat::DXT5) {
							stb_compress_dxt_block(dst, block, 1, STB_DXT_HIGHQUAL);
						} else {	// DXT3: 4-bit explicit alpha (two pixels a byte, low nibble first), then the colour block
							for (int i = 0; i < 8; ++i)
								dst[i] = static_cast<unsigned char>((block[(i * 2) * 4 + 3] >> 4) | (block[(i * 2 + 1) * 4 + 3] & 0xF0));
							stb_compress_dxt_block(dst + 8, block, 0, STB_DXT_HIGHQUAL);
						}
					}
				break;
			}
			case PixelFormat::A8R8G8B8:
				out.resize(static_cast<std::size_t>(w) * h * 4);
				for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {	// B, G, R, A in memory
					out[i * 4 + 0] = std::byte{rgba[i * 4 + 2]};
					out[i * 4 + 1] = std::byte{rgba[i * 4 + 1]};
					out[i * 4 + 2] = std::byte{rgba[i * 4 + 0]};
					out[i * 4 + 3] = std::byte{rgba[i * 4 + 3]};
				}
				break;
			case PixelFormat::G8:
				out.resize(static_cast<std::size_t>(w) * h);
				for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
					out[i] = std::byte(static_cast<std::uint8_t>((rgba[i * 4] * 77 + rgba[i * 4 + 1] * 150 + rgba[i * 4 + 2] * 29) >> 8));
				break;
			default:
				throw Fail(std::string("can't encode ") + std::string(Assets::to_string(format)) +
						   ": give the replacement as a .dds in that format");
		}
		return out;
	}

	Encoded EncodeChain(const std::uint8_t* rgba, int w, int h, PixelFormat format)
	{
		if (!PowerOfTwo(w) || !PowerOfTwo(h))
			throw Fail("the image is " + std::to_string(w) + "x" + std::to_string(h) + ": both sides must be powers of two");
		if (!CanEncode(format))
			throw Fail(std::string("can't encode ") + std::string(Assets::to_string(format)) +
					   ": give the replacement as a .dds in that format");
		Encoded enc;
		enc.format = format;
		std::vector<std::uint8_t> level(rgba, rgba + static_cast<std::size_t>(w) * h * 4), next;
		for (;;) {
			enc.mips.push_back({w, h, EncodeSurface(level.data(), w, h, format)});
			if (w == 1 && h == 1) break;
			const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
			next.assign(static_cast<std::size_t>(nw) * nh * 4, 0);
			for (int y = 0; y < nh; ++y)		// Box filter: the 2x2 (or 2x1 / 1x2) pixels each new one covers
				for (int x = 0; x < nw; ++x)
					for (int c = 0; c < 4; ++c) {
						const int x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
						const int y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
						const auto at = [&](int px, int py) { return level[(static_cast<std::size_t>(py) * w + px) * 4 + c]; };
						next[(static_cast<std::size_t>(y) * nw + x) * 4 + c] =
							static_cast<std::uint8_t>((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
					}
			level.swap(next);
			w = nw;
			h = nh;
		}
		return enc;
	}

	Encoded ParseDds(const std::vector<std::byte>& file)
	{
		const auto u32 = [&](std::size_t at) {
			std::uint32_t v = 0;
			std::memcpy(&v, file.data() + at, 4);
			return v;
		};
		if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0 || u32(4) != 124) throw Fail("not a DDS file");
		const int h = static_cast<int>(u32(12)), w = static_cast<int>(u32(16));
		const std::uint32_t mipCount = std::max<std::uint32_t>(1, u32(28));
		const std::uint32_t pfFlags = u32(80), fourCC = u32(84), bits = u32(88);
		const auto cc = [](const char* s) { return static_cast<std::uint32_t>(s[0]) | s[1] << 8 | s[2] << 16 | s[3] << 24; };
		Encoded enc;
		if (pfFlags & 0x4) {	// DDPF_FOURCC
			if (fourCC == cc("DXT1")) enc.format = PixelFormat::DXT1;
			else if (fourCC == cc("DXT3")) enc.format = PixelFormat::DXT3;
			else if (fourCC == cc("DXT5")) enc.format = PixelFormat::DXT5;
			else throw Fail("the DDS isn't DXT1, DXT3, DXT5, 32-bit ARGB or 8-bit luminance (DX10 headers aren't read)");
		} else if ((pfFlags & 0x40) && bits == 32 && u32(92) == 0x00FF0000 && u32(96) == 0x0000FF00 && u32(100) == 0x000000FF) {
			enc.format = PixelFormat::A8R8G8B8;
		} else if ((pfFlags & 0x20000) && bits == 8) {
			enc.format = PixelFormat::G8;
		} else {
			throw Fail("the DDS isn't DXT1, DXT3, DXT5, 32-bit ARGB or 8-bit luminance");
		}
		if (!PowerOfTwo(w) || !PowerOfTwo(h))
			throw Fail("the DDS is " + std::to_string(w) + "x" + std::to_string(h) + ": both sides must be powers of two");
		std::size_t at = 128;
		int mw = w, mh = h;
		for (std::uint32_t i = 0; i < mipCount; ++i) {
			const auto size = Assets::surface_size(enc.format, static_cast<std::uint32_t>(mw), static_cast<std::uint32_t>(mh));
			if (!size || at + *size > file.size()) throw Fail("the DDS is shorter than its header says");
			enc.mips.push_back({mw, mh, std::vector<std::byte>(file.begin() + static_cast<std::ptrdiff_t>(at),
															  file.begin() + static_cast<std::ptrdiff_t>(at + *size))});
			at += static_cast<std::size_t>(*size);
			if (mw == 1 && mh == 1) break;
			mw = std::max(1, mw / 2);
			mh = std::max(1, mh / 2);
		}
		return enc;
	}

	Encoded LoadReplacement(const fs::path& file, PixelFormat target)
	{
		const std::vector<std::byte> bytes = ReadFile(file);
		if (Lower(file.extension().string()) == ".dds") return ParseDds(bytes);
		int w = 0, h = 0, comp = 0;
		stbi_uc* rgba = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
											  &w, &h, &comp, 4);
		if (!rgba) throw Fail(std::string("can't read the image: ") + stbi_failure_reason());
		try {
			Encoded enc = EncodeChain(rgba, w, h, target);
			stbi_image_free(rgba);
			return enc;
		} catch (...) {
			stbi_image_free(rgba);
			throw;
		}
	}

	std::vector<std::string> ScanTextures(const fs::path& package)
	{
		std::vector<std::byte> head = ReadFile(package, 0, 64 * 1024);
		BinaryReader sr{std::span<const std::byte>(head)};
		const PackageSummary s = read_package_summary(sr);
		const SerializationContext ctx = s.context();
		const std::size_t headerEnd = static_cast<std::size_t>(std::max<std::int32_t>(s.header_size, 0));
		if (headerEnd == 0) return {};

		// The image up to the end of the tables
		std::vector<std::byte> image;
		if (!s.is_compressed()) {
			image = headerEnd <= head.size() ? std::vector<std::byte>(head.begin(), head.begin() + static_cast<std::ptrdiff_t>(headerEnd))
											 : ReadFile(package, 0, headerEnd);
		} else {
			image.assign(headerEnd, std::byte{0});
			const std::size_t summaryEnd = static_cast<std::size_t>(s.compressed_chunks.front().uncompressed_offset);
			std::memcpy(image.data(), head.data(), std::min({summaryEnd, head.size(), headerEnd}));
			const CompressionMethod method = compression_method_from_flags(s.compression_flags);
			for (const CompressedChunkInfo& c : s.compressed_chunks) {
				if (static_cast<std::size_t>(c.uncompressed_offset) >= headerEnd) continue;
				const std::vector<std::byte> stored = ReadFile(package, static_cast<std::uint64_t>(c.compressed_offset),
															   static_cast<std::uint64_t>(c.compressed_size));
				CompressedChunkInfo local = c;
				local.compressed_offset = 0;
				std::vector<std::byte> out(static_cast<std::size_t>(c.uncompressed_size));
				decompress_chunk(stored, local, method, out, ctx);
				const std::size_t n = std::min(out.size(), headerEnd - static_cast<std::size_t>(c.uncompressed_offset));
				std::memcpy(image.data() + c.uncompressed_offset, out.data(), n);
			}
		}

		BinaryReader r(std::span<const std::byte>(image), ctx);
		std::vector<NameEntry> names(static_cast<std::size_t>(s.name_count));
		r.seek(static_cast<std::size_t>(s.name_offset));
		for (auto& n : names) n = read_name_entry(r);
		std::vector<ImportEntry> imports(static_cast<std::size_t>(s.import_count));
		r.seek(static_cast<std::size_t>(s.import_offset));
		for (auto& i : imports) i = read_import_entry(r);
		std::vector<ExportEntry> exports(static_cast<std::size_t>(s.export_count));
		r.seek(static_cast<std::size_t>(s.export_offset));
		for (auto& e : exports) e = read_export_entry(r);

		const auto text = [&](const FName& n) {
			if (n.index < 0 || static_cast<std::size_t>(n.index) >= names.size()) return std::string("?");
			std::string t = names[static_cast<std::size_t>(n.index)].text.text;
			if (n.number > 0) t += "_" + std::to_string(n.number - 1);
			return t;
		};
		const auto name = [&](ObjectIndex o) {
			if (o.is_export() && o.export_index() < exports.size()) return text(exports[o.export_index()].object_name);
			if (o.is_import() && o.import_index() < imports.size()) return text(imports[o.import_index()].object_name);
			return std::string("?");
		};
		const auto outer = [&](ObjectIndex o) {
			if (o.is_export() && o.export_index() < exports.size()) return exports[o.export_index()].outer;
			if (o.is_import() && o.import_index() < imports.size()) return imports[o.import_index()].outer;
			return ObjectIndex{};
		};
		const auto className = [&](ObjectIndex o) {
			if (o.is_import()) return o.import_index() < imports.size() ? text(imports[o.import_index()].class_name) : std::string("?");
			if (!o.is_export() || o.export_index() >= exports.size()) return std::string("?");
			const ObjectIndex c = exports[o.export_index()].class_index;
			return c.is_null() ? std::string("Class") : name(c);
		};

		const std::string packageName = Lower(package.stem().string());
		std::vector<std::string> out;
		for (std::size_t i = 0; i < exports.size(); ++i) {
			const ObjectIndex idx = ObjectIndex::from_export(i);
			if (!Assets::is_texture2d_class(className(idx))) continue;
			out.push_back(ObjectPath(idx, packageName, name, outer, className));
		}
		return out;
	}

	Overlay BuildOverlay(const fs::path& file, const std::vector<Replacement>& replacements)
	{
		Overlay result;
		Linker linker;
		// Class hierarchies resolve through the game's script packages: search the CookedPC folder the file is in
		for (fs::path dir = file.parent_path(); !dir.empty(); dir = dir.parent_path()) {
			if (Lower(dir.filename().string()) == "cookedpc") {
				linker.add_search_path(dir);
				break;
			}
			if (dir == dir.parent_path()) break;
		}
		const Package& pkg = linker.load(file);
		const SerializationContext ctx = pkg.context();
		if (ctx.file_version >= 567) throw Fail("only Mirror's Edge's package version (536) is written");
		const std::string packageName = Lower(pkg.name());

		std::map<std::string, const Replacement*> wanted;
		for (const Replacement& r : replacements) wanted[NormalizePath(r.path)] = &r;

		struct Edit
		{
			ObjectIndex index;
			std::size_t propertiesOffset = 0, propertiesEnd = 0, oldNative = 0, nativeStart = 0;
			std::vector<Property> properties;
			BulkData sourceArt;
			Encoded encoded;
			PixelFormat original = PixelFormat::Unknown;
			std::string path;
		};
		std::vector<Edit> edits;
		for (std::size_t i = 0; i < pkg.exports().size(); ++i) {
			const ObjectIndex idx = ObjectIndex::from_export(i);
			if (!Assets::is_texture2d_class(pkg.class_name(idx))) continue;
			const std::string path = ObjectPath(idx, packageName, [&](ObjectIndex o) { return pkg.object_name(o); },
												[&](ObjectIndex o) { return pkg.outer(o); },
												[&](ObjectIndex o) { return pkg.class_name(o); });
			const auto it = wanted.find(path);
			if (it == wanted.end()) continue;
			try {
				const Assets::Texture2D tex = Assets::read_texture2d(linker, ObjectRef{&pkg, idx});
				ObjectProperties op = linker.read_properties(ObjectRef{&pkg, idx});
				Edit e;
				e.index = idx;
				e.path = path;
				e.propertiesOffset = op.header.properties_offset;
				e.propertiesEnd = op.end_offset;
				e.oldNative = pkg.export_data(i).size() - op.end_offset;
				e.properties = std::move(op.properties);
				e.sourceArt = tex.source_art;
				e.original = tex.format;
				e.encoded = LoadReplacement(it->second->file, tex.format);
				edits.push_back(std::move(e));
			} catch (const std::exception& ex) {
				result.problems.push_back(path + ": " + ex.what());
			}
		}
		if (edits.empty()) return result;

		PackageModel model = PackageModel::from_package(pkg);
		for (Edit& e : edits) {
			const Mip& top = e.encoded.mips.front();
			SetInt(e.properties, model, "SizeX", top.width);
			SetInt(e.properties, model, "SizeY", top.height);
			SetInt(e.properties, model, "MipTailBaseIdx", static_cast<std::int32_t>(e.encoded.mips.size()) - 1);
			SetBool(e.properties, model, "NeverStream", true);
			if (e.encoded.format != e.original) {
				Property* f = FindProperty(e.properties, "Format");
				if (!f) throw Fail(e.path + " has no Format property");
				f->value.value = EnumValue{model.make_name(Assets::to_string(e.encoded.format))};
			}
			replace_properties(model, e.index, e.propertiesOffset, e.propertiesEnd, e.properties);
			auto& data = model.export_data[e.index.export_index()];
			e.nativeStart = data.size() - e.oldNative;
			data.resize(e.nativeStart);
			const auto native = NativeData(ctx, e.sourceArt, e.encoded, 0);
			data.insert(data.end(), native.begin(), native.end());
		}

		// The bulk data offsets are absolute: lay the package out once to learn where each export lands, then write
		// the native data again with the right base (same sizes, so the layout doesn't move)
		{
			Package laid;
			laid.load_from_memory(write_package(model), pkg.name());
			for (Edit& e : edits) {
				const std::uint64_t base = static_cast<std::uint64_t>(laid.exports()[e.index.export_index()].serial_offset) + e.nativeStart;
				auto& data = model.export_data[e.index.export_index()];
				const auto native = NativeData(ctx, e.sourceArt, e.encoded, base);
				data.resize(e.nativeStart);
				data.insert(data.end(), native.begin(), native.end());
			}
		}
		WriteOptions options;
		options.compression = compression_method_from_flags(pkg.summary().compression_flags);
		result.bytes = write_package(model, options);
		for (const Edit& e : edits) result.replaced.push_back(e.path);
		return result;
	}
}
