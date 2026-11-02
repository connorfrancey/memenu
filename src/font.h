#pragma once
#include <cstddef>

// The menu's font file, built into the DLL (cmake/Fonts.cmake generates the data). MenuSize() is 0 when the build had
// no font: ImGui's built-in one is used then.
namespace font {

const unsigned char* MenuData();
std::size_t MenuSize();

} // namespace font
