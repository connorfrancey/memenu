#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The text language of Info Lines (features/visual/info_lines.cpp): lines of text placed on the screen, with live
// values, math, conditions, colors, columns and bars. No game and no ImGui in here: the host provides the values
// (hud::Context) and draws what Render() returns. Unit tested in tests/test_hud_template.cpp.
//
// ---- A profile ------------------------------------------------------------------------------------------------------
//
//   // A comment (only at the start of a line)
//   == top right, offset 0 40, bg 0.5 ==            A section: where it goes, then options (see below)
//   Speed &> {speed:.0f} u/s                        A line
//   #if speed > 700                                 Lines shown only sometimes: #if / #elif / #else / #endif
//   &(green)Fast!
//   #endif
//
// Text before the first section header goes in an implicit "top left" section. Blank lines are kept (as spacing),
// except at the end of a section.
//
// Section places: top left, top, top right, left, center, right, bottom left, bottom, bottom right, or
// "at <x> <y>" (pixels, or percent of the screen with a %: "at 50% 10%"; the box's top left corner goes there).
// Section options, after commas:
//   scale <n>            text size (1 = the menu's font)          bg <0..1>      background opacity (default 0)
//   padding <px>         space around the text (default 4)        spacing <px>   extra space between lines
//   offset <x> <y>       moved by this much (pixels)              width <px>     at least this wide
//   align left|center|right   lines without a second column      color <c>      default text color (#rrggbb, a name)
//   shadow on|off        drop shadow (default on)                 outline on|off dark outline (default off)
//
// ---- A line ---------------------------------------------------------------------------------------------------------
//
//   {expr}               the value of an expression           {{ and }}    literal braces
//   {expr:spec}          formatted with an fmt format spec: {speed:.1f} {x:>9.2f} {health:03d} {map:^12} {n:x}
//                        Numbers without a spec: whole numbers as integers, others with 2 decimals.
//   &(name) &#rrggbb &#rrggbbaa    the text color from here on       &r    back to the section's color
//   &>                   the rest of the line goes in a second, right-aligned column (tables: "Speed &> {speed}")
//   &&                   a literal &
//
// Expressions: numbers, "strings" (themselves lines: "&(red){x:.0f}" works, \" and \\ escape), true, false,
// variables (from the host), ( ), + - * / %, comparisons < <= > >= == !=, && || !, and functions:
//   if(cond, a, b)                  a or b (only the chosen one is evaluated)
//   abs floor ceil sqrt sign  min(a, b, ...) max(a, b, ...)  clamp(x, lo, hi)  round(x[, decimals])  lerp(a, b, t)
//   time(seconds[, decimals])       "1:02.35" (h:mm:ss.cc from an hour on)
//   upper(s) lower(s) len(s)  str(x[, spec])   (text of a value, formatted like {x:spec})
//   bar(fraction[, width px[, color]])           a progress bar
//   color(c) rgb(r, g, b[, a]) mix(c1, c2, t)    a color change, as text ("{mix("white", "red", t)}fast")
//   ... and the host's functions (Info Lines: on("Module name")).
// + joins text when either side is text. Comparing two texts compares them as text.
namespace hud
{
	using Color = std::uint32_t;	// 0xRRGGBBAA

	struct Value
	{
		enum class Kind : std::uint8_t { Number, Text, Bool };
		Kind kind = Kind::Number;
		double number = 0.0;
		std::string text;
		bool rich = false;		// Text with markup (&(red), &>, bars): from string literals and color functions

		static Value Num(double v);
		static Value Str(std::string v, bool rich = false);
		static Value Bool(bool v);

		double AsNumber() const;	// Text: 0 (or its number, if it is one)
		bool Truthy() const;		// Non-zero, true, non-empty
	};

	struct Error
	{
		int line = 0;			// 1-based line of the profile (0: not tied to a line)
		std::string message;
		bool operator==(const Error& o) const { return line == o.line && message == o.message; }
	};

	// What the host provides. Called while rendering (the host's thread), so it must answer quickly.
	class Context
	{
	public:
		virtual ~Context() = default;
		// The value of a variable. False: there is no such variable.
		virtual bool Variable(std::string_view name, Value& out) const = 0;
		// A function the built-ins don't have. False: no such function. Set `error` (and return true) for bad arguments.
		virtual bool Call(std::string_view name, const std::vector<Value>& args, Value& out, std::string& error) const;
		// A color name the built-in table doesn't have ("accent").
		virtual bool NamedColor(std::string_view name, Color& out) const;
	};

	// ---- What Render() gives back ----------------------------------------------------------------------------------
	struct Segment
	{
		std::string text;
		Color color = 0xFFFFFFFF;
		bool bar = false;			// A bar instead of text
		float fraction = 0.f;		// Bar: 0..1 filled
		float width = 0.f;			// Bar: width in pixels (before the section's scale)
	};

	struct RenderedLine
	{
		std::vector<Segment> left, right;	// right: after &>
		bool hasRight = false;
	};

	enum class Anchor { TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight, At };
	enum class Align { Left, Center, Right };

	struct SectionStyle
	{
		Anchor anchor = Anchor::TopLeft;
		float x = 0.f, y = 0.f;					// Anchor::At
		bool xPercent = false, yPercent = false;
		float offsetX = 0.f, offsetY = 0.f;
		float scale = 1.f;
		float background = 0.f;
		float padding = 4.f;
		float spacing = 0.f;
		float minWidth = 0.f;
		bool shadow = true;
		bool outline = false;
		Align align = Align::Left;
		Color color = 0xFFFFFFFF;
	};

	struct RenderedSection
	{
		SectionStyle style;
		std::vector<RenderedLine> lines;
		int line = 0;							// Of its header (0: the implicit first section)
	};

	// A parsed profile. Cheap to copy (the parsed form is shared and never changes).
	class Profile
	{
	public:
		Profile();
		static Profile Parse(std::string_view text);

		const std::vector<Error>& Errors() const;	// From parsing (the profile still renders what it could)
		std::size_t SectionCount() const;

		// The sections as they look now. `errors`: what went wrong evaluating (unknown variables, bad arguments, fmt
		// specs that don't fit), once each.
		std::vector<RenderedSection> Render(const Context& context, std::vector<Error>* errors = nullptr) const;

	private:
		struct Impl;
		std::shared_ptr<const Impl> impl_;
	};

	// One line on its own (no sections, no #if), in `color`.
	RenderedLine RenderLine(std::string_view line, const Context& context, Color color = 0xFFFFFFFF,
							std::vector<Error>* errors = nullptr);

	// ---- Helpers ---------------------------------------------------------------------------------------------------
	// `value` formatted like {x:spec} (fmt's format spec syntax). False and `error` set if the spec doesn't fit it.
	bool FormatValue(const Value& value, std::string_view spec, std::string& out, std::string& error);
	// "#rgb", "#rrggbb", "#rrggbbaa" or a built-in name ("red", "gray", ...). Not the host's names.
	bool ParseColor(std::string_view text, Color& out);
	std::string TimeText(double seconds, int decimals = 2);		// 62.345 -> "1:02.35"
	std::string PlainText(const std::vector<Segment>& segments);	// The text alone (bars as "[bar]")
	std::string PlainText(const RenderedLine& line);				// Left, then " | " and right if it has one
	const std::vector<std::string>& ColorNames();					// The built-in names
}
