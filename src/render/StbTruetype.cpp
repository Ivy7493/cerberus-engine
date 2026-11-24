// stb_truetype's implementation, alone in its own translation unit so the
// build can compile it optimized even in Debug (build.zig): rasterizing an
// atlas unoptimized took ~80ms, a visible hitch whenever text met a new size.
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <cstddef>
#include <cstdint>

namespace eng {

// Coverage → white RGBA with that alpha (font atlases go through the sprite
// pipeline). Here for the same reason: a 2048² atlas took ~125ms unoptimized.
void alphaToRgba(const uint8_t* alpha, uint8_t* rgba, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = alpha[i];
    }
}

}  // namespace eng
