#pragma once
// Enumerators by their lowercase names (magic_enum), for the file formats and messages whose words are exactly the
// enumerator names: mod.toml's phase = "early" is Phase::Early. One table both ways, so writing and reading can't
// drift apart. Renaming an enumerator renames its word in those files: the unit tests read and write them.
//
// Names whose words differ from the enumerators ("down" for Pressed, "on disk" for Persistent) keep their own switch.

#include <magic_enum/magic_enum.hpp>

#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace enums
{
	namespace detail
	{
		template <class E> const auto& Lower()
		{
			static const auto names = [] {
				std::array<std::string, magic_enum::enum_count<E>()> out;
				for (std::size_t i = 0; i < out.size(); ++i)
					for (const char c : magic_enum::enum_names<E>()[i])
						out[i] += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				return out;
			}();
			return names;
		}
	}

	// "early" for Phase::Early; "?" for a value that isn't one of the enumerators
	template <class E> const char* Name(E value)
	{
		const auto index = magic_enum::enum_index(value);
		return index ? detail::Lower<E>()[*index].c_str() : "?";
	}

	// The enumerator named exactly `text` (lowercase), if any
	template <class E> std::optional<E> Parse(std::string_view text)
	{
		const auto& names = detail::Lower<E>();
		for (std::size_t i = 0; i < names.size(); ++i)
			if (names[i] == text) return magic_enum::enum_value<E>(i);
		return std::nullopt;
	}

	// "early, engine or world": the accepted words, for an error message
	template <class E> std::string List()
	{
		const auto& names = detail::Lower<E>();
		std::string out;
		for (std::size_t i = 0; i < names.size(); ++i)
			out += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + names[i];
		return out;
	}
}
