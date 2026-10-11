// The whole menu is defined here with the Builder. Game-side logic (marked TODO) needs offsets
// you have to find yourself: Mirror's Edge runs on Unreal Engine 3.

#include "builder.h"
#include "config.h"
#include "game/game.h"
#include "theme.h"

#include <Windows.h>
#include <mirrorsedge_sdk.h>   // the mmultiplayer SDK: Classes::ATdPlayerPawn, Classes::AWorldInfo, ...

#include <cstdio>
#include <string>
#include <imgui.h>

namespace {

ImU32 ToU32(const std::array<float, 4>& c)
{
    return ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], c[3]});
}

} // namespace

void modules::Init()
{
    if (!Tabs().empty()) return;

    Builder ui;

    // ---------------------------------------------------------------------------------------
    ui.tab("Player")
        .module("God Mode", "Take no damage")
            .onTick([](Module&) {
                auto* pawn = game::Pawn();
                auto* controller = game::Controller();
                if (!pawn || !controller) return;

                controller->bGodMode = 1;               // engine-level "God" flag
                pawn->Health = pawn->MaxHealth;         // what mmultiplayer's trainer does...
                pawn->EnterFallingHeight = -1e30f;      // ...plus no fall deaths (also from the trainer)
            })
            .onDisable([](Module&) {
                game::RunOnGameThread([] {
                    if (auto* controller = game::Controller()) controller->bGodMode = 0;
                });
            })

        .module("No Clip", "Fly through geometry")
            .hotkey(VK_F5)
            .combo("Mode", {"Fly", "Ghost", "Spectate"})
            .slider("Speed", 1.f, 0.1f, 10.f, "%.1fx")
            .keybind("Boost key", VK_LSHIFT)
                .tooltip("Hold to move 3x faster")
            .checkbox("Auto land", true)
                .tooltip("Restore collision when No Clip is switched off")
            .onEnable ([](Module&) { /* TODO: pawn physics -> PHYS_Flying */ })
            .onDisable([](Module&) { /* TODO: pawn physics -> PHYS_Walking */ })
            .onTick   ([](Module& m) {
                float speed = m.GetFloat("Speed");
                if (m.KeyDown("Boost key")) speed *= 3.f;
                (void)speed;   // TODO: move the pawn using `speed` and m.GetCombo("Mode")
            })

        .action("Teleport to Spawn", "Move the player back to the level start",
                [](Module&) { /* TODO */ })

    // ---------------------------------------------------------------------------------------
    .tab("World")
        .module("Game Speed", "Change the world time dilation")
            .slider("Multiplier", 1.f, 0.1f, 3.f, "%.2fx")
            .button("Reset to 1.0x", [](Module& m) { if (auto* s = m.Find("Multiplier")) s->f = 1.f; })
            .onTick([](Module& m) {
                if (auto* world = game::World()) {
                    const float speed = m.GetFloat("Multiplier");
                    world->TimeDilation = speed;
                    if (world->Game) world->Game->GameSpeed = speed;
                }
            })
            .onDisable([](Module&) {
                // onDisable can run on the render thread: hand the cleanup to the game thread.
                game::RunOnGameThread([] {
                    if (auto* world = game::World()) {
                        world->TimeDilation = 1.f;
                        if (world->Game) world->Game->GameSpeed = 1.f;
                    }
                });
            })

        .module("Freeze Timer", "Stop the time trial clock")
            .label("Only affects time trials.")
            .onTick([](Module&) { /* TODO */ })

    // ---------------------------------------------------------------------------------------
    .tab("Visual")
        .module("FOV", "Override the field of view")
            .slider("Degrees", 90.f, 60.f, 130.f, "%.0f")
            .onTick([](Module& m) { (void)m.GetFloat("Degrees"); /* TODO: camera FOV */ })

        // The next two work out of the box: pure ImGui, no game offsets needed.
        .module("Show FPS", "FPS counter")
            .enabledByDefault()
            .combo("Corner", {"Top left", "Top right", "Bottom left", "Bottom right"})
            .color("Text color", {1.f, 1.f, 1.f, 1.f})
            .checkbox("Frametime", false)
            .onRender([](Module& m) {
                const ImGuiIO& io = ImGui::GetIO();
                char buf[64];
                if (m.GetBool("Frametime"))
                    std::snprintf(buf, sizeof buf, "%.0f FPS (%.2f ms)", io.Framerate, 1000.f / io.Framerate);
                else
                    std::snprintf(buf, sizeof buf, "%.0f FPS", io.Framerate);

                const ImVec2 ts = ImGui::CalcTextSize(buf);
                const int corner = m.GetCombo("Corner");          // bit0 = right, bit1 = bottom
                const ImVec2 pos{ (corner & 1) ? io.DisplaySize.x - ts.x - 10.f : 10.f,
                                  (corner & 2) ? io.DisplaySize.y - ts.y - 10.f : 10.f };
                ImDrawList* dl = ImGui::GetBackgroundDrawList();
                dl->AddText({pos.x + 1.f, pos.y + 1.f}, IM_COL32(0, 0, 0, 200), buf);
                dl->AddText(pos, ToU32(m.GetColor("Text color")), buf);
            })

        .module("Crosshair", "Draw a crosshair in the screen center")
            .combo("Style", {"Cross", "Dot", "Circle"})
            .sliderInt("Size", 8, 2, 40)
            .slider("Gap", 3.f, 0.f, 20.f, "%.0f")
                .visibleIf([](Module& m) { return m.GetCombo("Style") == 0; })
            .slider("Thickness", 2.f, 1.f, 6.f, "%.1f")
                .visibleIf([](Module& m) { return m.GetCombo("Style") != 1; })
            .color("Color", {0.9f, 0.2f, 0.18f, 1.f})
            .onRender([](Module& m) {
                const ImVec2 ds = ImGui::GetIO().DisplaySize;
                const ImVec2 c{ds.x * 0.5f, ds.y * 0.5f};
                const float size = static_cast<float>(m.GetInt("Size"));
                const float gap = m.GetFloat("Gap"), th = m.GetFloat("Thickness");
                const ImU32 col = ToU32(m.GetColor("Color"));
                ImDrawList* dl = ImGui::GetBackgroundDrawList();
                switch (m.GetCombo("Style")) {
                    case 0:
                        dl->AddLine({c.x - gap - size, c.y}, {c.x - gap, c.y}, col, th);
                        dl->AddLine({c.x + gap, c.y}, {c.x + gap + size, c.y}, col, th);
                        dl->AddLine({c.x, c.y - gap - size}, {c.x, c.y - gap}, col, th);
                        dl->AddLine({c.x, c.y + gap}, {c.x, c.y + gap + size}, col, th);
                        break;
                    case 1: dl->AddCircleFilled(c, size * 0.25f, col); break;
                    default: dl->AddCircle(c, size, col, 0, th); break;
                }
            })

    // ---------------------------------------------------------------------------------------
    .tab("Misc")
        .module("Watermark", "Small text in the bottom-left corner")
            .slider("Opacity", 0.6f, 0.1f, 1.f)
            .onRender([](Module& m) {
                const ImVec2 ds = ImGui::GetIO().DisplaySize;
                const int a = static_cast<int>(m.GetFloat("Opacity") * 255.f);
                ImGui::GetBackgroundDrawList()->AddText(
                    {10.f, ds.y - ImGui::GetFontSize() - 10.f}, IM_COL32(230, 50, 45, a), "Mirror's Edge Menu");
            })

        // Shows whether the game integration is alive, which is the first thing to check when a game module does nothing.
        .module("Game Status", "Diagnostics for the game integration")
            .enabledByDefault()
            .onRender([](Module&) {
                const game::Status st = game::GetStatus();
                char line[160];
                if (!st.ready)
                    std::snprintf(line, sizeof line, "GAME: NOT READY (see MirrorsEdgeMenu.log)");
                else
                    std::snprintf(line, sizeof line, "GAME: ready | ticks %llu | %s | pawn %s%s",
                                  st.ticks, st.loading ? "loading" : "in level",
                                  st.hasPawn ? "yes" : "NO",
                                  st.hasPawn ? (" | hp " + std::to_string(st.health) + "/" + std::to_string(st.maxHealth)).c_str() : "");
                ImDrawList* dl = ImGui::GetBackgroundDrawList();
                dl->AddText({11.f, 31.f}, IM_COL32(0, 0, 0, 200), line);
                dl->AddText({10.f, 30.f}, st.ready ? IM_COL32(120, 230, 120, 255) : IM_COL32(255, 110, 100, 255), line);
            })

        .action("Themes", "Left click: reload theme files from disk",
                [](Module&) { theme::Rescan(); theme::Apply(theme::Current()); })
            .dynamicCombo("Theme",
                [] { return theme::Names(); },
                [] { return theme::CurrentIndex(); },
                [](int i) {
                    const auto& names = theme::Names();
                    if (i >= 0 && i < static_cast<int>(names.size())) theme::Apply(names[static_cast<size_t>(i)]);
                })
                .tooltip("Theme files live in the themes/ folder next to the DLL")
            .button("Open themes folder", [](Module&) { theme::OpenFolder(); })

        .action("Save Config", "Write settings to MedgeMenu.json", [](Module&) { config::Save(); })
        .action("Load Config", "Reload settings from MedgeMenu.json", [](Module&) { config::Load(); });
}
