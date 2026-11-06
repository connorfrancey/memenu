#include "core/bindings.h"
#include "core/keys.h"
#include "core/module.h"

#include <unordered_map>

std::string bindings::Slot::Describe() const { return module->Name() + ": " + what; }

namespace {

// Modules and their controls are made once: the slot list only changes when they do (tests make their own modules).
std::vector<bindings::Slot> g_slots;
std::size_t g_slotsFor = static_cast<std::size_t>(-1);

// Which slots each bound key is on, rebuilt when a binding changes (every bind button asks every frame).
std::unordered_map<int, std::vector<std::size_t>> g_byKey;
std::uint64_t g_byKeyFor = 0;
bool g_byKeyValid = false;

std::uint64_t Fingerprint(const std::vector<bindings::Slot>& slots)
{
    std::uint64_t h = 1469598103934665603ull ^ slots.size();
    for (const auto& s : slots) h = (h ^ static_cast<std::uint32_t>(*s.binding)) * 1099511628211ull;
    return h;
}

const std::unordered_map<int, std::vector<std::size_t>>& ByKey()
{
    const auto& slots = bindings::All();
    const std::uint64_t fp = Fingerprint(slots);
    if (g_byKeyValid && fp == g_byKeyFor) return g_byKey;
    g_byKey.clear();
    for (std::size_t i = 0; i < slots.size(); ++i)
        if (*slots[i].binding) g_byKey[*slots[i].binding].push_back(i);
    g_byKeyFor = fp;
    g_byKeyValid = true;
    return g_byKey;
}

} // namespace

const std::vector<bindings::Slot>& bindings::All()
{
    std::size_t signature = 0;
    modules::ForEach([&](modules::Tab&, Module& m) {
        signature = signature * 31 + reinterpret_cast<std::size_t>(&m) + m.Settings().size();
    });
    if (signature == g_slotsFor) return g_slots;

    g_slots.clear();
    modules::ForEach([&](modules::Tab&, Module& m) {
        g_slots.push_back({Kind::Toggle, &m, nullptr, &m.key, m.IsAction() ? "Hotkey" : "Toggle"});
        for (ctl::Setting* s : m.Settings()) {
            if (auto* hold = dynamic_cast<ctl::KeyHold*>(s)) {
                g_slots.push_back({Kind::Key, &m, s, &hold->Binding(), s->Name()});
                continue;
            }
            for (int slot = 0; slot < 2; ++slot)
                if (const char* label = s->BindLabel(slot))
                    g_slots.push_back({Kind::Control, &m, s, slot == 0 ? &s->key : &s->keyAlt, s->Name() + ": " + label});
        }
    });
    g_slotsFor = signature;
    g_byKeyValid = false;
    return g_slots;
}

bool bindings::Clash(int a, int b) { return a != 0 && a == b; }

std::vector<std::string> bindings::ClashesWith(int binding, const int* self)
{
    std::vector<std::string> out;
    if (binding == 0) return out;
    const auto& index = ByKey();
    const auto it = index.find(binding);
    if (it == index.end()) return out;
    const auto& slots = All();
    for (std::size_t i : it->second)
        if (slots[i].binding != self) out.push_back(slots[i].Describe());
    return out;
}

int bindings::ClashCount()
{
    int n = 0;
    for (const auto& [key, at] : ByKey())
        if (at.size() > 1) n += static_cast<int>(at.size());
    return n;
}
