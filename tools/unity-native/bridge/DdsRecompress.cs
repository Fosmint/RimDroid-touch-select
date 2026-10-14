using System;
using UnityEngine;

namespace RimDroid
{
    // Mods ship PC textures as .dds (BC7, DXT); Android GPUs have no BC formats, so Unity decompresses
    // each one to RGBA32 on upload: four bytes a pixel instead of one, and "'BC7' is not supported on
    // this platform" per texture (13 on a tester's phone, 95 % RAM, 2026-10-12). After the game's
    // loader returns such a texture, it is copied through a RenderTexture into an RGBA32 texture and
    // compressed with Unity's own mobile encoder (what Texture2D.Compress does on Android: ETC2), and
    // the original is destroyed. A quarter of the memory; ETC2 looks a little below BC7.
    // Installed as a Harmony postfix on Verse.ModDdsLoader.TryLoadDds (Bridge.ScrollPatch).
    static class DdsRecompress
    {
        static int count;

        public static void AfterTryLoadDds(ref Texture2D __result)
        {
            try
            {
                Texture2D src = __result;
                if (src == null || !IsBlockCompressed(src.format) || SystemInfo.SupportsTextureFormat(src.format)) return;
                Texture2D copy = Recompress(src);
                if (copy == null) return;
                __result = copy;
                UnityEngine.Object.Destroy(src);
                count++;
                if (count == 1 || count % 50 == 0)
                    Debug.Log("RIMDROID bridge: " + count + " DDS texture(s) re-compressed to " + copy.format
                        + " (this GPU has no BC formats)");
            }
            catch (Exception e)
            {
                Debug.LogWarning("RIMDROID bridge: DDS re-compress failed: " + e.GetBaseException().Message);
            }
        }

        static bool IsBlockCompressed(TextureFormat f)
        {
            return f == TextureFormat.BC7 || f == TextureFormat.DXT1 || f == TextureFormat.DXT5
                || f == TextureFormat.BC4 || f == TextureFormat.BC5
                || f == TextureFormat.DXT1Crunched || f == TextureFormat.DXT5Crunched;
        }

        static Texture2D Recompress(Texture2D src)
        {
            int w = src.width, h = src.height;
            bool mips = src.mipmapCount > 1;
            RenderTexture rt = RenderTexture.GetTemporary(w, h, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB);
            RenderTexture previous = RenderTexture.active;
            try
            {
                Graphics.Blit(src, rt);   // the GPU already holds the decompressed pixels
                RenderTexture.active = rt;
                Texture2D copy = new Texture2D(w, h, TextureFormat.RGBA32, mips, false);
                copy.ReadPixels(new Rect(0, 0, w, h), 0, 0, false);
                copy.Apply(mips, false);
                copy.Compress(false);     // ETC2 on Android; the fast encoder, loading is waiting
                copy.Apply(false, true);
                copy.name = src.name;
                copy.filterMode = src.filterMode;
                copy.wrapMode = src.wrapMode;
                copy.anisoLevel = src.anisoLevel;
                return copy;
            }
            finally
            {
                RenderTexture.active = previous;
                RenderTexture.ReleaseTemporary(rt);
            }
        }
    }
}
