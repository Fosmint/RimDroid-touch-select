// Shared by texcache.cpp (loose PNG/DDS files → cache entries) and assetconv.cpp (textures inside
// the game's own resources.assets): RGBA8 → ETC2 / RGBA32 mip chains via etcpak, BC → RGBA8 via bcdec.
#ifndef RIMDROID_TEXENC_H
#define RIMDROID_TEXENC_H
#include <cstdint>
#include <vector>

// UnityEngine.TextureFormat values; the cache header carries them and the bridge passes them on.
enum { FMT_ALPHA8 = 1, FMT_RGB24 = 3, FMT_RGBA32 = 4, FMT_DXT1 = 10, FMT_DXT3 = 11, FMT_DXT5 = 12,
       FMT_BC7 = 25, FMT_ETC2_RGB = 45, FMT_ETC2_RGBA8 = 47 };

// Up to this size a texture is stored uncompressed: UI icons and buttons stay pixel-exact (the
// game itself would have compressed them). 64 costs the whole game +16 MB (1135 textures); 128
// would be +91 MB for 1509 more, mostly item and pawn art that compresses fine. Also the only way
// for sizes that are not a multiple of 4, which Unity refuses in ETC2.
static const int RAW_MAX = 64;

// Whole chain, mip 0 first — the layout Texture2D.LoadRawTextureData and the serialized file both
// use. Picks RGBA32 (small / odd sizes), ETC2_RGB (every pixel opaque) or ETC2_RGBA8.
void rd_encode_chain(const uint32_t* rgba, int w, int h, bool mips, std::vector<uint8_t>& out, int& format, int& levels);

// Top level of a BC-compressed image (bc = 1, 2, 3 or 7) → straight RGBA8, w*h*4 bytes. Returns
// false when the block data is shorter than the level needs.
bool rd_decode_bc(int bc, const uint8_t* blocks, size_t len, int w, int h, std::vector<uint8_t>& rgba);

#endif
