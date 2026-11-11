// The stb implementations used by texture replacement (runtime/texture_files.cpp): image decoding and DXT encoding.
// Their own code: warnings off.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb_image.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
