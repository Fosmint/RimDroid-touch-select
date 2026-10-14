using System;
using System.IO;
using System.Reflection;
using System.Text;
using UnityEngine;

namespace RimDroid
{
    // The launcher's texture cache (app: com.rimdroid.game.TextureCache): Core/DLC/mod PNG and DDS
    // files encoded once to ETC2 with their mip chain. Installed as a Harmony prefix on
    // Verse.ModContentLoader<Texture2D>.LoadTexture(VirtualFile): a cache hit builds the texture
    // from the raw blocks (what the game's own path ends with, minus the decode and the compress);
    // a miss or any error lets the original loader run. Keyed and validated exactly as the launcher
    // does: FNV-1a 64 of the path relative to the game folder, source size + mtime in the header.
    static class TextureCacheLoader
    {
        const int Magic = 0x58544452;   // "RDTX"
        const int Version = 4;
        const int Header = 40;

        static readonly string dir = Env("RIMDROID_TEXCACHE_DIR");
        static readonly string root = Env("RIMDROID_TEXCACHE_ROOT");
        static PropertyInfo fullPath;
        static int hits, misses, stale, failed, reported;
        static float changedAt;

        internal static bool Enabled { get { return dir != null; } }

        static string Env(string name)
        {
            string v = Environment.GetEnvironmentVariable(name);
            return string.IsNullOrEmpty(v) ? null : v;
        }

        // Harmony prefix. __0 = the VirtualFile (typed object: no game build dependency).
        public static bool BeforeLoadTexture(object __0, ref Texture2D __result)
        {
            if (dir == null || __0 == null) return true;
            string path = null;
            try
            {
                if (fullPath == null) fullPath = __0.GetType().GetProperty("FullPath");
                path = fullPath == null ? null : fullPath.GetValue(__0, null) as string;
                if (path == null) return true;
                Texture2D tex = Load(path);
                if (tex == null) return true;
                __result = tex;
                hits++;
                changedAt = Time.realtimeSinceStartup;
                return false;
            }
            catch (Exception e)
            {
                failed++;
                changedAt = Time.realtimeSinceStartup;
                if (failed <= 5)
                    Debug.LogWarning("RIMDROID bridge: texture cache miss for " + path + ": " + e.GetBaseException().Message);
                return true;
            }
        }

        // Once a second from the frame pump: a totals line when the counters settled (after mod
        // content loading, and again after anything loaded later).
        internal static void Tick()
        {
            int total = hits + misses + stale + failed;
            if (total == reported || Time.realtimeSinceStartup < changedAt + 2f) return;
            reported = total;
            Debug.Log("RIMDROID bridge: texture cache " + hits + " hits, " + misses + " misses, " + stale + " stale, " + failed + " failed");
        }

        static Texture2D Load(string path)
        {
            string entry = Path.Combine(dir, Key(path) + ".rdtex");
            changedAt = Time.realtimeSinceStartup;
            if (!File.Exists(entry)) { misses++; return null; }
            byte[] bytes = File.ReadAllBytes(entry);
            if (bytes.Length < Header || BitConverter.ToInt32(bytes, 0) != Magic || BitConverter.ToInt32(bytes, 4) != Version)
            { stale++; return null; }
            int format = BitConverter.ToInt32(bytes, 8);
            int width = BitConverter.ToInt32(bytes, 12);
            int height = BitConverter.ToInt32(bytes, 16);
            int mips = BitConverter.ToInt32(bytes, 20);
            long srcSize = BitConverter.ToInt64(bytes, 24);
            long srcMtime = BitConverter.ToInt64(bytes, 32);
            var info = new FileInfo(path);
            long mtime = (long)Math.Floor((info.LastWriteTimeUtc - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalMilliseconds);
            if (!info.Exists || info.Length != srcSize || Math.Abs(mtime - srcMtime) > 1) { stale++; return null; }

            var tex = new Texture2D(width, height, (TextureFormat)format, mips > 1, false);
            try
            {
                // Same rule as il2mono's LoadImage hook: the mipmap limit (texture tier) must not
                // blur icons and small sprites.
                if (width < 1024 && height < 1024) tex.ignoreMipmapLimit = true;
                var data = new byte[bytes.Length - Header];
                Buffer.BlockCopy(bytes, Header, data, 0, data.Length);
                tex.LoadRawTextureData(data);
                tex.name = Path.GetFileNameWithoutExtension(path);
                tex.filterMode = FilterMode.Trilinear;
                tex.anisoLevel = 2;
                tex.Apply(false, true);
                return tex;
            }
            catch
            {
                UnityEngine.Object.Destroy(tex);
                throw;
            }
        }

        // Mirrors TextureCache.key in the launcher.
        static string Key(string path)
        {
            string rel = path;
            if (root != null && rel.StartsWith(root, StringComparison.Ordinal)) rel = rel.Substring(root.Length);
            rel = rel.Replace('\\', '/').TrimStart('/').ToLowerInvariant();
            ulong h = 14695981039346656037UL;
            foreach (byte b in Encoding.UTF8.GetBytes(rel))
            {
                h ^= b;
                h *= 1099511628211UL;
            }
            return h.ToString("x16");
        }
    }
}
