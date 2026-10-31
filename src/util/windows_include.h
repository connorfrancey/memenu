#pragma once

#ifdef _WIN_32
	// System is Windows. Include Windows.h
	#include <Windows.h>
#elif __linux__
	// System is linux, don't include any windows libs.
#endif