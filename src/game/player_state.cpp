#include "game/player_state.h"
#include "game/flight.h"
#include "game/game.h"
#include "game/object_ref.h"
#include "game/uobject.h"

#include <Windows.h>

#include <ME_SDK.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace Classes;

// ---- UProperty layout ------------------------------------------------------------------------------------------------
// The SDK only has UProperty as 0x40 unknown bytes after UField (0x44..0x84). Offset, ElementSize and ArrayDim are
// somewhere in there; BoolProperty's BitMask and StructProperty's Struct follow right after it (both classes are
// exactly 4 bytes bigger than UProperty in the SDK).
constexpr int kPropertyStart = 0x44;
constexpr int kPropertyEnd   = 0x84;
constexpr int kSubclassField = 0x84;

struct Layout {
    bool tried = false;
    bool ok = false;
    int offset = 0, elementSize = 0, arrayDim = 0;
};
Layout g_layout;

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::uint32_t Dword(const void* p, int at)
{
    std::uint32_t v;
    std::memcpy(&v, static_cast<const std::uint8_t*>(p) + at, sizeof v);
    return v;
}

template <class T>
T* Read(const void* p, int at)
{
    return reinterpret_cast<T*>(static_cast<std::uintptr_t>(Dword(p, at)));
}

UProperty* FindProperty(UClass* cls, const char* name)
{
    for (UStruct* s = cls; s; s = game::SuperOf(s))
        for (UField* f = game::ChildrenOf(s); f; f = game::NextOf(f))
            if (game::IsA(f, UProperty::StaticClass()) && f->GetName() == name) return static_cast<UProperty*>(f);
    return nullptr;
}

struct Known {
    UObject* object;
    const char* name;
    const void* field;          // the SDK's member: tells the real offset
    std::uint32_t size;
};

// Finds the slots of Offset/ElementSize/ArrayDim by looking at properties whose offset and size the SDK already knows.
bool Calibrate(ATdPlayerPawn* pawn, ATdPlayerController* controller)
{
    const Known known[] = {
        {pawn, "Location", &pawn->Location, sizeof pawn->Location},
        {pawn, "Velocity", &pawn->Velocity, sizeof pawn->Velocity},
        {pawn, "Health", &pawn->Health, sizeof pawn->Health},
        {pawn, "EnterFallingHeight", &pawn->EnterFallingHeight, sizeof pawn->EnterFallingHeight},
        {controller, "ReactionTimeEnergy", &controller->ReactionTimeEnergy, sizeof controller->ReactionTimeEnergy},
        {controller, "Rotation", &controller->Rotation, sizeof controller->Rotation},
    };

    UProperty* props[std::size(known)];
    std::uint32_t offsets[std::size(known)];
    for (std::size_t i = 0; i < std::size(known); ++i) {
        props[i] = FindProperty(known[i].object->Class, known[i].name);
        if (!props[i]) {
            spdlog::error("Save state: property {} not found", known[i].name);
            return false;
        }
        offsets[i] = static_cast<std::uint32_t>(static_cast<const std::uint8_t*>(known[i].field) -
                                                reinterpret_cast<const std::uint8_t*>(known[i].object));
    }

    auto slotWhere = [&](auto&& matches, int skip) {
        for (int at = kPropertyStart; at < kPropertyEnd; at += 4) {
            if (at == skip) continue;
            bool all = true;
            for (std::size_t i = 0; i < std::size(known) && all; ++i) all = matches(i, Dword(props[i], at));
            if (all) return at;
        }
        return -1;
    };

    const int offset = slotWhere([&](std::size_t i, std::uint32_t v) { return v == offsets[i]; }, -1);
    const int size = slotWhere([&](std::size_t i, std::uint32_t v) { return v == known[i].size; }, offset);
    // UE3 keeps ArrayDim right before ElementSize; all the known properties are single values.
    const int dim = size - 4;
    bool dimOk = offset >= 0 && size >= 0 && dim >= kPropertyStart;
    for (std::size_t i = 0; i < std::size(known) && dimOk; ++i) dimOk = Dword(props[i], dim) == 1;
    if (!dimOk) {
        spdlog::error("Save state: could not work out the UProperty layout (offset slot {:#x}, size slot {:#x})", offset,
                      size);
        return false;
    }

    g_layout.offset = offset;
    g_layout.elementSize = size;
    g_layout.arrayDim = dim;
    spdlog::info("Save state: UProperty Offset at {:#x}, ElementSize at {:#x}, ArrayDim at {:#x}", offset, size, dim);
    return true;
}

bool EnsureLayout(ATdPlayerPawn* pawn, ATdPlayerController* controller)
{
    if (!g_layout.tried) {
        g_layout.tried = true;
        g_layout.ok = Calibrate(pawn, controller);
    }
    return g_layout.ok;
}

// ---- per-class copy plan -------------------------------------------------------------------------------------------
struct Field {
    std::uint32_t offset;
    std::uint32_t size;
    std::uint32_t mask;         // bool properties: the bit inside the dword, 0 otherwise
};

std::unordered_map<UClass*, std::vector<Field>> g_plans;

bool IsCopiedStruct(UProperty* prop)
{
    auto* st = Read<UObject>(prop, kSubclassField);
    if (!st || !game::IsA(st, UScriptStruct::StaticClass())) return false;
    const auto name = st->GetName();
    return name == "Vector" || name == "Rotator";           // the only structs the trainer saves
}

// Values the menu itself sets while it runs (input blocking while it is open, God Mode, No Clip). A state saved while
// they were set would bring them back on load, e.g. a camera that no longer turns after the menu was closed, or a
// player without collision. (Physics is saved, it is part of the move; Load fixes the one No Clip/Flight leave.)
bool IsOwnedByMenu(const std::string& name)
{
    return name == "bIgnoreLookInput" || name == "bIgnoreMoveInput" || name == "bIgnoreButtonInput" ||
           name == "bGodMode" || name == "bCollideWorld";
}

const std::vector<Field>& PlanFor(UClass* cls)
{
    if (auto it = g_plans.find(cls); it != g_plans.end()) return it->second;

    std::vector<Field> plan;
    const std::uint32_t classSize = game::PropertySizeOf(cls);
    for (UStruct* s = cls; s; s = game::SuperOf(s)) {
        for (UField* f = game::ChildrenOf(s); f; f = game::NextOf(f)) {
            if (!game::IsA(f, UProperty::StaticClass())) continue;
            auto* prop = static_cast<UProperty*>(f);

            const std::uint32_t offset = Dword(prop, g_layout.offset);
            const std::uint32_t elementSize = Dword(prop, g_layout.elementSize);
            const std::uint32_t dim = Dword(prop, g_layout.arrayDim);
            if (dim < 1 || dim > 256 || elementSize < 1 || elementSize > 64) continue;
            const std::uint32_t size = elementSize * dim;
            if (offset < sizeof(UObject) || (classSize && offset + size > classSize)) continue;
            if (IsOwnedByMenu(prop->GetName())) continue;

            if (game::IsA(prop, UBoolProperty::StaticClass())) {
                const std::uint32_t mask = Dword(prop, kSubclassField);
                if (mask && !(mask & (mask - 1))) plan.push_back({offset, 4, mask});
            } else if (game::IsA(prop, UByteProperty::StaticClass()) || game::IsA(prop, UIntProperty::StaticClass()) ||
                       game::IsA(prop, UFloatProperty::StaticClass()) ||
                       (game::IsA(prop, UStructProperty::StaticClass()) && IsCopiedStruct(prop))) {
                plan.push_back({offset, size, 0});
            }
        }
    }
    spdlog::debug("Save state: {} values in {}", plan.size(), cls->GetName());
    return g_plans.emplace(cls, std::move(plan)).first->second;
}

// ---- snapshots -----------------------------------------------------------------------------------------------------
std::uint32_t PlanBytes(const std::vector<Field>& plan)
{
    std::uint32_t n = 0;
    for (const Field& f : plan) n += f.size;
    return n;
}

// The values of one object. Kept by class name rather than UClass pointer, so a snapshot read back from a file (a later
// game session) can be checked against the object it goes on: same class, same number of bytes.
struct ObjectSnapshot {
    std::string className;
    std::vector<std::uint8_t> data;

    void Take(UObject* object)
    {
        data.clear();
        className.clear();
        if (!object) return;
        className = object->Class->GetName();
        const auto* base = reinterpret_cast<const std::uint8_t*>(object);
        for (const Field& f : PlanFor(object->Class)) {
            std::uint32_t bits;
            const std::uint8_t* src = base + f.offset;
            if (f.mask) {
                bits = Dword(base, static_cast<int>(f.offset)) & f.mask;
                src = reinterpret_cast<const std::uint8_t*>(&bits);
            }
            data.insert(data.end(), src, src + f.size);
        }
    }

    bool Fits(UObject* object) const
    {
        return object && !className.empty() && object->Class->GetName() == className &&
               data.size() == PlanBytes(PlanFor(object->Class));
    }

    void Restore(UObject* object) const
    {
        if (!Fits(object)) return;
        auto* base = reinterpret_cast<std::uint8_t*>(object);
        const std::uint8_t* src = data.data();
        for (const Field& f : PlanFor(object->Class)) {
            if (f.mask) {
                std::uint32_t bits, cur;
                std::memcpy(&bits, src, 4);
                std::memcpy(&cur, base + f.offset, 4);
                cur = (cur & ~f.mask) | (bits & f.mask);
                std::memcpy(base + f.offset, &cur, 4);
            } else {
                std::memcpy(base + f.offset, src, f.size);
            }
            src += f.size;
        }
    }
};

struct Snapshot {
    std::string map;                        // Lower case; a state only loads in the level it was saved in
    FVector location{}, velocity{};
    ObjectSnapshot pawn, controller;
    std::vector<ObjectSnapshot> moves;      // same index as pawn->Moves

    TEnumAsByte<EMovement> movementState;
    TEnumAsByte<ESoundMode> soundMode;
    bool reactionTime = false;
    float timeDilation = 1.f;

    // Values on the pawn's components (not part of the pawn object itself)
    float collisionHeight = 0.f, collisionRadius = 0.f;
    TEnumAsByte<ERootMotionMode> rootMotionMode;
    float startTranslateAtDegree = 0.f, quadraticForward = 0.f, quadraticDownward = 0.f;
};

Snapshot g_save;
bool g_hasSave = false;
unsigned g_loadCount = 0;

} // namespace

bool player_state::Save()
{
    auto* pawn = game::Pawn();
    auto* controller = game::Controller();
    auto* world = game::World();
    if (!pawn || !controller || !world) return false;

    if (!EnsureLayout(pawn, controller)) return false;

    Snapshot& s = g_save;
    s.map = Lower(game::MapName());
    s.location = pawn->Location;
    s.velocity = pawn->Velocity;
    s.pawn.Take(pawn);
    s.controller.Take(controller);
    s.moves.resize(pawn->Moves.Num());
    for (std::size_t i = 0; i < pawn->Moves.Num(); ++i) s.moves[i].Take(pawn->Moves[i]);

    s.movementState = pawn->MovementState;
    s.soundMode = controller->CurrentSoundMode;
    s.reactionTime = controller->bReactionTime;
    s.timeDilation = world->TimeDilation;

    if (auto* cyl = pawn->CylinderComponent) {
        s.collisionHeight = cyl->CollisionHeight;
        s.collisionRadius = cyl->CollisionRadius;
    }
    if (pawn->Mesh) s.rootMotionMode = pawn->Mesh->RootMotionMode;
    if (auto* neck = pawn->SwanNeck1p) {
        s.startTranslateAtDegree = neck->StartTranslateAtDegree;
        s.quadraticForward = neck->QuadraticForwardTranslation;
        s.quadraticDownward = neck->QuadraticDownwardTranslation;
    }

    g_hasSave = true;
    spdlog::info("Save state: saved at ({:.0f}, {:.0f}, {:.0f})", pawn->Location.X, pawn->Location.Y, pawn->Location.Z);
    return true;
}

bool player_state::Load()
{
    auto* pawn = game::Pawn();
    auto* controller = game::Controller();
    auto* world = game::World();
    if (!g_hasSave || !pawn || !controller || !world) {
        spdlog::info("Save state: not loaded ({})", !g_hasSave ? "nothing saved" : "no player");
        return false;
    }
    if (pawn->bCutsceneIsSkippable) {                        // in a cutscene
        spdlog::info("Save state: not loaded (a cutscene is playing)");
        return false;
    }

    const Snapshot& s = g_save;
    if (!s.map.empty() && s.map != Lower(game::MapName())) {
        spdlog::info("Save state: not loaded (saved in {}, this is {})", s.map, game::MapName());
        return false;
    }
    // Checked before anything changes: a state read from a file may come from another game version
    if (!EnsureLayout(pawn, controller) || !s.pawn.Fits(pawn) || !s.controller.Fits(controller)) {
        spdlog::warn("Save state: not loaded (it does not fit this player: {} / {})", s.pawn.className,
                     s.controller.className);
        return false;
    }

    // Back into the walking states first, like the trainer. Without naming them, SetInitialState sends the controller
    // to its default state, which ignores movement and jump input: the player couldn't jump and kept the restored
    // acceleration, as if the saved velocity were forced on them.
    pawn->InitialState = game::Name("Walking");
    pawn->SetInitialState();
    controller->InitialState = game::Name("PlayerWalking");
    controller->SetInitialState();

    if (auto* hud = static_cast<ATdHUD*>(controller->myHUD)) {
        if (auto* fx = hud->EffectManager; fx && fx->ReactionTimeEffect && fx->ReactionTimeEffect->bIsActive)
            fx->ReactionTimeEffect->ResetPP();
        hud->ToggleReactionTime(s.reactionTime);
    }
    controller->SetSoundMode(s.soundMode, 0.f, true, 0.f);
    if (world->Game) world->Game->GameSpeed = s.timeDilation;

    if (pawn->LeftHandWorldIKController) pawn->LeftHandWorldIKController->StrengthTarget = 0.f;
    if (pawn->RightHandWorldIKController) pawn->RightHandWorldIKController->StrengthTarget = 0.f;

    pawn->StopSlideEffect();
    pawn->StopAllCustomAnimations(0.f);
    pawn->SetMove(s.movementState, false, false);

    s.pawn.Restore(pawn);
    s.controller.Restore(controller);
    for (std::size_t i = 0; i < pawn->Moves.Num() && i < s.moves.size(); ++i) s.moves[i].Restore(pawn->Moves[i]);

    // What the trainer clears after its copy: a state saved in a cutscene or a scripted move (look constraint,
    // cinematic input locks) would otherwise keep the player locked after the load. Jump input is the live keyboard's,
    // not the saved one: a jump held while saving must not count as held now.
    pawn->bConstrainLook = false;
    controller->bIgnoreMovementFocus = false;
    controller->bCinemaDisableInputMove = false;
    controller->bCinemaDisableInputLook = false;
    controller->bCinematicMode = false;
    controller->bPressedJump = false;
    controller->bReleasedJump = true;

    if (auto* cyl = pawn->CylinderComponent) {
        cyl->CollisionHeight = s.collisionHeight;
        cyl->CollisionRadius = s.collisionRadius;
    }
    if (pawn->Mesh) pawn->Mesh->RootMotionMode = s.rootMotionMode;
    if (auto* neck = pawn->SwanNeck1p) {
        neck->StartTranslateAtDegree = s.startTranslateAtDegree;
        neck->QuadraticForwardTranslation = s.quadraticForward;
        neck->QuadraticDownwardTranslation = s.quadraticDownward;
    }

    // Saved during Flight/No Clip (no physics): outside them that would leave the player hanging in the air
    if (!flight::Active() && pawn->Physics == EPhysics::PHYS_None) pawn->Physics = EPhysics::PHYS_Falling;

    ++g_loadCount;
    spdlog::info("Save state: loaded, player at ({:.0f}, {:.0f}, {:.0f})", pawn->Location.X, pawn->Location.Y,
                 pawn->Location.Z);
    return true;
}

namespace {

std::string ToHex(const std::vector<std::uint8_t>& bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

bool FromHex(const std::string& hex, std::vector<std::uint8_t>& out)
{
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() % 2) return false;
    out.resize(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = digit(hex[2 * i]), lo = digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return true;
}

nlohmann::json ToJson(const ObjectSnapshot& o) { return {{"class", o.className}, {"data", ToHex(o.data)}}; }

bool FromJson(const nlohmann::json& j, ObjectSnapshot& o)
{
    o.className = j.at("class").get<std::string>();
    return FromHex(j.at("data").get<std::string>(), o.data);
}

nlohmann::json Vec(const FVector& v) { return {v.X, v.Y, v.Z}; }

FVector Vec(const nlohmann::json& j)
{
    FVector v{};
    v.X = j.at(0).get<float>();
    v.Y = j.at(1).get<float>();
    v.Z = j.at(2).get<float>();
    return v;
}

template <class E>
TEnumAsByte<E> Enum(const nlohmann::json& j)
{
    return TEnumAsByte<E>(static_cast<std::uint8_t>(j.get<int>()));
}

constexpr int kFormat = 1;

} // namespace

nlohmann::json player_state::Export()
{
    if (!g_hasSave) return nullptr;
    const Snapshot& s = g_save;
    nlohmann::json moves = nlohmann::json::array();
    for (const ObjectSnapshot& m : s.moves) moves.push_back(ToJson(m));
    return {
        {"format", kFormat},
        {"map", s.map},
        {"location", Vec(s.location)},
        {"velocity", Vec(s.velocity)},
        {"movementState", static_cast<int>(s.movementState.GetValue())},
        {"soundMode", static_cast<int>(s.soundMode.GetValue())},
        {"reactionTime", s.reactionTime},
        {"timeDilation", s.timeDilation},
        {"collision", {s.collisionHeight, s.collisionRadius}},
        {"rootMotionMode", static_cast<int>(s.rootMotionMode.GetValue())},
        {"neck", {s.startTranslateAtDegree, s.quadraticForward, s.quadraticDownward}},
        {"pawn", ToJson(s.pawn)},
        {"controller", ToJson(s.controller)},
        {"moves", std::move(moves)},
    };
}

bool player_state::Import(const nlohmann::json& j)
{
    try {
        if (!j.is_object() || j.value("format", 0) != kFormat) return false;
        Snapshot s;
        s.map = Lower(j.at("map").get<std::string>());
        s.location = Vec(j.at("location"));
        s.velocity = Vec(j.at("velocity"));
        if (!FromJson(j.at("pawn"), s.pawn) || !FromJson(j.at("controller"), s.controller)) return false;
        for (const auto& m : j.at("moves"))
            if (!FromJson(m, s.moves.emplace_back())) return false;
        s.movementState = Enum<EMovement>(j.at("movementState"));
        s.soundMode = Enum<ESoundMode>(j.at("soundMode"));
        s.reactionTime = j.at("reactionTime").get<bool>();
        s.timeDilation = j.at("timeDilation").get<float>();
        s.collisionHeight = j.at("collision").at(0).get<float>();
        s.collisionRadius = j.at("collision").at(1).get<float>();
        s.rootMotionMode = Enum<ERootMotionMode>(j.at("rootMotionMode"));
        s.startTranslateAtDegree = j.at("neck").at(0).get<float>();
        s.quadraticForward = j.at("neck").at(1).get<float>();
        s.quadraticDownward = j.at("neck").at(2).get<float>();
        g_save = std::move(s);
        g_hasSave = true;
        return true;
    } catch (const nlohmann::json::exception& e) {
        spdlog::warn("Save state: can't read a stored state: {}", e.what());
        return false;
    }
}

bool player_state::HasSave() { return g_hasSave; }

void player_state::Clear() { g_hasSave = false; }

unsigned player_state::LoadCount() { return g_loadCount; }

void player_state::ResetMovement(ATdPlayerPawn* pawn, ATdPlayerController* controller)
{
    const auto state = pawn->MovementState.GetValue();

    pawn->InitialState = game::Name("Walking");
    pawn->SetInitialState();
    controller->InitialState = game::Name("PlayerWalking");
    controller->SetInitialState();

    pawn->StopSlideEffect();
    pawn->StopAllCustomAnimations(0.f);
    pawn->SetMove(EMovement::MOVE_Walking, false, false);

    controller->bIgnoreLookInput = 0;
    controller->bIgnoreMoveInput = 0;
    controller->bIgnoreButtonInput = 0;

    if (state == EMovement::MOVE_RumpSlide) {
        pawn->WalkableFloorZ = 0.71f;
        pawn->GravityModifier = 1.f;
        pawn->StreakEffectOverride = 0;
    }
}
