// Input Overlay: the keyboard and mouse on screen, like NohBoard. It shows what the game gets: your own input while you
// play, and the macro's while the TAS records or plays one back (tas/input_monitor.h). Without the TAS engine it falls
// back to the physical keys. The layout is a file (features/visual/input_layout.h); the look is in the settings.
#include "core/module.h"
#include "features/visual/input_layout.h"
#include "gui.h"
#include "paths.h"

#if MEMENU_FEATURE_TAS
#include "tas/input_monitor.h"
#include "tas/tas.h"
#endif

#include <Windows.h>
#include <shellapi.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

using inputlayout::Element;
using inputlayout::Kind;
using inputlayout::Layout;

// ---- the fallback: physical keys -----------------------------------------------------------------------------------
int VirtualKey(const std::string& name)
{
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') return std::toupper(static_cast<unsigned char>(name[0]));
    static const std::unordered_map<std::string, int> keys = {
        {"zero", '0'}, {"one", '1'}, {"two", '2'}, {"three", '3'}, {"four", '4'}, {"five", '5'}, {"six", '6'},
        {"seven", '7'}, {"eight", '8'}, {"nine", '9'}, {"spacebar", VK_SPACE}, {"leftshift", VK_LSHIFT},
        {"rightshift", VK_RSHIFT}, {"leftcontrol", VK_LCONTROL}, {"rightcontrol", VK_RCONTROL}, {"leftalt", VK_LMENU},
        {"rightalt", VK_RMENU}, {"tab", VK_TAB}, {"capslock", VK_CAPITAL}, {"escape", VK_ESCAPE}, {"enter", VK_RETURN},
        {"backspace", VK_BACK}, {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
        {"leftmousebutton", VK_LBUTTON}, {"rightmousebutton", VK_RBUTTON}, {"middlemousebutton", VK_MBUTTON},
        {"thumbmousebutton", VK_XBUTTON1}, {"thumbmousebutton2", VK_XBUTTON2},
    };
    if (name.size() >= 2 && name[0] == 'f' && std::isdigit(static_cast<unsigned char>(name[1]))) {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    const auto it = keys.find(name);
    return it == keys.end() ? 0 : it->second;
}

// What the overlay draws from: held keys and their times, the mouse movement, the presses per second.
struct State {
    struct Key { bool held = false; double down = -1.0, up = -1.0; unsigned presses = 0; };
    std::unordered_map<std::string, Key> keys;
    float mouseX = 0.f, mouseY = 0.f;
    int kps = 0;
    double now = 0.0;
    bool engine = false;            // From the engine (else the physical keys: no mouse movement, counts or KPS)

    const Key* Find(const std::string& name) const
    {
        const auto it = keys.find(name);
        return it == keys.end() ? nullptr : &it->second;
    }
};

double Seconds()
{
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

ImU32 Col(const std::array<float, 4>& c, float alpha = 1.f)
{
    return ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], c[3] * alpha});
}

ImU32 Mix(const std::array<float, 4>& a, const std::array<float, 4>& b, float t)
{
    return ImGui::ColorConvertFloat4ToU32({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t,
                                           a[3] + (b[3] - a[3]) * t});
}

class InputOverlay final : public Module {
public:
    InputOverlay() : Module("Input Overlay", "Visual", "Keyboard and mouse on screen (like NohBoard), also for TAS playback", 40)
    {
        const auto in = [](ctl::Section& section, std::initializer_list<ctl::Setting*> controls) {
            for (ctl::Setting* c : controls) c->VisibleIf([&section] { return section.Get(); });
        };
        layoutName.VisibleIf([] { return false; });     // Picked in the list, remembered here
        in(layoutSection, {&layoutPicker, &showWhen});
        in(lookSection, {&keySize, &spacing, &rounding, &outline, &textSize, &fade, &backdrop});
        in(colorsSection, {&keyColor, &pressedColor, &textColor, &pressedTextColor, &outlineColor});
        in(extrasSection, {&counts, &mouseScale, &mouseTrail, &invertY});
    }

    void OnEnable() override { loadedFrom_.clear(); }

    void OnRender() override
    {
        EnsureLayout();
        if (!Showing()) return;
        const State st = Read();
        const float unit = static_cast<float>(keySize.Get());
        const ImVec2 size{layout_.Width() * unit, layout_.Height() * unit};

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
        if (!gui::visible) flags |= ImGuiWindowFlags_NoInputs;      // Only movable while the menu is open
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({20.f, screen.y - size.y - 110.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(gui::visible ? std::max(backdrop.Get(), 0.25f) : backdrop.Get());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6.f, 6.f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, gui::visible ? 1.f : 0.f);
        if (ImGui::Begin("Input overlay##inputoverlay", nullptr, flags)) {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(size);
            if (gui::visible) ImGui::SetItemTooltip("Drag to move it. The layout is in the module's settings");
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (const Element& e : layout_.elements) {
                const float gap = static_cast<float>(spacing.Get()) * 0.5f;
                const ImVec2 a{origin.x + e.x * unit + gap, origin.y + e.y * unit + gap};
                const ImVec2 b{origin.x + (e.x + e.w) * unit - gap, origin.y + (e.y + e.h) * unit - gap};
                switch (e.kind) {
                    case Kind::Key:   DrawKey(dl, e, a, b, st); break;
                    case Kind::Mouse: DrawMouse(dl, a, b, st); break;
                    case Kind::Kps:   DrawKps(dl, a, b, st); break;
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

private:
    // ---- what to show -----------------------------------------------------------------------------------------------
    bool Showing() const
    {
#if MEMENU_FEATURE_TAS
        const bool session = tas::GetStatus().mode != tas::Mode::Idle;
        if (showWhen.Get() == 1 && session) return false;
        if (showWhen.Get() == 2 && !session) return false;
#endif
        return !layout_.elements.empty();
    }

    State Read() const
    {
        State s;
#if MEMENU_FEATURE_TAS
        if (tas::input::Installed()) {
            const auto snap = tas::input::Get(0.05);
            for (const auto& [name, k] : snap.keys) s.keys[name] = {k.held, k.down, k.up, k.presses};
            s.mouseX = snap.mouseX;
            s.mouseY = snap.mouseY;
            s.kps = snap.kps;
            s.now = snap.now;
            s.engine = true;
            return s;
        }
#endif
        // Physical keys: only what is held (the menu's own focus doesn't matter, it reads the keyboard)
        s.now = Seconds();
        for (const Element& e : layout_.elements)
            for (const std::string& k : e.keys)
                if (const int vk = VirtualKey(k)) s.keys[k].held = (GetAsyncKeyState(vk) & 0x8000) != 0;
        return s;
    }

    // ---- drawing ----------------------------------------------------------------------------------------------------
    float TextSize() const
    {
        return textSize.Get() > 0 ? static_cast<float>(textSize.Get()) : std::max(9.f, keySize.Get() * 0.36f);
    }

    void CenteredText(ImDrawList* dl, ImVec2 a, ImVec2 b, float size, ImU32 col, const char* text, float dy = 0.f) const
    {
        ImFont* font = ImGui::GetFont();
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
        dl->AddText(font, size, {(a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f + dy}, col, text);
    }

    void Box(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill) const
    {
        const float r = static_cast<float>(rounding.Get());
        dl->AddRectFilled(a, b, fill, r);
        if (outline.Get() > 0.f) dl->AddRect(a, b, Col(outlineColor), r, 0, outline.Get());
    }

    // How lit a key is: 1 while held, then fading out over the fade time. The wheel's "keys" are only down for an
    // instant: they get at least a short flash.
    float Lit(const Element& e, const State& st) const
    {
        float lit = 0.f;
        const double fadeS = fade.Get() / 1000.0;
        for (const std::string& name : e.keys) {
            const State::Key* k = st.Find(name);
            if (!k || k->down < 0.0) continue;
            if (k->held) return 1.f;
            const bool wheel = name == "mousescrollup" || name == "mousescrolldown";
            const double release = wheel ? std::max(k->up, k->down + 0.12) : k->up;     // When it went dark
            if (st.now < release) return 1.f;
            const double since = st.now - release;
            if (fadeS > 0.0 && since < fadeS) lit = std::max(lit, 1.f - static_cast<float>(since / fadeS));
        }
        return lit;
    }

    void DrawKey(ImDrawList* dl, const Element& e, ImVec2 a, ImVec2 b, const State& st) const
    {
        const float lit = Lit(e, st);
        Box(dl, a, b, Mix(keyColor.Get(), pressedColor.Get(), lit));
        const ImU32 text = Mix(textColor.Get(), pressedTextColor.Get(), lit);
        const float size = std::min(TextSize(), (b.y - a.y) * 0.8f);
        const bool showCount = counts && st.engine && b.y - a.y > size * 1.9f;
        CenteredText(dl, a, b, size, text, e.label.c_str(), showCount ? -size * 0.3f : 0.f);
        if (showCount) {
            unsigned presses = 0;
            for (const std::string& name : e.keys)
                if (const State::Key* k = st.Find(name)) presses += k->presses;
            char n[16];
            std::snprintf(n, sizeof n, "%u", presses);
            CenteredText(dl, a, b, size * 0.6f, (text & 0x00FFFFFFu) | 0xA0000000u, n, size * 0.55f);
        }
    }

    // The mouse's movement: a dot pushed away from the middle the way it moves, and how far (with a line to it).
    void DrawMouse(ImDrawList* dl, ImVec2 a, ImVec2 b, const State& st) const
    {
        Box(dl, a, b, Col(keyColor));
        const ImVec2 c{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        const float radius = std::min(b.x - a.x, b.y - a.y) * 0.5f - 6.f;
        if (radius <= 2.f) return;
        const ImU32 faint = (Col(textColor) & 0x00FFFFFFu) | 0x40000000u;
        dl->AddLine({c.x - radius, c.y}, {c.x + radius, c.y}, faint);
        dl->AddLine({c.x, c.y - radius}, {c.x, c.y + radius}, faint);
        if (!st.engine) {
            CenteredText(dl, a, b, TextSize() * 0.7f, faint | 0xFF000000u, "mouse");
            return;
        }
        float dx = st.mouseX, dy = invertY ? st.mouseY : -st.mouseY;    // The engine's Y goes up: the screen's goes down
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.f) {
            // Big movements squash toward the edge instead of leaving the pad
            const float reach = std::tanh(len * mouseScale.Get() / 60.f) * radius;
            dx = dx / len * reach;
            dy = dy / len * reach;
        }
        const ImVec2 dot{c.x + dx, c.y + dy};
        if (mouseTrail && len > 0.f) dl->AddLine(c, dot, Col(pressedColor, 0.7f), 2.f);
        dl->AddCircleFilled(dot, std::max(3.f, radius * 0.14f), len > 0.f ? Col(pressedColor) : faint | 0xFF000000u);
    }

    void DrawKps(ImDrawList* dl, ImVec2 a, ImVec2 b, const State& st) const
    {
        Box(dl, a, b, Col(keyColor));
        const float size = std::min(TextSize(), (b.y - a.y) * 0.45f);
        char n[16];
        std::snprintf(n, sizeof n, "%d", st.engine ? st.kps : 0);
        CenteredText(dl, a, b, size * 0.6f, (Col(textColor) & 0x00FFFFFFu) | 0x90000000u, "KPS", -size * 0.45f);
        CenteredText(dl, a, b, size, Col(textColor), n, size * 0.3f);
    }

    // ---- layouts ------------------------------------------------------------------------------------------------------
    static fs::path Folder() { return paths::Dir() / "input_layouts"; }

    // The built-in layouts are written as files the first time, so they can be copied and edited.
    static void WriteBuiltins()
    {
        std::error_code ec;
        fs::create_directories(Folder(), ec);
        for (const Layout& l : inputlayout::Builtins()) {
            const fs::path p = Folder() / (l.name + ".json");
            if (fs::exists(p, ec)) continue;
            std::ofstream(p, std::ios::binary) << inputlayout::ToJson(l);
        }
    }

    void RefreshNames()
    {
        names_.clear();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(Folder(), ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".json") names_.push_back(e.path().stem().string());
        std::sort(names_.begin(), names_.end());
    }

    // Loads the picked layout when another is picked, and again when its file changes (looked at once a second).
    void EnsureLayout()
    {
        const std::string want = layoutName.Get().empty() ? "Movement" : layoutName.Get();
        const double now = ImGui::GetTime();
        if (want == loadedFrom_ && now < nextLook_) return;
        nextLook_ = now + 1.0;
        const fs::path path = Folder() / (want + ".json");
        std::error_code ec;
        const auto stamp = fs::last_write_time(path, ec);
        if (want == loadedFrom_ && !ec && stamp == loadedStamp_) return;
        loadedFrom_ = want;
        loadedStamp_ = stamp;

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            // No such file (deleted, or the first start): the built-in layout of that name, else Movement
            WriteBuiltins();
            const auto builtins = inputlayout::Builtins();
            layout_ = builtins.front();
            for (const Layout& l : builtins)
                if (l.name == want) layout_ = l;
            error_ = layout_.name == want ? "" : "no " + path.filename().string() + ": showing Movement";
            return;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        Layout l;
        std::string error;
        if (inputlayout::Parse(ss.str(), l, &error)) {
            layout_ = std::move(l);
            error_.clear();
        } else {
            error_ = path.filename().string() + ": " + error + " (showing the last good one)";
            if (layout_.elements.empty()) layout_ = inputlayout::Builtins().front();
        }
    }

    void DrawLayoutPicker(float width)
    {
        if (names_.empty() || ImGui::GetTime() > namesAt_ + 2.0) {
            if (names_.empty()) WriteBuiltins();
            RefreshNames();
            namesAt_ = ImGui::GetTime();
        }
        const std::string current = layoutName.Get().empty() ? "Movement" : layoutName.Get();
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##layout", current.c_str())) {
            for (const std::string& n : names_)
                if (ImGui::Selectable(n.c_str(), n == current)) layoutName.Set(n);
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Layouts are the .json files in memenu\\input_layouts\\: copy one, move its keys, rename it");
        if (!error_.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextColored({1.f, 0.6f, 0.35f, 1.f}, "%s", error_.c_str());
            ImGui::PopTextWrapPos();
        }
        const float third = (width - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.f;
        if (ImGui::Button("Open folder", {third, 0})) {
            WriteBuiltins();
            ShellExecuteW(nullptr, L"open", Folder().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload", {third, 0})) {
            loadedFrom_.clear();
            namesAt_ = -10.0;
        }
        ImGui::SetItemTooltip("Read the layout file again (edits are also picked up by themselves)");
        ImGui::SameLine();
        if (ImGui::Button("Reset counts", {third, 0})) {
#if MEMENU_FEATURE_TAS
            tas::input::ResetCounts();
#endif
        }
        ImGui::SetItemTooltip("The press counts on the keys (Extras > Press counts)");
#if MEMENU_FEATURE_TAS
        if (!tas::input::Installed())
#endif
            ImGui::TextDisabled("Reading the keyboard itself: no mouse movement, counts or TAS playback");
    }

    Layout layout_;
    std::string loadedFrom_, error_;
    fs::file_time_type loadedStamp_{};
    double nextLook_ = 0.0;
    std::vector<std::string> names_;
    double namesAt_ = -10.0;

    // ---- controls ---------------------------------------------------------------------------------------------------
    ctl::Text layoutName{this, "Layout name", "Movement"};

    ctl::Section layoutSection{this, "Layout", true};
    ctl::Custom layoutPicker{this, [this](float w) { DrawLayoutPicker(w); }};
#if MEMENU_FEATURE_TAS
    ctl::Combo showWhen{this, "Show", {"Always", "Not during a TAS", "Only during a TAS"}, 0,
                        "During a TAS it shows the macro's input: what it presses, frame by frame"};
#else
    ctl::Combo showWhen{this, "Show", {"Always"}, 0};
#endif

    ctl::Section lookSection{this, "Look", false};
    ctl::SliderInt keySize{this, "Key size", 36, 16, 96, "%d px"};
    ctl::SliderInt spacing{this, "Spacing", 4, 0, 16, "%d px"};
    ctl::SliderInt rounding{this, "Rounding", 5, 0, 20, "%d px"};
    ctl::Slider outline{this, "Outline", 1.f, 0.f, 4.f, "%.1f px", 0.5f};
    ctl::SliderInt textSize{this, "Text size", 0, 0, 48, "%d px", 1, "0 = from the key size"};
    ctl::SliderInt fade{this, "Fade", 90, 0, 500, "%d ms", 10, "How long a key takes to go dark after it is let go"};
    ctl::Slider backdrop{this, "Backdrop", 0.f, 0.f, 1.f, "%.2f", 0.05f, "The window's background behind the keys"};

    ctl::Section colorsSection{this, "Colors", false};
    ctl::ColorPick keyColor{this, "Key", {0.08f, 0.08f, 0.10f, 0.72f}};
    ctl::ColorPick pressedColor{this, "Pressed", {0.93f, 0.93f, 0.95f, 0.95f}};
    ctl::ColorPick textColor{this, "Text", {0.85f, 0.85f, 0.88f, 1.f}};
    ctl::ColorPick pressedTextColor{this, "Pressed text", {0.06f, 0.06f, 0.08f, 1.f}};
    ctl::ColorPick outlineColor{this, "Outline color", {1.f, 1.f, 1.f, 0.18f}};

    ctl::Section extrasSection{this, "Extras", false};
    ctl::Checkbox counts{this, "Press counts", false, "How many times each key was pressed, under its name"};
    ctl::Slider mouseScale{this, "Mouse pad sensitivity", 1.f, 0.1f, 5.f, "%.1fx", 0.1f};
    ctl::Checkbox mouseTrail{this, "Mouse pad line", true, "A line from the middle to the dot"};
    ctl::Checkbox invertY{this, "Invert mouse pad Y", false};
};
MEM_MODULE(InputOverlay)

} // namespace
