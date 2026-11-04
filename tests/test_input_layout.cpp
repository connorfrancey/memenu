// The input overlay's layouts (features/visual/input_layout.h).
#include "features/visual/input_layout.h"

#include <gtest/gtest.h>

using namespace inputlayout;

TEST(InputLayout, BuiltinsSurviveAFile)
{
    const auto builtins = Builtins();
    ASSERT_GE(builtins.size(), 4u);
    EXPECT_EQ(builtins.front().name, "Movement");
    for (const Layout& l : builtins) {
        Layout back;
        std::string error;
        ASSERT_TRUE(Parse(ToJson(l), back, &error)) << l.name << ": " << error;
        EXPECT_EQ(back.name, l.name);
        ASSERT_EQ(back.elements.size(), l.elements.size()) << l.name;
        for (std::size_t i = 0; i < l.elements.size(); ++i) {
            EXPECT_EQ(back.elements[i].kind, l.elements[i].kind);
            EXPECT_EQ(back.elements[i].keys, l.elements[i].keys);
            EXPECT_EQ(back.elements[i].label, l.elements[i].label);
            EXPECT_FLOAT_EQ(back.elements[i].x, l.elements[i].x);
            EXPECT_FLOAT_EQ(back.elements[i].w, l.elements[i].w);
            EXPECT_FLOAT_EQ(back.elements[i].h, l.elements[i].h);
        }
        EXPECT_FLOAT_EQ(back.Width(), l.Width());
    }
}

TEST(InputLayout, HandWritten)
{
    Layout l;
    ASSERT_TRUE(Parse(R"({"elements": [
        {"key": "W", "x": 1},
        {"keys": ["LeftShift", "RightShift"], "label": "Run", "y": 1, "w": 2},
        {"type": "mouse", "x": 3},
        {"type": "KPS", "x": 5, "y": 2}
    ]})", l));
    ASSERT_EQ(l.elements.size(), 4u);
    EXPECT_EQ(l.elements[0].label, "W");
    EXPECT_EQ(l.elements[0].keys.front(), "w");                 // Names are compared in lower case
    EXPECT_EQ(l.elements[1].keys.size(), 2u);
    EXPECT_EQ(l.elements[1].label, "Run");
    EXPECT_EQ(l.elements[2].kind, Kind::Mouse);
    EXPECT_FLOAT_EQ(l.elements[2].w, 2.f);                      // A mouse pad is 2 x 2 by default
    EXPECT_EQ(l.elements[3].kind, Kind::Kps);
    EXPECT_FLOAT_EQ(l.Width(), 6.f);
    EXPECT_FLOAT_EQ(l.Height(), 3.f);
}

TEST(InputLayout, Errors)
{
    Layout l;
    std::string error;
    EXPECT_FALSE(Parse("nope", l, &error));
    EXPECT_FALSE(Parse(R"({"elements": []})", l, &error));
    EXPECT_FALSE(Parse(R"({"elements": [{"x": 1}]})", l, &error));       // A key needs a key
    EXPECT_NE(error.find("key"), std::string::npos);
    EXPECT_FALSE(Parse(R"({"elements": [{"type": "joystick"}]})", l, &error));
}

TEST(InputLayout, ShortLabels)
{
    EXPECT_EQ(ShortLabel("SpaceBar"), "Space");
    EXPECT_EQ(ShortLabel("leftmousebutton"), "LMB");
    EXPECT_EQ(ShortLabel("one"), "1");
    EXPECT_EQ(ShortLabel("q"), "Q");
    EXPECT_EQ(ShortLabel("PageUp"), "PageUp");
}
