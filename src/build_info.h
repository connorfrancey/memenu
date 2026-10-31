#pragma once

// Identifies the build. Defined in <build dir>/generated/build_info.cpp, regenerated on every build by
// cmake/BuildInfo.cmake, and written in the header of every log (logging::Init, format described in log.cpp).
namespace build_info {
    extern const unsigned kNumber;              // Build number: +1 on every build (CI: the workflow run number)
    extern const unsigned long long kMagic;     // Hash of kDetails: same magic = same code built the same way
    extern const char* const kMagicHex;         // kMagic as 16 hex digits
    extern const char* const kId;               // Random UUID, new on every build
    extern const char* const kVersion;          // MEMENU_VERSION
    extern const char* const kGitCommit;        // Short hash, "-dirty" with uncommitted changes, "unknown" without git
    extern const char* const kTimestamp;        // When it was built, UTC ISO 8601 ("2026-10-31T14:03:22Z")
    extern const char* const kConfig;           // Debug / Release / ...
    extern const char* const kCompiler;         // "MSVC 19.44.35211.0"
    extern const char* const kOptions;          // "game=1 optimizations=1 minimal=0"
    extern const char* const kFeatures;         // Enabled feature modules, space separated
    extern const char* const kDetails;          // "key=value" lines the magic is the hash of
}
