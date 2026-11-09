#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// str::Intern: one shared copy of each distinct string, for strings used over and over (object and class names, FName
// texts, setting and module names, script event names).
//
//   str::Atom a = str::Intern("TdPlayerPawn");
//   str::Atom b = str::Intern(someStdString);
//   if (a == b) ...                     // one pointer compare, no strcmp
//   std::unordered_map<str::Atom, int> byName;   // hashed by pointer
//
// An Atom is a pointer-sized handle to text that lives as long as the process (interned strings are never freed, so
// intern names, not arbitrary data). Interning is thread-safe and doesn't allocate when the string is already known.
// The empty string is the default Atom. Interning never depends on MEMENU_OPTIMIZATIONS: Atoms compare by identity,
// so turning the sharing off would make equal strings unequal.
namespace str
{
	class Atom
	{
	public:
		Atom() = default;		// The empty string

		const char* c_str() const { return text_ ? text_ : ""; }
		std::string_view view() const;
		std::size_t size() const { return view().size(); }
		bool empty() const { return !text_ || !*text_; }
		std::string str() const { return std::string(view()); }

		bool operator==(const Atom& o) const { return text_ == o.text_ || (empty() && o.empty()); }
		bool operator!=(const Atom& o) const { return !(*this == o); }
		// An order for sorted containers (by address: stable, not alphabetical).
		bool operator<(const Atom& o) const { return std::less<const char*>{}(text_, o.text_); }

		const void* id() const { return empty() ? nullptr : text_; }

	private:
		friend Atom Intern(std::string_view);
		friend Atom Find(std::string_view);
		explicit Atom(const char* text) : text_(text) {}
		const char* text_ = nullptr;		// Null-terminated, length stored just before it
	};

	Atom Intern(std::string_view text);
	inline Atom Intern(const char* text) { return Intern(std::string_view(text ? text : "")); }
	inline Atom Intern(const std::string& text) { return Intern(std::string_view(text)); }

	// The Atom for `text` if it was interned already, else the empty Atom (doesn't add it).
	Atom Find(std::string_view text);

	struct Stats
	{
		std::size_t strings = 0;		// Distinct strings interned
		std::size_t bytes = 0;			// Their text, terminators and lengths included
		std::uint64_t lookups = 0, added = 0;
	};
	Stats GetStats();
}

namespace std
{
	template <>
	struct hash<str::Atom>
	{
		std::size_t operator()(const str::Atom& a) const noexcept { return std::hash<const void*>{}(a.id()); }
	};
}
