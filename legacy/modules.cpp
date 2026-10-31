#include "modules.h"

#include <Windows.h>

namespace
{
    std::vector<std::unique_ptr<Tab>> g_tabs;
}

bool Module::KeyDown(std::string_view n)
{
    const int vk = GetKey(n);
    return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

std::vector<std::unique_ptr<Tab>>& modules::Tabs() { return g_tabs; }

Tab& modules::AddTab(std::string name)
{
    for (auto& t : g_tabs)
        if (t->name == name) return *t;
    auto t = std::make_unique<Tab>();
    t->name = std::move(name);
    g_tabs.push_back(std::move(t));
    return *g_tabs.back();
}

Module* modules::Find(std::string_view name)
{
    Module* found = nullptr;
    ForEachModule([&](Tab&, Module& m) { if (!found && m.name == name) found = &m; });
    return found;
}

void modules::ApplyDefaults()
{
    ForEachModule([](Tab&, Module& m) { if (m.enabledByDefault) m.SetEnabled(true); });
}

void modules::Tick()
{
    ForEachModule([](Tab&, Module& m) { if (m.enabled && m.onTick) m.onTick(m); });
}

void modules::Render()
{
    ForEachModule([](Tab&, Module& m) { if (m.enabled && m.onRender) m.onRender(m); });
}

void modules::OnKey(int vk)
{
    ForEachModule([vk](Tab&, Module& m)
    {
        if (m.key == vk) m.Toggle();
        if (m.enabled || m.isAction)
        {
            for (auto& s : m.settings)
                if (s.type == Setting::Type::Key && s.i == vk && s.onClick) s.onClick(m);
        }
    });
}

void modules::DisableAll()
{
    ForEachModule([](Tab&, Module& m) { m.SetEnabled(false); });
}
