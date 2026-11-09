#pragma once
#include "hud/template.h"

#include <cstddef>
#include <string>
#include <vector>

// Info Lines (info_lines.cpp): what it drew last, for the self-test. Any thread.
namespace info_lines
{
	struct Status
	{
		unsigned long long frames = 0;		// Frames it has drawn since it was turned on
		std::size_t sections = 0, lines = 0;	// In the last frame
		std::vector<hud::Error> errors;		// Of the profile: parsing, and the last frame's evaluation
		std::string profile;				// Its name
	};
	Status LastStatus();

	// `line` rendered (as plain text) with the values of the last frame drawn. Errors go to `errors`.
	std::string Evaluate(const std::string& line, std::vector<hud::Error>* errors = nullptr);

	// The editor's folded parts (Values, Functions, Formatting) drawn open, so the self-test runs all of its UI.
	void SetEditorOpenForTest(bool open);
	unsigned long long EditorFrames();		// Frames its editor was drawn in (never reset)
}
