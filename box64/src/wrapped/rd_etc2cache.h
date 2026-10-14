#ifndef RD_ETC2CACHE_H
#define RD_ETC2CACHE_H
// RimDroid: on-disk cache of the S3TC->ETC2 transcode output (see rd_etc2cache.c). On MobileGlues
// every launch re-encodes ~29k texture uploads (10 s of the 25 s to the main menu on a Galaxy S25,
// 13-17 s of encoding alone on Dimensity 7300 phones); the cache hands back the ETC2 blocks from the
// second launch on. Content-addressed: the key is a hash of the source DXT bytes plus format and
// size, so nothing has to be invalidated when the game, its mods or the texture tier change.
// RimDroid-fork-only, never for upstream.
#include <stdint.h>
#include <stddef.h>

// True when the cache is usable: RIMDROID_GLT_ETC2_CACHE is not "0" and RIMDROID_CACHE_DIR is set
// and writable. Opens the cache file on first use.
int rd_etc2cache_on(void);

// Look up the ETC2 blocks for one DXT upload. Returns a thread-local buffer (valid until the next
// call on the same thread) and writes its size to out_sz, or NULL on a miss.
const void* rd_etc2cache_get(const void* src, size_t srcsize, uint32_t fmt, uint32_t efmt,
                             int32_t w, int32_t h, size_t* out_sz);

// Store the ETC2 blocks encoded for one DXT upload.
void rd_etc2cache_put(const void* src, size_t srcsize, uint32_t fmt, uint32_t efmt,
                      int32_t w, int32_t h, const void* dst, size_t dstsize);

// out = { hits, misses, entries at open, file MB at open }.
void rd_etc2cache_stats(uint64_t out[4]);

#endif
