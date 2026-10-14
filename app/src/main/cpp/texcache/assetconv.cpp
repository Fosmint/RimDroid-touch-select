// The game's own textures: RimWorld 1.6 keeps Core and DLC art inside RimWorldLinux_Data/
// resources.assets (a Unity SerializedFile; the pixel data streams from resources.assets.resS),
// compressed for PC GPUs — DXT1, DXT5, BC7. A phone GPU without BC formats (Mali, Xclipse,
// PowerVR) makes Unity unpack each one to RGBA32 on load: 4-6x the memory, slow loads. The
// launcher converts them once (TextureCache.convertAssets): every streamed BC texture is decoded
// (bcdec) and re-encoded (texenc.h: ETC2, or RGBA32 when small) into a NEW .resS; the Texture2D
// records in a copy of the .assets are patched in place — format, image size, mip count, stream
// offset and size are fixed-width fields, so the file layout does not change. The new .resS is the
// original followed by the converted data: anything else streaming from it (meshes, unconverted
// textures) keeps its offsets.
//
// Layout facts (Unity 2022.3, SerializedFile version 22, little-endian data; verified against the
// game's file with UnityPy): header ints are big-endian; Texture2D fields in order: m_Name,
// m_ForcedFallbackFormat, 2 bools, m_Width, m_Height, m_CompleteImageSize, m_MipsStripped,
// m_TextureFormat, m_MipCount, 3 bools, m_MipmapLimitGroupName, m_StreamingMipmaps,
// m_StreamingMipmapsPriority, m_ImageCount, m_TextureDimension, GLTextureSettings (6 ints),
// m_LightmapFormat, m_ColorSpace, m_PlatformBlob, image data, m_StreamData {offset u64, size u32,
// path}. Strings and byte arrays are length-prefixed and 4-aligned.
#include <jni.h>
#include <android/log.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "texenc.h"

#define TAG "RimDroid/assetconv"

namespace {

struct Reader {
    const std::vector<uint8_t>& b;
    size_t p;
    bool ok = true;
    explicit Reader(const std::vector<uint8_t>& buf, size_t at) : b(buf), p(at) {}
    bool have(size_t n) { if (p + n > b.size()) ok = false; return ok; }
    uint32_t u32be() { if (!have(4)) return 0; uint32_t v = (uint32_t)b[p] << 24 | (uint32_t)b[p+1] << 16 | (uint32_t)b[p+2] << 8 | b[p+3]; p += 4; return v; }
    int64_t i64be() { uint64_t hi = u32be(), lo = u32be(); return (int64_t)(hi << 32 | lo); }
    uint32_t u32() { if (!have(4)) return 0; uint32_t v; memcpy(&v, &b[p], 4); p += 4; return v; }
    int32_t i32() { return (int32_t)u32(); }
    uint64_t u64() { if (!have(8)) return 0; uint64_t v; memcpy(&v, &b[p], 8); p += 8; return v; }
    uint8_t u8() { if (!have(1)) return 0; return b[p++]; }
    void align() { p = (p + 3) & ~(size_t)3; }
    std::string str() { uint32_t n = u32(); if (!have(n)) return ""; std::string s((const char*)&b[p], n); p += n; align(); return s; }
    std::string cstr() { size_t e = p; while (e < b.size() && b[e]) e++; std::string s((const char*)&b[p], e - p); p = e + 1; return s; }
    void skipArray() { uint32_t n = u32(); if (have(n)) p += n; align(); }
};

struct TexRecord {
    size_t completeImageSize, textureFormat, mipCount, streamOffset, streamSize;   // field positions
    int width, height, format, mips;
    uint64_t srcOffset; uint32_t srcSize;
    bool convertible;
};

// Parses one Texture2D object at `at`; returns false when its size does not come out as recorded
// (a layout we do not know: leave the file alone).
bool parseTexture(const std::vector<uint8_t>& buf, size_t at, size_t size, TexRecord& r, const char* resSName) {
    Reader rd(buf, at);
    rd.str();                                   // m_Name
    rd.u32();                                   // m_ForcedFallbackFormat
    rd.u8(); rd.u8(); rd.align();               // m_DownscaleFallback, m_IsAlphaChannelOptional
    r.width = rd.i32(); r.height = rd.i32();
    r.completeImageSize = rd.p; rd.u32();
    rd.u32();                                   // m_MipsStripped
    r.textureFormat = rd.p; r.format = rd.i32();
    r.mipCount = rd.p; r.mips = rd.i32();
    rd.u8(); rd.u8(); rd.u8(); rd.align();      // m_IsReadable, m_IsPreProcessed, m_IgnoreMipmapLimit
    rd.str();                                   // m_MipmapLimitGroupName
    rd.u8(); rd.align();                        // m_StreamingMipmaps
    rd.u32();                                   // m_StreamingMipmapsPriority
    int imageCount = rd.i32();
    int dimension = rd.i32();
    rd.p += 24;                                 // GLTextureSettings
    rd.u32(); rd.u32();                         // m_LightmapFormat, m_ColorSpace
    rd.skipArray();                             // m_PlatformBlob
    uint32_t inlineLen = rd.u32(); if (rd.have(inlineLen)) rd.p += inlineLen; rd.align();   // image data
    r.streamOffset = rd.p; r.srcOffset = rd.u64();
    r.streamSize = rd.p; r.srcSize = rd.u32();
    std::string path = rd.str();
    if (!rd.ok || rd.p - at != size) return false;
    bool bc = r.format == FMT_DXT1 || r.format == FMT_DXT3 || r.format == FMT_DXT5 || r.format == FMT_BC7;
    r.convertible = bc && imageCount == 1 && dimension == 2 && inlineLen == 0 && r.srcSize > 0
        && path == resSName && r.width > 0 && r.height > 0 && r.width <= 16384 && r.height <= 16384;
    return true;
}

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { memcpy(&b[at], &v, 4); }
void put64(std::vector<uint8_t>& b, size_t at, uint64_t v) { memcpy(&b[at], &v, 8); }

bool readFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    out.resize((size_t)n);
    bool ok = n == 0 || fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

}  // namespace

// Returns a one-line summary, or null after logging why nothing was written. The progress object
// is a TextureCache.Progress (onProgress(done, total)), called every 50 textures.
extern "C" JNIEXPORT jstring JNICALL
Java_com_rimdroid_game_TextureEncoder_convertAssets(JNIEnv* env, jclass, jstring jAssetsIn, jstring jResSIn,
                                                    jstring jAssetsOut, jstring jResSOut, jobject progress) {
    const char* assetsIn = env->GetStringUTFChars(jAssetsIn, nullptr);
    const char* resSIn = env->GetStringUTFChars(jResSIn, nullptr);
    const char* assetsOut = env->GetStringUTFChars(jAssetsOut, nullptr);
    const char* resSOut = env->GetStringUTFChars(jResSOut, nullptr);
    jmethodID onProgress = nullptr;
    if (progress) onProgress = env->GetMethodID(env->GetObjectClass(progress), "onProgress", "(II)V");
    auto started = std::chrono::steady_clock::now();
    std::string summary;
    std::vector<uint8_t> buf;
    FILE* in = nullptr;
    FILE* out = nullptr;
    auto fail = [&](const char* why) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "%s: %s", assetsIn, why);
        if (in) fclose(in);
        if (out) { fclose(out); remove(resSOut); }
        return false;
    };
    bool ok = [&]() -> bool {
        if (!readFile(assetsIn, buf) || buf.size() < 48) return fail("cannot read");
        Reader hd(buf, 0);
        hd.u32be(); hd.u32be();
        uint32_t version = hd.u32be();
        hd.u32be();
        uint8_t endian = hd.u8();
        if (version != 22 || endian != 0) return fail("unsupported serialized file version/endian");
        hd.p = 20;
        hd.u32be(); hd.i64be();
        int64_t dataOffset = hd.i64be();
        hd.i64be();
        Reader md(buf, 48);
        std::string unityVersion = md.cstr();
        md.u32();                                   // target platform
        if (md.u8() != 0) return fail("embedded type trees: unexpected for a player build");
        uint32_t typeCount = md.u32();
        if (typeCount > 100000) return fail("bad type count");
        std::vector<int32_t> classIds(typeCount);
        for (uint32_t i = 0; i < typeCount; i++) {
            classIds[i] = md.i32();
            md.u8();                                // is stripped
            int16_t scriptIndex = (int16_t)(md.u8() | md.u8() << 8);
            if (classIds[i] == 114 || scriptIndex >= 0) md.p += 16;   // script id
            md.p += 16;                             // old type hash
        }
        uint32_t objectCount = md.u32();
        if (!md.ok || objectCount > 10000000) return fail("bad object table");

        // The resS name as the records spell it: the input file's own base name.
        std::string resSName = resSIn;
        size_t slash = resSName.find_last_of('/');
        if (slash != std::string::npos) resSName = resSName.substr(slash + 1);

        std::vector<TexRecord> textures;
        for (uint32_t i = 0; i < objectCount; i++) {
            md.align();
            md.u64();                               // path id
            int64_t start = (int64_t)md.u64();
            uint32_t size = md.u32();
            uint32_t typeId = md.u32();
            if (!md.ok) return fail("truncated object table");
            if (typeId >= typeCount || classIds[typeId] != 28) continue;   // Texture2D
            size_t at = (size_t)(dataOffset + start);
            if (start < 0 || at + size > buf.size()) return fail("object outside the file");
            TexRecord r;
            if (!parseTexture(buf, at, size, r, resSName.c_str())) return fail("unknown Texture2D layout");
            if (r.convertible) textures.push_back(r);
        }
        if (textures.empty()) {
            summary = "no convertible textures";
            return fail("nothing to convert");
        }

        in = fopen(resSIn, "rb");
        if (!in) return fail("cannot open the resS");
        out = fopen(resSOut, "wb");
        if (!out) return fail("cannot create the new resS");
        // Original first, as it is.
        std::vector<uint8_t> chunk(1 << 20);
        uint64_t base = 0;
        for (size_t n; (n = fread(chunk.data(), 1, chunk.size(), in)) > 0; ) {
            if (fwrite(chunk.data(), 1, n, out) != n) return fail("write failed");
            base += n;
        }
        while (base & 15) { fputc(0, out); base++; }

        uint64_t at = base, oldBytes = 0, newBytes = 0;
        int converted = 0, skipped = 0;
        std::vector<uint8_t> blocks, rgba, encoded;
        int total = (int)textures.size();
        for (int i = 0; i < total; i++) {
            const TexRecord& r = textures[i];
            int bc = r.format == FMT_DXT1 ? 1 : r.format == FMT_DXT3 ? 2 : r.format == FMT_DXT5 ? 3 : 7;
            blocks.resize(r.srcSize);
            if (fseek(in, (long)r.srcOffset, SEEK_SET) != 0 || fread(blocks.data(), 1, r.srcSize, in) != r.srcSize) { skipped++; continue; }
            if (!rd_decode_bc(bc, blocks.data(), blocks.size(), r.width, r.height, rgba)) { skipped++; continue; }
            int format, levels;
            rd_encode_chain((const uint32_t*)rgba.data(), r.width, r.height, r.mips > 1, encoded, format, levels);
            if (fwrite(encoded.data(), 1, encoded.size(), out) != encoded.size()) return fail("write failed");
            put32(buf, r.textureFormat, (uint32_t)format);
            put32(buf, r.mipCount, (uint32_t)levels);
            put32(buf, r.completeImageSize, (uint32_t)encoded.size());
            put64(buf, r.streamOffset, at);
            put32(buf, r.streamSize, (uint32_t)encoded.size());
            at += encoded.size();
            while (at & 15) { fputc(0, out); at++; }
            oldBytes += r.srcSize; newBytes += encoded.size();
            converted++;
            if (onProgress && (i % 50 == 49 || i + 1 == total)) {
                env->CallVoidMethod(progress, onProgress, (jint)(i + 1), (jint)total);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
        fclose(in); in = nullptr;
        if (fclose(out) != 0) { out = nullptr; return fail("write failed"); }
        out = nullptr;

        FILE* a = fopen(assetsOut, "wb");
        if (!a) return fail("cannot create the new assets file");
        bool wrote = fwrite(buf.data(), 1, buf.size(), a) == buf.size();
        if (fclose(a) != 0 || !wrote) { remove(assetsOut); remove(resSOut); return fail("write failed"); }

        long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        char line[256];
        snprintf(line, sizeof line, "%d textures converted (%d skipped) in %ld ms: %llu -> %llu MB of texture data, Unity %s",
                 converted, skipped, ms, (unsigned long long)(oldBytes >> 20), (unsigned long long)(newBytes >> 20), unityVersion.c_str());
        summary = line;
        __android_log_print(ANDROID_LOG_INFO, TAG, "%s", line);
        return true;
    }();
    env->ReleaseStringUTFChars(jAssetsIn, assetsIn);
    env->ReleaseStringUTFChars(jResSIn, resSIn);
    env->ReleaseStringUTFChars(jAssetsOut, assetsOut);
    env->ReleaseStringUTFChars(jResSOut, resSOut);
    return ok ? env->NewStringUTF(summary.c_str()) : nullptr;
}
