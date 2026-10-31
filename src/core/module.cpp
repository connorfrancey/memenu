#include "core/module.h"
#include "core/clock.h"
#include "core/notify.h"
#include "core/scheduler.h"

#include <algorithm>
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

std::vector<std::unique_ptr<Module>> g_modules;
std::vector<std::unique_ptr<modules::Tab>> g_tabs;

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

    for (Factory make : Factories()) g_modules.push_back(make());

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

Module* modules::Find(std::string_view name)
{
    for (auto& m : g_modules)
        if (m->Name() == name) return m.get();
    return nullptr;
}

void modules::ApplyDefaults()
{
    for (auto& m : g_modules)
        if (m->enabledByDefault) m->SetEnabled(true);
}

void modules::DisableAll()
{
    for (auto& m : g_modules) m->SetEnabled(false);
}

void modules::Tick() { sched::Tick(); }

void modules::Render()
{
    for (auto& m : g_modules) {
        if (!m->Enabled()) continue;
        const clk::Ticks t0 = clk::Now();
        m->OnRender();
        sched::RecordRender(*m, clk::Micros(clk::Now() - t0));
    }
}

void modules::OnKey(int vk)
{
    for (auto& m : g_modules) {
        if (m->key == vk) {
            m->Toggle();
            if (!m->IsAction()) notify::Push(m->Name() + ": " + (m->Enabled() ? "ON" : "OFF"));
        }
        // Control keys work whether or not the module is on: that is what makes "bind a key to this slider" useful.
        for (ctl::Setting* s : m->Settings()) {
            if (s->key == vk) s->Press(0);
            if (s->keyAlt == vk) s->Press(1);
        }
    }
}
