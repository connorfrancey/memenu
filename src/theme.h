#pragma once
#include <imgui.h>
#include <string>
#include <vector>

// Themes are JSON files in <dll folder>/themes/ (comments allowed), applied with moleium/imjson.
// Built-in ones are written there on first run (existing files are never overwritten).
//
//   { "Colors":   { "WindowBg": [0.07, 0.07, 0.08, 0.96], "CheckMark": "#E6332E" },
//     "Rounding": { "WindowRounding": 4.0, "FrameRounding": 3.0 },
//     "Menu":     { "accent": [0.9, 0.2, 0.18, 1.0], "row_height": 26.0 } }
//
// imjson works on ImGui's style stack, so the theme is *pushed* every frame (Begin) and popped
// again (End) instead of being written into ImGuiStyle once.
namespace theme {

    // Colors/sizes for the custom-drawn parts of the menu (module rows, panels).
    struct MenuColors {
        ImVec4 accent     {0.90f, 0.20f, 0.18f, 1.00f};
        ImVec4 rowBg      {0.09f, 0.09f, 0.10f, 1.00f};
        ImVec4 rowHover   {0.14f, 0.14f, 0.16f, 1.00f};
        ImVec4 text       {0.78f, 0.78f, 0.80f, 1.00f};
        ImVec4 textActive {1.00f, 1.00f, 1.00f, 1.00f};
        ImVec4 textDim    {0.50f, 0.50f, 0.55f, 1.00f};
        ImVec4 settingsBg {0.11f, 0.11f, 0.12f, 1.00f};
        float  rowHeight  = 26.f;
    };

    void Init();                                   // create themes/ and the built-in theme files, scan
    void Rescan();                                 // re-read the list of theme files
    bool Apply(const std::string& name);           // load themes/<name>.json; falls back to the default theme
    void OpenFolder();                             // open themes/ in Explorer
    void ApplyBaseStyle();                         // the style every theme is layered on (needs an ImGui context)

    // Call around the menu drawing, inside a frame. Begin() pushes the active theme through imjson,
    // End() pops it again (must happen before ImGui::EndFrame()).
    void Begin();
    void End();

    const std::vector<std::string>& Names();       // available theme names (file names without .json)
    const std::string& Current();
    int  CurrentIndex();                           // index into Names(), -1 if not found
    const MenuColors& Menu();

} // namespace theme
