package com.rimdroid.game;

import android.graphics.Bitmap;

/** JNI face of librimdroidtex.so (app/src/main/cpp/texcache): ETC2 encoding for the native engine's
 *  texture cache. See TextureCache for the files it produces. */
public final class TextureEncoder {
    private static boolean loaded;

    public static synchronized boolean available() {
        if (!loaded) {
            try { System.loadLibrary("rimdroidtex"); loaded = true; } catch (Throwable ignored) {}
        }
        return loaded;
    }

    /** ETC2 mip chain of a straight-alpha RGBA_8888 bitmap; info[0] = Unity TextureFormat
     *  (ETC2_RGB when every pixel is opaque, else ETC2_RGBA8), info[1] = mip count. Null on failure. */
    public static native byte[] encodeBitmap(Bitmap bitmap, boolean mips, int[] info);

    /** Same for raw straight-alpha RGBA bytes (w * h * 4). */
    public static native byte[] encodeRgba(byte[] rgba, int w, int h, boolean mips, int[] info);

    /** Top level of a DDS file (BC1/BC2/BC3/BC7) as straight RGBA bytes; wh[0..1] = size. Null when
     *  the format is not one of those: the game's own loader keeps the file. */
    public static native byte[] decodeDds(byte[] dds, int[] wh);

    /** Re-encodes the BC textures inside a Unity resources.assets (+ .resS) into a converted copy
     *  (assetconv.cpp); progress every 50 textures. Returns a summary line, null when nothing was
     *  written (unsupported file, read error: the game then uses the originals). */
    public static native String convertAssets(String assetsIn, String resSIn, String assetsOut, String resSOut,
                                              TextureCache.Progress progress);

    /** Vulkan textureCompressionBC of the first GPU: 1 yes, 0 no, -1 unknown. */
    public static native int gpuSupportsBc();

    private TextureEncoder() {}
}
