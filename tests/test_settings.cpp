// The controls every module's settings are made of (core/setting.h): values kept in range, saved and loaded back,
// changed by their keys, found by name.
#include "core/module.h"
#include "core/setting.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

namespace {

class Sample final : public Module {
public:
    Sample() : Module("Sample", "Tests", "settings under test") {}

    ctl::Checkbox on{this, "On", true};
    ctl::Slider speed{this, "Speed", 1.f, 0.f, 10.f, "%.1f", 2.f};
    ctl::SliderInt count{this, "Count", 5, 1, 9, "%d", 1};
    ctl::Combo mode{this, "Mode", {"A", "B", "C"}, 1};
    ctl::ColorPick color{this, "Color", {0.1f, 0.2f, 0.3f, 1.f}};
    ctl::Text name{this, "Name", "first"};
    ctl::Button button{this, "Do it", [this] { ++pressed; }};
    ctl::Label label{this, "just text"};
    int pressed = 0;
};

// Every saved setting of a module, by name (what the config file holds for it).
json SaveAll(const Module& m)
{
    json out = json::object();
    for (const ctl::Setting* s : m.Settings())
        if (s->Persistent()) s->Save(out[s->Name()]);
    return out;
}

void LoadAll(Module& m, const json& in)
{
    for (ctl::Setting* s : m.Settings())
        if (s->Persistent() && in.contains(s->Name())) s->Load(in[s->Name()]);
}

} // namespace

TEST(Settings, KeptInRange)
{
    Sample m;
    m.speed.Set(99.f);
    EXPECT_FLOAT_EQ(m.speed, 10.f);
    m.speed.Set(-5.f);
    EXPECT_FLOAT_EQ(m.speed, 0.f);
    m.count.Set(42);
    EXPECT_EQ(m.count.Get(), 9);
    m.mode.Set(7);                                      // Not an option: clamped to the last one
    EXPECT_EQ(m.mode.Get(), 2);
    EXPECT_EQ(m.mode.Selected(), "C");
    m.color.Set({2.f, -1.f, 0.5f, 1.f});
    EXPECT_FLOAT_EQ(m.color.Get()[0], 1.f);
    EXPECT_FLOAT_EQ(m.color.Get()[1], 0.f);
}

TEST(Settings, SavedAndLoadedBack)
{
    Sample a;
    a.on.Set(false);
    a.speed.Set(7.5f);
    a.count.Set(3);
    a.mode.Set(2);
    a.color.Set({0.5f, 0.6f, 0.7f, 0.8f});
    a.name.Set("second");
    const json saved = SaveAll(a);
    EXPECT_FALSE(saved.contains("Do it"));              // Buttons and labels have nothing to save
    EXPECT_FALSE(saved.contains("just text"));

    Sample b;
    LoadAll(b, json::parse(saved.dump()));              // Through text, like the config file
    EXPECT_FALSE(b.on.Get());
    EXPECT_FLOAT_EQ(b.speed, 7.5f);
    EXPECT_EQ(b.count.Get(), 3);
    EXPECT_EQ(b.mode.Get(), 2);
    EXPECT_FLOAT_EQ(b.color.Get()[3], 0.8f);
    EXPECT_EQ(b.name.Get(), "second");
}

TEST(Settings, BadValuesInTheFileAreIgnored)
{
    Sample m;
    LoadAll(m, json{{"Speed", "fast"}, {"Count", 1000}, {"On", nullptr}, {"Name", 12}, {"Mode", -3}});
    EXPECT_FLOAT_EQ(m.speed, 1.f);                      // Wrong type: kept
    EXPECT_EQ(m.count.Get(), 9);                        // Out of range: clamped
    EXPECT_TRUE(m.on.Get());
    EXPECT_EQ(m.name.Get(), "first");
    EXPECT_EQ(m.mode.Get(), 0);
}

TEST(Settings, Keys)
{
    Sample m;
    m.on.Press(0);                                      // Toggle
    EXPECT_FALSE(m.on.Get());
    m.speed.Press(0);                                   // + step (2)
    EXPECT_FLOAT_EQ(m.speed, 3.f);
    m.speed.Press(1);                                   // - step
    m.speed.Press(1);
    EXPECT_FLOAT_EQ(m.speed, 0.f);                      // Not below the minimum
    m.mode.Press(0);
    EXPECT_EQ(m.mode.Get(), 2);
    m.button.Press(0);
    EXPECT_EQ(m.pressed, 1);
    EXPECT_TRUE(m.on.Bindable());
    EXPECT_FALSE(m.label.Bindable());
    EXPECT_TRUE(m.TakeDirty());                         // A key press asks the module to update
}

TEST(Settings, FoundByName)
{
    Sample m;
    EXPECT_EQ(m.FindSetting("Speed"), &m.speed);
    EXPECT_EQ(m.FindSetting("Nope"), nullptr);
    EXPECT_EQ(m.Settings().size(), 8u);
}
