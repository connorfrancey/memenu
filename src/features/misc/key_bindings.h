#pragma once
#include <filesystem>

// Keep Key Bindings: where it keeps the player's bindings (default <menu folder>/keybindings.json). The self-test points
// it elsewhere. Any thread; takes effect at the next reset moment (switch the module off and on to force one).
namespace keep_bindings
{
	void SetFile(const std::filesystem::path& file);    // Empty: the default
}
