#pragma once
#include <string>

// Thin wrapper around moleium/imjson. Kept in its own translation unit on purpose: imjson.h is
// the only header that ever sees imjson's own includes, so nothing here can clash with the
// nlohmann/json copy used by the rest of the project.
namespace theme::backend {
    // Pushes the theme (colors + style vars) onto ImGui's style stack. Must be called inside a frame.
    // Returns false (and leaves the stack untouched) if imjson throws.
    bool Push(const std::string& json);
    // Pops everything the last successful Push() pushed.
    void Pop();
}
