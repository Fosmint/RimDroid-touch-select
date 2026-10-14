package com.rimdroid.input;

import com.rimdroid.GameActivity;

/**
 * Where the on-screen controls (and a gamepad, through them) deliver their input. By default the
 * game's X server, through GameActivity's natives, as always. The native engine (Unity's Android
 * player, in the ":unity" process, where those natives are not loaded) installs a {@link Backend}
 * that takes everything instead (NativeGameOverlay). Coordinates are game pixels, as the natives
 * take them; buttons and keys are SDL's, as the bindings are.
 */
public final class InputSink {
    private InputSink() {}

    public interface Backend {
        /** The cursor moved. */
        void touch(int x, int y);
        /** 1 = left, 2 = middle, 3 = right. */
        void button(int button, boolean down, int x, int y);
        /** One wheel step; dy +1 = up (zoom in). */
        void scroll(int x, int y, int dy);
        void key(int sdlScancode, int sdlKeycode, boolean down);
        void text(String text);
        /** The on-screen TOGGLE_KEYBOARD button. */
        void toggleKeyboard();
    }

    private static volatile Backend backend;

    public static void setBackend(Backend b) {
        backend = b;
    }

    public static boolean hasBackend() {
        return backend != null;
    }

    public static void touch(int x, int y) {
        Backend b = backend;
        if (b != null) { b.touch(x, y); return; }
        try { GameActivity.touchInput(0, x, y); } catch (UnsatisfiedLinkError ignored) {}
    }

    public static void button(int button, boolean down, int x, int y) {
        Backend b = backend;
        if (b != null) { b.button(button, down, x, y); return; }
        try { GameActivity.buttonInput(button, down ? 1 : 0, x, y); } catch (UnsatisfiedLinkError ignored) {}
    }

    public static void scroll(int x, int y, int dy) {
        Backend b = backend;
        if (b != null) { b.scroll(x, y, dy); return; }
        try { GameActivity.scrollInput(x, y, dy); } catch (UnsatisfiedLinkError ignored) {}
    }

    public static void key(int sdlScancode, int sdlKeycode, boolean down) {
        Backend b = backend;
        if (b != null) { b.key(sdlScancode, sdlKeycode, down); return; }
        try { GameActivity.keyInput(sdlScancode, sdlKeycode, down ? 1 : 0); } catch (UnsatisfiedLinkError ignored) {}
    }

    public static void text(String text) {
        Backend b = backend;
        if (b != null) { b.text(text); return; }
        try { GameActivity.nativeText(text); } catch (UnsatisfiedLinkError ignored) {}
    }

    public static void toggleKeyboard() {
        Backend b = backend;
        if (b != null) b.toggleKeyboard();
    }
}
