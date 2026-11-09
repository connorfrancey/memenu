#pragma once
#include <cstddef>

// The example macros of examples/replays/, built into the DLL (cmake/Examples.cmake generates the data).
//
//     for (const auto& f : tas::examples::All()) write(f.name, f.data, f.size);
namespace tas::examples {

struct File {
    const char* name;                 // file name, "showcase.json"
    const unsigned char* data;
    std::size_t size;
};

const File* begin();
const File* end();

struct Range {
    const File* begin() const { return examples::begin(); }
    const File* end() const { return examples::end(); }
};
inline Range All() { return {}; }

} // namespace tas::examples
