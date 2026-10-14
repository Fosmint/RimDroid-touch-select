// librimdroidtex.so — the native engine's texture cache encoder (JNI side of TextureEncoder.java).
//
// RimWorld keeps mod textures as loose PNG/DDS files and decodes them on every load; on Android the
// game's own compression then runs per texture, per launch (BC7 DDS files cannot be compressed at
// all: no BC formats on mobile GPUs, they stay RGBA32). The launcher encodes each texture ONCE into
// ETC2 with the full mip chain (etcpak, BSD-3) and the bridge feeds the raw blocks to Unity, so
// loading is a file read. DDS block formats are decoded with bcdec (MIT, in etcpak). The game's own
// Core/DLC textures live inside resources.assets: assetconv.cpp.
#include <jni.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "texenc.h"
#include "etcpak/ProcessRGB.hpp"
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "etcpak/bcdec.h"

#define TAG "RimDroid/texcache"

// Box filter, as Unity's own mip generation for LoadImage: odd edges clamp.
static void downsample(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int y0 = std::min(2 * y, sh - 1), y1 = std::min(2 * y + 1, sh - 1);
        for (int x = 0; x < dw; x++) {
            int x0 = std::min(2 * x, sw - 1), x1 = std::min(2 * x + 1, sw - 1);
            const uint8_t* p[4] = { (const uint8_t*)&src[y0 * sw + x0], (const uint8_t*)&src[y0 * sw + x1],
                                    (const uint8_t*)&src[y1 * sw + x0], (const uint8_t*)&src[y1 * sw + x1] };
            uint8_t* d = (uint8_t*)&dst[y * dw + x];
            for (int c = 0; c < 4; c++) d[c] = (uint8_t)((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) >> 2);
        }
    }
}

// One mip level: pad to whole 4x4 blocks by repeating the edge, then etcpak. Unity stores a
// non-multiple-of-4 level the same way (ceil(w/4) x ceil(h/4) blocks). etcpak takes BGRA (its own
// PNG loader sets png_set_bgr), so red and blue are swapped on the way in.
static void encodeLevel(const uint32_t* src, int w, int h, bool alpha, std::vector<uint8_t>& out) {
    int pw = (w + 3) & ~3, ph = (h + 3) & ~3;
    std::vector<uint32_t> bgra((size_t)pw * ph);
    for (int y = 0; y < ph; y++) {
        int sy = std::min(y, h - 1);
        for (int x = 0; x < pw; x++) {
            uint32_t p = src[(size_t)sy * w + std::min(x, w - 1)];
            bgra[(size_t)y * pw + x] = (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
        }
    }
    uint32_t blocks = (uint32_t)(pw / 4) * (uint32_t)(ph / 4);
    size_t bytes = (size_t)blocks * (alpha ? 16 : 8);
    size_t at = out.size();
    out.resize(at + bytes);
    if (alpha) CompressEtc2Rgba(bgra.data(), (uint64_t*)(out.data() + at), blocks, (size_t)pw, true);
    else       CompressEtc2Rgb(bgra.data(), (uint64_t*)(out.data() + at), blocks, (size_t)pw, true);
}

void rd_encode_chain(const uint32_t* rgba, int w, int h, bool mips, std::vector<uint8_t>& out, int& format, int& levels) {
    bool raw = (w <= RAW_MAX && h <= RAW_MAX) || (w & 3) || (h & 3);
    bool alpha = false;
    for (size_t i = 0, n = (size_t)w * h; i < n && !alpha; i++) alpha = (rgba[i] >> 24) != 0xff;
    out.clear();
    out.reserve((size_t)w * h * (raw ? 4 : alpha ? 1 : 0.5) * 4 / 3 + 64);
    levels = 1;
    if (mips) for (int m = std::max(w, h); m > 1; m >>= 1) levels++;
    std::vector<uint32_t> cur, next;
    const uint32_t* level = rgba;
    int lw = w, lh = h;
    for (int i = 0; i < levels; i++) {
        if (raw) out.insert(out.end(), (const uint8_t*)level, (const uint8_t*)level + (size_t)lw * lh * 4);
        else encodeLevel(level, lw, lh, alpha, out);
        if (i + 1 == levels) break;
        int nw = std::max(1, lw >> 1), nh = std::max(1, lh >> 1);
        next.resize((size_t)nw * nh);
        downsample(level, lw, lh, next.data(), nw, nh);
        cur.swap(next);
        level = cur.data(); lw = nw; lh = nh;
    }
    format = raw ? FMT_RGBA32 : alpha ? FMT_ETC2_RGBA8 : FMT_ETC2_RGB;
}

bool rd_decode_bc(int bc, const uint8_t* blocks, size_t len, int w, int h, std::vector<uint8_t>& rgba) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    size_t blockSize = (bc == 1) ? 8 : 16;
    if (len < (size_t)bw * bh * blockSize) return false;
    // Decode into a block-aligned buffer, then crop to the real size.
    int pw = bw * 4, ph = bh * 4;
    std::vector<uint8_t> padded((size_t)pw * ph * 4);
    const uint8_t* block = blocks;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++, block += blockSize) {
            uint8_t* dst = padded.data() + ((size_t)by * 4 * pw + (size_t)bx * 4) * 4;
            switch (bc) {
                case 1: bcdec_bc1(block, dst, pw * 4); break;
                case 2: bcdec_bc2(block, dst, pw * 4); break;
                case 3: bcdec_bc3(block, dst, pw * 4); break;
                default: bcdec_bc7(block, dst, pw * 4); break;
            }
        }
    }
    if (pw == w && ph == h) { rgba.swap(padded); return true; }
    rgba.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++) memcpy(&rgba[(size_t)y * w * 4], &padded[(size_t)y * pw * 4], (size_t)w * 4);
    return true;
}

// info[0] = Unity format, info[1] = mip count.
static jbyteArray encodeChain(JNIEnv* env, const uint32_t* rgba, int w, int h, bool mips, jintArray info) {
    std::vector<uint8_t> out;
    int format, levels;
    rd_encode_chain(rgba, w, h, mips, out, format, levels);
    jint meta[2] = { format, levels };
    env->SetIntArrayRegion(info, 0, 2, meta);
    jbyteArray result = env->NewByteArray((jsize)out.size());
    if (result) env->SetByteArrayRegion(result, 0, (jsize)out.size(), (const jbyte*)out.data());
    return result;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_rimdroid_game_TextureEncoder_encodeBitmap(JNIEnv* env, jclass, jobject bitmap, jboolean mips, jintArray info) {
    AndroidBitmapInfo bi;
    if (AndroidBitmap_getInfo(env, bitmap, &bi) != ANDROID_BITMAP_RESULT_SUCCESS
            || bi.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || bi.width == 0 || bi.height == 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "encodeBitmap: unsupported bitmap format %d", bi.format);
        return nullptr;
    }
    void* pixels;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) return nullptr;
    // The bitmap is decoded with inPremultiplied = false: bytes are straight R,G,B,A, as the PNG
    // holds them and as Unity's LoadImage would produce. Rows are reversed on the way: a bitmap
    // runs top to bottom, a Unity texture bottom to top (LoadImage flips the same way), and
    // without it every cached mod PNG showed upside down (tester, 2026-10-12). DDS files already
    // store Unity's order and go through encodeRgba unflipped.
    std::vector<uint32_t> rgba((size_t)bi.width * bi.height);
    for (uint32_t y = 0; y < bi.height; y++)
        memcpy(&rgba[(size_t)(bi.height - 1 - y) * bi.width], (const uint8_t*)pixels + (size_t)y * bi.stride, (size_t)bi.width * 4);
    AndroidBitmap_unlockPixels(env, bitmap);
    return encodeChain(env, rgba.data(), (int)bi.width, (int)bi.height, mips, info);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_rimdroid_game_TextureEncoder_encodeRgba(JNIEnv* env, jclass, jbyteArray rgba, jint w, jint h, jboolean mips, jintArray info) {
    if (w <= 0 || h <= 0 || env->GetArrayLength(rgba) < (jsize)((size_t)w * h * 4)) return nullptr;
    std::vector<uint32_t> px((size_t)w * h);
    env->GetByteArrayRegion(rgba, 0, (jsize)((size_t)w * h * 4), (jbyte*)px.data());
    return encodeChain(env, px.data(), w, h, mips, info);
}

// DDS → straight RGBA8 of the top level (we rebuild the mips ourselves). BC1/BC2/BC3 by FourCC,
// BC1-BC3 and BC7 through a DX10 header. Anything else (BC4/5/6H, uncompressed) returns null and
// the game's own loader keeps that file. wh[0..1] = width, height.
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_rimdroid_game_TextureEncoder_decodeDds(JNIEnv* env, jclass, jbyteArray data, jintArray wh) {
    jsize len = env->GetArrayLength(data);
    if (len < 128) return nullptr;
    std::vector<uint8_t> dds((size_t)len);
    env->GetByteArrayRegion(data, 0, len, (jbyte*)dds.data());
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, &dds[o], 4); return v; };
    if (u32(0) != 0x20534444u) return nullptr;   // "DDS "
    int h = (int)u32(12), w = (int)u32(16);
    uint32_t pfFlags = u32(80), fourcc = u32(84);
    size_t offset = 128;
    int bc = 0;   // 1, 2, 3, 7
    if (pfFlags & 0x4) {   // DDPF_FOURCC
        switch (fourcc) {
            case 0x31545844: bc = 1; break;   // DXT1
            case 0x33545844: bc = 2; break;   // DXT3
            case 0x35545844: bc = 3; break;   // DXT5
            case 0x30315844: {                // DX10
                if (len < 148) return nullptr;
                uint32_t dxgi = u32(128);
                offset = 148;
                if (dxgi == 71 || dxgi == 72) bc = 1;
                else if (dxgi == 74 || dxgi == 75) bc = 2;
                else if (dxgi == 77 || dxgi == 78) bc = 3;
                else if (dxgi == 98 || dxgi == 99) bc = 7;
                break;
            }
        }
    }
    if (bc == 0 || w <= 0 || h <= 0 || w > 16384 || h > 16384) return nullptr;
    std::vector<uint8_t> rgba;
    if (!rd_decode_bc(bc, dds.data() + offset, (size_t)len - offset, w, h, rgba)) return nullptr;
    jbyteArray result = env->NewByteArray((jsize)rgba.size());
    if (!result) return nullptr;
    env->SetByteArrayRegion(result, 0, (jsize)rgba.size(), (const jbyte*)rgba.data());
    jint size[2] = { w, h };
    env->SetIntArrayRegion(wh, 0, 2, size);
    return result;
}

// Does this GPU take BC (DXT/BC7) textures as they are? Vulkan's textureCompressionBC feature of
// the first physical device, asked without creating a device: 1 yes, 0 no, -1 unknown. Linked, not
// dlopen'ed: the launcher process carries box64's linker hooks (librimdroidlinker), whose dlopen is
// not for calls from here (it crashed). Adreno says yes; Mali, Xclipse and PowerVR say no, and
// for them the game's own textures are worth converting (assetconv.cpp).
#include <vulkan/vulkan.h>
extern "C" JNIEXPORT jint JNICALL
Java_com_rimdroid_game_TextureEncoder_gpuSupportsBc(JNIEnv*, jclass) {
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "RimDroid";
    appInfo.pEngineName = "RimDroid";
    appInfo.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS || !instance) return -1;
    int result = -1;
    uint32_t count = 1;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    VkResult r = vkEnumeratePhysicalDevices(instance, &count, &device);
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && count >= 1 && device) {
        VkPhysicalDeviceFeatures features = {};
        vkGetPhysicalDeviceFeatures(device, &features);
        result = features.textureCompressionBC ? 1 : 0;
    }
    vkDestroyInstance(instance, nullptr);
    return result;
}
