#include "hud/template.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace hud
{
	// ---- Value --------------------------------------------------------------------------------------------------------
	Value Value::Num(double v)
	{
		Value out;
		out.number = v;
		return out;
	}

	Value Value::Str(std::string v, bool rich)
	{
		Value out;
		out.kind = Kind::Text;
		out.text = std::move(v);
		out.rich = rich;
		return out;
	}

	Value Value::Bool(bool v)
	{
		Value out;
		out.kind = Kind::Bool;
		out.number = v ? 1.0 : 0.0;
		return out;
	}

	double Value::AsNumber() const
	{
		if (kind != Kind::Text) return number;
		if (text.empty()) return 0.0;
		char* end = nullptr;
		const double v = std::strtod(text.c_str(), &end);
		return end && *end == '\0' ? v : 0.0;
	}

	bool Value::Truthy() const
	{
		return kind == Kind::Text ? !text.empty() : number != 0.0;
	}

	bool Context::Call(std::string_view, const std::vector<Value>&, Value&, std::string&) const { return false; }
	bool Context::NamedColor(std::string_view, Color&) const { return false; }

	namespace
	{
		// ---- Small helpers ------------------------------------------------------------------------------------------
		std::string_view Trim(std::string_view s)
		{
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
			return s;
		}

		std::string Lower(std::string_view s)
		{
			std::string out(s);
			for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return out;
		}

		std::vector<std::string_view> Split(std::string_view s, char by)
		{
			std::vector<std::string_view> out;
			std::size_t start = 0;
			for (std::size_t i = 0; i <= s.size(); ++i)
				if (i == s.size() || s[i] == by) {
					out.push_back(s.substr(start, i - start));
					start = i + 1;
				}
			return out;
		}

		std::vector<std::string_view> Words(std::string_view s)
		{
			std::vector<std::string_view> out;
			for (std::string_view w : Split(s, ' '))
				if (!Trim(w).empty()) out.push_back(Trim(w));
			return out;
		}

		bool ParseNumber(std::string_view s, double& out)
		{
			const std::string text(Trim(s));
			if (text.empty()) return false;
			char* end = nullptr;
			out = std::strtod(text.c_str(), &end);
			return end && *end == '\0' && std::isfinite(out);
		}

		int HexDigit(char c)
		{
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		}

		std::uint8_t Channel(double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }
		Color Pack(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
		{
			return (Color{r} << 24) | (Color{g} << 16) | (Color{b} << 8) | Color{a};
		}

		struct NamedEntry { const char* name; Color color; };
		const NamedEntry kNamed[] = {
			{"white", 0xFFFFFFFF},     {"black", 0x000000FF},      {"gray", 0xA0A0A8FF},      {"grey", 0xA0A0A8FF},
			{"lightgray", 0xD0D0D8FF}, {"darkgray", 0x606068FF},   {"red", 0xFF5050FF},       {"darkred", 0xB02020FF},
			{"orange", 0xFF9A30FF},    {"gold", 0xFFC830FF},       {"yellow", 0xFFF050FF},    {"lime", 0xA0FF40FF},
			{"green", 0x50E070FF},     {"darkgreen", 0x208040FF},  {"cyan", 0x40E8F0FF},      {"aqua", 0x60F0FFFF},
			{"lightblue", 0x80C0FFFF}, {"blue", 0x4080FFFF},       {"darkblue", 0x2040A0FF},  {"purple", 0xA060F0FF},
			{"magenta", 0xF050E0FF},   {"pink", 0xFF90C0FF},       {"brown", 0xA06830FF},     {"transparent", 0x00000000},
		};

		// ---- Syntax tree --------------------------------------------------------------------------------------------
		struct Template;

		struct Node
		{
			enum class Type { Literal, Variable, Unary, Binary, Call, Text };
			Type type = Type::Literal;
			Value literal;
			std::string name;							// Variable, function, operator
			std::vector<std::unique_ptr<Node>> args;
			std::shared_ptr<const Template> text;		// Type::Text: a string literal with {} in it
		};
		using NodePtr = std::unique_ptr<Node>;

		struct Piece
		{
			std::string text;		// Markup (when there is no expr)
			NodePtr expr;
			std::string spec;		// fmt format spec, after the ':'
		};

		struct Template
		{
			std::vector<Piece> pieces;
			bool Empty() const { return pieces.empty(); }
		};

		bool ParseTemplate(std::string_view s, Template& out, std::string& error);

		// ---- Expressions: tokens ------------------------------------------------------------------------------------
		struct Token
		{
			enum class Type { Number, String, Ident, Op, End };
			Type type = Type::End;
			std::string text;
			double number = 0.0;
		};

		bool Tokenize(std::string_view s, std::vector<Token>& out, std::string& error)
		{
			std::size_t i = 0;
			while (i < s.size()) {
				const char c = s[i];
				if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
				Token t;
				if (std::isdigit(static_cast<unsigned char>(c)) ||
					(c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
					std::size_t j = i;
					while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
					t.type = Token::Type::Number;
					t.text = std::string(s.substr(i, j - i));
					if (!ParseNumber(t.text, t.number)) {
						error = "'" + t.text + "' is not a number";
						return false;
					}
					i = j;
				} else if (c == '"') {
					std::size_t j = i + 1;
					bool closed = false;
					for (; j < s.size(); ++j) {
						if (s[j] == '\\' && j + 1 < s.size()) {
							const char e = s[j + 1];
							if (e == '"' || e == '\\') t.text += e;
							else if (e == 'n') t.text += '\n';
							else { t.text += '\\'; t.text += e; }
							++j;
						} else if (s[j] == '"') {
							closed = true;
							break;
						} else {
							t.text += s[j];
						}
					}
					if (!closed) {
						error = "a \" without its closing \"";
						return false;
					}
					t.type = Token::Type::String;
					i = j + 1;
				} else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
					std::size_t j = i;
					while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.')) ++j;
					t.type = Token::Type::Ident;
					t.text = std::string(s.substr(i, j - i));
					i = j;
				} else {
					static const char* const kTwo[] = {"&&", "||", "==", "!=", "<=", ">="};
					t.type = Token::Type::Op;
					for (const char* op : kTwo)
						if (s.substr(i, 2) == op) t.text = op;
					if (t.text.empty()) {
						if (std::string_view("+-*/%<>!(),").find(c) == std::string_view::npos) {
							error = std::string("unexpected '") + c + "'" + (c == '=' ? " (compare with ==)" : "");
							return false;
						}
						t.text = std::string(1, c);
					}
					i += t.text.size();
				}
				out.push_back(std::move(t));
			}
			out.push_back(Token{});		// End
			return true;
		}

		// ---- Expressions: parser (recursive descent, C-like precedence) ---------------------------------------------
		class Parser
		{
		public:
			explicit Parser(std::vector<Token> tokens) : t_(std::move(tokens)) {}

			NodePtr Parse(std::string& error)
			{
				NodePtr n = Or();
				if (n && t_[i_].type != Token::Type::End) Fail("unexpected '" + t_[i_].text + "'");
				if (!error_.empty()) {
					error = error_;
					return nullptr;
				}
				return n;
			}

		private:
			const Token& Peek() const { return t_[i_]; }
			bool IsOp(const char* op) const { return t_[i_].type == Token::Type::Op && t_[i_].text == op; }
			bool Accept(const char* op)
			{
				if (!IsOp(op)) return false;
				++i_;
				return true;
			}
			NodePtr Fail(std::string message)
			{
				if (error_.empty()) error_ = std::move(message);
				return nullptr;
			}

			static NodePtr Binary(std::string op, NodePtr l, NodePtr r)
			{
				auto n = std::make_unique<Node>();
				n->type = Node::Type::Binary;
				n->name = std::move(op);
				n->args.push_back(std::move(l));
				n->args.push_back(std::move(r));
				return n;
			}

			template <class Next>
			NodePtr Level(Next next, std::initializer_list<const char*> ops)
			{
				NodePtr l = (this->*next)();
				while (l) {
					const char* found = nullptr;
					for (const char* op : ops)
						if (IsOp(op)) found = op;
					if (!found) break;
					++i_;
					if (Peek().type == Token::Type::End) return Fail("something is missing after '" + std::string(found) + "'");
					NodePtr r = (this->*next)();
					if (!r) return Fail("something is missing after '" + std::string(found) + "'");
					l = Binary(found, std::move(l), std::move(r));
				}
				return l;
			}

			NodePtr Or() { return Level(&Parser::And, {"||"}); }
			NodePtr And() { return Level(&Parser::Equality, {"&&"}); }
			NodePtr Equality() { return Level(&Parser::Relational, {"==", "!="}); }
			NodePtr Relational() { return Level(&Parser::Additive, {"<", "<=", ">", ">="}); }
			NodePtr Additive() { return Level(&Parser::Multiplicative, {"+", "-"}); }
			NodePtr Multiplicative() { return Level(&Parser::Unary, {"*", "/", "%"}); }

			NodePtr Unary()
			{
				for (const char* op : {"-", "!", "+"}) {
					if (!Accept(op)) continue;
					if (Peek().type == Token::Type::End) return Fail("something is missing after '" + std::string(op) + "'");
					NodePtr operand = Unary();
					if (!operand) return Fail("something is missing after '" + std::string(op) + "'");
					auto n = std::make_unique<Node>();
					n->type = Node::Type::Unary;
					n->name = op;
					n->args.push_back(std::move(operand));
					return n;
				}
				return Primary();
			}

			NodePtr Primary()
			{
				const Token& t = Peek();
				auto n = std::make_unique<Node>();
				switch (t.type) {
					case Token::Type::Number:
						n->literal = Value::Num(t.number);
						++i_;
						return n;
					case Token::Type::String: {
						auto tmpl = std::make_shared<Template>();
						std::string error;
						if (!ParseTemplate(t.text, *tmpl, error)) return Fail("in \"" + t.text + "\": " + error);
						++i_;
						bool plain = true;
						for (const Piece& p : tmpl->pieces) plain = plain && !p.expr;
						if (plain) {	// No {} inside: a constant
							std::string text;
							for (const Piece& p : tmpl->pieces) text += p.text;
							n->literal = Value::Str(std::move(text), true);
						} else {
							n->type = Node::Type::Text;
							n->text = std::move(tmpl);
						}
						return n;
					}
					case Token::Type::Ident: {
						const std::string name = t.text;
						++i_;
						if (name == "true" || name == "false") {
							n->literal = Value::Bool(name == "true");
							return n;
						}
						if (!Accept("(")) {
							n->type = Node::Type::Variable;
							n->name = name;
							return n;
						}
						n->type = Node::Type::Call;
						n->name = Lower(name);
						if (!Accept(")")) {
							do {
								NodePtr arg = Or();
								if (!arg) return Fail("a missing argument in " + name + "()");
								n->args.push_back(std::move(arg));
							} while (Accept(","));
							if (!Accept(")")) return Fail(name + "( without its )");
						}
						return n;
					}
					case Token::Type::Op:
						if (Accept("(")) {
							NodePtr inner = Or();
							if (!inner) return Fail("nothing inside ( )");
							if (!Accept(")")) return Fail("a ( without its )");
							return inner;
						}
						return Fail("unexpected '" + t.text + "'");
					case Token::Type::End:
					default:
						return Fail("the expression ends too early");
				}
			}

			std::vector<Token> t_;
			std::size_t i_ = 0;
			std::string error_;
		};

		NodePtr ParseExpression(std::string_view s, std::string& error)
		{
			std::vector<Token> tokens;
			if (!Tokenize(s, tokens, error)) return nullptr;
			if (tokens.size() == 1) {
				error = "empty { }";
				return nullptr;
			}
			return Parser(std::move(tokens)).Parse(error);
		}

		// ---- Templates (a line, or a string literal's content) ------------------------------------------------------
		bool ParseTemplate(std::string_view s, Template& out, std::string& error)
		{
			std::string text;
			const auto flush = [&] {
				if (text.empty()) return;
				Piece p;
				p.text = std::move(text);
				out.pieces.push_back(std::move(p));
				text.clear();
			};
			std::size_t i = 0;
			while (i < s.size()) {
				const char c = s[i];
				if ((c == '{' || c == '}') && i + 1 < s.size() && s[i + 1] == c) {	// {{ and }}
					text += c;
					i += 2;
					continue;
				}
				if (c == '}') {
					error = "a } without its { (write }} for a literal one)";
					return false;
				}
				if (c != '{') {
					text += c;
					++i;
					continue;
				}

				// The matching }: not inside a string or parentheses. The first ':' at depth 0 starts the format spec.
				std::size_t j = i + 1, colon = std::string_view::npos;
				int depth = 0;
				bool inString = false, closed = false;
				for (; j < s.size(); ++j) {
					const char d = s[j];
					if (inString) {
						if (d == '\\') ++j;
						else if (d == '"') inString = false;
						continue;
					}
					if (d == '"') inString = true;
					else if (d == '(') ++depth;
					else if (d == ')') --depth;
					else if (d == ':' && depth == 0 && colon == std::string_view::npos) colon = j;
					else if (d == '}' && depth <= 0) { closed = true; break; }
				}
				if (!closed) {
					error = inString ? "a \" without its closing \"" : "a { without its } (write {{ for a literal one)";
					return false;
				}
				const std::size_t exprEnd = colon == std::string_view::npos ? j : colon;
				const std::string_view expr = s.substr(i + 1, exprEnd - i - 1);
				Piece p;
				p.expr = ParseExpression(expr, error);
				if (!p.expr) {
					error = "{" + std::string(Trim(expr)) + "}: " + error;
					return false;
				}
				if (colon != std::string_view::npos) p.spec = std::string(s.substr(colon + 1, j - colon - 1));
				flush();
				out.pieces.push_back(std::move(p));
				i = j + 1;
			}
			flush();
			return true;
		}

		// ---- Colors -------------------------------------------------------------------------------------------------
		bool ResolveColor(std::string_view name, const Context& ctx, Color& out)
		{
			name = Trim(name);
			if (!name.empty() && name.front() == '&') name.remove_prefix(1);		// "&#ff0000" from color()
			if (ParseColor(name, out)) return true;
			if (name.size() > 2 && name.front() == '(' && name.back() == ')') name = name.substr(1, name.size() - 2);
			return ParseColor(name, out) || ctx.NamedColor(Lower(name), out);
		}

		std::string ColorCode(Color c) { return fmt::format("&#{:08x}", c); }

		// ---- Rendering ----------------------------------------------------------------------------------------------
		void AddError(std::vector<Error>* errors, int line, std::string message)
		{
			if (!errors) return;
			Error e{line, std::move(message)};
			if (std::find(errors->begin(), errors->end(), e) == errors->end()) errors->push_back(std::move(e));
		}

		std::string EscapeMarkup(std::string_view s)
		{
			std::string out;
			for (char c : s) {
				out += c;
				if (c == '&') out += '&';
			}
			return out;
		}

		class Evaluator
		{
		public:
			explicit Evaluator(const Context& ctx) : ctx_(ctx) {}

			bool Run(const Node& n, Value& out, std::string& error)
			{
				switch (n.type) {
					case Node::Type::Literal:
						out = n.literal;
						return true;
					case Node::Type::Variable:
						if (ctx_.Variable(n.name, out)) return true;
						error = "unknown value '" + n.name + "'";
						return false;
					case Node::Type::Text:
						return Expand(*n.text, out, error);
					case Node::Type::Unary: {
						Value v;
						if (!Run(*n.args[0], v, error)) return false;
						if (n.name == "!") out = Value::Bool(!v.Truthy());
						else if (n.name == "-") out = Value::Num(-v.AsNumber());
						else out = Value::Num(v.AsNumber());
						return true;
					}
					case Node::Type::Binary:
						return RunBinary(n, out, error);
					case Node::Type::Call:
						return RunCall(n, out, error);
				}
				return false;
			}

			// A string literal with {} in it: the text, with its values filled in (still markup).
			bool Expand(const Template& t, Value& out, std::string& error)
			{
				std::string text;
				for (const Piece& p : t.pieces) {
					if (!p.expr) { text += p.text; continue; }
					Value v;
					if (!Run(*p.expr, v, error)) return false;
					std::string formatted;
					if (!FormatValue(v, p.spec, formatted, error)) return false;
					text += v.rich ? formatted : EscapeMarkup(formatted);
				}
				out = Value::Str(std::move(text), true);
				return true;
			}

		private:
			static bool Text(const Value& v, std::string& out, std::string& error) { return FormatValue(v, {}, out, error); }

			bool RunBinary(const Node& n, Value& out, std::string& error)
			{
				const std::string& op = n.name;
				Value l, r;
				if (!Run(*n.args[0], l, error)) return false;
				if (op == "&&" || op == "||") {		// Short-circuit
					if ((op == "&&") != l.Truthy()) {
						out = Value::Bool(l.Truthy());
						return true;
					}
					if (!Run(*n.args[1], r, error)) return false;
					out = Value::Bool(r.Truthy());
					return true;
				}
				if (!Run(*n.args[1], r, error)) return false;

				const bool lText = l.kind == Value::Kind::Text, rText = r.kind == Value::Kind::Text;
				if (op == "+" && (lText || rText)) {
					std::string a, b;
					if (!Text(l, a, error) || !Text(r, b, error)) return false;
					const bool rich = l.rich || r.rich;
					if (rich) {
						if (!l.rich) a = EscapeMarkup(a);
						if (!r.rich) b = EscapeMarkup(b);
					}
					out = Value::Str(a + b, rich);
					return true;
				}
				if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
					int cmp = 0;
					if (lText && rText) {
						cmp = l.text.compare(r.text);
					} else if ((lText || rText) && (op == "==" || op == "!=")) {
						out = Value::Bool(op == "!=");		// Text never equals a number
						return true;
					} else {
						const double a = l.AsNumber(), b = r.AsNumber();
						cmp = a < b ? -1 : a > b ? 1 : 0;
					}
					bool result = false;
					if (op == "==") result = cmp == 0;
					else if (op == "!=") result = cmp != 0;
					else if (op == "<") result = cmp < 0;
					else if (op == "<=") result = cmp <= 0;
					else if (op == ">") result = cmp > 0;
					else result = cmp >= 0;
					out = Value::Bool(result);
					return true;
				}
				const double a = l.AsNumber(), b = r.AsNumber();
				if (op == "+") out = Value::Num(a + b);
				else if (op == "-") out = Value::Num(a - b);
				else if (op == "*") out = Value::Num(a * b);
				else if (op == "/") out = Value::Num(a / b);
				else out = Value::Num(std::fmod(a, b));
				return true;
			}

			bool RunCall(const Node& n, Value& out, std::string& error)
			{
				const std::string& f = n.name;
				const std::size_t count = n.args.size();
				const auto arity = [&](std::size_t lo, std::size_t hi) {
					if (count >= lo && count <= hi) return true;
					error = f + "() takes " +
							(lo == hi ? std::to_string(lo) : std::to_string(lo) + " to " + std::to_string(hi)) +
							" argument" + (hi == 1 ? "" : "s") + ", not " + std::to_string(count);
					return false;
				};

				if (f == "if") {		// Only the chosen branch is evaluated
					if (!arity(2, 3)) return false;
					Value cond;
					if (!Run(*n.args[0], cond, error)) return false;
					if (cond.Truthy()) return Run(*n.args[1], out, error);
					if (count == 3) return Run(*n.args[2], out, error);
					out = Value::Str({});
					return true;
				}

				std::vector<Value> a(count);
				for (std::size_t i = 0; i < count; ++i)
					if (!Run(*n.args[i], a[i], error)) return false;
				const auto num = [&](std::size_t i) { return a[i].AsNumber(); };

				if (f == "abs" || f == "floor" || f == "ceil" || f == "sqrt" || f == "sign") {
					if (!arity(1, 1)) return false;
					const double x = num(0);
					out = Value::Num(f == "abs" ? std::fabs(x) : f == "floor" ? std::floor(x) : f == "ceil" ? std::ceil(x)
								   : f == "sqrt" ? std::sqrt(x) : static_cast<double>((x > 0) - (x < 0)));
					return true;
				}
				if (f == "min" || f == "max") {
					if (!arity(1, 64)) return false;
					double v = num(0);
					for (std::size_t i = 1; i < count; ++i) v = f == "min" ? std::min(v, num(i)) : std::max(v, num(i));
					out = Value::Num(v);
					return true;
				}
				if (f == "clamp") {
					if (!arity(3, 3)) return false;
					out = Value::Num(std::max(num(1), std::min(num(2), num(0))));
					return true;
				}
				if (f == "lerp") {
					if (!arity(3, 3)) return false;
					out = Value::Num(num(0) + (num(1) - num(0)) * num(2));
					return true;
				}
				if (f == "round") {
					if (!arity(1, 2)) return false;
					const double scale = std::pow(10.0, count == 2 ? std::clamp(std::round(num(1)), -12.0, 12.0) : 0.0);
					out = Value::Num(std::round(num(0) * scale) / scale);
					return true;
				}
				if (f == "time") {
					if (!arity(1, 2)) return false;
					out = Value::Str(TimeText(num(0), count == 2 ? static_cast<int>(num(1)) : 2));
					return true;
				}
				if (f == "str") {
					if (!arity(1, 2)) return false;
					std::string s;
					if (!FormatValue(a[0], count == 2 ? std::string_view(a[1].text) : std::string_view{}, s, error))
						return false;
					out = Value::Str(std::move(s), a[0].rich);
					return true;
				}
				if (f == "upper" || f == "lower" || f == "len") {
					if (!arity(1, 1)) return false;
					std::string s;
					if (!Text(a[0], s, error)) return false;
					if (f == "len") {
						out = Value::Num(static_cast<double>(s.size()));
						return true;
					}
					for (char& c : s)
						c = static_cast<char>(f == "upper" ? std::toupper(static_cast<unsigned char>(c))
														   : std::tolower(static_cast<unsigned char>(c)));
					out = Value::Str(std::move(s), a[0].rich);
					return true;
				}
				if (f == "color") {
					if (!arity(1, 1)) return false;
					Color c;
					if (!ColorArg(a[0], c, error)) return false;
					out = Value::Str(ColorCode(c), true);
					return true;
				}
				if (f == "rgb") {
					if (!arity(3, 4)) return false;
					out = Value::Str(ColorCode(Pack(Channel(num(0)), Channel(num(1)), Channel(num(2)),
													count == 4 ? Channel(num(3)) : 255)), true);
					return true;
				}
				if (f == "mix") {
					if (!arity(3, 3)) return false;
					Color c1, c2;
					if (!ColorArg(a[0], c1, error) || !ColorArg(a[1], c2, error)) return false;
					const double t = std::clamp(num(2), 0.0, 1.0);
					const auto ch = [&](int shift) {
						const double x = (c1 >> shift) & 0xFF, y = (c2 >> shift) & 0xFF;
						return Channel(x + (y - x) * t);
					};
					out = Value::Str(ColorCode(Pack(ch(24), ch(16), ch(8), ch(0))), true);
					return true;
				}
				if (f == "bar") {
					if (!arity(1, 3)) return false;
					const double fraction = std::clamp(std::isfinite(num(0)) ? num(0) : 0.0, 0.0, 1.0);
					const double width = count >= 2 ? std::clamp(num(1), 1.0, 2000.0) : 80.0;
					std::string color = "-";		// The text color
					if (count == 3) {
						Color c;
						if (!ColorArg(a[2], c, error)) return false;
						color = fmt::format("#{:08x}", c);
					}
					out = Value::Str(fmt::format("&[bar {:.4f} {:.0f} {}]", fraction, width, color), true);
					return true;
				}

				std::string hostError;
				if (ctx_.Call(f, a, out, hostError)) {
					if (hostError.empty()) return true;
					error = f + "(): " + hostError;
					return false;
				}
				error = "unknown function '" + f + "'";
				return false;
			}

			bool ColorArg(const Value& v, Color& out, std::string& error) const
			{
				if (v.kind == Value::Kind::Text && ResolveColor(v.text, ctx_, out)) return true;
				error = "'" + v.text + "' is not a color (#rrggbb, #rrggbbaa or a name like \"red\")";
				return false;
			}

			const Context& ctx_;
		};

		// Builds the segments of one line: markup applied, values filled in.
		class LineBuilder
		{
		public:
			LineBuilder(const Context& ctx, Color base, int line, std::vector<Error>* errors)
				: ctx_(ctx), base_(base), current_(base), lineNo_(line), errors_(errors) {}

			void Text(std::string_view text, Color color)
			{
				if (text.empty()) return;
				std::vector<Segment>& out = line_.hasRight ? line_.right : line_.left;
				if (!out.empty() && !out.back().bar && out.back().color == color) {
					out.back().text += text;
					return;
				}
				Segment s;
				s.text = std::string(text);
				s.color = color;
				out.push_back(std::move(s));
			}

			void Markup(std::string_view s)
			{
				std::string run;
				const auto flush = [&] {
					Text(run, current_);
					run.clear();
				};
				std::size_t i = 0;
				while (i < s.size()) {
					if (s[i] != '&' || i + 1 >= s.size()) {
						run += s[i++];
						continue;
					}
					const char next = s[i + 1];
					if (next == '&') {
						run += '&';
						i += 2;
					} else if (next == 'r') {
						flush();
						current_ = base_;
						i += 2;
					} else if (next == '>') {
						flush();
						line_.hasRight = true;		// A second &> stays in the right column
						i += 2;
					} else if (next == '#') {
						std::size_t digits = 0;
						while (digits < 8 && i + 2 + digits < s.size() && HexDigit(s[i + 2 + digits]) >= 0) ++digits;
						const std::size_t take = digits == 8 ? 8 : digits >= 6 ? 6 : 0;
						if (!take) {
							run += '&';
							++i;
							continue;
						}
						flush();
						ParseColor(s.substr(i + 1, take + 1), current_);
						i += 2 + take;
					} else if (next == '(') {
						const std::size_t close = s.find(')', i + 2);
						if (close == std::string_view::npos) {
							run += '&';
							++i;
							continue;
						}
						const std::string_view name = s.substr(i + 2, close - i - 2);
						Color c;
						flush();
						if (ResolveColor(name, ctx_, c)) current_ = c;
						else AddError(errors_, lineNo_, "unknown color '" + std::string(name) + "'");
						i = close + 1;
					} else if (next == '[') {
						const std::size_t close = s.find(']', i + 2);
						if (close == std::string_view::npos) {
							run += '&';
							++i;
							continue;
						}
						flush();
						Bar(s.substr(i + 2, close - i - 2));
						i = close + 1;
					} else {
						run += '&';
						++i;
					}
				}
				flush();
			}

			void Add(const hud::Piece& p, Evaluator& eval)
			{
				if (!p.expr) {
					Markup(p.text);
					return;
				}
				Value v;
				std::string error, formatted;
				if (!eval.Run(*p.expr, v, error) || !FormatValue(v, p.spec, formatted, error)) {
					AddError(errors_, lineNo_, error);
					Text("?", 0xFF5050FF);
					return;
				}
				if (v.rich) Markup(formatted);
				else Text(formatted, current_);
			}

			RenderedLine Take() { return std::move(line_); }

		private:
			// "bar <fraction> <width> <#color or ->", as bar() writes it
			void Bar(std::string_view spec)
			{
				const auto w = Words(spec);
				double fraction = 0.0, width = 80.0;
				if (w.empty() || w[0] != "bar" || (w.size() > 1 && !ParseNumber(w[1], fraction)) ||
					(w.size() > 2 && !ParseNumber(w[2], width))) {
					AddError(errors_, lineNo_, "a bad &[...]: write bars with bar(fraction, width, color)");
					return;
				}
				Segment s;
				s.bar = true;
				s.fraction = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
				s.width = static_cast<float>(std::clamp(width, 1.0, 2000.0));
				s.color = current_;
				if (w.size() > 3 && w[3] != "-") ResolveColor(w[3], ctx_, s.color);
				(line_.hasRight ? line_.right : line_.left).push_back(std::move(s));
			}

			const Context& ctx_;
			Color base_, current_;
			int lineNo_;
			std::vector<Error>* errors_;
			RenderedLine line_;
		};

		RenderedLine RenderTemplate(const Template& t, const Context& ctx, Color base, int line, std::vector<Error>* errors)
		{
			Evaluator eval(ctx);
			LineBuilder builder(ctx, base, line, errors);
			for (const Piece& p : t.pieces) builder.Add(p, eval);
			return builder.Take();
		}

		// ---- Profiles -----------------------------------------------------------------------------------------------
		struct Block;
		struct IfChain;

		struct Item
		{
			int line = 0;
			std::unique_ptr<Template> text;		// A line, or
			std::unique_ptr<IfChain> chain;		// #if ... #endif
		};

		struct Block
		{
			std::vector<Item> items;
		};

		struct IfChain
		{
			std::vector<NodePtr> conditions;
			std::vector<int> conditionLines;
			std::vector<Block> blocks;			// One per condition, and one more for #else
		};

		struct SectionDef
		{
			SectionStyle style;
			std::string colorName;				// Not a built-in color: asked from the host when rendering
			int line = 0;
			Block body;
		};

		bool ParseAnchor(std::string_view text, SectionStyle& style)
		{
			std::string s = Lower(Trim(text));
			for (char& c : s)
				if (c == '-' || c == '_') c = ' ';
			const auto words = Words(s);
			std::string joined;
			for (std::size_t i = 0; i < words.size(); ++i) joined += (i ? " " : "") + std::string(words[i]);

			if (!words.empty() && words[0] == "at") {
				if (words.size() != 3) return false;
				const auto coord = [](std::string_view w, float& v, bool& percent) {
					percent = !w.empty() && w.back() == '%';
					if (percent) w.remove_suffix(1);
					double d;
					if (!ParseNumber(w, d)) return false;
					v = static_cast<float>(d);
					return true;
				};
				style.anchor = Anchor::At;
				return coord(words[1], style.x, style.xPercent) && coord(words[2], style.y, style.yPercent);
			}
			static const std::pair<const char*, Anchor> kNames[] = {
				{"top left", Anchor::TopLeft},       {"topleft", Anchor::TopLeft},
				{"top", Anchor::Top},                {"top center", Anchor::Top},           {"top middle", Anchor::Top},
				{"top right", Anchor::TopRight},     {"topright", Anchor::TopRight},
				{"left", Anchor::Left},              {"middle left", Anchor::Left},         {"center left", Anchor::Left},
				{"center", Anchor::Center},          {"middle", Anchor::Center},
				{"right", Anchor::Right},            {"middle right", Anchor::Right},       {"center right", Anchor::Right},
				{"bottom left", Anchor::BottomLeft}, {"bottomleft", Anchor::BottomLeft},
				{"bottom", Anchor::Bottom},          {"bottom center", Anchor::Bottom},     {"bottom middle", Anchor::Bottom},
				{"bottom right", Anchor::BottomRight}, {"bottomright", Anchor::BottomRight},
			};
			for (const auto& [name, anchor] : kNames)
				if (joined == name) {
					style.anchor = anchor;
					return true;
				}
			return false;
		}

		// "scale 1.5", "offset 10 40", ... False with `error` set.
		bool ParseOption(std::string_view text, SectionDef& section, std::string& error)
		{
			const auto w = Words(text);
			if (w.empty()) {
				error = "an empty option (two commas in a row?)";
				return false;
			}
			const std::string key = Lower(w[0]);
			SectionStyle& st = section.style;
			const auto number = [&](std::size_t i, float& out, double lo, double hi) {
				double d;
				if (w.size() <= i || !ParseNumber(w[i], d) || d < lo || d > hi) {
					error = key + " needs a number" + (hi < 1e8 ? fmt::format(" from {} to {}", lo, hi) : std::string());
					return false;
				}
				out = static_cast<float>(d);
				return true;
			};
			const auto onOff = [&](bool& out) {
				const std::string v = w.size() > 1 ? Lower(w[1]) : "on";
				if (v == "on" || v == "true" || v == "yes") out = true;
				else if (v == "off" || v == "false" || v == "no") out = false;
				else {
					error = key + " is on or off";
					return false;
				}
				return true;
			};
			bool ok = true;
			std::size_t expected = 2;
			if (key == "scale") ok = number(1, st.scale, 0.1, 10.0);
			else if (key == "bg" || key == "background") ok = number(1, st.background, 0.0, 1.0);
			else if (key == "padding" || key == "pad") ok = number(1, st.padding, 0.0, 200.0);
			else if (key == "spacing") ok = number(1, st.spacing, -50.0, 200.0);
			else if (key == "width") ok = number(1, st.minWidth, 0.0, 10000.0);
			else if (key == "offset") {
				ok = number(1, st.offsetX, -1e9, 1e9) && number(2, st.offsetY, -1e9, 1e9);
				expected = 3;
			} else if (key == "shadow") ok = onOff(st.shadow), expected = w.size() > 1 ? 2 : 1;
			else if (key == "outline") ok = onOff(st.outline), expected = w.size() > 1 ? 2 : 1;
			else if (key == "align") {
				const std::string v = w.size() > 1 ? Lower(w[1]) : "";
				if (v == "left") st.align = Align::Left;
				else if (v == "center" || v == "middle") st.align = Align::Center;
				else if (v == "right") st.align = Align::Right;
				else {
					error = "align is left, center or right";
					return false;
				}
			} else if (key == "color" || key == "colour") {
				if (w.size() < 2) {
					error = "color needs a color (#rrggbb or a name)";
					return false;
				}
				if (!ParseColor(w[1], st.color)) section.colorName = Lower(w[1]);
			} else {
				error = "unknown option '" + std::string(w[0]) +
						"' (scale, bg, padding, spacing, width, offset, align, color, shadow, outline)";
				return false;
			}
			if (ok && w.size() > expected) {
				error = "too much after " + key + ": '" + std::string(text) + "'";
				return false;
			}
			return ok;
		}
	} // namespace

	// ---- Profile ----------------------------------------------------------------------------------------------------
	struct Profile::Impl
	{
		std::vector<SectionDef> sections;
		std::vector<Error> errors;
	};

	Profile::Profile() : impl_(std::make_shared<Impl>()) {}

	Profile Profile::Parse(std::string_view text)
	{
		auto impl = std::make_shared<Impl>();
		struct Open { IfChain* chain; int line; bool sawElse; };
		std::vector<Open> open;
		const auto error = [&](int line, std::string message) { impl->errors.push_back({line, std::move(message)}); };
		const auto closeSection = [&] {
			for (const Open& o : open) error(o.line, "#if without #endif");
			open.clear();
			if (impl->sections.empty()) return;
			auto& items = impl->sections.back().body.items;		// Blank lines at the end are just the gap before the next one
			while (!items.empty() && items.back().text && items.back().text->Empty()) items.pop_back();
		};
		const auto current = [&]() -> Block& {
			if (impl->sections.empty()) impl->sections.emplace_back();		// Implicit top left section
			return open.empty() ? impl->sections.back().body : open.back().chain->blocks.back();
		};

		const auto lines = Split(text, '\n');
		for (std::size_t n = 0; n < lines.size(); ++n) {
			const int lineNo = static_cast<int>(n) + 1;
			std::string_view line = lines[n];
			if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
			while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
			const std::string_view trimmed = Trim(line);

			if (trimmed.substr(0, 2) == "//") continue;

			// == section ==
			if (trimmed.size() >= 4 && trimmed.substr(0, 2) == "==" && trimmed.substr(trimmed.size() - 2) == "==") {
				closeSection();
				SectionDef section;
				section.line = lineNo;
				const auto parts = Split(Trim(trimmed.substr(2, trimmed.size() - 4)), ',');
				for (std::size_t i = 0; i < parts.size(); ++i) {
					if (i == 0 && Trim(parts[0]).empty() && parts.size() == 1) break;		// "====": top left
					if (i == 0 && ParseAnchor(parts[0], section.style)) continue;
					std::string why;
					if (!ParseOption(parts[i], section, why)) {
						if (i == 0)
							why = "'" + std::string(Trim(parts[0])) +
								  "' is not a place (top left, top, top right, left, center, right, bottom left, bottom, "
								  "bottom right, at <x> <y>) or an option";
						error(lineNo, why);
					}
				}
				impl->sections.push_back(std::move(section));
				continue;
			}

			// #if / #elif / #else / #endif
			if (!trimmed.empty() && trimmed.front() == '#') {
				const std::string_view rest = trimmed.substr(1);
				const std::size_t space = rest.find_first_of(" \t");
				const std::string word = Lower(rest.substr(0, space));
				const std::string_view arg = space == std::string_view::npos ? std::string_view{} : Trim(rest.substr(space));
				if (word == "if" || word == "elif") {
					std::string why;
					NodePtr cond = arg.empty() ? nullptr : ParseExpression(arg, why);
					if (arg.empty()) why = "#" + word + " needs a condition";
					if (!cond) {
						error(lineNo, "#" + word + ": " + why);
						auto never = std::make_unique<Node>();		// Keep the structure: an always-false branch
						never->literal = Value::Bool(false);
						cond = std::move(never);
					}
					if (word == "if") {
						Item item;
						item.line = lineNo;
						item.chain = std::make_unique<IfChain>();
						IfChain* chain = item.chain.get();
						current().items.push_back(std::move(item));
						chain->conditions.push_back(std::move(cond));
						chain->conditionLines.push_back(lineNo);
						chain->blocks.emplace_back();
						open.push_back({chain, lineNo, false});
					} else if (open.empty()) {
						error(lineNo, "#elif without #if");
					} else if (open.back().sawElse) {
						error(lineNo, "#elif after #else");
					} else {
						open.back().chain->conditions.push_back(std::move(cond));
						open.back().chain->conditionLines.push_back(lineNo);
						open.back().chain->blocks.emplace_back();
					}
					continue;
				}
				if (word == "else") {
					if (open.empty()) error(lineNo, "#else without #if");
					else if (open.back().sawElse) error(lineNo, "a second #else");
					else {
						open.back().chain->blocks.emplace_back();
						open.back().sawElse = true;
					}
					continue;
				}
				if (word == "endif") {
					if (open.empty()) error(lineNo, "#endif without #if");
					else open.pop_back();
					continue;
				}
				// Anything else starting with # is text ("#1 place")
			}

			if (impl->sections.empty() && trimmed.empty()) continue;	// Blank lines before anything
			auto tmpl = std::make_unique<Template>();
			std::string why;
			if (!ParseTemplate(line, *tmpl, why)) {
				error(lineNo, why);
				continue;
			}
			Item item;
			item.line = lineNo;
			item.text = std::move(tmpl);
			current().items.push_back(std::move(item));
		}
		closeSection();

		Profile p;
		p.impl_ = std::move(impl);
		return p;
	}

	const std::vector<Error>& Profile::Errors() const { return impl_->errors; }
	std::size_t Profile::SectionCount() const { return impl_->sections.size(); }

	namespace
	{
		void RenderBlock(const Block& block, const Context& ctx, Color base, std::vector<RenderedLine>& out,
						 std::vector<Error>* errors)
		{
			for (const Item& item : block.items) {
				if (item.text) {
					out.push_back(RenderTemplate(*item.text, ctx, base, item.line, errors));
					continue;
				}
				const IfChain& chain = *item.chain;
				const Block* chosen = nullptr;
				Evaluator eval(ctx);
				for (std::size_t i = 0; i < chain.conditions.size() && !chosen; ++i) {
					Value v;
					std::string error;
					if (!eval.Run(*chain.conditions[i], v, error)) AddError(errors, chain.conditionLines[i], error);
					else if (v.Truthy()) chosen = &chain.blocks[i];
				}
				if (!chosen && chain.blocks.size() > chain.conditions.size()) chosen = &chain.blocks.back();
				if (chosen) RenderBlock(*chosen, ctx, base, out, errors);
			}
		}
	}

	std::vector<RenderedSection> Profile::Render(const Context& context, std::vector<Error>* errors) const
	{
		std::vector<RenderedSection> out;
		out.reserve(impl_->sections.size());
		for (const SectionDef& def : impl_->sections) {
			RenderedSection rs;
			rs.style = def.style;
			rs.line = def.line;
			if (!def.colorName.empty() && !ResolveColor(def.colorName, context, rs.style.color))
				AddError(errors, def.line, "unknown color '" + def.colorName + "'");
			RenderBlock(def.body, context, rs.style.color, rs.lines, errors);
			out.push_back(std::move(rs));
		}
		return out;
	}

	RenderedLine RenderLine(std::string_view line, const Context& context, Color color, std::vector<Error>* errors)
	{
		Template t;
		std::string why;
		if (!ParseTemplate(line, t, why)) {
			AddError(errors, 1, why);
			return {};
		}
		return RenderTemplate(t, context, color, 1, errors);
	}

	// ---- Helpers ----------------------------------------------------------------------------------------------------
	bool FormatValue(const Value& value, std::string_view spec, std::string& out, std::string& error)
	{
		const char type = spec.empty() ? '\0' : spec.back();
		const bool integerType = type != '\0' && std::string_view("dxXobBc").find(type) != std::string_view::npos;
		try {
			if (spec.empty()) {
				switch (value.kind) {
					case Value::Kind::Text: out = value.text; return true;
					case Value::Kind::Bool: out = value.number != 0.0 ? "true" : "false"; return true;
					case Value::Kind::Number: {
						const double v = value.number;
						if (std::isfinite(v) && std::floor(v) == v && std::fabs(v) < 1e15)
							out = fmt::format("{}", static_cast<long long>(v));
						else
							out = fmt::format("{:.2f}", v);
						return true;
					}
				}
			}
			const std::string pattern = "{:" + std::string(spec) + "}";
			if (value.kind == Value::Kind::Text) {
				const std::string& s = value.text;
				out = fmt::vformat(pattern, fmt::make_format_args(s));
			} else if (value.kind == Value::Kind::Bool && !integerType) {
				const std::string s = value.number != 0.0 ? "true" : "false";
				out = fmt::vformat(pattern, fmt::make_format_args(s));
			} else if (integerType) {
				if (!std::isfinite(value.number)) {
					error = "{:" + std::string(spec) + "} needs a whole number, the value is " + fmt::format("{}", value.number);
					return false;
				}
				const long long i = std::llround(value.number);
				out = fmt::vformat(pattern, fmt::make_format_args(i));
			} else {
				const double d = value.number;
				out = fmt::vformat(pattern, fmt::make_format_args(d));
			}
			return true;
		} catch (const fmt::format_error& e) {
			error = "format spec '" + std::string(spec) + "': " + e.what();
			return false;
		}
	}

	bool ParseColor(std::string_view text, Color& out)
	{
		text = Trim(text);
		if (!text.empty() && text.front() == '#') {
			const std::string_view hex = text.substr(1);
			for (char c : hex)
				if (HexDigit(c) < 0) return false;
			const auto pair = [&](std::size_t i) { return static_cast<std::uint8_t>(HexDigit(hex[i]) * 16 + HexDigit(hex[i + 1])); };
			const auto single = [&](std::size_t i) { return static_cast<std::uint8_t>(HexDigit(hex[i]) * 17); };
			if (hex.size() == 3) out = Pack(single(0), single(1), single(2), 255);
			else if (hex.size() == 6) out = Pack(pair(0), pair(2), pair(4), 255);
			else if (hex.size() == 8) out = Pack(pair(0), pair(2), pair(4), pair(6));
			else return false;
			return true;
		}
		const std::string name = Lower(text);
		for (const NamedEntry& e : kNamed)
			if (name == e.name) {
				out = e.color;
				return true;
			}
		return false;
	}

	std::string TimeText(double seconds, int decimals)
	{
		if (!std::isfinite(seconds)) return "-:--";
		decimals = std::clamp(decimals, 0, 3);
		const bool negative = seconds < 0.0;
		const long long scale = decimals == 0 ? 1 : decimals == 1 ? 10 : decimals == 2 ? 100 : 1000;
		const long long total = std::llround(std::fabs(seconds) * static_cast<double>(scale));
		const long long whole = total / scale, fraction = total % scale;
		const long long h = whole / 3600, m = (whole / 60) % 60, s = whole % 60;
		std::string out = negative ? "-" : "";
		out += h > 0 ? fmt::format("{}:{:02}:{:02}", h, m, s) : fmt::format("{}:{:02}", m, s);
		if (decimals > 0) out += fmt::format(".{:0{}}", fraction, decimals);
		return out;
	}

	std::string PlainText(const std::vector<Segment>& segments)
	{
		std::string out;
		for (const Segment& s : segments) out += s.bar ? "[bar]" : s.text;
		return out;
	}

	std::string PlainText(const RenderedLine& line)
	{
		return line.hasRight ? PlainText(line.left) + " | " + PlainText(line.right) : PlainText(line.left);
	}

	const std::vector<std::string>& ColorNames()
	{
		static const std::vector<std::string> names = [] {
			std::vector<std::string> v;
			for (const NamedEntry& e : kNamed) v.emplace_back(e.name);
			return v;
		}();
		return names;
	}
}
