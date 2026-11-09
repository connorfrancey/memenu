#include "shell.h"

#include <algorithm>
#include <cctype>

namespace shell {
namespace {

bool Space(char c) { return c == ' ' || c == '\t'; }

char LowerChar(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool IsNumber(std::string_view s)
{
    if (s.empty()) return false;
    if (EqualsNoCase(s, "true") || EqualsNoCase(s, "false")) return true;
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    bool digits = false, dot = false;
    for (; i < s.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i]))) digits = true;
        else if (s[i] == '.' && !dot) dot = true;
        else return false;
    }
    return digits;
}

} // namespace

bool StartsWithNoCase(std::string_view s, std::string_view prefix)
{
    if (prefix.size() > s.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (LowerChar(s[i]) != LowerChar(prefix[i])) return false;
    return true;
}

bool EqualsNoCase(std::string_view a, std::string_view b) { return a.size() == b.size() && StartsWithNoCase(a, b); }

std::vector<Command> Parse(std::string_view line)
{
    std::vector<Command> out;
    Command cmd;
    std::size_t i = 0;
    const auto finish = [&](std::size_t end) {
        cmd.end = end;
        out.push_back(std::move(cmd));
        cmd = Command{};
    };
    cmd.begin = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (Space(c)) { ++i; continue; }
        if (c == ';') {
            finish(i);
            cmd.begin = ++i;
            continue;
        }
        if (c == '#' && cmd.words.empty()) {        // A comment: the rest of this command
            cmd.comment = true;
            while (i < line.size() && line[i] != ';') ++i;
            continue;
        }
        Word w;
        w.begin = i;
        if (c == '"') {
            w.quoted = true;
            ++i;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) ++i;
                w.text.push_back(line[i++]);
            }
            if (i < line.size()) ++i;               // The closing quote
            else w.unterminated = true;
        } else {
            while (i < line.size() && !Space(line[i]) && line[i] != ';' && line[i] != '"') w.text.push_back(line[i++]);
        }
        w.end = i;
        cmd.words.push_back(std::move(w));
    }
    finish(line.size());
    return out;
}

std::vector<Style> Highlight(std::string_view line, const Highlighter& h)
{
    std::vector<Style> styles(line.size(), Style::Default);
    const auto paint = [&](std::size_t b, std::size_t e, Style s) {
        for (std::size_t i = b; i < e && i < styles.size(); ++i) styles[i] = s;
    };
    for (std::size_t i = 0; i < line.size(); ++i) {
        // Separators outside quotes (Parse ended a command there)
        if (line[i] == ';') styles[i] = Style::Separator;
    }
    for (const Command& c : Parse(line)) {
        if (c.comment) {
            const std::size_t at = line.find('#', c.begin);
            paint(at, c.end, Style::Comment);
            continue;
        }
        for (std::size_t w = 0; w < c.words.size(); ++w) {
            const Word& word = c.words[w];
            Style s = Style::Default;
            if (word.unterminated) s = Style::Error;
            else if (w == 0) s = h.commandStyle ? h.commandStyle(word.text) : Style::Default;
            else if (h.knownArg && h.knownArg(c, w)) s = Style::Known;
            else if (word.quoted) s = Style::String;
            else if (IsNumber(word.text)) s = Style::Number;
            paint(word.begin, word.end, s);
        }
    }
    return styles;
}

Target Locate(std::string_view line, std::size_t cursor)
{
    cursor = std::min(cursor, line.size());
    Target t;
    // The command the cursor is in: the last one starting at or before it
    const auto commands = Parse(line.substr(0, cursor));
    const Command& c = commands.back();
    // The cursor is in a word when the last word ends at it (no space typed after it yet)
    const bool inWord = !c.words.empty() && c.words.back().end == cursor && !(c.words.back().quoted && !c.words.back().unterminated);
    const std::size_t before = inWord ? c.words.size() - 1 : c.words.size();
    t.argIndex = before;
    if (before > 0) {
        t.command.resize(c.words[0].text.size());
        std::transform(c.words[0].text.begin(), c.words[0].text.end(), t.command.begin(), LowerChar);
        for (std::size_t i = 1; i < before; ++i) t.args.push_back(c.words[i].text);
    }
    if (inWord) {
        t.typed = c.words.back().text;
        t.replaceBegin = c.words.back().begin;
    } else {
        t.replaceBegin = cursor;
    }
    // Replace to the end of the word the cursor is in (it may go on after the cursor)
    std::size_t end = cursor;
    if (inWord) {
        bool quoted = c.words.back().quoted;
        while (end < line.size() && !Space(line[end]) && line[end] != ';' && (quoted || line[end] != '"')) ++end;
        if (quoted && end < line.size() && line[end] == '"') ++end;
    }
    t.replaceEnd = end;
    return t;
}

std::vector<std::string> Filter(const std::vector<std::string>& candidates, std::string_view typed)
{
    std::vector<std::string> out;
    for (const auto& c : candidates)
        if (StartsWithNoCase(c, typed)) out.push_back(c);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const auto n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i)
            if (LowerChar(a[i]) != LowerChar(b[i])) return LowerChar(a[i]) < LowerChar(b[i]);
        return a.size() < b.size();
    });
    out.erase(std::unique(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return EqualsNoCase(a, b); }),
              out.end());
    return out;
}

std::string CommonPrefix(const std::vector<std::string>& candidates)
{
    if (candidates.empty()) return {};
    std::size_t n = candidates[0].size();
    for (const auto& c : candidates) {
        std::size_t i = 0;
        while (i < n && i < c.size() && LowerChar(c[i]) == LowerChar(candidates[0][i])) ++i;
        n = i;
    }
    return candidates[0].substr(0, n);
}

std::string Quote(const std::string& word)
{
    if (word.find_first_of(" \t;\"") == std::string::npos && !word.empty()) return word;
    std::string out = "\"";
    for (char c : word) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out + '"';
}

Applied Apply(std::string_view line, const Target& target, const std::string& choice, bool final)
{
    std::string word = Quote(choice);
    if (!final && word.size() > 1 && word.front() == '"') word.pop_back();   // Still being typed: leave the quote open
    std::string out(line.substr(0, target.replaceBegin));
    out += word;
    std::size_t cursor = out.size();
    std::string_view rest = line.substr(std::min(target.replaceEnd, line.size()));
    if (final && (rest.empty() || !Space(rest.front()))) {
        out += ' ';
        ++cursor;
    } else if (final && !rest.empty() && Space(rest.front())) {
        ++cursor;
    }
    out += rest;
    return {out, cursor};
}

std::string Suggest(const std::vector<std::string>& history, std::string_view line)
{
    if (line.empty()) return {};
    for (auto it = history.rbegin(); it != history.rend(); ++it)
        if (it->size() > line.size() && it->compare(0, line.size(), line) == 0) return it->substr(line.size());
    return {};
}

} // namespace shell
