#include "game/player_state.h"
#include "game/game.h"
#include "game/object_ref.h"

#include <Windows.h>

#include <mirrorsedge_sdk.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstring>
#include <iterator>
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
    for (auto* s = static_cast<UStruct*>(cls); s; s = static_cast<UStruct*>(s->SuperField))
        for (UField* f = s->Children; f; f = f->Next)
            if (f->IsA(UProperty::StaticClass()) && f->GetName() == name) return static_cast<UProperty*>(f);
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
    if (!st || !st->IsA(UScriptStruct::StaticClass())) return false;
    const auto name = st->GetName();
    return name == "Vector" || name == "Rotator";           // the only structs the trainer saves
}

const std::vector<Field>& PlanFor(UClass* cls)
{
    if (auto it = g_plans.find(cls); it != g_plans.end()) return it->second;

    std::vector<Field> plan;
    const std::uint32_t classSize = static_cast<UStruct*>(cls)->PropertySize;
    for (auto* s = static_cast<UStruct*>(cls); s; s = static_cast<UStruct*>(s->SuperField)) {
        for (UField* f = s->Children; f; f = f->Next) {
            if (!f->IsA(UProperty::StaticClass())) continue;
            auto* prop = static_cast<UProperty*>(f);

            const std::uint32_t offset = Dword(prop, g_layout.offset);
            const std::uint32_t elementSize = Dword(prop, g_layout.elementSize);
            const std::uint32_t dim = Dword(prop, g_layout.arrayDim);
            if (dim < 1 || dim > 256 || elementSize < 1 || elementSize > 64) continue;
            const std::uint32_t size = elementSize * dim;
            if (offset < sizeof(UObject) || (classSize && offset + size > classSize)) continue;

            if (prop->IsA(UBoolProperty::StaticClass())) {
                const std::uint32_t mask = Dword(prop, kSubclassField);
                if (mask && !(mask & (mask - 1))) plan.push_back({offset, 4, mask});
            } else if (prop->IsA(UByteProperty::StaticClass()) || prop->IsA(UIntProperty::StaticClass()) ||
                       prop->IsA(UFloatProperty::StaticClass()) ||
                       (prop->IsA(UStructProperty::StaticClass()) && IsCopiedStruct(prop))) {
                plan.push_back({offset, size, 0});
            }
        }
    }
    spdlog::debug("Save state: {} values in {}", plan.size(), cls->GetName());
    return g_plans.emplace(cls, std::move(plan)).first->second;
}

// ---- snapshots -----------------------------------------------------------------------------------------------------
struct ObjectSnapshot {
    UClass* cls = nullptr;
    std::vector<std::uint8_t> data;

    void Take(UObject* object)
    {
        data.clear();
        cls = object ? object->Class : nullptr;
        if (!object) return;
        const auto* base = reinterpret_cast<const std::uint8_t*>(object);
        for (const Field& f : PlanFor(cls)) {
            std::uint32_t bits;
            const std::uint8_t* src = base + f.offset;
            if (f.mask) {
                bits = Dword(base, static_cast<int>(f.offset)) & f.mask;
                src = reinterpret_cast<const std::uint8_t*>(&bits);
            }
            data.insert(data.end(), src, src + f.size);
        }
    }

    void Restore(UObject* object) const
    {
        if (!object || object->Class != cls) return;
        auto* base = reinterpret_cast<std::uint8_t*>(object);
        const std::uint8_t* src = data.data();
        for (const Field& f : PlanFor(cls)) {
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

    if (!g_layout.tried) {
        g_layout.tried = true;
        g_layout.ok = Calibrate(pawn, controller);
    }
    if (!g_layout.ok) return false;

    Snapshot& s = g_save;
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
    return true;
}

bool player_state::Load()
{
    auto* pawn = game::Pawn();
    auto* controller = game::Controller();
    auto* world = game::World();
    if (!g_hasSave || !pawn || !controller || !world) return false;
    if (pawn->bCutsceneIsSkippable) return false;            // in a cutscene

    const Snapshot& s = g_save;

    pawn->SetInitialState();
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

    ++g_loadCount;
    return true;
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
