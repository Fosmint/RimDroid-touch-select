package com.rimdroid.game;

/**
 * The native engine's numbers for the overlay, over JNI into il2mono (libil2cpp.so, pad.c). Only in
 * the ":unity" process, where the player has loaded that library.
 */
public final class NativeInput {
    static {
        System.loadLibrary("il2cpp");   // already loaded by the player; this binds the JNI methods
    }

    private NativeInput() {}

    private static native long nativeGetFrameCount();
    private static native boolean nativeMouseScroll(float x, float y, float notches);

    /** Viewport fractions, top-left origin; positive notches scroll up. False until the bridge is ready. */
    public static boolean scroll(float x, float y, float notches) {
        return nativeMouseScroll(x, y, notches);
    }

    /** Frames the engine presented so far (vkshim counts them), for the FPS counter. */
    public static long frameCount() {
        return nativeGetFrameCount();
    }
}
