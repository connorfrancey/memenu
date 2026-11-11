#include "runtime/semver.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>

namespace mirage::semver
{
	namespace
	{
		bool Fail(std::string* error, std::string message)
		{
			if (error) *error = std::move(message);
			return false;
		}

		bool Number(std::string_view s, std::uint32_t& out)
		{
			if (s.empty() || (s.size() > 1 && s[0] == '0')) return false;	// No leading zeros
			const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
			return ec == std::errc() && end == s.data() + s.size();
		}

		bool Identifiers(std::string_view s, bool numericNoZeros)
		{
			if (s.empty()) return false;
			std::size_t start = 0;
			while (start <= s.size()) {
				const std::size_t dot = s.find('.', start);
				const std::string_view id = s.substr(start, dot == std::string_view::npos ? s.npos : dot - start);
				if (id.empty()) return false;
				bool numeric = true;
				for (char c : id) {
					if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-')) return false;
					numeric = numeric && c >= '0' && c <= '9';
				}
				if (numericNoZeros && numeric && id.size() > 1 && id[0] == '0') return false;
				if (dot == std::string_view::npos) break;
				start = dot + 1;
			}
			return true;
		}

		bool IsNumeric(std::string_view id)
		{
			return !id.empty() && std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
		}

		std::string_view Trim(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
			return s;
		}

		// A version where parts may be missing or wildcards ("1.2", "1.x", "*"): how many parts were given
		struct Partial
		{
			Version v;
			int parts = 0;		// 0..3
		};
		bool ParsePartial(std::string_view s, Partial& out, std::string* error)
		{
			if (s.empty() || s == "*" || s == "x" || s == "X") {
				out = {};
				return true;
			}
			if (s[0] == 'v') s.remove_prefix(1);
			std::string_view core = s, rest;
			if (const auto at = s.find_first_of("-+"); at != std::string_view::npos) {
				core = s.substr(0, at);
				rest = s.substr(at);
			}
			std::uint32_t* fields[] = {&out.v.major, &out.v.minor, &out.v.patch};
			out.parts = 0;
			std::size_t start = 0;
			for (int i = 0; i < 3 && start <= core.size(); ++i) {
				const std::size_t dot = core.find('.', start);
				const std::string_view part = core.substr(start, dot == std::string_view::npos ? core.npos : dot - start);
				if (part == "*" || part == "x" || part == "X") break;
				if (!Number(part, *fields[i])) return Fail(error, "'" + std::string(s) + "' is not a version");
				out.parts = i + 1;
				if (dot == std::string_view::npos) {
					start = core.size() + 1;
					break;
				}
				start = dot + 1;
				if (i == 2) return Fail(error, "'" + std::string(s) + "' has more than three numbers");
			}
			if (!rest.empty()) {
				if (out.parts < 3) return Fail(error, "'" + std::string(s) + "': a pre-release needs all three numbers");
				const auto full = Version::Parse(s, error);
				if (!full) return false;
				out.v = *full;
			}
			return true;
		}
	}

	std::optional<Version> Version::Parse(std::string_view text, std::string* error)
	{
		text = Trim(text);
		Version v;
		std::string_view core = text;
		if (const auto plus = core.find('+'); plus != std::string_view::npos) {
			v.build = std::string(core.substr(plus + 1));
			core = core.substr(0, plus);
			if (!Identifiers(v.build, false)) {
				Fail(error, "'" + std::string(text) + "': bad build metadata");
				return std::nullopt;
			}
		}
		if (const auto dash = core.find('-'); dash != std::string_view::npos) {
			v.pre = std::string(core.substr(dash + 1));
			core = core.substr(0, dash);
			if (!Identifiers(v.pre, true)) {
				Fail(error, "'" + std::string(text) + "': bad pre-release");
				return std::nullopt;
			}
		}
		const auto d1 = core.find('.'), d2 = d1 == std::string_view::npos ? d1 : core.find('.', d1 + 1);
		if (d1 == std::string_view::npos || d2 == std::string_view::npos || core.find('.', d2 + 1) != std::string_view::npos ||
			!Number(core.substr(0, d1), v.major) || !Number(core.substr(d1 + 1, d2 - d1 - 1), v.minor) ||
			!Number(core.substr(d2 + 1), v.patch)) {
			Fail(error, "'" + std::string(text) + "' is not a version (MAJOR.MINOR.PATCH, like 1.0.0)");
			return std::nullopt;
		}
		return v;
	}

	std::string Version::ToString() const
	{
		std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
		if (!pre.empty()) s += "-" + pre;
		if (!build.empty()) s += "+" + build;
		return s;
	}

	int Compare(const Version& a, const Version& b)
	{
		if (a.major != b.major) return a.major < b.major ? -1 : 1;
		if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
		if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
		if (a.pre == b.pre) return 0;
		if (a.pre.empty()) return 1;		// A release is after its pre-releases
		if (b.pre.empty()) return -1;
		std::string_view x = a.pre, y = b.pre;
		while (true) {
			const auto dx = x.find('.'), dy = y.find('.');
			const std::string_view ix = x.substr(0, dx), iy = y.substr(0, dy);
			if (ix != iy) {
				const bool nx = IsNumeric(ix), ny = IsNumeric(iy);
				if (nx && ny) {
					if (ix.size() != iy.size()) return ix.size() < iy.size() ? -1 : 1;
					return ix < iy ? -1 : 1;
				}
				if (nx != ny) return nx ? -1 : 1;	// Numeric identifiers sort first
				return ix < iy ? -1 : 1;
			}
			if (dx == std::string_view::npos || dy == std::string_view::npos) {
				if (dx == dy) return 0;
				return dx == std::string_view::npos ? -1 : 1;	// Fewer identifiers sort first
			}
			x.remove_prefix(dx + 1);
			y.remove_prefix(dy + 1);
		}
	}

	Range Range::Any()
	{
		Range r;
		r.sets_.push_back({});
		r.text_ = "*";
		return r;
	}

	bool Range::AddComparator(std::string_view token, std::vector<Comparator>& out, std::string* error)
	{
		const auto upper = [](Version v, int parts) {	// The first version past a partial one
			if (parts <= 1) return Version{v.major + 1, 0, 0, {}, {}};
			if (parts == 2) return Version{v.major, v.minor + 1, 0, {}, {}};
			return v;
		};
		std::string_view rest = token;
		auto take = [&](std::string_view prefix) {
			if (rest.substr(0, prefix.size()) != prefix) return false;
			rest.remove_prefix(prefix.size());
			rest = Trim(rest);
			return true;
		};
		Partial p;
		if (take("^")) {
			if (!ParsePartial(rest, p, error)) return false;
			if (p.parts == 0) return true;
			Version hi;
			if (p.v.major > 0 || p.parts == 1) hi = {p.v.major + 1, 0, 0, {}, {}};
			else if (p.v.minor > 0 || p.parts == 2) hi = {0, p.v.minor + 1, 0, {}, {}};
			else hi = {0, 0, p.v.patch + 1, {}, {}};
			out.push_back({Op::Ge, p.v});
			out.push_back({Op::Lt, hi});
			return true;
		}
		if (take("~")) {
			if (!ParsePartial(rest, p, error)) return false;
			if (p.parts == 0) return true;
			out.push_back({Op::Ge, p.v});
			out.push_back({Op::Lt, upper(p.v, p.parts == 1 ? 1 : 2)});
			return true;
		}
		for (const auto& [text, op] : {std::pair{">=", Op::Ge}, {"<=", Op::Le}, {">", Op::Gt}, {"<", Op::Lt}}) {
			if (!take(text)) continue;
			if (!ParsePartial(rest, p, error)) return false;
			if (p.parts == 0) {
				if (op == Op::Lt || op == Op::Gt) return Fail(error, "'" + std::string(token) + "' matches nothing");
				return true;
			}
			if (p.parts == 3) out.push_back({op, p.v});
			else if (op == Op::Ge) out.push_back({Op::Ge, p.v});				// >=1.2: from 1.2.0
			else if (op == Op::Lt) out.push_back({Op::Lt, p.v});				// <1.2: before 1.2.0
			else if (op == Op::Gt) out.push_back({Op::Ge, upper(p.v, p.parts)});	// >1.2: from 1.3.0
			else out.push_back({Op::Lt, upper(p.v, p.parts)});					// <=1.2: before 1.3.0
			return true;
		}
		take("=");
		if (!ParsePartial(rest, p, error)) return false;
		if (p.parts == 0) return true;
		if (p.parts == 3) {
			out.push_back({Op::Eq, p.v});
		} else {	// 1.2 = 1.2.x
			out.push_back({Op::Ge, p.v});
			out.push_back({Op::Lt, upper(p.v, p.parts)});
		}
		return true;
	}

	std::optional<Range> Range::Parse(std::string_view text, std::string* error)
	{
		Range r;
		r.text_ = std::string(Trim(text));
		std::string_view s = r.text_;
		if (s.empty()) {
			Fail(error, "an empty version range (use \"*\" for any version)");
			return std::nullopt;
		}
		while (true) {
			const auto bar = s.find("||");
			std::string_view part = Trim(s.substr(0, bar));
			std::vector<Comparator> set;
			// Tokens split by spaces and commas; an operator alone (">= 1.2") joins the next token
			std::string pending;
			std::size_t i = 0;
			bool any = false;
			while (i <= part.size()) {
				std::size_t j = part.find_first_of(" ,\t", i);
				if (j == std::string_view::npos) j = part.size();
				const std::string_view token = part.substr(i, j - i);
				i = j + 1;
				if (token.empty()) continue;
				if (token.find_first_not_of("<>=^~") == std::string_view::npos) {
					pending += token;
					continue;
				}
				const std::string full = pending + std::string(token);
				pending.clear();
				if (!AddComparator(full, set, error)) return std::nullopt;
				any = true;
			}
			if (!pending.empty()) {
				Fail(error, "'" + pending + "' has no version after it");
				return std::nullopt;
			}
			if (!any) {
				Fail(error, "an empty part in '" + r.text_ + "'");
				return std::nullopt;
			}
			r.sets_.push_back(std::move(set));
			if (bar == std::string_view::npos) break;
			s.remove_prefix(bar + 2);
		}
		return r;
	}

	bool Range::Matches(const Version& v) const
	{
		for (const auto& set : sets_) {
			bool all = true;
			for (const Comparator& c : set) {
				const int cmp = Compare(v, c.v);
				const bool ok = c.op == Op::Eq ? cmp == 0 : c.op == Op::Lt ? cmp < 0 : c.op == Op::Le ? cmp <= 0
							  : c.op == Op::Gt ? cmp > 0 : cmp >= 0;
				if (!ok) {
					all = false;
					break;
				}
			}
			if (all) return true;
		}
		return false;
	}
}
