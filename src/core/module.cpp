#include "core/module.h"
#include "core/clock.h"
#include "core/profile.h"
#include "core/keys.h"
#include "core/notify.h"
#include "core/scheduler.h"

#include <algorithm>
#include <set>
#include <functional>
#include <atomic>
#include <mutex>
#include <spdlog/spdlog.h>

// ---- Module --------------------------------------------------------------------------------------------------------------
Module::Module(std::string name, std::string tab, std::string description, int order)
    : name_(std::move(name)), tab_(std::move(tab)), description_(std::move(description)), order_(order) {}

ctl::Setting* Module::FindSetting(std::string_view name) const
{
    for (ctl::Setting* s : settings_)
        if (s->Name() == name) return s;
    return nullptr;
}

void Module::SetEnabled(bool on)
{
    if (IsAction() || on == enabled_) return;

    if (on && !group.empty()) {
        // Only one module of a group can be on at a time (they would fight over the same thing).
        modules::ForEach([this](modules::Tab&, Module& other) {
            if (&other != this && other.Enabled() && other.group == group) other.SetEnabled(false);
        });
    }

    enabled_ = on;
    if (on) OnEnable(); else OnDisable();
}

void Module::Toggle()
{
    if (IsAction()) OnTrigger();
    else            SetEnabled(!enabled_);
}

// ---- registry ------------------------------------------------------------------------------------------------------------
namespace {

struct TabDecl {
    std::string name;
    int order;
    float width;
};

// Function-local statics: modules register from static initializers in many files, in no particular order.
std::vector<modules::Factory>& Factories() { static std::vector<modules::Factory> v; return v; }
std::vector<TabDecl>& TabDecls() { static std::vector<TabDecl> v; return v; }

// The modules. Once the menu runs, the list only grows (a mod's modules join at its load phase) and is read from every
// thread: its capacity is reserved up front so it never moves, and readers see the first g_count entries, published
// after each new entry is in place.
constexpr std::size_t kCapacity = 1024;
std::vector<std::unique_ptr<Module>> g_modules;
std::atomic<std::size_t> g_count{0};
std::vector<std::unique_ptr<modules::Tab>> g_tabs;     // Changed only on the render thread once started

bool g_started = false;
std::function<void(std::function<void()>)> g_runOnRender;
std::function<void(Module&)> g_onAdded;
std::mutex g_pendingMutex;
std::set<std::string> g_pendingNames;                  // Published, not in the list yet

template <class F>
void EachModule(F&& f)
{
    const std::size_t n = g_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) f(*g_modules[i]);
}

void InsertInTab(Module* m)
{
    auto it = std::find_if(g_tabs.begin(), g_tabs.end(), [&](const auto& t) { return t->name == m->Tab(); });
    if (it == g_tabs.end()) {
        auto tab = std::make_unique<modules::Tab>();
        tab->name = m->Tab();
        g_tabs.push_back(std::move(tab));
        it = g_tabs.end() - 1;
    }
    auto& list = (*it)->modules;
    const auto at = std::find_if(list.begin(), list.end(), [&](const Module* o) {
        return o->Order() != m->Order() ? m->Order() < o->Order() : m->Name() < o->Name();
    });
    list.insert(at, m);
}

bool Append(std::unique_ptr<Module> module)
{
    if (g_modules.size() >= g_modules.capacity()) {
        spdlog::error("Too many modules ({}): {} is left out", g_modules.size(), module->Name());
        return false;
    }
    g_modules.push_back(std::move(module));
    g_count.store(g_modules.size(), std::memory_order_release);
    return true;
}

} // namespace

void modules::Register(Factory factory) { Factories().push_back(factory); }

modules::TabRegistrar::TabRegistrar(std::string name, int order, float width)
{
    TabDecls().push_back({std::move(name), order, width});
}

std::vector<std::unique_ptr<modules::Tab>>& modules::Tabs() { return g_tabs; }

void modules::Init()
{
    if (!g_modules.empty()) return;

    g_modules.reserve(std::max(kCapacity, Factories().size() * 2));
    for (Factory make : Factories()) g_modules.push_back(make());
    g_count.store(g_modules.size(), std::memory_order_release);

    // Tabs: the declared ones first (by their order), then any tab only modules mention, in the order they first appear.
    std::vector<TabDecl> decls = TabDecls();
    std::stable_sort(decls.begin(), decls.end(), [](const TabDecl& a, const TabDecl& b) { return a.order < b.order; });
    for (const TabDecl& d : decls) {
        auto tab = std::make_unique<Tab>();
        tab->name = d.name;
        tab->order = d.order;
        tab->width = d.width;
        g_tabs.push_back(std::move(tab));
    }

    // Registration order depends on the linker, so sort the modules first: tab, then order, then name.
    std::vector<Module*> sorted;
    for (auto& m : g_modules) sorted.push_back(m.get());
    std::sort(sorted.begin(), sorted.end(), [](const Module* a, const Module* b) {
        if (a->Order() != b->Order()) return a->Order() < b->Order();
        return a->Name() < b->Name();
    });
    for (Module* m : sorted) {
        auto it = std::find_if(g_tabs.begin(), g_tabs.end(), [&](const auto& t) { return t->name == m->Tab(); });
        if (it == g_tabs.end()) {
            auto tab = std::make_unique<Tab>();
            tab->name = m->Tab();
            g_tabs.push_back(std::move(tab));
            it = g_tabs.end() - 1;
        }
        (*it)->modules.push_back(m);
    }
    spdlog::info("{} modules in {} tabs", g_modules.size(), g_tabs.size());
}

Module* modules::Add(std::unique_ptr<Module> module)
{
    if (!module || g_started || NameTaken(module->Name())) return nullptr;     // Names are unique: the config saves by name
    Module* m = module.get();
    if (!Append(std::move(module))) return nullptr;
    InsertInTab(m);
    return m;
}

void modules::Remove(Module* module)
{
    if (g_started) return;
    // Its tab goes too when this was its last module (a tab only a mod's modules used)
    std::erase_if(g_tabs, [&](const auto& t) { return std::erase(t->modules, module) > 0 && t->modules.empty(); });
    std::erase_if(g_modules, [&](const auto& m) { return m.get() == module; });
    g_count.store(g_modules.size(), std::memory_order_release);
}

void modules::Start(std::function<void(std::function<void()>)> runOnRender, std::function<void(Module&)> onAdded)
{
    g_runOnRender = std::move(runOnRender);
    g_onAdded = std::move(onAdded);
    g_started = true;
}

void modules::Publish(std::vector<std::unique_ptr<Module>> list)
{
    if (!g_started) {
        for (auto& m : list) Add(std::move(m));
        return;
    }
    {
        std::lock_guard lock(g_pendingMutex);
        for (auto& m : list) g_pendingNames.insert(m->Name());
    }
    auto holder = std::make_shared<std::vector<std::unique_ptr<Module>>>(std::move(list));
    g_runOnRender([holder] {
        for (auto& owned : *holder) {
            Module* m = owned.get();
            {
                std::lock_guard lock(g_pendingMutex);
                g_pendingNames.erase(m->Name());
            }
            if (!Append(std::move(owned))) continue;
            InsertInTab(m);
            if (g_onAdded) g_onAdded(*m);
        }
    });
}

bool modules::NameTaken(std::string_view name)
{
    if (Find(name)) return true;
    std::lock_guard lock(g_pendingMutex);
    return g_pendingNames.count(std::string(name)) > 0;
}

Module* modules::Find(std::string_view name)
{
    Module* found = nullptr;
    EachModule([&](Module& m) {
        if (!found && m.Name() == name) found = &m;
    });
    return found;
}

void modules::ApplyDefaults()
{
    EachModule([](Module& m) {
        if (m.enabledByDefault) m.SetEnabled(true);
    });
}

void modules::DisableAll()
{
    EachModule([](Module& m) { m.SetEnabled(false); });
}

void modules::Tick() { sched::Tick(); }

void modules::Render()
{
    EachModule([](Module& m) {
        if (!m.Enabled()) return;
        PROF_ZONE_NAMED(m.Name().c_str(), m.Name().size());
        const clk::Ticks t0 = clk::Now();
        m.OnRender();
        sched::RecordRender(m, clk::Micros(clk::Now() - t0));
    });
}

namespace {
std::mutex g_reservedMutex;
std::vector<int> g_reserved;
}

void modules::SetReservedKeys(std::vector<int> keys)
{
    std::lock_guard lock(g_reservedMutex);
    g_reserved = std::move(keys);
}

void modules::OnKey(int vk)
{
    const int mods = keys::HeldMods();
    {
        std::lock_guard lock(g_reservedMutex);
        for (int r : g_reserved)
            if (keys::Fires(r, vk, mods)) return;
    }
    // Only the bindings asking for the most modifiers fire: Ctrl+F5 doesn't also do what F5 does.
    int best = -1;
    const auto consider = [&](int binding) {
        if (keys::Fires(binding, vk, mods)) best = std::max(best, keys::Specificity(binding));
    };
    EachModule([&](Module& m) {
        if (m.Hidden()) return;
        consider(m.key);
        for (ctl::Setting* s : m.Settings()) {
            consider(s->key);
            consider(s->keyAlt);
        }
    });
    if (best < 0) return;
    const auto fires = [&](int binding) { return keys::Fires(binding, vk, mods) && keys::Specificity(binding) == best; };

    EachModule([&](Module& m) {
        if (m.Hidden()) return;
        if (fires(m.key)) {
            m.Toggle();
            if (!m.IsAction()) notify::Push(m.Name() + ": " + (m.Enabled() ? "ON" : "OFF"));
        }
        // Control keys work whether or not the module is on: that is what makes "bind a key to this slider" useful.
        for (ctl::Setting* s : m.Settings()) {
            if (fires(s->key)) s->Press(0);
            if (fires(s->keyAlt)) s->Press(1);
        }
    });
}
