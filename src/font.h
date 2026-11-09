#pragma once
#include <cstddef>

struct ImFont;

// The menu's fonts, built into the DLL (cmake/Fonts.cmake generates the data). A size is 0 when the build had no such
// font: ImGui's built-in one is used then.
namespace font {

const unsigned char* MenuData();
std::size_t MenuSize();

// Monospace (Cousine): the console. Mono() is the loaded font (hooks.cpp), or ImGui's built-in one, which is
// monospace too; nullptr before ImGui is set up.
const unsigned char* MonoData();
std::size_t MonoSize();
inline ImFont* g_mono = nullptr;
inline ImFont* Mono() { return g_mono; }

} // namespace font
