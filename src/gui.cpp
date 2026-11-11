#include "gui.h"
#include "config.h"
#include "core/keybinds.h"
#include "core/keys.h"
#include "core/module.h"
#include "core/notify.h"
#include "core/setting.h"
#include "theme.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <map>
#include <mutex>

#include <imgui_internal.h>
#include <string>
#include <imgui.h>

namespace
{
    ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    }
    ImU32 U32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
    void Approach(float& v, float target, float dt) { v += (target - v) * std::min(1.f, dt * 14.f); }

    void DrawSettings(Module& m, float w)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();

        // Content goes on channel 1, the background rect (sized after layout) on channel 0.
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f, 6.f});
        ImGui::Indent(10.f);
        ImGui::Dummy({0.f, 2.f});

        const float itemW = w - 20.f;
        for (ctl::Setting* s : m.Settings())
            if (s->Visible()) s->Show(itemW);

        keybinds::Button(m.IsAction() ? "Hotkey" : "Toggle key", m.key, itemW);

        ImGui::Dummy({0.f, 2.f});
        ImGui::Unindent(10.f);
        ImGui::PopStyleVar();
        ImGui::EndGroup();

        const float endY = ImGui::GetItemRectMax().y;
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(start, {start.x + w, endY}, U32(theme::Menu().settingsBg));
        dl->ChannelsMerge();
    }

    Module* g_reveal = nullptr;     // A search result was clicked: its column opens its settings and scrolls there
    std::map<const Module*, float> g_resultHover;   // Search results animate their own hover

    // `highlight`: the row matches the search (its name is drawn bright). `result`: drawn in the search column, where a
    // click on the name opens the settings in the module's own column instead of here.
    void DrawModule(Module& m, float dt, bool highlight = false, bool result = false)
    {
        float& hover = result ? g_resultHover[&m] : m.hover;
        const theme::MenuColors& tc = theme::Menu();

        ImGui::PushID(&m);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = tc.rowHeight;

        // The title opens the settings (right click closes them); the switch on the right turns the module on and off,
        // or runs it for an action.
        const float switchW = m.IsAction() ? 36.f : 30.f, switchH = 16.f;
        const float titleW = w - switchW - 16.f;

        ImGui::InvisibleButton("##title", {titleW, h}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            m.expanded = true;
            if (result) g_reveal = &m;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !result) m.expanded = false;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            const std::string how = result ? "In " + m.Tab() + ". Click: show its settings there"
                                           : std::string("Click: settings | Right click: close");
            ImGui::SetTooltip("%s%s%s", m.Description().c_str(), m.Description().empty() ? "" : "\n", how.c_str());
        }

        ImGui::SameLine(0.f, 0.f);
        ImGui::InvisibleButton("##switch", {w - titleW, h});
        const bool switchHovered = ImGui::IsItemHovered();
        hovered |= switchHovered;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            m.Toggle();
            if (m.IsAction()) m.anim = 1.f; // flash on click
        }
        if (switchHovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip(m.IsAction() ? "Run" : m.Enabled() ? "Turn off" : "Turn on");

        if (!result) Approach(m.anim, (!m.IsAction() && m.Enabled()) ? 1.f : 0.f, dt);   // Once a frame: in the column
        Approach(hover, hovered ? 1.f : 0.f, dt);

        ImVec4 bg = Mix(tc.rowBg, tc.rowHover, hover);
        bg = Mix(bg, Mix(tc.rowBg, tc.accent, 0.18f), m.anim);
        dl->AddRectFilled(p, {p.x + w, p.y + h}, U32(bg));
        dl->AddRectFilled(p, {p.x + 3.f * m.anim, p.y + h}, U32(tc.accent));

        const ImVec4 txt = Mix(tc.text, tc.textActive, std::max(m.anim, hover * 0.6f));
        const float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
        dl->AddText({p.x + 10.f, ty}, U32(highlight ? tc.textActive : txt), m.Name().c_str());
        const float nameEnd = p.x + 10.f + ImGui::CalcTextSize(m.Name().c_str()).x;

        // Switch: a pill whose knob slides right (accent) when on. Actions get a small "Run" pill instead.
        const ImVec2 s0{p.x + w - switchW - 8.f, p.y + (h - switchH) * 0.5f};

        // Left of the switch: a chevron (settings open or closed), and the module's key when it has one
        const ImU32 dim = U32(Mix(tc.textDim, tc.text, hover * 0.5f));
        const float cx = s0.x - 10.f, cy = p.y + h * 0.5f, c = 3.5f;
        if (m.expanded && !result) dl->AddTriangleFilled({cx - c, cy - c * 0.6f}, {cx + c, cy - c * 0.6f}, {cx, cy + c * 0.8f}, dim);
        else dl->AddTriangleFilled({cx - c * 0.6f, cy - c}, {cx - c * 0.6f, cy + c}, {cx + c * 0.8f, cy}, dim);
        if (m.key) {
            const std::string k = keys::Name(m.key);
            const ImVec2 ks = ImGui::CalcTextSize(k.c_str());
            const float kx = cx - 10.f - ks.x;
            if (kx > nameEnd + 8.f) dl->AddText({kx, ty}, dim, k.c_str());   // Only where the name leaves room
        }
        const ImVec2 s1{s0.x + switchW, s0.y + switchH};
        const float r = switchH * 0.5f;
        const ImVec4 hoverTint = Mix(tc.textDim, tc.text, switchHovered ? 0.5f : 0.f);
        if (m.IsAction())
        {
            dl->AddRectFilled(s0, s1, U32(Mix(Mix(tc.rowBg, hoverTint, 0.35f), tc.accent, m.anim)), r);
            const ImVec2 ts = ImGui::CalcTextSize("Run");
            dl->AddText({s0.x + (switchW - ts.x) * 0.5f, s0.y + (switchH - ts.y) * 0.5f}, U32(tc.text), "Run");
        }
        else
        {
            dl->AddRectFilled(s0, s1, U32(Mix(Mix(tc.rowBg, hoverTint, 0.45f), tc.accent, m.anim)), r);
            const float kx = s0.x + r + (switchW - 2.f * r) * m.anim;
            dl->AddCircleFilled({kx, s0.y + r}, r - 2.5f, U32(Mix(tc.textDim, tc.textActive, m.anim)));
        }

        if (m.expanded && !result) DrawSettings(m, w);
        ImGui::PopID();
    }
}

namespace
{
    char g_search[64] = "";
    bool g_focusSearch = false;
    std::map<std::string, ImVec2> g_columnPos;      // Where each column was last frame (for its height limit)

    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // `query` lower case, not empty: the module's name, description, tab or one of its settings has it
    bool Matches(const Module& m, const std::string& query)
    {
        if (Lower(m.Name() + "\n" + m.Description() + "\n" + m.Tab()).find(query) != std::string::npos) return true;
        for (const ctl::Setting* s : m.Settings())
            if (Lower(s->Name()).find(query) != std::string::npos) return true;
        return false;
    }

    std::mutex g_testColumnLock;
    std::string g_testColumn;       // gui::OpenColumnForTest
    std::map<std::string, bool> g_testCollapsed;    // Columns the test opened: collapsed before? Put back when cleared

    // A column never grows past the bottom of the screen: taller content scrolls with the mouse wheel.
    bool BeginColumn(const std::string& name, float width, ImVec2 defaultPos, ImGuiWindowFlags flags)
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const auto it = g_columnPos.find(name);
        const float top = it != g_columnPos.end() ? it->second.y : defaultPos.y;
        const float maxH = display.y > 0.f ? std::max(120.f, display.y - top - 8.f) : FLT_MAX;
        ImGui::SetNextWindowPos(defaultPos, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints({width, 0.f}, {width, maxH});
        {
            std::lock_guard lock(g_testColumnLock);
            if (!g_testColumn.empty() && g_testColumn == name) {
                if (!g_testCollapsed.count(name)) {
                    const ImGuiWindow* w = ImGui::FindWindowByName(name.c_str());
                    g_testCollapsed[name] = w && w->Collapsed;
                }
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
            } else if (const auto was = g_testCollapsed.find(name); was != g_testCollapsed.end()) {
                ImGui::SetNextWindowCollapsed(was->second, ImGuiCond_Always);   // As the player left it
                g_testCollapsed.erase(was);
            }
        }
        return ImGui::Begin(name.c_str(), nullptr, flags);   // EndColumn either way
    }

    // Its title bar stays on the screen (a resolution change, or dragged off), and the column shows where it scrolls.
    void EndColumn(const std::string& name)
    {
        const theme::MenuColors& tc = theme::Menu();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        if (display.x > 0.f && display.y > 0.f)
        {
            const ImVec2 clamped{std::clamp(pos.x, 40.f - size.x, std::max(0.f, display.x - 40.f)),
                                 std::clamp(pos.y, 0.f, std::max(0.f, display.y - ImGui::GetFrameHeight()))};
            if (clamped.x != pos.x || clamped.y != pos.y)
            {
                ImGui::SetWindowPos(clamped);
                pos = clamped;
            }
        }
        g_columnPos[name] = pos;

        const float maxScroll = ImGui::GetScrollMaxY();
        if (maxScroll > 0.5f && !ImGui::IsWindowCollapsed())
        {
            // A thin accent bar on the right edge
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float top = pos.y + ImGui::GetFrameHeight(), bottom = pos.y + size.y;
            dl->PushClipRect(pos, {pos.x + size.x, bottom}, false);
            const float track = bottom - top;
            const float thumb = std::max(16.f, track * track / (track + maxScroll));
            const float y = top + (track - thumb) * (ImGui::GetScrollY() / maxScroll);
            ImVec4 bar = tc.accent;
            bar.w *= 0.8f;
            dl->AddRectFilled({pos.x + size.x - 3.f, y}, {pos.x + size.x - 1.f, y + thumb}, U32(bar), 1.f);
            dl->PopClipRect();
        }
        ImGui::End();
    }

    // The search column: a box (Ctrl+F) and every module that matches, by tab. A result's switch works right there;
    // clicking its name opens its settings in its own column.
    void DrawSearch(float dt, const std::string& query)
    {
        const theme::MenuColors& tc = theme::Menu();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy({0.f, 4.f});
        ImGui::Indent(8.f);
        ImGui::SetNextItemWidth(w - 16.f);
        if (g_focusSearch)
        {
            ImGui::SetKeyboardFocusHere();
            g_focusSearch = false;
        }
        ImGui::InputTextWithHint("##search", "Search (Ctrl+F)", g_search, sizeof g_search, ImGuiInputTextFlags_EscapeClearsAll);
        ImGui::SetItemTooltip("Modules and settings by name or description. Esc clears");
        ImGui::Unindent(8.f);
        ImGui::Dummy({0.f, 2.f});
        if (query.empty()) return;

        bool any = false;
        for (auto& tab : modules::Tabs())
        {
            bool header = false;
            for (Module* m : tab->modules)
            {
                if (m->Hidden() || !Matches(*m, query)) continue;
                if (!header)
                {
                    ImGui::Indent(8.f);
                    ImGui::PushStyleColor(ImGuiCol_Text, tc.textDim);
                    ImGui::TextUnformatted(tab->name.c_str());
                    ImGui::PopStyleColor();
                    ImGui::Unindent(8.f);
                    header = true;
                }
                DrawModule(*m, dt, false, true);
                any = true;
            }
        }
        if (!any)
        {
            ImGui::Indent(8.f);
            ImGui::PushStyleColor(ImGuiCol_Text, tc.textDim);
            ImGui::TextUnformatted("Nothing found");
            ImGui::PopStyleColor();
            ImGui::Unindent(8.f);
        }
        ImGui::Dummy({0.f, 4.f});
    }
}

void gui::ApplyStyle()
{
    theme::ApplyBaseStyle(); // fixed layout + default colors; JSON themes are layered on top by imjson
    theme::Apply(theme::Current()); // load the selected theme file (menu colors + imjson document)
}

bool gui::CaptureKey(int vk)
{
    return keybinds::Capture(vk);
}

// Middle mouse / Mouse 4 / Mouse 5: bound like keys (the menu gets no key message for them)
static void PollMouseButtons()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    for (const int vk : keybinds::PollMouseButtons(pid == GetCurrentProcessId()))
        if (!keybinds::Capture(vk)) modules::OnKey(vk);
}

void gui::Render()
{
    PollMouseButtons();
    modules::Render(); // HUD modules draw even when the menu is hidden
    config::AutoSave();    // Cheap unless a second has passed; writes only a changed config
    notify::Draw();

    if (!visible)
    {
        keybinds::Cancel(); // closing the menu cancels a pending key binding
        return;
    }

    theme::Begin(); // imjson pushes the theme onto ImGui's style stack
    const float dt = ImGui::GetIO().DeltaTime;
    // Auto sized, never past the bottom of the screen: a long panel (TAS) scrolls with the mouse wheel instead
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) g_focusSearch = true;
    const std::string query = Lower(g_search);
    Module* const reveal = g_reveal;
    g_reveal = nullptr;

    float x = 24.f;
    bool anyTab = false;
    for (auto& tab : modules::Tabs())
    {
        if (tab->modules.empty()) continue;
        anyTab = true;

        if (BeginColumn(tab->name, tab->width, {x, 24.f}, kFlags))
        {
            for (Module* m : tab->modules)
            {
                if (m->Hidden()) continue;
                if (m == reveal)    // From the search: bring this column forward and scroll to the module
                {
                    ImGui::SetWindowFocus();
                    ImGui::SetScrollHereY(0.f);
                }
                DrawModule(*m, dt, !query.empty() && Matches(*m, query));
            }
        }
        EndColumn(tab->name);
        x += tab->width + 12.f;
    }
    if (anyTab)
    {
        // First time: the top right corner (the columns already reach past the right edge of smaller screens)
        const float right = ImGui::GetIO().DisplaySize.x - 220.f - 24.f;
        if (BeginColumn("Search", 220.f, {right > 24.f ? std::min(x, right) : x, 24.f}, kFlags)) DrawSearch(dt, query);
        EndColumn("Search");
    }

    // Built with every feature off (MEMENU_FEATURES=OFF): still show that the menu is loaded
    if (!anyTab)
    {
        ImGui::SetNextWindowPos({x, 24.f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Mirror's Edge Menu", nullptr, kFlags))
            ImGui::TextDisabled("Built without features");
        ImGui::End();
    }
    theme::End(); // must be popped before ImGui::EndFrame()

    keybinds::EndFrame(); // clicking anywhere except a bind button cancels a pending binding
}

void gui::OpenColumnForTest(const char* tab)
{
    std::lock_guard lock(g_testColumnLock);
    g_testColumn = tab ? tab : "";
}
