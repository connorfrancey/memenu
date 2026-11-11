#include "features/visual/info_lines.h"

#include "core/module.h"
#include "core/notify.h"
#include "font.h"
#include "game/game.h"
#include "game/player_state.h"
#include "gui.h"
#include "paths.h"
#include "theme.h"
#if MEMENU_FEATURE_SPEEDRUN_TIMER
#include "speedrun/live.h"
#endif

#include <Windows.h>
#include <shellapi.h>
#include <imgui.h>
#include "sdk/SDK.hpp"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <unordered_map>

// Info Lines: the player's own lines of text on the screen, written in the language of hud/template.h. Profiles are
// text files in memenu\hud\<name>.txt, edited in the menu (or any editor: a changed file is read again). A profile
// that has no file yet is the built-in example; it is written only once it is edited.
namespace {

using hud::Value;

const char* const kExample =
R"(// Info Lines: your own lines on the screen. Edit them here (changes show right away) or in
// memenu\hud\<profile>.txt with any editor. "Values" and "Functions" below the editor list what you can show.
//
// == <place>, <option> <value>, ... ==   starts a section. Places: top left, top, top right, left, center, right,
//    bottom left, bottom, bottom right, at <x> <y> (pixels, or 50% for half the screen)
//    Options: scale 1.2  bg 0.5  padding 6  spacing 2  offset 10 40  width 160  align right
//             color #rrggbb  shadow off  outline on
// {speed}  {speed:.1f}  {x:>9.2f}  {health:03d}  {map:^12}    values, with fmt format specs
// {speed * 0.036:.1f}  {max(a, b)}  {round(z / 100, 1)}         math
// {if(speed > 700, "&(green)fast", "slow")}                    a choice ("strings" can hold {values} too)
// &(red) &(accent) &#ff8800 &#ff880080   text color from there on; &r goes back to the section's color
// Speed &> {speed}     text after &> goes in a second, right-aligned column ("&&" is a literal &)
// {bar(health / maxhealth, 80, "red")}  a bar     {time(gametime)}  1:02.35
// #if <condition> / #elif / #else / #endif   show lines only sometimes.  // starts a comment line

== top right, offset 0 40, bg 0.45, padding 6 ==
#if haspawn
&(gray)Speed &>{mix("white", "#40ff70", clamp((speed - 400) / 500, 0, 1))}{speed:.0f} &(gray)u/s
&(gray)Top speed &>{topspeed:.0f} &(gray)u/s
&(gray)Height &>{z:.0f}
&(gray)Move &>{statename}
#if health < maxhealth
&(gray)Health &>{bar(health / maxhealth, 70, if(health < 40, "red", "white"))}
#endif
#if reactionactive
&(accent)Reaction time &>{reaction:.0f}%
#endif
#else
&(gray)Info Lines: load a level
#endif
)";

// TdPawn.EMovement, by value (the SDK's names without MOVE_)
const char* MoveName(int m)
{
    static const char* const kNames[] = {
        "None", "Walking", "Falling", "Grabbing", "WallRunningRight", "WallRunningLeft", "WallClimbing", "SpringBoarding",
        "SpeedVaulting", "VaultOver", "GrabPullUp", "Jump", "WallRunJump", "GrabJump", "IntoGrab", "Crouch", "Slide",
        "Melee", "Snatch", "Barge", "Landing", "Climb", "IntoClimb", "WallKick", "180Turn", "180TurnInAir",
        "LayOnGround", "IntoZipLine", "ZipLine", "Balance", "LedgeWalk", "GrabTransfer", "MeleeAir", "DodgeJump",
        "WallRunDodgeJump", "Stumble", "Snatched", "StepUp", "RumpSlide", "Interact", "WallRun", "BotStop",
        "BotStartWalking", "BotStartRunning", "BotTurnRunning", "BotTurnStanding", "ExitCover", "Vertigo",
        "MeleeSlide", "WallClimbDodgeJump", "WallClimb180TurnJump", "WallClimbDodgeJumpLeft",
        "WallClimbDodgeJumpRight", "MeleeVault", "BotMeleeSecondSwing", "StumbleHard", "BotRoll", "BotFlip", nullptr,
        nullptr, "Swing", "Coil", "MeleeWallrun", "MeleeCrouch", "BotJumpShort", "BotJumpMedium", "BotJumpLong",
        "JumpIntoGrab", "StandGrabHeaveBot", "BotMeleeDodge", "FinishAttack", "MeleeBarge", "FallingUncontrolled",
        "SwingJump", "AnimationPlayback", "EnterCover", "Cover", "StumbleFalling", "SoftLanding",
        "HeadButtedByCeleste", nullptr, "AutoStepUp", "MeleeAirAbove", nullptr, "Block", "AirBarge",
    };
    if (m >= 0 && m < static_cast<int>(std::size(kNames)) && kNames[m]) return kNames[m];
    return "?";
}

// ---- The values -------------------------------------------------------------------------------------------------------
struct GameData {       // Copied on the game thread
    bool hasPawn = false;
    double x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
    double topSpeed = 0, topHeight = 0;
    double pitch = 0, yaw = 0;
    int health = 0, maxHealth = 100, move = 0;
    double reaction = 0, jumpZ = 0;
    bool reactionActive = false, onGround = false;
    std::string map;
    bool loading = false;
    double gameSpeed = 1, gameTime = 0, realTime = 0;
    unsigned deaths = 0, loads = 0;
    unsigned long long ticks = 0;
};

struct Frame {          // Everything a line can show, for one frame
    GameData game;
    double fps = 0, frameMs = 0;
    bool menuOpen = false;
    std::tm local{};
#if MEMENU_FEATURE_SPEEDRUN_TIMER
    double runTime = 0, runRta = 0;
#endif
};

struct VarDef {
    const char* name;
    const char* group;
    const char* help;
    std::function<Value(const Frame&)> get;
};

double Hypot(double a, double b) { return std::sqrt(a * a + b * b); }

const std::vector<VarDef>& Vars()
{
    using F = const Frame&;
    static const std::vector<VarDef> vars = {
        {"x", "Player", "Position X (game units, 1 = 1 cm)", [](F f) { return Value::Num(f.game.x); }},
        {"y", "Player", "Position Y", [](F f) { return Value::Num(f.game.y); }},
        {"z", "Player", "Position Z (height)", [](F f) { return Value::Num(f.game.z); }},
        {"speed", "Player", "Horizontal speed (units/s)", [](F f) { return Value::Num(Hypot(f.game.vx, f.game.vy)); }},
        {"speed3d", "Player", "Speed in every direction (units/s)",
         [](F f) { return Value::Num(std::sqrt(f.game.vx * f.game.vx + f.game.vy * f.game.vy + f.game.vz * f.game.vz)); }},
        {"vspeed", "Player", "Vertical speed (units/s, up is positive)", [](F f) { return Value::Num(f.game.vz); }},
        {"kmh", "Player", "Horizontal speed in km/h", [](F f) { return Value::Num(Hypot(f.game.vx, f.game.vy) * 0.036); }},
        {"ms", "Player", "Horizontal speed in m/s", [](F f) { return Value::Num(Hypot(f.game.vx, f.game.vy) * 0.01); }},
        {"topspeed", "Player", "Highest horizontal speed of the last 2.75 s", [](F f) { return Value::Num(f.game.topSpeed); }},
        {"topz", "Player", "Highest Z of the last 2.75 s", [](F f) { return Value::Num(f.game.topHeight); }},
        {"pitch", "Player", "Looking up (+) or down (-), degrees", [](F f) { return Value::Num(f.game.pitch); }},
        {"yaw", "Player", "Facing, degrees (0-360)", [](F f) { return Value::Num(f.game.yaw); }},
        {"health", "Player", "Health", [](F f) { return Value::Num(f.game.health); }},
        {"maxhealth", "Player", "Full health", [](F f) { return Value::Num(f.game.maxHealth); }},
        {"state", "Player", "Movement state, as a number", [](F f) { return Value::Num(f.game.move); }},
        {"statename", "Player", "Movement state (Walking, Falling, WallRunningLeft, ...)",
         [](F f) { return Value::Str(MoveName(f.game.move)); }},
        {"onground", "Player", "Standing or running on the ground", [](F f) { return Value::Bool(f.game.onGround); }},
        {"reaction", "Player", "Reaction time meter, 0-100", [](F f) { return Value::Num(f.game.reaction); }},
        {"reactionactive", "Player", "Reaction time is on", [](F f) { return Value::Bool(f.game.reactionActive); }},
        {"jumpz", "Player", "Z where the last jump started", [](F f) { return Value::Num(f.game.jumpZ); }},
        {"sincejump", "Player", "Height above the last jump's start", [](F f) { return Value::Num(f.game.z - f.game.jumpZ); }},
        {"haspawn", "Player", "There is a player (false in menus and while loading)", [](F f) { return Value::Bool(f.game.hasPawn); }},

        {"map", "World", "Level package (edge_p, ...)", [](F f) { return Value::Str(f.game.map); }},
        {"loading", "World", "A level is loading", [](F f) { return Value::Bool(f.game.loading); }},
        {"gamespeed", "World", "Game speed (1 = normal)", [](F f) { return Value::Num(f.game.gameSpeed); }},
        {"gametime", "World", "Seconds of game time in this level (slows down with the game)", [](F f) { return Value::Num(f.game.gameTime); }},
        {"realtime", "World", "Seconds of real time in this level", [](F f) { return Value::Num(f.game.realTime); }},
        {"deaths", "World", "Deaths since the game started", [](F f) { return Value::Num(f.game.deaths); }},
        {"loads", "World", "Save states loaded", [](F f) { return Value::Num(f.game.loads); }},
        {"ticks", "World", "Engine ticks since the menu loaded", [](F f) { return Value::Num(static_cast<double>(f.game.ticks)); }},

#if MEMENU_FEATURE_SPEEDRUN_TIMER
        {"runtime", "Speedrun", "Speedrun timer, loads removed (seconds: use time(runtime))", [](F f) { return Value::Num(f.runTime); }},
        {"runrta", "Speedrun", "Speedrun timer, real time (seconds)", [](F f) { return Value::Num(f.runRta); }},
#endif

        {"fps", "Menu", "Frames per second", [](F f) { return Value::Num(f.fps); }},
        {"frametime", "Menu", "Milliseconds per frame", [](F f) { return Value::Num(f.frameMs); }},
        {"menuopen", "Menu", "The menu is open", [](F f) { return Value::Bool(f.menuOpen); }},
        {"clock", "Menu", "Time of day, 14:05:09", [](F f) {
             char b[16];
             std::strftime(b, sizeof b, "%H:%M:%S", &f.local);
             return Value::Str(b);
         }},
        {"date", "Menu", "Today, 2026-11-09", [](F f) {
             char b[16];
             std::strftime(b, sizeof b, "%Y-%m-%d", &f.local);
             return Value::Str(b);
         }},
        {"hour", "Menu", "Hour of the day (0-23)", [](F f) { return Value::Num(f.local.tm_hour); }},
        {"minute", "Menu", "Minute (0-59)", [](F f) { return Value::Num(f.local.tm_min); }},
        {"second", "Menu", "Second (0-59)", [](F f) { return Value::Num(f.local.tm_sec); }},
    };
    return vars;
}

const VarDef* FindVar(std::string_view name)
{
    static const std::unordered_map<std::string_view, const VarDef*> byName = [] {
        std::unordered_map<std::string_view, const VarDef*> m;
        for (const VarDef& v : Vars()) m.emplace(v.name, &v);
        return m;
    }();
    const auto it = byName.find(name);
    return it == byName.end() ? nullptr : it->second;
}

hud::Color ToColor(const ImVec4& c)
{
    const auto ch = [](float v) { return static_cast<hud::Color>(std::clamp(std::lround(v * 255.f), 0L, 255L)); };
    return (ch(c.x) << 24) | (ch(c.y) << 16) | (ch(c.z) << 8) | ch(c.w);
}

ImU32 ToImGui(hud::Color c, float alpha = 1.f)
{
    const auto a = static_cast<int>(static_cast<float>(c & 0xFF) * alpha);
    return IM_COL32((c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF, std::clamp(a, 0, 255));
}

class FrameContext final : public hud::Context {
public:
    explicit FrameContext(const Frame& f) : f_(f) {}

    bool Variable(std::string_view name, Value& out) const override
    {
        const VarDef* v = FindVar(name);
        if (!v) return false;
        out = v->get(f_);
        return true;
    }

    bool Call(std::string_view name, const std::vector<Value>& args, Value& out, std::string& error) const override
    {
        if (name != "on") return false;
        if (args.size() != 1 || args[0].kind != Value::Kind::Text) {
            error = "on(\"Module name\") takes the name of a module";
            return true;
        }
        Module* m = modules::Find(args[0].text);
        if (!m) error = "there is no module '" + args[0].text + "'";
        else out = Value::Bool(m->Enabled());
        return true;
    }

    bool NamedColor(std::string_view name, hud::Color& out) const override
    {
        const theme::MenuColors& t = theme::Menu();
        if (name == "accent") out = ToColor(t.accent);
        else if (name == "text") out = ToColor(t.text);
        else if (name == "dim") out = ToColor(t.textDim);
        else return false;
        return true;
    }

private:
    const Frame& f_;
};

// Shared with info_lines::LastStatus / Evaluate (the self-test thread)
std::atomic<bool> g_openForTest{false};      // info_lines::SetEditorOpenForTest
std::atomic<unsigned long long> g_editorFrames{0};
std::mutex g_shareMutex;
info_lines::Status g_status;
Frame g_lastFrame;

// Highest value of the last 2.75 s (like the Speedometer's top speed)
struct Peak {
    double value = 0.0, at = -1e9;
    bool any = false;
    void Update(double v, double now)
    {
        if (now - at > 2.75) any = false;
        if (!any || v > value) { value = v; at = now; any = true; }
    }
};

std::filesystem::path Folder() { return paths::Dir() / "hud"; }

std::string ReadFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int ResizeCallback(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(data->UserData);
        s->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = s->data();
    }
    return 0;
}

class InfoLines final : public Module {
public:
    InfoLines() : Module("Info Lines", "Visual", "Your own lines of text on the screen: live values, math, colors, conditions", 30)
    {
        profileName.VisibleIf([] { return false; });     // Picked with the editor's list
    }

    void OnEnable() override
    {
        std::lock_guard lock(gameMutex_);
        game_ = {};
        topSpeed_ = {};
        topHeight_ = {};
    }

    void OnDisable() override
    {
        std::lock_guard lock(g_shareMutex);
        g_status = {};
    }

    // Game thread: copy what the lines can show
    void OnTick() override
    {
        GameData d;
        auto* pawn = game::Pawn();
        auto* controller = game::Controller();
        auto* world = game::World();
        if (pawn && controller) {
            d.hasPawn = true;
            d.x = pawn->Location.X;
            d.y = pawn->Location.Y;
            d.z = pawn->Location.Z;
            d.vx = pawn->Velocity.X;
            d.vy = pawn->Velocity.Y;
            d.vz = pawn->Velocity.Z;
            d.pitch = static_cast<std::int16_t>(controller->Rotation.Pitch & 0xFFFF) / 65536.0 * 360.0;
            d.yaw = static_cast<double>(controller->Rotation.Yaw & 0xFFFF) / 65536.0 * 360.0;
            d.health = pawn->Health;
            d.maxHealth = pawn->MaxHealth > 0 ? pawn->MaxHealth : 100;
            d.move = static_cast<int>(pawn->MovementState.GetValue());
            d.reaction = std::min(100.0, static_cast<double>(controller->ReactionTimeEnergy));
            d.reactionActive = controller->bReactionTime;
            d.onGround = pawn->Physics == Classes::EPhysics::PHYS_Walking;
            d.jumpZ = pawn->LastJumpLocation.Z;
        }
        if (world) {
            d.gameSpeed = world->TimeDilation;
            d.gameTime = world->TimeSeconds;
            d.realTime = world->RealTimeSeconds;
            if (d.hasPawn) {
                topSpeed_.Update(Hypot(d.vx, d.vy), d.realTime);
                topHeight_.Update(d.z, d.realTime);
            }
        }
        if (!d.hasPawn) {
            topSpeed_ = {};
            topHeight_ = {};
        }
        d.topSpeed = topSpeed_.value;
        d.topHeight = topHeight_.value;
        d.map = game::MapName();
        d.loading = game::IsLoading();
        const game::Status st = game::GetStatus();
        d.deaths = st.deaths;
        d.ticks = st.ticks;
        d.loads = player_state::LoadCount();

        std::lock_guard lock(gameMutex_);
        game_ = std::move(d);
    }

    void OnRender() override
    {
        Sync();
        const Frame frame = MakeFrame();
        FrameContext ctx(frame);
        std::vector<hud::Error> errors;
        const std::vector<hud::RenderedSection> sections = profile_.Render(ctx, &errors);
        renderErrors_ = errors;

        std::size_t lines = 0;
        const bool hidden = hideInMenu && gui::visible;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        for (const auto& s : sections) {
            lines += s.lines.size();
            if (!hidden) DrawSection(s, dl);
        }
        if (showErrors && !hidden) DrawErrorNote(dl);

        std::lock_guard lock(g_shareMutex);
        g_lastFrame = frame;
        g_status.frames++;
        g_status.sections = sections.size();
        g_status.lines = lines;
        g_status.profile = loadedName_;
        g_status.errors = profile_.Errors();
        g_status.errors.insert(g_status.errors.end(), errors.begin(), errors.end());
    }

private:
    // ---- Profile files ------------------------------------------------------------------------------------------------
    static std::filesystem::path PathOf(const std::string& name) { return Folder() / (name + ".txt"); }

    void Load(const std::string& name)
    {
        loadedName_ = name;
        const auto path = PathOf(name);
        std::error_code ec;
        fromFile_ = std::filesystem::exists(path, ec);
        text_ = fromFile_ ? ReadFile(path) : std::string(kExample);
        mtime_ = fromFile_ ? std::filesystem::last_write_time(path, ec) : std::filesystem::file_time_type{};
        dirty_ = false;
        Reparse();
    }

    void Reparse()
    {
        profile_ = hud::Profile::Parse(text_);
        renderErrors_.clear();
    }

    bool Save()
    {
        std::error_code ec;
        std::filesystem::create_directories(Folder(), ec);
        const auto path = PathOf(loadedName_);
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << text_;
            if (!out) {
                notify::Push("Info Lines: could not save " + path.string());
                spdlog::warn("Info Lines: could not write {}", path.string());
                return false;
            }
        }
        mtime_ = std::filesystem::last_write_time(path, ec);
        fromFile_ = true;
        dirty_ = false;
        return true;
    }

    // Once a frame: the picked profile is the loaded one, edits are saved after a moment, a file changed outside is read
    // again.
    void Sync()
    {
        if (loadedName_ != profileName.Get() || profileName.Get().empty()) {
            if (profileName.Get().empty()) profileName.Set("default");
            if (dirty_) Save();
            Load(profileName.Get());
        }
        const double now = ImGui::GetTime();
        if (dirty_ && now - lastEdit_ > 0.8) Save();
        if (!dirty_ && now - lastCheck_ > 1.0) {
            lastCheck_ = now;
            std::error_code ec;
            const auto path = PathOf(loadedName_);
            if (std::filesystem::exists(path, ec)) {
                const auto t = std::filesystem::last_write_time(path, ec);
                if (!ec && (!fromFile_ || t != mtime_)) {
                    Load(loadedName_);
                    spdlog::info("Info Lines: {} changed, read again", path.string());
                }
            }
        }
    }

    std::vector<std::string> ProfileNames() const
    {
        std::vector<std::string> names{"default"};
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(Folder(), ec))
            if (e.path().extension() == ".txt") names.push_back(e.path().stem().string());
        names.push_back(loadedName_);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

    Frame MakeFrame()
    {
        Frame f;
        {
            std::lock_guard lock(gameMutex_);
            f.game = game_;
        }
        const ImGuiIO& io = ImGui::GetIO();
        f.fps = io.Framerate;
        f.frameMs = io.Framerate > 0.f ? 1000.0 / io.Framerate : 0.0;
        f.menuOpen = gui::visible;
        const std::time_t t = std::time(nullptr);
        localtime_s(&f.local, &t);
#if MEMENU_FEATURE_SPEEDRUN_TIMER
        const speedrun::View run = speedrun::Snapshot();
        const double now = speedrun::Now();
        f.runTime = run.timer.Lrt(now);
        f.runRta = run.timer.Rta(now);
#endif
        return f;
    }

    // ---- Drawing ------------------------------------------------------------------------------------------------------
    void DrawSection(const hud::RenderedSection& s, ImDrawList* dl) const
    {
        if (s.lines.empty()) return;
        const hud::SectionStyle& st = s.style;
        const ImGuiIO& io = ImGui::GetIO();
        ImFont* font = ImGui::GetFont();
        const float scale = st.scale * globalScale;
        const float size = ImGui::GetFontSize() * scale;
        const float gap = size * 0.8f;

        const auto width = [&](const std::vector<hud::Segment>& segs) {
            float w = 0.f;
            for (const auto& seg : segs)
                w += seg.bar ? seg.width * scale : font->CalcTextSizeA(size, FLT_MAX, 0.f, seg.text.c_str()).x;
            return w;
        };

        struct Measured { float left, right; };
        std::vector<Measured> m;
        m.reserve(s.lines.size());
        float plainMax = 0.f, leftMax = 0.f, rightMax = 0.f;
        bool anyRight = false;
        for (const auto& line : s.lines) {
            const Measured mm{width(line.left), width(line.right)};
            m.push_back(mm);
            if (line.hasRight) {
                anyRight = true;
                leftMax = std::max(leftMax, mm.left);
                rightMax = std::max(rightMax, mm.right);
            } else {
                plainMax = std::max(plainMax, mm.left);
            }
        }
        const float contentW = std::max({plainMax, anyRight ? leftMax + gap + rightMax : 0.f, st.minWidth * scale});
        const float spacing = st.spacing * scale;
        const float contentH = static_cast<float>(s.lines.size()) * size + static_cast<float>(s.lines.size() - 1) * spacing;
        const float pad = st.padding * scale;
        const float boxW = contentW + 2.f * pad, boxH = contentH + 2.f * pad;

        const float margin = 10.f, W = io.DisplaySize.x, H = io.DisplaySize.y;
        float x = margin, y = margin;
        using A = hud::Anchor;
        switch (st.anchor) {
            case A::Top: case A::Center: case A::Bottom: x = (W - boxW) * 0.5f; break;
            case A::TopRight: case A::Right: case A::BottomRight: x = W - boxW - margin; break;
            default: break;
        }
        switch (st.anchor) {
            case A::Left: case A::Center: case A::Right: y = (H - boxH) * 0.5f; break;
            case A::BottomLeft: case A::Bottom: case A::BottomRight: y = H - boxH - margin; break;
            default: break;
        }
        if (st.anchor == A::At) {
            x = st.xPercent ? W * st.x / 100.f : st.x;
            y = st.yPercent ? H * st.y / 100.f : st.y;
        }
        x = std::floor(x + st.offsetX);
        y = std::floor(y + st.offsetY);

        if (st.background > 0.f)
            dl->AddRectFilled({x, y}, {x + boxW, y + boxH}, IM_COL32(0, 0, 0, static_cast<int>(st.background * 255.f)),
                              4.f * scale);

        const float x0 = x + pad;
        float ly = y + pad;
        for (std::size_t i = 0; i < s.lines.size(); ++i) {
            const auto& line = s.lines[i];
            float lx = x0;
            if (!line.hasRight) {
                if (st.align == hud::Align::Center) lx = x0 + (contentW - m[i].left) * 0.5f;
                else if (st.align == hud::Align::Right) lx = x0 + contentW - m[i].left;
            }
            DrawSegments(line.left, lx, ly, size, scale, st, dl, font);
            if (line.hasRight) DrawSegments(line.right, x0 + contentW - m[i].right, ly, size, scale, st, dl, font);
            ly += size + spacing;
        }
    }

    static void DrawSegments(const std::vector<hud::Segment>& segs, float x, float y, float size, float scale,
                             const hud::SectionStyle& st, ImDrawList* dl, ImFont* font)
    {
        x = std::floor(x);
        for (const auto& seg : segs) {
            if (seg.bar) {
                const float w = seg.width * scale, h = std::max(3.f, size * 0.55f);
                const float by = std::floor(y + (size - h) * 0.5f);
                dl->AddRectFilled({x, by}, {x + w, by + h}, IM_COL32(0, 0, 0, 140), 2.f);
                if (seg.fraction > 0.f)
                    dl->AddRectFilled({x + 1.f, by + 1.f}, {x + 1.f + (w - 2.f) * seg.fraction, by + h - 1.f},
                                      ToImGui(seg.color), 1.5f);
                x += w;
                continue;
            }
            const char* text = seg.text.c_str();
            const float alpha = static_cast<float>(seg.color & 0xFF) / 255.f;
            const ImU32 dark = IM_COL32(0, 0, 0, static_cast<int>(200.f * alpha));
            if (st.outline) {
                for (const ImVec2 d : {ImVec2{-1, 0}, ImVec2{1, 0}, ImVec2{0, -1}, ImVec2{0, 1}})
                    dl->AddText(font, size, {x + d.x, y + d.y}, dark, text);
            } else if (st.shadow) {
                dl->AddText(font, size, {x + 1.f, y + 1.f}, dark, text);
            }
            dl->AddText(font, size, {x, y}, ToImGui(seg.color), text);
            x += font->CalcTextSizeA(size, FLT_MAX, 0.f, text).x;
        }
    }

    // A note at the top of the screen while the profile has errors (it may be edited outside the menu)
    void DrawErrorNote(ImDrawList* dl) const
    {
        const std::size_t count = profile_.Errors().size() + renderErrors_.size();
        if (count == 0) return;
        const hud::Error& e = profile_.Errors().empty() ? renderErrors_.front() : profile_.Errors().front();
        const std::string text = "Info Lines (" + loadedName_ + "): " + std::to_string(count) + " problem" +
                                 (count == 1 ? "" : "s") + ", line " + std::to_string(e.line) + ": " + e.message;
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        const float x = std::floor((ImGui::GetIO().DisplaySize.x - ts.x) * 0.5f);
        dl->AddText({x + 1.f, 9.f}, IM_COL32(0, 0, 0, 200), text.c_str());
        dl->AddText({x, 8.f}, IM_COL32(255, 110, 100, 255), text.c_str());
    }

    // ---- The editor (in the module's settings) ------------------------------------------------------------------------
    void DrawEditor(float width)
    {
        g_editorFrames++;
        Sync();
        const theme::MenuColors& tc = theme::Menu();

        // Profile: pick one, or make a new one from this one
        const std::vector<std::string> names = ProfileNames();
        ImGui::SetNextItemWidth(width - 110.f);
        if (ImGui::BeginCombo("##profile", loadedName_.c_str())) {
            for (const std::string& n : names)
                if (ImGui::Selectable(n.c_str(), n == loadedName_)) profileName.Set(n);
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Profile: memenu\\hud\\<name>.txt");
        ImGui::SameLine();
        if (ImGui::Button("New", {48.f, 0.f})) {
            int n = 2;
            while (std::find(names.begin(), names.end(), "profile " + std::to_string(n)) != names.end()) ++n;
            const std::string copyOf = text_;
            if (dirty_) Save();
            loadedName_ = "profile " + std::to_string(n);
            text_ = copyOf;
            profileName.Set(loadedName_);
            Save();
            notify::Push("Info Lines: new profile '" + loadedName_ + "'");
        }
        ImGui::SetItemTooltip("A new profile, starting as a copy of this one");
        ImGui::SameLine();
        if (ImGui::Button("Folder", {-FLT_MIN, 0.f})) {
            std::error_code ec;
            std::filesystem::create_directories(Folder(), ec);
            ShellExecuteW(nullptr, L"open", Folder().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SetItemTooltip("Open memenu\\hud: edit the profiles with any editor, they are read again when saved");

        // The text
        if (ImFont* mono = font::Mono()) ImGui::PushFont(mono);
        const float lineH = ImGui::GetTextLineHeight();
        const bool edited = ImGui::InputTextMultiline("##text", text_.data(), text_.capacity() + 1,
                                                      {width, lineH * static_cast<float>(editorLines) + 8.f},
                                                      ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput,
                                                      &ResizeCallback, &text_);
        if (font::Mono()) ImGui::PopFont();
        if (edited) {
            Reparse();
            dirty_ = true;
            lastEdit_ = ImGui::GetTime();
        }

        ImGui::PushStyleColor(ImGuiCol_Text, tc.textDim);
        if (dirty_) ImGui::TextUnformatted("Saving...");
        else if (fromFile_) ImGui::Text("Saved: hud\\%s.txt", loadedName_.c_str());
        else ImGui::TextUnformatted("The built-in example (saved to a file once you edit it)");
        ImGui::PopStyleColor();
        if (!Enabled()) ImGui::TextColored({1.f, 0.75f, 0.35f, 1.f}, "Off: turn it on to see the lines and the values");

        // Problems, with their line
        std::vector<hud::Error> problems = profile_.Errors();
        problems.insert(problems.end(), renderErrors_.begin(), renderErrors_.end());
        if (problems.empty()) {
            ImGui::TextColored({0.45f, 0.85f, 0.5f, 1.f}, "No problems");
        } else {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            for (std::size_t i = 0; i < problems.size() && i < 10; ++i) {
                const auto& p = problems[i];
                ImGui::TextColored({1.f, 0.45f, 0.4f, 1.f}, "%s%s", p.line ? ("line " + std::to_string(p.line) + ": ").c_str() : "",
                                   p.message.c_str());
            }
            if (problems.size() > 10) ImGui::TextDisabled("... and %zu more", problems.size() - 10);
            ImGui::PopTextWrapPos();
        }

        if (ImGui::Button("Back to the example", {width, 0.f})) {
            text_ = kExample;
            Reparse();
            dirty_ = true;
            lastEdit_ = ImGui::GetTime();
        }
        ImGui::SetItemTooltip("Replace this profile's text with the built-in example");

        DrawValues(width);
        DrawReference();
    }

    void DrawValues(float width)
    {
        if (g_openForTest) ImGui::SetNextItemOpen(true);
        if (!ImGui::TreeNode("Values")) return;
        const Frame frame = MakeFrame();
        ImGui::TextDisabled("Click a name to copy {name}");
        const char* heading = nullptr;
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##values", 2, kFlags, {width - ImGui::GetTreeNodeToLabelSpacing(), 0.f})) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 110.f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            for (const VarDef& v : Vars()) {
                if (!heading || std::string_view(heading) != v.group) {
                    heading = v.group;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(theme::Menu().accent, "%s", heading);
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(v.name)) {
                    ImGui::SetClipboardText((std::string("{") + v.name + "}").c_str());
                    notify::Push(std::string("Copied {") + v.name + "}");
                }
                ImGui::SetItemTooltip("%s", v.help);
                ImGui::TableSetColumnIndex(1);
                std::string text, error;
                hud::FormatValue(v.get(frame), {}, text, error);
                ImGui::TextUnformatted(Enabled() ? text.c_str() : "-");
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }

    static void DrawReference()
    {
        if (g_openForTest) ImGui::SetNextItemOpen(true);
        if (ImGui::TreeNode("Functions")) {
            ImGui::TextUnformatted(
                "if(cond, a, b)        a when cond is true, else b\n"
                "abs floor ceil sqrt sign (x)\n"
                "min(a, b, ...)  max(a, b, ...)  clamp(x, lo, hi)\n"
                "round(x[, decimals])  lerp(a, b, t)\n"
                "time(seconds[, decimals])   1:02.35\n"
                "upper(s) lower(s) len(s)  str(x[, \"spec\"])\n"
                "bar(fraction[, width[, color]])\n"
                "color(\"red\")  rgb(r, g, b[, a])  mix(c1, c2, t)\n"
                "on(\"Module name\")     that module is on");
            ImGui::TreePop();
        }
        if (g_openForTest) ImGui::SetNextItemOpen(true);
        if (ImGui::TreeNode("Formatting")) {
            ImGui::TextUnformatted(
                "{value:spec} takes fmt's format specs:\n"
                "  {speed:.1f}   one decimal       {x:>9.2f}  right in 9\n"
                "  {health:03d}  zero padded       {map:^12}  centered\n"
                "  {speed:+.0f}  with a sign       {n:x}      hex\n"
                "Operators: + - * / %  < <= > >= == !=  && || !\n"
                "Colors: &(name) &#rrggbb &#rrggbbaa  &r resets\n"
                "&> starts the right column, && is a literal &\n"
                "{{ and }} are literal braces");
            std::string names;
            for (const std::string& n : hud::ColorNames()) names += n + " ";
            ImGui::TextWrapped("Color names: %saccent text dim", names.c_str());
            ImGui::TreePop();
        }
    }

    std::mutex gameMutex_;
    GameData game_;                     // Written by OnTick
    Peak topSpeed_, topHeight_;         // Game thread

    // Render thread (OnRender and the editor)
    std::string loadedName_;
    std::string text_;
    hud::Profile profile_;
    std::vector<hud::Error> renderErrors_;
    bool fromFile_ = false, dirty_ = false;
    double lastEdit_ = 0.0, lastCheck_ = 0.0;
    std::filesystem::file_time_type mtime_{};

    ctl::Text profileName{this, "Profile", "default"};
    ctl::Separator editorTitle{this, "Lines"};
    ctl::Custom editor{this, [this](float w) { DrawEditor(w); }};
    ctl::Separator lookTitle{this, "Look"};
    ctl::Slider globalScale{this, "Scale", 1.f, 0.5f, 3.f, "%.2fx", 0.1f, "Every section's size (on top of its own scale)"};
    ctl::SliderInt editorLines{this, "Editor height", 16, 6, 40, "%d lines", 2};
    ctl::Checkbox hideInMenu{this, "Hide while the menu is open", false};
    ctl::Checkbox showErrors{this, "Show problems on screen", true,
                             "A red note at the top while the profile has errors (handy when editing it outside)"};
};
MEM_MODULE(InfoLines)

} // namespace

info_lines::Status info_lines::LastStatus()
{
    std::lock_guard lock(g_shareMutex);
    return g_status;
}

std::string info_lines::Evaluate(const std::string& line, std::vector<hud::Error>* errors)
{
    Frame frame;
    {
        std::lock_guard lock(g_shareMutex);
        frame = g_lastFrame;
    }
    FrameContext ctx(frame);
    return hud::PlainText(hud::RenderLine(line, ctx, 0xFFFFFFFF, errors));
}

void info_lines::SetEditorOpenForTest(bool open) { g_openForTest = open; }
unsigned long long info_lines::EditorFrames() { return g_editorFrames.load(); }
