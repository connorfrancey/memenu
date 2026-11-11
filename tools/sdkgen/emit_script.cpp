// mirage_sdkgen: the decompiled UnrealScript of every class (<reference>/script/<Package>/<Class>.uc), with UnrealLib's
// decompiler: what a mod author reads to see what a function does before hooking or patching it.

#include "emit.h"

#include <UnrealLib/Decompiler/Decompiler.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>

#include <fmt/format.h>

#include <cstdio>
#include <exception>
#include <fstream>
#include <vector>

namespace sdkgen
{
	bool EmitScript(const Model& m, const Options& o, std::string& error)
	{
		using namespace UnrealLib;
		const std::filesystem::path dir = o.reference / "script";
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);

		try {
			Linker linker;
			linker.add_search_path(o.cooked, false);
			ReflectionSchema schema(linker);
			std::vector<const Package*> packages;
			for (const std::string& name : m.packages)
				if (const Package* p = linker.find_package(name)) {
					packages.push_back(p);
					schema.add_package(*p);
				}
			Decompiler::ScriptDecompiler decompiler(linker, Decompiler::NativeTable::from_packages(linker, packages));
			decompiler.set_schema(&schema);

			std::size_t classes = 0, clean = 0, failed = 0;
			for (const Package* p : packages) {
				const std::string pkg = p->name();
				std::filesystem::create_directories(dir / pkg);
				for (std::size_t i = 0; i < p->exports().size(); ++i) {
					const ObjectIndex index = ObjectIndex::from_export(i);
					if (p->class_name(index) != "Class") continue;
					const std::string cls = p->object_name(index);
					std::string text;
					try {
						const Decompiler::DecompiledScript s = decompiler.decompile_class(ObjectRef{p, index});
						text = s.source;
						clean += s.clean();
					} catch (const std::exception& e) {
						++failed;
						text = fmt::format("// mirage_sdkgen: {}.{} could not be decompiled: {}\n", pkg, cls, e.what());
					}
					++classes;
					std::ofstream(dir / pkg / (cls + ".uc"), std::ios::binary | std::ios::trunc) << text;
				}
			}
			std::printf("script    %zu classes decompiled (%zu with no placeholders), %zu failed\n", classes, clean,
						failed);
			return true;
		} catch (const std::exception& e) {
			error = std::string("decompiler: ") + e.what();
			return false;
		}
	}
}
