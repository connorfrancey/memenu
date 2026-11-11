#pragma once
// mirage_sdkgen's configuration: sdkgen.toml (read with toml11), then the command line on top.

#include "emit.h"

#include <string>

namespace sdkgen
{
	// Reads `file` into `o`: relative paths are made relative to the file's folder; of the [game] cooked list, the
	// first folder that exists is used. False with `error` when the file can't be read or a value has the wrong type.
	bool LoadConfig(const std::filesystem::path& file, Options& o, std::string& error);
}
