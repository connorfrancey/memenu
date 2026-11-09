#include "exec_commands.h"

#include "game/uobject.h"

#include <ME_SDK.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace {

constexpr std::uint32_t kExec = 0x200, kNative = 0x400, kEvent = 0x800;
// After FunctionFlags: WORD iNative, WORD RepOffset, FName FriendlyName (8), BYTE OperPrecedence, then BYTE NumParms
constexpr int kNumParmsAfterFlags = 4 + 2 + 2 + 8 + 1;

template <class T>
T Read(const void* object, int offset)
{
    T v;
    std::memcpy(&v, static_cast<const std::uint8_t*>(object) + offset, sizeof v);
    return v;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "FloatProperty" -> "float"
std::string TypeName(const Classes::UObject* property)
{
    std::string t = property && property->Class ? property->Class->GetName() : "?";
    if (t.size() > 8 && t.compare(t.size() - 8, 8, "Property") == 0) t.resize(t.size() - 8);
    t = Lower(t);
    return t == "str" ? "string" : t;
}

std::vector<game::ExecCommand> Scan()
{
    std::vector<game::ExecCommand> out;
    Classes::UClass* fnClass = Classes::UFunction::StaticClass();
    if (!fnClass) return out;

    struct Fn { Classes::UObject* fn; std::string name, owner; };
    std::vector<Fn> fns;
    std::unordered_map<std::string, Classes::UObject*> anchors;   // "Owner.Name" of the calibration functions
    const auto& objects = Classes::UObject::GetGlobalObjects();
    for (std::size_t i = 0; i < objects.Num(); ++i) {
        Classes::UObject* o = objects.GetByIndex(i);
        if (!o || o->Class != fnClass || !o->Outer) continue;
        Fn f{o, o->GetName(), o->Outer->GetName()};
        // Lower case: the engine spells them its own way (SloMo)
        const std::string key = Lower(f.owner + "." + f.name);
        if (key == "cheatmanager.slomo" || key == "cheatmanager.god" || key == "object.sin" || key == "actor.tick")
            anchors.emplace(key, o);
        fns.push_back(std::move(f));
    }
    if (anchors.size() < 4) {
        spdlog::warn("Console: the functions to check the flag layout against weren't found ({} of 4)", anchors.size());
        return out;
    }

    // FunctionFlags: the first offset after UStruct where all four functions have the flags they are known to have
    const auto flagsOk = [&](int at) {
        const auto f = [&](const char* k) { return Read<std::uint32_t>(anchors[k], at); };
        return (f("cheatmanager.slomo") & kExec) && (f("cheatmanager.god") & kExec) && !(f("object.sin") & kExec) &&
               (f("object.sin") & kNative) && !(f("actor.tick") & kExec) && (f("actor.tick") & kEvent);
    };
    int flagsAt = -1;
    for (int at = 0x54; at <= 0xA0 && flagsAt < 0; at += 4)
        if (flagsOk(at)) flagsAt = at;
    if (flagsAt < 0) {
        spdlog::warn("Console: UFunction::FunctionFlags not found: the game's exec commands won't be listed");
        return out;
    }
    const int parmsAt = flagsAt + kNumParmsAfterFlags;
    const auto parms = [&](const char* k) { return Read<std::uint8_t>(anchors[k], parmsAt); };
    const bool parmsOk = parms("cheatmanager.slomo") == 1 && parms("cheatmanager.god") == 0 && parms("object.sin") == 2;

    std::unordered_map<std::string, std::size_t> seen;   // Lower-case name -> index in out (overrides list once)
    for (const Fn& f : fns) {
        if (!(Read<std::uint32_t>(f.fn, flagsAt) & kExec)) continue;
        if (seen.count(Lower(f.name))) continue;
        game::ExecCommand c{f.name, f.owner, {}};
        if (parmsOk) {
            int left = Read<std::uint8_t>(f.fn, parmsAt);
            for (Classes::UField* p = game::ChildrenOf(reinterpret_cast<Classes::UStruct*>(f.fn)); p && left > 0;
                 p = game::NextOf(p), --left) {
                const std::string name = p->GetName();
                if (name == "ReturnValue") continue;
                if (!c.params.empty()) c.params += ", ";
                c.params += TypeName(p) + " " + name;
            }
        }
        seen.emplace(Lower(f.name), out.size());
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return Lower(a.name) < Lower(b.name); });
    spdlog::info("Console: {} exec commands (FunctionFlags at 0x{:X}, parameters {})", out.size(), flagsAt,
                 parmsOk ? "checked" : "not shown: their layout didn't check out");
    // A few signatures, to see the parameters come out right (names and order)
    std::string examples;
    int shown = 0;
    for (const auto& c : out)
        if ((Lower(c.name) == "slomo" || c.params.find(',') != std::string::npos) && shown++ < 4)
            examples += (examples.empty() ? "" : ", ") + c.owner + "." + c.name + "(" + c.params + ")";
    if (!examples.empty()) spdlog::info("Console: for example {}", examples);
    return out;
}

} // namespace

const std::vector<game::ExecCommand>& game::ExecCommands()
{
    static const std::vector<ExecCommand> commands = Scan();
    return commands;
}
