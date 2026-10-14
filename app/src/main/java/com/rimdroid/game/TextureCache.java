package com.rimdroid.game;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The native engine's texture cache: every PNG/JPG/DDS under a Textures folder of Core, the DLC and
 * the instance's mods, encoded once to ETC2 with its mip chain (TextureEncoder). The bridge
 * (tools/unity-native/bridge) looks a file up by the same key and hands Unity the blocks instead of
 * letting the game decode and re-compress the image on every launch.
 *
 * Layout: {@code <game>/native_engine/texcache/<fnv1a64 of the path>.rdtex}, where the path is the
 * source file relative to the game folder, lower case, '/' separators. Each file = 40-byte header
 * (little-endian: "RDTX", version, Unity TextureFormat, width, height, mip count, source size,
 * source mtime ms) + the ETC2 data, mip 0 first. The source size + mtime in the header are what
 * decides staleness on both sides, so a changed mod re-encodes only its changed files.
 */
public final class TextureCache {
    private static final String TAG = "RimDroid/texcache";
    public static final String DIR = "native_engine/texcache";
    public static final String EXT = ".rdtex";
    static final int MAGIC = 0x58544452;   // "RDTX"
    static final int VERSION = 4;   // 1: red/blue swapped in ETC2 entries; 2: raw up to 128 px; 3: PNGs upside down
    static final int HEADER = 40;

    public interface Progress { void onProgress(int done, int total); }

    public static File dir(GameInstance gi) { return new File(gi.getGamePath(), DIR); }

    /** Source textures the game will load: Data/<content>/Textures and Mods/<mod>/**&#47;Textures of
     *  the ACTIVE mods (ModsConfig.xml; an installed but disabled mod is not encoded), skipping
     *  version folders of other game versions (a mod's 1.5/ next to its 1.6/). */
    public static List<File> sources(GameInstance gi) {
        File root = new File(gi.getGamePath());
        String version = gameVersion(root);
        Set<String> active = activeMods(root);
        List<File> out = new ArrayList<>();
        File[] tops = { new File(root, "Data"), new File(root, "Mods") };
        for (File top : tops) {
            File[] entries = top.listFiles();
            if (entries == null) continue;
            for (File entry : entries) {
                if (!entry.isDirectory()) continue;
                if (active != null && !active.contains(packageId(entry))) continue;
                walk(entry, false, version, out);
            }
        }
        return out;
    }

    /** Lower-case package ids of the active mods, or null when ModsConfig.xml is unreadable (then
     *  every installed mod counts). */
    static Set<String> activeMods(File root) {
        File config = new File(root,
                "unity3d/Ludeon Studios/RimWorld by Ludeon Studios/Config/ModsConfig.xml");
        if (!config.isFile()) return null;
        try {
            org.w3c.dom.Document doc = javax.xml.parsers.DocumentBuilderFactory.newInstance()
                    .newDocumentBuilder().parse(config);
            org.w3c.dom.NodeList lists = doc.getElementsByTagName("activeMods");
            if (lists.getLength() == 0) return null;
            Set<String> ids = new HashSet<>();
            org.w3c.dom.NodeList items = ((org.w3c.dom.Element) lists.item(0)).getElementsByTagName("li");
            for (int i = 0; i < items.getLength(); i++)
                ids.add(items.item(i).getTextContent().trim().toLowerCase(Locale.ROOT));
            return ids;
        } catch (Exception e) {
            Log.w(TAG, "ModsConfig.xml unreadable, caching every installed mod: " + e);
            return null;
        }
    }

    /** The mod folder's About/About.xml packageId, lower case; "" when missing. */
    static String packageId(File modDir) {
        File about = new File(modDir, "About/About.xml");
        if (!about.isFile()) return "";
        try {
            org.w3c.dom.Document doc = javax.xml.parsers.DocumentBuilderFactory.newInstance()
                    .newDocumentBuilder().parse(about);
            org.w3c.dom.NodeList ids = doc.getElementsByTagName("packageId");
            return ids.getLength() == 0 ? "" : ids.item(0).getTextContent().trim().toLowerCase(Locale.ROOT);
        } catch (Exception e) {
            return "";
        }
    }

    private static void walk(File dir, boolean inTextures, String version, List<File> out) {
        File[] entries = dir.listFiles();
        if (entries == null) return;
        for (File f : entries) {
            String name = f.getName();
            if (f.isDirectory()) {
                if (name.equals("native_engine") || name.startsWith(".")) continue;
                if (!inTextures && isVersionFolder(name) && !name.equals(version)) continue;
                walk(f, inTextures || name.equalsIgnoreCase("Textures"), version, out);
            } else if (inTextures && isTexture(name)) {
                out.add(f);
            }
        }
    }

    private static boolean isVersionFolder(String name) {
        int dot = name.indexOf('.');
        if (dot <= 0 || dot == name.length() - 1) return false;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (i != dot && (c < '0' || c > '9')) return false;
        }
        return true;
    }

    static boolean isTexture(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        return n.endsWith(".png") || n.endsWith(".jpg") || n.endsWith(".jpeg") || n.endsWith(".dds");
    }

    /** "1.6" from Version.txt ("1.6.4871 rev..."), or "" when unreadable. */
    static String gameVersion(File root) {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(
                new FileInputStream(new File(root, "Version.txt")), StandardCharsets.UTF_8))) {
            String line = r.readLine();
            if (line == null) return "";
            String[] parts = line.trim().split("[ .]");
            return parts.length >= 2 ? parts[0] + "." + parts[1] : "";
        } catch (IOException e) {
            return "";
        }
    }

    /** Key = FNV-1a 64 of the path relative to the game folder; the bridge computes the same. */
    public static String key(File root, File source) {
        String rel = source.getAbsolutePath();
        String base = root.getAbsolutePath();
        if (rel.startsWith(base)) rel = rel.substring(base.length());
        rel = rel.replace('\\', '/');
        while (rel.startsWith("/")) rel = rel.substring(1);
        rel = rel.toLowerCase(Locale.ROOT);
        long h = 0xcbf29ce484222325L;
        for (byte b : rel.getBytes(StandardCharsets.UTF_8)) {
            h ^= (b & 0xff);
            h *= 0x100000001b3L;
        }
        return String.format(Locale.ROOT, "%016x", h);
    }

    /** Sources without an up-to-date cache entry. Also removes entries whose source is gone. */
    public static List<File> stale(GameInstance gi) {
        File root = new File(gi.getGamePath());
        File cache = dir(gi);
        List<File> todo = new ArrayList<>();
        Set<String> expected = new HashSet<>();
        for (File src : sources(gi)) {
            String name = key(root, src) + EXT;
            expected.add(name);
            if (!upToDate(new File(cache, name), src)) todo.add(src);
        }
        File[] present = cache.listFiles();
        if (present != null)
            for (File f : present)
                if (!expected.contains(f.getName()) && !f.delete())
                    Log.w(TAG, "Cannot remove orphan " + f.getName());
        return todo;
    }

    static boolean upToDate(File entry, File src) {
        if (!entry.isFile() || entry.length() < HEADER) return false;
        try (RandomAccessFile raf = new RandomAccessFile(entry, "r")) {
            byte[] head = new byte[HEADER];
            raf.readFully(head);
            ByteBuffer b = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN);
            return b.getInt(0) == MAGIC && b.getInt(4) == VERSION
                    && b.getLong(24) == src.length() && b.getLong(32) == src.lastModified();
        } catch (IOException e) {
            return false;
        }
    }

    /** Encodes the given sources on a thread pool; progress from worker threads; stop flag honoured
     *  between files. Returns how many entries were written. */
    public static int build(GameInstance gi, List<File> todo, Progress progress, AtomicBoolean stop) {
        if (!TextureEncoder.available()) {
            Log.w(TAG, "librimdroidtex.so missing, texture cache not built");
            return 0;
        }
        File root = new File(gi.getGamePath());
        File cache = dir(gi);
        if (!cache.isDirectory() && !cache.mkdirs()) {
            Log.w(TAG, "Cannot create " + cache);
            return 0;
        }
        // Big textures decode to tens of MB each; a few workers are plenty for the CPU and safe for RAM.
        int threads = Math.max(1, Math.min(4, Runtime.getRuntime().availableProcessors() - 1));
        ExecutorService pool = Executors.newFixedThreadPool(threads);
        AtomicInteger done = new AtomicInteger(), written = new AtomicInteger();
        int total = todo.size();
        long started = System.currentTimeMillis();
        for (File src : todo) {
            pool.execute(() -> {
                if (!stop.get()) {
                    try {
                        if (encode(src, new File(cache, key(root, src) + EXT))) written.incrementAndGet();
                    } catch (Throwable t) {
                        Log.w(TAG, "Texture cache: " + src.getName() + ": " + t);
                    }
                }
                int d = done.incrementAndGet();
                if (progress != null) progress.onProgress(d, total);
            });
        }
        pool.shutdown();
        try { pool.awaitTermination(1, TimeUnit.DAYS); } catch (InterruptedException ignored) {}
        Log.i(TAG, "Texture cache: " + written.get() + " of " + total + " encoded in "
                + (System.currentTimeMillis() - started) / 1000 + " s" + (stop.get() ? " (stopped)" : ""));
        return written.get();
    }

    /** One source → one cache entry; false when the format is not ours (left to the game). */
    static boolean encode(File src, File dst) throws IOException {
        int[] info = new int[2];
        byte[] data;
        String lower = src.getName().toLowerCase(Locale.ROOT);
        if (lower.endsWith(".dds")) {
            int[] wh = new int[2];
            byte[] rgba = TextureEncoder.decodeDds(Files.readAllBytes(src.toPath()), wh);
            if (rgba == null) return false;
            data = TextureEncoder.encodeRgba(rgba, wh[0], wh[1], true, info);
            if (data == null) return false;
            write(dst, info, wh[0], wh[1], src, data);
            return true;
        }
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inPreferredConfig = Bitmap.Config.ARGB_8888;
        o.inPremultiplied = false;   // straight alpha, as the PNG stores it and Unity reads it
        o.inScaled = false;
        o.inMutable = false;
        Bitmap bmp = BitmapFactory.decodeFile(src.getAbsolutePath(), o);
        if (bmp == null) return false;
        try {
            if (bmp.getConfig() != Bitmap.Config.ARGB_8888 || bmp.isPremultiplied()) {
                Bitmap copy = Bitmap.createBitmap(bmp.getWidth(), bmp.getHeight(), Bitmap.Config.ARGB_8888);
                copy.setPremultiplied(false);
                int[] px = new int[bmp.getWidth() * bmp.getHeight()];
                bmp.getPixels(px, 0, bmp.getWidth(), 0, 0, bmp.getWidth(), bmp.getHeight());
                copy.setPixels(px, 0, bmp.getWidth(), 0, 0, bmp.getWidth(), bmp.getHeight());
                bmp.recycle();
                bmp = copy;
            }
            data = TextureEncoder.encodeBitmap(bmp, true, info);
            if (data == null) return false;
            write(dst, info, bmp.getWidth(), bmp.getHeight(), src, data);
            return true;
        } finally {
            bmp.recycle();
        }
    }

    private static void write(File dst, int[] info, int w, int h, File src, byte[] data) throws IOException {
        ByteBuffer head = ByteBuffer.allocate(HEADER).order(ByteOrder.LITTLE_ENDIAN);
        head.putInt(MAGIC).putInt(VERSION).putInt(info[0]).putInt(w).putInt(h).putInt(info[1])
            .putLong(src.length()).putLong(src.lastModified());
        File tmp = new File(dst.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(head.array());
            out.write(data);
        }
        if (!tmp.renameTo(dst)) {
            tmp.delete();
            throw new IOException("Cannot replace " + dst.getName());
        }
    }

    // ---- The game's own textures (resources.assets): converted copy, see assetconv.cpp ----

    public static final String CONVERTED_DIR = "native_engine/converted";
    public static final String ASSETS = "resources.assets", RESS = "resources.assets.resS";
    /** Bump when the converter's output changes; old copies are rebuilt. */
    static final int CONVERTER_VERSION = 3;   // 1: red/blue swapped; 2: raw up to 128 px

    public static File convertedDir(GameInstance gi) { return new File(gi.getGamePath(), CONVERTED_DIR); }

    private static String assetsStamp(File dataDir) {
        File a = new File(dataDir, ASSETS), r = new File(dataDir, RESS);
        return CONVERTER_VERSION + ":" + a.length() + ":" + a.lastModified() + ":" + r.length() + ":" + r.lastModified();
    }

    private static int gpuBc = -2;

    /** Converting the game's own textures only pays where the GPU cannot take DXT/BC7 as they are
     *  (Mali, Xclipse, PowerVR); Adreno keeps the originals. Unknown counts as "convert". */
    public static synchronized boolean assetsWanted() {
        if (gpuBc == -2) {
            gpuBc = TextureEncoder.available() ? TextureEncoder.gpuSupportsBc() : -1;
            Log.i(TAG, "GPU BC texture support: " + gpuBc + (gpuBc == 1 ? " -> game assets stay as they are" : " -> game assets are converted"));
        }
        return gpuBc != 1;
    }

    /** True when the converted copy is wanted and missing or made from other originals. */
    public static boolean assetsNeedConversion(GameInstance gi) {
        if (!assetsWanted()) return false;
        File dataDir = new File(gi.getGamePath(), "RimWorldLinux_Data");
        if (!new File(dataDir, ASSETS).isFile() || !new File(dataDir, RESS).isFile()) return false;
        File dir = convertedDir(gi);
        if (!new File(dir, ASSETS).isFile() || !new File(dir, RESS).isFile()) return true;
        try {
            return !assetsStamp(dataDir).equals(new String(Files.readAllBytes(new File(dir, "stamp").toPath()), StandardCharsets.UTF_8));
        } catch (IOException e) {
            return true;
        }
    }

    /** The converted copy is used by NativeEngine's data archive only while wanted and current. */
    public static File convertedFile(GameInstance gi, String name) {
        if (!assetsWanted() || assetsNeedConversion(gi)) return null;
        File f = new File(convertedDir(gi), name);
        return f.isFile() ? f : null;
    }

    /** Converts resources.assets (+ .resS); returns the summary or null. Takes the texture count of
     *  the file as the progress total. */
    public static String convertAssets(GameInstance gi, Progress progress) {
        if (!TextureEncoder.available()) return null;
        File dataDir = new File(gi.getGamePath(), "RimWorldLinux_Data");
        File dir = convertedDir(gi);
        if (!dir.isDirectory() && !dir.mkdirs()) return null;
        File outAssets = new File(dir, ASSETS), outResS = new File(dir, RESS), stamp = new File(dir, "stamp");
        stamp.delete();
        String summary = TextureEncoder.convertAssets(new File(dataDir, ASSETS).getAbsolutePath(),
                new File(dataDir, RESS).getAbsolutePath(), outAssets.getAbsolutePath(), outResS.getAbsolutePath(), progress);
        if (summary == null) {
            outAssets.delete();
            outResS.delete();
            return null;
        }
        try (FileOutputStream out = new FileOutputStream(stamp)) {
            out.write(assetsStamp(dataDir).getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.w(TAG, "Cannot write the conversion stamp: " + e);
        }
        Log.i(TAG, "resources.assets: " + summary);
        return summary;
    }

    /** Bytes on disk (cache entries + the converted game assets), for the settings card. */
    public static long size(GameInstance gi) {
        long total = 0;
        File[] files = dir(gi).listFiles();
        if (files != null) for (File f : files) total += f.length();
        files = convertedDir(gi).listFiles();
        if (files != null) for (File f : files) total += f.length();
        return total;
    }

    public static void clear(GameInstance gi) {
        File[] files = dir(gi).listFiles();
        if (files != null) for (File f : files) f.delete();
        clearConverted(gi);
    }

    /** Drops the converted game assets (half a gigabyte): when the GPU turns out not to need them. */
    public static void clearConverted(GameInstance gi) {
        File[] files = convertedDir(gi).listFiles();
        if (files != null) for (File f : files) f.delete();
    }

    private TextureCache() {}
}
