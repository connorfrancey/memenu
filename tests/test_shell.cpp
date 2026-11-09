// The menu console's line handling (src/core/shell.h): parsing, highlighting, completion, suggestions.
#include "core/shell.h"

#include <gtest/gtest.h>

using namespace shell;

TEST(Shell, ParsesWordsQuotesAndCommands)
{
    const auto c = Parse(R"(toggle "God Mode"; stat fps  ;# note; echo "a \"b\"")");
    ASSERT_EQ(c.size(), 4u);
    ASSERT_EQ(c[0].words.size(), 2u);
    EXPECT_EQ(c[0].words[0].text, "toggle");
    EXPECT_EQ(c[0].words[1].text, "God Mode");
    EXPECT_TRUE(c[0].words[1].quoted);
    EXPECT_EQ(c[1].words.size(), 2u);
    EXPECT_TRUE(c[2].comment);
    EXPECT_TRUE(c[2].words.empty());
    ASSERT_EQ(c[3].words.size(), 2u);
    EXPECT_EQ(c[3].words[1].text, "a \"b\"");
}

TEST(Shell, UnclosedQuote)
{
    const auto c = Parse(R"(echo "never closed; still in it)");
    ASSERT_EQ(c.size(), 1u);   // The ';' is inside the quote
    ASSERT_EQ(c[0].words.size(), 2u);
    EXPECT_TRUE(c[0].words[1].unterminated);
}

TEST(Shell, HighlightsLikeZsh)
{
    Highlighter h;
    h.commandStyle = [](std::string_view w) {
        if (EqualsNoCase(w, "toggle")) return Style::Builtin;
        if (EqualsNoCase(w, "slomo")) return Style::GameCommand;
        if (StartsWithNoCase("slomo", w)) return Style::Partial;
        return Style::Unknown;
    };
    h.knownArg = [](const Command& c, std::size_t i) { return i == 1 && c.words[i].text == "God Mode"; };

    const std::string line = R"(toggle "God Mode"; slomo 0.5; bogus "x" #)";
    const auto s = Highlight(line, h);
    ASSERT_EQ(s.size(), line.size());
    EXPECT_EQ(s[0], Style::Builtin);
    EXPECT_EQ(s[line.find("God")], Style::Known);
    EXPECT_EQ(s[line.find(';')], Style::Separator);
    EXPECT_EQ(s[line.find("slomo")], Style::GameCommand);
    EXPECT_EQ(s[line.find("0.5")], Style::Number);
    EXPECT_EQ(s[line.find("bogus")], Style::Unknown);
    EXPECT_EQ(s[line.find("\"x\"")], Style::String);

    EXPECT_EQ(Highlight("slo", h)[0], Style::Partial);
    EXPECT_EQ(Highlight("echo \"open", h)[6], Style::Error);
    EXPECT_EQ(Highlight("# just a note", h)[3], Style::Comment);
}

TEST(Shell, LocatesTheWordUnderTheCursor)
{
    auto t = Locate("tog", 3);
    EXPECT_EQ(t.argIndex, 0u);
    EXPECT_EQ(t.typed, "tog");

    t = Locate("toggle \"God M", 13);
    EXPECT_EQ(t.command, "toggle");
    EXPECT_EQ(t.argIndex, 1u);
    EXPECT_EQ(t.typed, "God M");
    EXPECT_EQ(t.replaceBegin, 7u);

    t = Locate("setting \"God Mode\" ", 19);
    EXPECT_EQ(t.argIndex, 2u);
    ASSERT_EQ(t.args.size(), 1u);
    EXPECT_EQ(t.args[0], "God Mode");
    EXPECT_EQ(t.typed, "");

    t = Locate("stat fps; ena", 13);   // The second command
    EXPECT_EQ(t.argIndex, 0u);
    EXPECT_EQ(t.typed, "ena");
    EXPECT_EQ(t.replaceBegin, 10u);
}

TEST(Shell, CompletesAndQuotes)
{
    const std::vector<std::string> names{"God Mode", "Game Speed", "game_status", "FOV"};
    const auto g = Filter(names, "g");
    ASSERT_EQ(g.size(), 3u);
    EXPECT_EQ(CommonPrefix(g), "G");
    EXPECT_EQ(CommonPrefix(Filter(names, "game")), "Game");

    const std::string line = "toggle go";
    auto a = Apply(line, Locate(line, line.size()), "God Mode", true);
    EXPECT_EQ(a.line, "toggle \"God Mode\" ");
    EXPECT_EQ(a.cursor, a.line.size());

    a = Apply(line, Locate(line, line.size()), "God M", false);   // Common prefix only: the quote stays open
    EXPECT_EQ(a.line, "toggle \"God M");

    const std::string mid = "stat f; echo x";
    a = Apply(mid, Locate(mid, 6), "fps", true);
    EXPECT_EQ(a.line, "stat fps ; echo x");
    EXPECT_EQ(Quote("plain"), "plain");
    EXPECT_EQ(Quote("a;b"), "\"a;b\"");
}

TEST(Shell, SuggestsFromNewestHistory)
{
    const std::vector<std::string> history{"slomo 2", "stat fps", "slomo 0.5"};
    EXPECT_EQ(Suggest(history, "slo"), "mo 0.5");
    EXPECT_EQ(Suggest(history, "stat"), " fps");
    EXPECT_EQ(Suggest(history, "nothing"), "");
    EXPECT_EQ(Suggest(history, ""), "");
}
