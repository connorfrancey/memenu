#include "hud/template.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

using namespace hud;

namespace {

class TestContext : public Context {
public:
    std::map<std::string, Value, std::less<>> values{
        {"speed", Value::Num(712.456)}, {"x", Value::Num(-1234.5)}, {"health", Value::Num(40)},
        {"maxhealth", Value::Num(100)}, {"map", Value::Str("edge_p")}, {"onground", Value::Bool(true)},
        {"weird", Value::Str("a&b")}, {"zero", Value::Num(0)},
    };

    bool Variable(std::string_view name, Value& out) const override
    {
        const auto it = values.find(name);
        if (it == values.end()) return false;
        out = it->second;
        return true;
    }

    bool Call(std::string_view name, const std::vector<Value>& args, Value& out, std::string& error) const override
    {
        if (name != "on") return false;
        if (args.size() != 1) error = "takes 1 argument";
        else out = Value::Bool(args[0].text == "God Mode");
        return true;
    }

    bool NamedColor(std::string_view name, Color& out) const override
    {
        if (name != "accent") return false;
        out = 0x112233FF;
        return true;
    }
};

std::string Line(std::string_view text, std::vector<Error>* errors = nullptr)
{
    static TestContext ctx;
    return PlainText(RenderLine(text, ctx, 0xFFFFFFFF, errors));
}

std::string Expect(std::string_view text)
{
    std::vector<Error> errors;
    const std::string out = Line(text, &errors);
    EXPECT_TRUE(errors.empty()) << text << ": " << (errors.empty() ? "" : errors[0].message);
    return out;
}

std::string FirstError(std::string_view text)
{
    std::vector<Error> errors;
    Line(text, &errors);
    return errors.empty() ? std::string() : errors[0].message;
}

} // namespace

TEST(HudTemplate, PlainTextAndBraces)
{
    EXPECT_EQ(Expect("hello"), "hello");
    EXPECT_EQ(Expect("{{literal}} braces"), "{literal} braces");
    EXPECT_EQ(Expect("a && b"), "a & b");
    EXPECT_EQ(Expect("&x stays"), "&x stays");
    EXPECT_EQ(Expect("#1 place"), "#1 place");
}

TEST(HudTemplate, ValuesUseFmtSpecs)
{
    EXPECT_EQ(Expect("{speed}"), "712.46");                 // Default: 2 decimals
    EXPECT_EQ(Expect("{health}"), "40");                    // Whole numbers as integers
    EXPECT_EQ(Expect("{speed:.1f}"), "712.5");
    EXPECT_EQ(Expect("[{x:>10.1f}]"), "[   -1234.5]");
    EXPECT_EQ(Expect("{health:04d}"), "0040");
    EXPECT_EQ(Expect("{health:x}"), "28");
    EXPECT_EQ(Expect("{speed:d}"), "712");                  // Integer types round
    EXPECT_EQ(Expect("[{map:^10}]"), "[  edge_p  ]");
    EXPECT_EQ(Expect("[{map:*<8}]"), "[edge_p**]");
    EXPECT_EQ(Expect("{onground}"), "true");
    EXPECT_EQ(Expect("{onground:d}"), "1");
    EXPECT_EQ(Expect("{speed:+.0f}"), "+712");
}

TEST(HudTemplate, BadSpecIsAnErrorNotACrash)
{
    EXPECT_NE(FirstError("{map:d}"), "");
    EXPECT_NE(FirstError("{speed:q}"), "");
    EXPECT_EQ(Line("{map:d}"), "?");
}

TEST(HudTemplate, Math)
{
    EXPECT_EQ(Expect("{speed * 0.036:.1f}"), "25.6");
    EXPECT_EQ(Expect("{(1 + 2) * 3}"), "9");
    EXPECT_EQ(Expect("{1 + 2 * 3}"), "7");
    EXPECT_EQ(Expect("{-health + 50}"), "10");
    EXPECT_EQ(Expect("{7 % 4}"), "3");
    EXPECT_EQ(Expect("{10 / 4}"), "2.50");
    EXPECT_EQ(Expect("{round(speed, 1)}"), "712.50");
    EXPECT_EQ(Expect("{round(speed)}"), "712");
    EXPECT_EQ(Expect("{max(1, health, 7)} {min(3, 2)} {clamp(500, 0, 100)} {abs(x)}"), "40 2 100 1234.50");
    EXPECT_EQ(Expect("{floor(2.7)} {ceil(2.1)} {sqrt(16)} {sign(x)} {lerp(0, 10, 0.25)}"), "2 3 4 -1 2.50");
}

TEST(HudTemplate, LogicAndComparisons)
{
    EXPECT_EQ(Expect("{speed > 700}"), "true");
    EXPECT_EQ(Expect("{speed > 700 && health >= 50}"), "false");
    EXPECT_EQ(Expect("{speed > 700 || health >= 50}"), "true");
    EXPECT_EQ(Expect("{!onground}"), "false");
    EXPECT_EQ(Expect("{map == \"edge_p\"} {map != \"x\"} {map == 3}"), "true true false");
    EXPECT_EQ(Expect("{\"abc\" < \"abd\"}"), "true");
    // Short-circuit: the unknown value on the right is never looked at
    EXPECT_EQ(Expect("{zero && nosuchvalue}"), "false");
}

TEST(HudTemplate, IfFunctionIsLazy)
{
    EXPECT_EQ(Expect("{if(speed > 700, \"fast\", \"slow\")}"), "fast");
    EXPECT_EQ(Expect("{if(speed > 900, \"fast\", \"slow\")}"), "slow");
    EXPECT_EQ(Expect("[{if(false, \"x\")}]"), "[]");
    EXPECT_EQ(Expect("{if(true, 1, nosuchvalue)}"), "1");
}

TEST(HudTemplate, StringsAreTemplates)
{
    EXPECT_EQ(Expect("{if(true, \"at {speed:.0f} u/s\", \"\")}"), "at 712 u/s");
    EXPECT_EQ(Expect("{\"say \\\"hi\\\"\"}"), "say \"hi\"");
    EXPECT_EQ(Expect("{\"a\" + 1 + \"b\"}"), "a1b");
    EXPECT_EQ(Expect("{upper(map)} {len(map)} {lower(\"ABC\")}"), "EDGE_P 6 abc");
    EXPECT_EQ(Expect("{str(speed, \".0f\") + \"!\"}"), "712!");
}

TEST(HudTemplate, TimeText)
{
    EXPECT_EQ(TimeText(62.345), "1:02.35");
    EXPECT_EQ(TimeText(3725.5, 1), "1:02:05.5");
    EXPECT_EQ(TimeText(5.0, 0), "0:05");
    EXPECT_EQ(TimeText(-1.5), "-0:01.50");
    EXPECT_EQ(Expect("{time(62.345)}"), "1:02.35");
}

TEST(HudTemplate, ColorsMakeSegments)
{
    TestContext ctx;
    const RenderedLine line = RenderLine("a&(red)b&#00ff00c&rd&(accent)e&#11223344f", ctx, 0xFFFFFFFF);
    ASSERT_EQ(line.left.size(), 6u);
    EXPECT_EQ(line.left[0].text, "a");
    EXPECT_EQ(line.left[0].color, 0xFFFFFFFFu);
    EXPECT_EQ(line.left[1].text, "b");
    EXPECT_EQ(line.left[1].color, 0xFF5050FFu);
    EXPECT_EQ(line.left[2].color, 0x00FF00FFu);
    EXPECT_EQ(line.left[3].text, "d");
    EXPECT_EQ(line.left[3].color, 0xFFFFFFFFu);     // &r: back to the line's color
    EXPECT_EQ(line.left[4].color, 0x112233FFu);     // From the host
    EXPECT_EQ(line.left[5].color, 0x11223344u);     // With alpha
}

TEST(HudTemplate, ColorFunctions)
{
    TestContext ctx;
    RenderedLine line = RenderLine("{mix(\"#000000\", \"#ffffff\", 0.5)}x", ctx);
    ASSERT_EQ(line.left.size(), 1u);
    EXPECT_EQ(line.left[0].color, 0x808080FFu);
    line = RenderLine("{rgb(255, 0, 128)}x{color(\"blue\")}y", ctx);
    ASSERT_EQ(line.left.size(), 2u);
    EXPECT_EQ(line.left[0].color, 0xFF0080FFu);
    EXPECT_EQ(line.left[1].color, 0x4080FFFFu);
    // A color change from a value carries on to the rest of the line
    line = RenderLine("{if(speed > 700, \"&(green)\", \"\")}fast {speed:.0f}", ctx);
    ASSERT_EQ(line.left.size(), 1u);
    EXPECT_EQ(line.left[0].text, "fast 712");
    EXPECT_EQ(line.left[0].color, 0x50E070FFu);
    EXPECT_NE(FirstError("{color(\"nope\")}"), "");
    EXPECT_NE(FirstError("&(nope)x"), "");
}

TEST(HudTemplate, PlainValuesAreNotMarkup)
{
    // A value's & is text, not a color code: only string literals and color functions carry markup
    EXPECT_EQ(Expect("{weird}"), "a&b");
    EXPECT_EQ(Expect("{\"<\" + weird + \">\"}"), "<a&b>");
}

TEST(HudTemplate, Columns)
{
    TestContext ctx;
    const RenderedLine line = RenderLine("Speed &>{speed:.0f} u/s", ctx);
    EXPECT_TRUE(line.hasRight);
    EXPECT_EQ(PlainText(line.left), "Speed ");
    EXPECT_EQ(PlainText(line.right), "712 u/s");
}

TEST(HudTemplate, Bars)
{
    TestContext ctx;
    const RenderedLine line = RenderLine("HP {bar(health / maxhealth, 60, \"red\")} {bar(2)}", ctx);
    ASSERT_EQ(line.left.size(), 4u);
    EXPECT_TRUE(line.left[1].bar);
    EXPECT_FLOAT_EQ(line.left[1].fraction, 0.4f);
    EXPECT_FLOAT_EQ(line.left[1].width, 60.f);
    EXPECT_EQ(line.left[1].color, 0xFF5050FFu);
    EXPECT_TRUE(line.left[3].bar);
    EXPECT_FLOAT_EQ(line.left[3].fraction, 1.f);    // Clamped
    EXPECT_FLOAT_EQ(line.left[3].width, 80.f);      // Default width
    EXPECT_EQ(line.left[3].color, 0xFFFFFFFFu);     // The text color
}

TEST(HudTemplate, HostFunctions)
{
    EXPECT_EQ(Expect("{on(\"God Mode\")} {on(\"No Clip\")}"), "true false");
    EXPECT_NE(FirstError("{on()}").find("takes 1 argument"), std::string::npos);
    EXPECT_NE(FirstError("{nosuch(1)}").find("unknown function"), std::string::npos);
}

TEST(HudTemplate, ErrorsSayWhatIsWrong)
{
    EXPECT_NE(FirstError("{nosuchvalue}").find("unknown value 'nosuchvalue'"), std::string::npos);
    EXPECT_NE(FirstError("{speed +}").find("missing"), std::string::npos);
    EXPECT_NE(FirstError("{(speed}").find(")"), std::string::npos);
    EXPECT_NE(FirstError("{speed").find("without its }"), std::string::npos);
    EXPECT_NE(FirstError("speed}").find("without its {"), std::string::npos);
    EXPECT_NE(FirstError("{}").find("empty"), std::string::npos);
    EXPECT_NE(FirstError("{x = 1}").find("=="), std::string::npos);
    EXPECT_NE(FirstError("{\"open}").find("\""), std::string::npos);
    EXPECT_NE(FirstError("{abs(1, 2)}").find("takes 1 argument"), std::string::npos);
    EXPECT_EQ(Line("before {nosuchvalue} after"), "before ? after");
}

TEST(HudProfile, SectionsAndOptions)
{
    const Profile p = Profile::Parse(
        "// comment\n"
        "first line\n"
        "\n"
        "== top right, scale 1.5, bg 0.4, offset 10 -20, align right, padding 8, spacing 2, width 120 ==\n"
        "Speed &>{speed:.0f}\n"
        "== at 50% 100, shadow off, outline on, color #ff0000 ==\n"
        "x\n"
        "== bottom, color accent ==\n"
        "y\n");
    EXPECT_TRUE(p.Errors().empty()) << p.Errors()[0].message;
    ASSERT_EQ(p.SectionCount(), 3u + 1u);       // The implicit first section, then three

    TestContext ctx;
    std::vector<Error> errors;
    const auto sections = p.Render(ctx, &errors);
    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(sections.size(), 4u);
    EXPECT_EQ(sections[0].style.anchor, Anchor::TopLeft);
    ASSERT_EQ(sections[0].lines.size(), 1u);    // The blank line before the next header is dropped
    EXPECT_EQ(PlainText(sections[0].lines[0]), "first line");

    const SectionStyle& s1 = sections[1].style;
    EXPECT_EQ(s1.anchor, Anchor::TopRight);
    EXPECT_FLOAT_EQ(s1.scale, 1.5f);
    EXPECT_FLOAT_EQ(s1.background, 0.4f);
    EXPECT_FLOAT_EQ(s1.offsetX, 10.f);
    EXPECT_FLOAT_EQ(s1.offsetY, -20.f);
    EXPECT_EQ(s1.align, Align::Right);
    EXPECT_FLOAT_EQ(s1.padding, 8.f);
    EXPECT_FLOAT_EQ(s1.spacing, 2.f);
    EXPECT_FLOAT_EQ(s1.minWidth, 120.f);
    EXPECT_EQ(PlainText(sections[1].lines[0]), "Speed  | 712");
    EXPECT_EQ(sections[1].line, 4);

    const SectionStyle& s2 = sections[2].style;
    EXPECT_EQ(s2.anchor, Anchor::At);
    EXPECT_TRUE(s2.xPercent);
    EXPECT_FALSE(s2.yPercent);
    EXPECT_FLOAT_EQ(s2.x, 50.f);
    EXPECT_FLOAT_EQ(s2.y, 100.f);
    EXPECT_FALSE(s2.shadow);
    EXPECT_TRUE(s2.outline);
    EXPECT_EQ(s2.color, 0xFF0000FFu);
    EXPECT_EQ(sections[2].lines[0].left[0].color, 0xFF0000FFu);     // The section color is the line's

    EXPECT_EQ(sections[3].style.anchor, Anchor::Bottom);
    EXPECT_EQ(sections[3].style.color, 0x112233FFu);                // Host color name
}

TEST(HudProfile, AnchorSpellings)
{
    for (const char* place : {"top left", "Top-Left", "topleft", "top_left"}) {
        const Profile p = Profile::Parse(std::string("== ") + place + " ==\nx");
        EXPECT_TRUE(p.Errors().empty()) << place;
    }
    const Profile p = Profile::Parse("== middle right ==\nx\n== bottom center ==\ny\n== center ==\nz");
    TestContext ctx;
    const auto s = p.Render(ctx);
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(s[0].style.anchor, Anchor::Right);
    EXPECT_EQ(s[1].style.anchor, Anchor::Bottom);
    EXPECT_EQ(s[2].style.anchor, Anchor::Center);
}

TEST(HudProfile, Conditionals)
{
    const Profile p = Profile::Parse(
        "== top ==\n"
        "always\n"
        "#if speed > 900\n"
        "very fast\n"
        "#elif speed > 700\n"
        "fast\n"
        "#if health < 50\n"
        "  and hurt\n"
        "#endif\n"
        "#else\n"
        "slow\n"
        "#endif\n"
        "#if on(\"No Clip\")\n"
        "noclip\n"
        "#else\n"
        "walking\n"
        "#endif\n");
    ASSERT_TRUE(p.Errors().empty()) << p.Errors()[0].message;
    TestContext ctx;
    auto lines = p.Render(ctx)[0].lines;
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(PlainText(lines[0]), "always");
    EXPECT_EQ(PlainText(lines[1]), "fast");
    EXPECT_EQ(PlainText(lines[2]), "  and hurt");
    EXPECT_EQ(PlainText(lines[3]), "walking");

    ctx.values["speed"] = Value::Num(100);
    lines = p.Render(ctx)[0].lines;
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(PlainText(lines[1]), "slow");
}

TEST(HudProfile, ParseErrorsHaveLineNumbers)
{
    const Profile p = Profile::Parse(
        "== top left, scale banana ==\n"      // 1
        "== nowhere ==\n"                     // 2
        "#else\n"                             // 3
        "#endif\n"                            // 4
        "#if\n"                               // 5
        "{oops\n"                             // 6
        "== top, bogus 3 ==\n"                // 7 (also closes the #if from line 5)
        "#if speed >\n"                       // 8
        "#endif\n");
    std::vector<int> lines;
    for (const Error& e : p.Errors()) lines.push_back(e.line);
    EXPECT_EQ(lines, (std::vector<int>{1, 2, 3, 4, 5, 6, 5, 7, 8}));
    EXPECT_NE(p.Errors()[0].message.find("scale"), std::string::npos);
    EXPECT_NE(p.Errors()[1].message.find("not a place"), std::string::npos);
    EXPECT_NE(p.Errors()[6].message.find("#if without #endif"), std::string::npos);
    EXPECT_NE(p.Errors()[7].message.find("unknown option 'bogus'"), std::string::npos);
}

TEST(HudProfile, RenderErrorsAreReportedOnce)
{
    const Profile p = Profile::Parse("{nosuchvalue}\n{nosuchvalue}\n#if alsonot\nx\n#endif\n");
    ASSERT_TRUE(p.Errors().empty());
    TestContext ctx;
    std::vector<Error> errors;
    p.Render(ctx, &errors);
    p.Render(ctx, &errors);
    ASSERT_EQ(errors.size(), 3u);       // Per line, not per render
    EXPECT_EQ(errors[0].line, 1);
    EXPECT_EQ(errors[1].line, 2);
    EXPECT_EQ(errors[2].line, 3);
}

TEST(HudProfile, EmptyProfile)
{
    const Profile p = Profile::Parse("");
    EXPECT_TRUE(p.Errors().empty());
    EXPECT_EQ(p.SectionCount(), 0u);
    TestContext ctx;
    EXPECT_TRUE(p.Render(ctx).empty());
    EXPECT_EQ(Profile().SectionCount(), 0u);
}
