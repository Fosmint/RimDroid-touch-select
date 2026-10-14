using System;
using System.Collections;
using System.Reflection;
using System.Runtime.CompilerServices;
using UnityEngine;

namespace RimDroid
{
    // Loaded by the player, not by RimWorld's mod loader. No game or Harmony build dependency.
    public static class Bridge
    {
        [MethodImpl(MethodImplOptions.InternalCall)]
        static extern float TakeScroll(out float x, out float y);

        [MethodImpl(MethodImplOptions.InternalCall)]
        static extern void SetScrollReady(bool ready);

        static bool initialized, failed, ready;
        static float pending, mouseX, mouseY, nextHarmonyCheck;
        static int frame = -1;
        static GameApi game;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        public static void Initialize()
        {
            if (initialized) return;
            initialized = true;
            try
            {
                game = new GameApi();
                // The game loads every mod's assemblies on its loading thread and only then, in one
                // main-thread pass, their textures: the texture cache prefix has to be in place the
                // moment 0Harmony (built-in mod) appears, not at the next once-a-second frame check.
                AppDomain.CurrentDomain.AssemblyLoad += (sender, args) =>
                {
                    try
                    {
                        if (args.LoadedAssembly.GetName().Name == "0Harmony") ScrollPatch.TryInstall(game.BeginScrollView);
                    }
                    catch (Exception e) { Debug.LogWarning("RIMDROID bridge: patch on assembly load failed: " + e.GetBaseException().Message); }
                };
                var go = new GameObject("RimDroid scroll bridge");
                UnityEngine.Object.DontDestroyOnLoad(go);
                go.AddComponent<ScrollPump>();
                // Which compressed formats this GPU takes as they are: the game's own assets are
                // DXT/BC7; where those are unsupported Unity unpacks them to RGBA32 on load.
                Debug.Log("RIMDROID bridge: GPU texture formats: DXT1 " + SystemInfo.SupportsTextureFormat(TextureFormat.DXT1)
                    + ", DXT5 " + SystemInfo.SupportsTextureFormat(TextureFormat.DXT5)
                    + ", BC7 " + SystemInfo.SupportsTextureFormat(TextureFormat.BC7)
                    + ", ETC2 " + SystemInfo.SupportsTextureFormat(TextureFormat.ETC2_RGBA8)
                    + ", ASTC " + SystemInfo.SupportsTextureFormat(TextureFormat.ASTC_4x4));
                Debug.Log("RIMDROID bridge: initialized; zoom available, waiting for Harmony (built-in mod) for lists");
            }
            catch (Exception e) { Fail(e); }
        }

        internal static void BeginFrame()
        {
            if (failed) return;
            try
            {
                if (!ready) { SetScrollReady(true); ready = true; }
                // Read exactly once per rendered frame. Never carry unhandled input into another UI.
                pending = TakeScroll(out mouseX, out mouseY);
                frame = Time.frameCount;
                if (Time.realtimeSinceStartup >= nextHarmonyCheck)
                {
                    nextHarmonyCheck = Time.realtimeSinceStartup + 1f;
                    ScrollPatch.TryInstall(game.BeginScrollView);
                    TextureCacheLoader.Tick();
                }
            }
            catch (Exception e) { Fail(e); }
        }

        internal static void EndFrame()
        {
            float notches = pending;
            pending = 0;
            if (failed || notches == 0 || frame != Time.frameCount) return;
            try { game.Zoom(notches, mouseX, mouseY); }
            catch (Exception e) { Fail(e); }
        }

        // Harmony supplies arguments by index, independent of game parameter names.
        public static void BeforeScrollView(Rect __0, ref Vector2 __1, Rect __2, bool __3)
        {
            if (failed || pending == 0 || frame != Time.frameCount) return;
            // Layout and input passes may call the same list repeatedly. Only Repaint spends input.
            if (Event.current == null || Event.current.type != EventType.Repaint) return;
            try
            {
                // This includes GUI.matrix (UI scale) and the current window/group/scroll offset.
                Vector2 local = GUIUtility.ScreenToGUIPoint(new Vector2(mouseX * Screen.width, mouseY * Screen.height));
                if (!__0.Contains(local) || game.InputBlocked()) return;
                float notches = pending;
                pending = 0; // Also consume at a list boundary: never zoom the map behind the list.
                float visibleHeight = __0.height;
                if (__3)
                {
                    float visibleWidth = __0.width;
                    if (__2.height > visibleHeight)
                        visibleWidth -= GUI.skin.verticalScrollbar.fixedWidth + GUI.skin.verticalScrollbar.margin.left;
                    if (__2.width > visibleWidth)
                        visibleHeight -= GUI.skin.horizontalScrollbar.fixedHeight + GUI.skin.horizontalScrollbar.margin.top;
                }
                // Android notches are positive up; IMGUI ScrollWheel.delta.y is positive down.
                __1.y = Mathf.Clamp(__1.y - notches * 20f, 0f, Mathf.Max(0f, __2.height - visibleHeight));
            }
            catch (Exception e) { Fail(e); }
        }

        internal static void Suspend()
        {
            pending = 0;
            ready = false;
            DoubleClick.Reset();
            SetScrollReady(false); // Clears native input too, so a resume cannot replay old notches.
        }

        static void Fail(Exception e)
        {
            if (failed) return;
            failed = true;
            Suspend();
            Debug.LogWarning("RIMDROID bridge: scroll disabled; using zoom keys (" + e.GetBaseException().Message + ")");
        }
    }

    public sealed class ScrollPump : MonoBehaviour
    {
        bool paused;

        void Awake() { useGUILayout = false; }
        void Update() { if (!paused) Bridge.BeginFrame(); }
        void OnGUI() { if (!paused) DoubleClick.OnGUI(); }

        IEnumerator Start()
        {
            var end = new WaitForEndOfFrame();
            while (true)
            {
                // Runs after all IMGUI passes. A hovered list gets first refusal before camera zoom.
                yield return end;
                if (!paused) Bridge.EndFrame();
            }
        }

        void OnApplicationPause(bool value) { paused = value; if (value) Bridge.Suspend(); }
        void OnDisable() { Bridge.Suspend(); }
    }

    static class ScrollPatch
    {
        static bool finished;

        internal static void TryInstall(MethodInfo original)
        {
            if (finished) return;
            // Use whichever 0Harmony the mod loader already loaded: on the native engine RimDroid's
            // built-in mod always carries one, so this no longer depends on the player's mods. The
            // bridge never loads a Harmony DLL itself (a disabled mod's copy could conflict).
            foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
            {
                if (assembly.GetName().Name != "0Harmony") continue;
                finished = true;
                try
                {
                    Type harmonyType = assembly.GetType("HarmonyLib.Harmony", true);
                    Type methodType = assembly.GetType("HarmonyLib.HarmonyMethod", true);
                    object harmony = Activator.CreateInstance(harmonyType, new object[] { "rimdroid.native.scroll" });
                    var prefix = typeof(Bridge).GetMethod("BeforeScrollView");
                    object patchInfo = Activator.CreateInstance(methodType, new object[] { prefix });
                    MethodInfo patch = null;
                    foreach (MethodInfo candidate in harmonyType.GetMethods())
                    {
                        ParameterInfo[] p = candidate.GetParameters();
                        if (candidate.Name == "Patch" && p.Length >= 2 && p[0].ParameterType == typeof(MethodBase)
                            && p[1].ParameterType == methodType) { patch = candidate; break; }
                    }
                    if (patch == null) throw new MissingMethodException("Harmony.Patch");
                    object[] args = new object[patch.GetParameters().Length];
                    args[0] = original;
                    args[1] = patchInfo;
                    patch.Invoke(harmony, args);
                    Debug.Log("RIMDROID bridge: list scrolling enabled with Harmony " + assembly.GetName().Version);
                    // DDS textures the GPU cannot hold compressed (DdsRecompress): a postfix on the loader.
                    try
                    {
                        Type dds = Type.GetType("Verse.ModDdsLoader, Assembly-CSharp", false);
                        MethodInfo loader = dds == null ? null : dds.GetMethod("TryLoadDds",
                            BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
                        if (loader == null) Debug.Log("RIMDROID bridge: no ModDdsLoader.TryLoadDds in this game build; DDS textures stay as they are");
                        else
                        {
                            object postfix = Activator.CreateInstance(methodType, new object[] { typeof(DdsRecompress).GetMethod("AfterTryLoadDds") });
                            object[] ddsArgs = new object[patch.GetParameters().Length];
                            ddsArgs[0] = loader;
                            ddsArgs[2] = postfix;
                            patch.Invoke(harmony, ddsArgs);
                            Debug.Log("RIMDROID bridge: DDS textures are re-compressed for this GPU");
                        }
                    }
                    catch (Exception e)
                    {
                        Debug.LogWarning("RIMDROID bridge: DDS re-compress patch unavailable (" + e.GetBaseException().Message + ")");
                    }
                    // Restart through the launcher (RestartPatch): a prefix on GenCommandLine.Restart.
                    try
                    {
                        Type cmd = Type.GetType("Verse.GenCommandLine, Assembly-CSharp", false);
                        MethodInfo restart = cmd == null ? null : cmd.GetMethod("Restart", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
                        if (restart == null) Debug.Log("RIMDROID bridge: no GenCommandLine.Restart in this game build");
                        else
                        {
                            object restartPrefix = Activator.CreateInstance(methodType, new object[] { typeof(RestartPatch).GetMethod("BeforeRestart") });
                            object[] restartArgs = new object[patch.GetParameters().Length];
                            restartArgs[0] = restart;
                            restartArgs[1] = restartPrefix;
                            patch.Invoke(harmony, restartArgs);
                        }
                    }
                    catch (Exception e)
                    {
                        Debug.LogWarning("RIMDROID bridge: restart patch unavailable (" + e.GetBaseException().Message + ")");
                    }
                    // The launcher's texture cache (TextureCacheLoader): a prefix on the texture loader.
                    try
                    {
                        if (!TextureCacheLoader.Enabled) Debug.Log("RIMDROID bridge: texture cache off");
                        else
                        {
                            Type loaderType = Type.GetType("Verse.ModContentLoader`1, Assembly-CSharp", false);
                            MethodInfo loadTexture = loaderType == null ? null : loaderType.MakeGenericType(typeof(Texture2D))
                                .GetMethod("LoadTexture", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
                            if (loadTexture == null) Debug.Log("RIMDROID bridge: no ModContentLoader<Texture2D>.LoadTexture in this game build; texture cache unused");
                            else
                            {
                                object cachePrefix = Activator.CreateInstance(methodType, new object[] { typeof(TextureCacheLoader).GetMethod("BeforeLoadTexture") });
                                object[] cacheArgs = new object[patch.GetParameters().Length];
                                cacheArgs[0] = loadTexture;
                                cacheArgs[1] = cachePrefix;
                                patch.Invoke(harmony, cacheArgs);
                                Debug.Log("RIMDROID bridge: texture cache serves the texture loader");
                            }
                        }
                    }
                    catch (Exception e)
                    {
                        Debug.LogWarning("RIMDROID bridge: texture cache patch unavailable (" + e.GetBaseException().Message + ")");
                    }
                }
                catch (Exception e)
                {
                    // Zoom remains independent of Harmony, including unsupported Harmony versions.
                    Debug.LogWarning("RIMDROID bridge: list scrolling unavailable (" + e.GetBaseException().Message + ")");
                }
                return;
            }
        }
    }
}
