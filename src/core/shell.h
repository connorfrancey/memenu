#pragma once
// The menu console's line handling, without ImGui or the game (unit tested in tests/test_shell.cpp):
//
//   Parse       a line into commands (split at ';' outside quotes) and their words ("double quotes" keep spaces)
//   Highlight   a style per byte of the line, like zsh-syntax-highlighting: the command word by what it is (menu
//               command, alias, game command, or unknown in red), strings, numbers, separators, comments, an
//               unclosed quote, and arguments the console knows (a module's name)
//   Locate      the word the cursor is in, for Tab completion: which command, which argument, what is typed so far
//   Complete    candidates filtered by what is typed, their common prefix, and the line with one applied
//   Suggest     the rest of the newest history entry that starts with the line (zsh-autosuggestions)
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace shell {

struct Word {
    std::size_t begin = 0, end = 0;     // Bytes of the line, quotes included
    std::string text;                   // Without the quotes
    bool quoted = false;
    bool unterminated = false;          // A quote that is never closed
};

struct Command {
    std::size_t begin = 0, end = 0;     // Bytes of the line (the ';' after it not included)
    std::vector<Word> words;
    bool comment = false;               // Starts with '#': nothing to run
};

std::vector<Command> Parse(std::string_view line);

// ---- Highlighting --------------------------------------------------------------------------------------------------------
enum class Style : std::uint8_t {
    Default,        // Plain text (an argument)
    Builtin,        // Command word: a menu command (help, toggle, ...)
    Alias,          // Command word: an alias
    GameCommand,    // Command word: a command of the game (exec function or engine command)
    Partial,        // Command word still being typed: the start of a known command
    Unknown,        // Command word: nothing by that name
    Known,          // An argument the console recognizes (a module or setting name): underlined
    String,         // "quoted"
    Number,         // 12, -0.5, true/false
    Separator,      // ;
    Comment,        // # to the end of the command
    Error,          // An unclosed quote
};

struct Highlighter {
    std::function<Style(std::string_view word)> commandStyle;               // Builtin/Alias/GameCommand/Partial/Unknown
    std::function<bool(const Command&, std::size_t argIndex)> knownArg;     // Optional: argument recognized
};

std::vector<Style> Highlight(std::string_view line, const Highlighter& h);

// ---- Completion ----------------------------------------------------------------------------------------------------------
struct Target {
    std::string command;                // The command word, lower case ("" while completing it)
    std::size_t argIndex = 0;           // 0: the command word itself, 1: its first argument, ...
    std::vector<std::string> args;      // The words before the one being completed (without the command)
    std::string typed;                  // What is typed of the word so far (unquoted), up to the cursor
    std::size_t replaceBegin = 0, replaceEnd = 0;   // The bytes a completion replaces
};

Target Locate(std::string_view line, std::size_t cursor);

// Candidates starting with `typed` (case-insensitive), sorted and without duplicates
std::vector<std::string> Filter(const std::vector<std::string>& candidates, std::string_view typed);
// The longest start every candidate shares (case-insensitive, spelled like the first one)
std::string CommonPrefix(const std::vector<std::string>& candidates);
// In quotes when it has a space, a ';' or a quote in it
std::string Quote(const std::string& word);

struct Applied {
    std::string line;
    std::size_t cursor = 0;
};
// The line with `choice` in place of the target word. `final`: the word is complete (a space follows it).
Applied Apply(std::string_view line, const Target& target, const std::string& choice, bool final);

// ---- Suggestions -----------------------------------------------------------------------------------------------------------
// What the newest history entry that starts with `line` adds to it ("" when none does, or the line is empty).
// `history` is oldest first.
std::string Suggest(const std::vector<std::string>& history, std::string_view line);

bool StartsWithNoCase(std::string_view s, std::string_view prefix);
bool EqualsNoCase(std::string_view a, std::string_view b);

} // namespace shell
