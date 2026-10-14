// Start-up API, in the order libunity calls it (see spy/spy_run1.log).
// libunity calls several of these BEFORE il2cpp_init (icall registration, arguments, policy), so they
// are recorded here and applied once the Mono domain exists.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "il2mono.h"

void* il2mono_domain;

typedef void (*Il2CppLogCallback)(const char*);
static Il2CppLogCallback g_log_callback;
static char* g_config_dir;
static char* g_data_dir;

static int g_argc;
static char** g_argv;
static int g_exception_policy = -1;
static size_t g_pool_region_size;

// Internal calls registered before il2cpp_init; Mono's icall table exists only after init.
typedef struct
{
    char* name;
    const void* method;
} PendingIcall;
static PendingIcall* g_pending;
static int g_pending_count, g_pending_cap;
static pthread_mutex_t g_icall_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_str(char** dst, const char* src)
{
    free(*dst);
    *dst = src ? strdup(src) : NULL;
}

const char* il2mono_managed_dir(void)
{
    return il2mono_cfg.primary_managed_dir;
}

IL2MONO_API void il2cpp_register_log_callback(Il2CppLogCallback callback)
{
    FIRST_CALL();
    g_log_callback = callback;
}

IL2MONO_API void il2cpp_unity_set_android_network_up_state_func(void* func)
{
    FIRST_CALL();
}

// Unity's allocator hooks. Mono keeps using its own allocator; engine memory stats will not
// include managed metadata, which is fine for the PoC.
IL2MONO_API void il2cpp_set_memory_callbacks(void* callbacks)
{
    FIRST_CALL();
}

IL2MONO_API size_t il2cpp_memory_pool_get_region_size(void)
{
    FIRST_CALL();
    return g_pool_region_size;
}

IL2MONO_API void il2cpp_memory_pool_set_region_size(size_t size)
{
    FIRST_CALL();
    g_pool_region_size = size;
}

// ---- internal-call overrides -----------------------------------------------------------------
// Unity 6 bindings return strings as a ManagedSpanWrapper filled by the engine; managed code copies
// it and frees the buffer through BindingsAllocator.Free, so an override must allocate with the
// engine's BindingsAllocator.Malloc (captured when the engine registers it).
typedef struct
{
    void* begin;
    int32_t length;
} ManagedSpanWrapper;

typedef void (*GetPathFn)(ManagedSpanWrapper* ret);
typedef void* (*BindingsMallocFn)(int32_t size);
typedef void (*BindingsFreeFn)(void* ptr);

static GetPathFn g_orig_streaming_assets;
static GetPathFn g_orig_persistent_data;
static BindingsMallocFn g_bindings_malloc;
static BindingsFreeFn g_bindings_free;

// Answers a path icall with `path` instead of the engine's value, in the engine's own encoding and
// allocator. Falls back to the engine's answer when anything is missing.
static void answer_path(ManagedSpanWrapper* ret, GetPathFn orig_fn, const char* path, const char* what)
{
    ManagedSpanWrapper orig = {0};
    if (orig_fn)
        orig_fn(&orig);
    if (!path || !*path)
    {
        *ret = orig;
        return;
    }
    // Encoding check on the engine's own answer: UTF-16 has a 0 high byte.
    bool utf16 = orig.begin && orig.length > 1 && ((const uint8_t*)orig.begin)[1] == 0;
    size_t n = strlen(path);
    int32_t bytes = (int32_t)(utf16 ? n * 2 : n);
    void* buf = g_bindings_malloc ? g_bindings_malloc(bytes) : NULL;
    if (!buf)
    {
        *ret = orig;
        return;
    }
    if (utf16)
        for (size_t i = 0; i < n; i++)
            ((uint16_t*)buf)[i] = (uint8_t)path[i];
    else
        memcpy(buf, path, n);
    if (orig.begin && g_bindings_free)
        g_bindings_free(orig.begin);
    ret->begin = buf;
    ret->length = (int32_t)n;
    if (*what)
        LOGI("%s -> %s (%s)", what, path, utf16 ? "utf16" : "utf8");
}

// Bundles stay in the game instance (Linux layout): <instance>/RimWorldLinux_Data/StreamingAssets.
static void streaming_assets_override(ManagedSpanWrapper* ret)
{
    static atomic_uchar logged;
    answer_path(ret, g_orig_streaming_assets, il2mono_cfg.streaming_assets,
                atomic_exchange(&logged, 1) ? "" : "streamingAssetsPath");
}

// Saves shared with the emulated launch: the instance's unity3d/<company>/<product> folder.
static void persistent_data_override(ManagedSpanWrapper* ret)
{
    static atomic_uchar logged;
    answer_path(ret, g_orig_persistent_data, il2mono_cfg.persistent_data,
                atomic_exchange(&logged, 1) ? "" : "persistentDataPath");
}

// Unity 2022.3 (RimWorld 1.6) has the older binding shape: the path getters return a managed string
// (the engine builds it through il2cpp_string_new*, so a Mono string here). Answer with ours, or with
// the engine's own value when the launcher gave none.
typedef void* (*GetPathStringFn)(void);
static GetPathStringFn g_orig_streaming_assets_str;
static GetPathStringFn g_orig_persistent_data_str;

static void* answer_path_string(GetPathStringFn orig_fn, const char* path, const char* what)
{
    if (!path || !*path)
        return orig_fn ? orig_fn() : NULL;
    if (*what)
        LOGI("%s -> %s", what, path);
    return p_mono_string_new_wrapper(path);
}

static void* streaming_assets_string_override(void)
{
    static atomic_uchar logged;
    return answer_path_string(g_orig_streaming_assets_str, il2mono_cfg.streaming_assets,
                              atomic_exchange(&logged, 1) ? "" : "streamingAssetsPath");
}

static void* persistent_data_string_override(void)
{
    static atomic_uchar logged;
    return answer_path_string(g_orig_persistent_data_str, il2mono_cfg.persistent_data,
                              atomic_exchange(&logged, 1) ? "" : "persistentDataPath");
}

// Application.dataPath: on Android the player answers with its "APK" (our data archive). RimWorld takes
// the game root as dataPath's parent and loads Data/ (Core and the DLC) and Mods/ from there, so it
// gets the instance's <game>/RimWorldLinux_Data, as the Linux player would answer.
static GetPathStringFn g_orig_data_path_str;

static void* data_path_string_override(void)
{
    static atomic_uchar logged;
    return answer_path_string(g_orig_data_path_str, il2mono_cfg.data_path,
                              atomic_exchange(&logged, 1) ? "" : "dataPath");
}

// The game is a desktop build: games may pick their config by Application.platform (Valheim did) and has
// none for Android ("Couldn't find any graphics configurations for the specified platform!").
// Managed code sees LinuxPlayer, matching the Linux data it runs on; the native engine is unaffected.
#define RUNTIME_PLATFORM_LINUX_PLAYER 13

// Keep the screen on: Unity applies Screen.sleepTimeout (SystemSetting by default) and clears the
// window's keep-screen-on flag. Every value the game sets becomes NeverSleep, and NeverSleep is
// applied once from the main thread (the first Application.platform query comes from there).
#define SLEEP_TIMEOUT_NEVER_SLEEP (-1)
typedef void (*SetSleepTimeoutFn)(int32_t value);
static SetSleepTimeoutFn g_orig_set_sleep_timeout;

static void set_sleep_timeout_override(int32_t value)
{
    if (g_orig_set_sleep_timeout)
        g_orig_set_sleep_timeout(SLEEP_TIMEOUT_NEVER_SLEEP);
}

// Always full screen: the game applies its Prefs "fullscreen" (a window on the desktop) through
// Screen.fullScreen / fullScreenMode / SetResolution, and on Android "windowed" means the system
// bars come back over the game (Samsung's gesture pill). FullScreenMode: 0 ExclusiveFullScreen,
// 1 FullScreenWindow, 2 MaximizedWindow, 3 Windowed. The resolution the game asks for is kept.
#define FULL_SCREEN_MODE_FULL_SCREEN_WINDOW 1
typedef void (*SetFullScreenFn)(int32_t value);
typedef void (*SetFullScreenModeFn)(int32_t mode);
typedef void (*SetResolutionFn)(int32_t width, int32_t height, int32_t mode, void* refresh_rate);
static SetFullScreenFn g_orig_set_full_screen;
static SetFullScreenModeFn g_orig_set_full_screen_mode;
static SetResolutionFn g_orig_set_resolution;

static void set_full_screen_override(int32_t value)
{
    if (g_orig_set_full_screen)
        g_orig_set_full_screen(1);
}

static void set_full_screen_mode_override(int32_t mode)
{
    if (g_orig_set_full_screen_mode)
        g_orig_set_full_screen_mode(FULL_SCREEN_MODE_FULL_SCREEN_WINDOW);
}

// The launcher's render resolution (RIMDROID_RENDER_SIZE) replaces the size the game asks for: fewer
// pixels on a weak GPU, a bigger UI. The game asks once at start-up (its Prefs); if it never does
// (Prefs already at the screen's size), the size is applied once from the main thread below.
static void set_resolution_override(int32_t width, int32_t height, int32_t mode, void* refresh_rate)
{
    int32_t w = il2mono_cfg.render_width > 0 ? il2mono_cfg.render_width : width;
    int32_t h = il2mono_cfg.render_height > 0 ? il2mono_cfg.render_height : height;
    if (mode != FULL_SCREEN_MODE_FULL_SCREEN_WINDOW || w != width || h != height)
        LOGI("Screen.SetResolution(%d x %d, mode %d) -> %d x %d, full screen window", width, height, mode, w, h);
    if (g_orig_set_resolution)
        g_orig_set_resolution(w, h, FULL_SCREEN_MODE_FULL_SCREEN_WINDOW, refresh_rate);
}

// The texture tier shrinks through Unity's global mipmap limit, which would also quarter the UI
// icons and pawn textures the game loads from PNG files (64-512 px: blurry buttons, TECNO tester
// 2026-10-11). As the box64 mip shim, only the big ones should shrink: every texture the game loads
// with LoadImage that comes out narrower than SMALL_TEXTURE_MAX is flagged to ignore the limit. The
// atlases the game bakes at run time (where the memory is) are not loaded this way and keep shrinking.
#define SMALL_TEXTURE_MAX 1024
typedef int32_t (*LoadImageFn)(void* texture, void* data, int32_t mark_non_readable);
typedef int32_t (*GetDataWidthFn)(void* texture);
typedef void (*SetIgnoreMipmapLimitFn)(void* texture, int32_t value);
static LoadImageFn g_orig_load_image;
static GetDataWidthFn g_get_data_width;
static SetIgnoreMipmapLimitFn g_set_ignore_mipmap_limit;
static atomic_int g_textures_kept, g_textures_shrunk;

static int32_t load_image_override(void* texture, void* data, int32_t mark_non_readable)
{
    int32_t ok = g_orig_load_image(texture, data, mark_non_readable);
    if ((uint8_t)ok && texture && il2mono_cfg.texture_mipmap_limit > 0 && g_get_data_width && g_set_ignore_mipmap_limit)
    {
        int32_t width = g_get_data_width(texture);
        if (width > 0 && width < SMALL_TEXTURE_MAX)
        {
            g_set_ignore_mipmap_limit(texture, 1);
            if (atomic_fetch_add(&g_textures_kept, 1) == 0)
                LOGI("texture tier: textures under %d px keep their size (first: %d px)", SMALL_TEXTURE_MAX, width);
        }
        else if (width > 0 && atomic_fetch_add(&g_textures_shrunk, 1) == 0)
            LOGI("texture tier: first texture shrunk by the mipmap limit: %d px", width);
    }
    return ok;
}

// Desktop games use targetFrameRate -1 for "unlimited"; on Android -1 means the platform default,
// 30 fps. The launcher's frame-rate mode gives the cap (RIMDROID_FPS_CAP: the planned rate, or the
// screen's top rate with the mode off): "unlimited" (<= 0) and anything above it become the cap, a
// lower limit of the game's own passes through. Without the variable "unlimited" is 120.
#define UNLIMITED_FRAME_RATE 120
typedef void (*SetTargetFrameRateFn)(int32_t value);
static SetTargetFrameRateFn g_orig_set_target_frame_rate;

static int32_t frame_rate_cap(void)
{
    const char* env = getenv("RIMDROID_FPS_CAP");
    int cap = env ? atoi(env) : 0;
    return cap > 0 ? cap : UNLIMITED_FRAME_RATE;
}

static void set_target_frame_rate_override(int32_t value)
{
    int32_t cap = frame_rate_cap();
    int32_t applied = (value <= 0 || value > cap) ? cap : value;
    static int32_t last = -2;
    if (applied != last)
    {
        LOGI("Application.targetFrameRate %d -> %d", value, applied);
        last = applied;
    }
    if (g_orig_set_target_frame_rate)
        g_orig_set_target_frame_rate(applied);
}

// Texture tier (Settings → Video): Unity's own global texture mipmap limit, the engine-side twin of
// the box64 mip shim. Applied once from the main thread, with the sleep timeout.
typedef void (*SetMipmapLimitFn)(int32_t limit);
static SetMipmapLimitFn g_set_texture_mipmap_limit;

static int32_t platform_override(void)
{
    static atomic_uchar applied;
    if (!atomic_exchange(&applied, 1))
    {
        if (g_orig_set_sleep_timeout)
        {
            g_orig_set_sleep_timeout(SLEEP_TIMEOUT_NEVER_SLEEP);
            LOGI("Screen.sleepTimeout -> NeverSleep");
        }
        if (g_set_texture_mipmap_limit && il2mono_cfg.texture_mipmap_limit > 0)
        {
            g_set_texture_mipmap_limit(il2mono_cfg.texture_mipmap_limit);
            LOGI("QualitySettings.globalTextureMipmapLimit -> %d", il2mono_cfg.texture_mipmap_limit);
        }
        if (g_orig_set_resolution && il2mono_cfg.render_width > 0)
        {
            // UnityEngine.RefreshRate { uint numerator; uint denominator; }: 0/1 = the default rate.
            struct { uint32_t numerator, denominator; } rate = { 0, 1 };
            g_orig_set_resolution(il2mono_cfg.render_width, il2mono_cfg.render_height,
                                  FULL_SCREEN_MODE_FULL_SCREEN_WINDOW, &rate);
            LOGI("render size -> %d x %d (launcher preset)", il2mono_cfg.render_width, il2mono_cfg.render_height);
        }
    }
    return RUNTIME_PLATFORM_LINUX_PLAYER;
}

static const void* override_icall(const char* name, const void* method)
{
    if (!strcmp(name, "UnityEngine.Application::set_targetFrameRate"))
    {
        g_orig_set_target_frame_rate = (SetTargetFrameRateFn)method;
        LOGI("icall override: %s (unlimited -> %d)", name, UNLIMITED_FRAME_RATE);
        return (const void*)set_target_frame_rate_override;
    }
    if (!strcmp(name, "UnityEngine.Screen::set_sleepTimeout"))
    {
        g_orig_set_sleep_timeout = (SetSleepTimeoutFn)method;
        LOGI("icall override: %s -> NeverSleep", name);
        return (const void*)set_sleep_timeout_override;
    }
    if (!strcmp(name, "UnityEngine.Screen::set_fullScreen"))
    {
        g_orig_set_full_screen = (SetFullScreenFn)method;
        LOGI("icall override: %s -> true", name);
        return (const void*)set_full_screen_override;
    }
    if (!strcmp(name, "UnityEngine.Screen::set_fullScreenMode"))
    {
        g_orig_set_full_screen_mode = (SetFullScreenModeFn)method;
        LOGI("icall override: %s -> FullScreenWindow", name);
        return (const void*)set_full_screen_mode_override;
    }
    if (!strcmp(name, "UnityEngine.Screen::SetResolution_Injected"))
    {
        g_orig_set_resolution = (SetResolutionFn)method;
        LOGI("icall override: %s -> FullScreenWindow", name);
        return (const void*)set_resolution_override;
    }
    if (!strcmp(name, "UnityEngine.Application::get_platform"))
    {
        LOGI("icall override: %s -> LinuxPlayer", name);
        return (const void*)platform_override;
    }
    if (!strcmp(name, "UnityEngine.QualitySettings::set_globalTextureMipmapLimit"))
        g_set_texture_mipmap_limit = (SetMipmapLimitFn)method;   // kept, not replaced
    if (!strcmp(name, "UnityEngine.Texture::GetDataWidth"))
        g_get_data_width = (GetDataWidthFn)method;               // kept, not replaced
    if (!strcmp(name, "UnityEngine.Texture2D::set_ignoreMipmapLimit"))
        g_set_ignore_mipmap_limit = (SetIgnoreMipmapLimitFn)method;   // kept, not replaced
    if (!strcmp(name, "UnityEngine.ImageConversion::LoadImage"))
    {
        g_orig_load_image = (LoadImageFn)method;
        LOGI("icall override: %s (small textures keep their size under the texture tier)", name);
        return (const void*)load_image_override;
    }
    if (!strcmp(name, "UnityEngine.Bindings.BindingsAllocator::Malloc"))
        g_bindings_malloc = (BindingsMallocFn)method;
    else if (!strcmp(name, "UnityEngine.Bindings.BindingsAllocator::Free"))
        g_bindings_free = (BindingsFreeFn)method;
    else if (!strcmp(name, "UnityEngine.Application::get_streamingAssetsPath_Injected"))
    {
        g_orig_streaming_assets = (GetPathFn)method;
        LOGI("icall override: %s", name);
        return (const void*)streaming_assets_override;
    }
    else if (!strcmp(name, "UnityEngine.Application::get_persistentDataPath_Injected"))
    {
        g_orig_persistent_data = (GetPathFn)method;
        LOGI("icall override: %s", name);
        return (const void*)persistent_data_override;
    }
    else if (!strcmp(name, "UnityEngine.Application::get_streamingAssetsPath"))   // Unity 2022.3
    {
        g_orig_streaming_assets_str = (GetPathStringFn)method;
        LOGI("icall override: %s", name);
        return (const void*)streaming_assets_string_override;
    }
    else if (!strcmp(name, "UnityEngine.Application::get_persistentDataPath"))    // Unity 2022.3
    {
        g_orig_persistent_data_str = (GetPathStringFn)method;
        LOGI("icall override: %s", name);
        return (const void*)persistent_data_string_override;
    }
    else if (!strcmp(name, "UnityEngine.Application::get_dataPath"))              // Unity 2022.3
    {
        g_orig_data_path_str = (GetPathStringFn)method;
        LOGI("icall override: %s", name);
        return (const void*)data_path_string_override;
    }
    return method;
}

IL2MONO_API void il2cpp_add_internal_call(const char* name, const void* method)
{
    FIRST_CALL();
    method = override_icall(name, method);
    pthread_mutex_lock(&g_icall_lock);
    if (il2mono_domain)
    {
        pthread_mutex_unlock(&g_icall_lock);
        p_mono_add_internal_call(name, method);
        return;
    }
    if (g_pending_count == g_pending_cap)
    {
        g_pending_cap = g_pending_cap ? g_pending_cap * 2 : 1024;
        g_pending = realloc(g_pending, g_pending_cap * sizeof(*g_pending));
    }
    g_pending[g_pending_count].name = strdup(name);
    g_pending[g_pending_count].method = method;
    g_pending_count++;
    pthread_mutex_unlock(&g_icall_lock);
}

IL2MONO_API void il2cpp_runtime_unhandled_exception_policy_set(int policy)
{
    FIRST_CALL();
    g_exception_policy = policy;
    if (il2mono_domain)
        p_mono_runtime_unhandled_exception_policy_set(policy);
}

IL2MONO_API void il2cpp_set_commandline_arguments(int argc, const char* const argv[], const char* basedir)
{
    FIRST_CALL();
    g_argc = argc;
    g_argv = calloc(argc + 1, sizeof(char*));
    for (int i = 0; i < argc; i++)
        g_argv[i] = strdup(argv[i] ? argv[i] : "");
}

IL2MONO_API void il2cpp_set_config_dir(const char* path)
{
    FIRST_CALL();
    set_str(&g_config_dir, path);
}

IL2MONO_API void il2cpp_set_data_dir(const char* path)
{
    FIRST_CALL();
    set_str(&g_data_dir, path);
    LOGI("data dir: %s", path ? path : "(null)");
}

IL2MONO_API void il2cpp_debugger_set_agent_options(const char* options)
{
    FIRST_CALL();
}

IL2MONO_API void il2cpp_set_config(const char* executable_path)
{
    FIRST_CALL();
}

IL2MONO_API void il2cpp_override_stack_backtrace(void* callback)
{
    FIRST_CALL();
}

IL2MONO_API void il2cpp_set_find_plugin_callback(void* callback)
{
    FIRST_CALL();
    if (il2mono_domain)
        p_mono_set_find_plugin_callback(callback);
}

static void mono_log_to_logcat(const char* log_domain, const char* log_level, const char* message, int fatal, void* user_data)
{
    __android_log_print(fatal ? ANDROID_LOG_FATAL : ANDROID_LOG_INFO, "IL2MONO-mono", "[%s %s] %s",
                        log_domain ? log_domain : "", log_level ? log_level : "", message);
}

static void mono_print_to_logcat(const char* text, int is_stdout)
{
    __android_log_print(is_stdout ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "IL2MONO-mono", "%s", text);
}

IL2MONO_API int il2cpp_init(const char* domain_name)
{
    FIRST_CALL();
    if (!il2mono_load_mono() || !il2mono_config_load(g_data_dir))
        return 0;
    LOGI("booting Mono: assemblies=%s config=%s", il2mono_cfg.managed_dirs, il2mono_cfg.etc_dir);

    // Games read files relative to the working directory (steam_appid.txt for Steamworks), which is
    // the game folder on desktop but "/" in an Android app process.
    if (il2mono_cfg.cwd[0] && chdir(il2mono_cfg.cwd) != 0)
        LOGW("chdir(%s) failed", il2mono_cfg.cwd);

    p_mono_trace_set_log_handler(mono_log_to_logcat, NULL);
    p_mono_trace_set_print_handler(mono_print_to_logcat);
    p_mono_trace_set_printerr_handler(mono_print_to_logcat);
    p_mono_set_dirs(il2mono_cfg.primary_managed_dir, il2mono_cfg.etc_dir);
    p_mono_set_assemblies_path(il2mono_cfg.managed_dirs);
    p_mono_config_parse(NULL);

    il2mono_install_native_fallback();
    void* domain = p_mono_jit_init_version(domain_name ? domain_name : "IL2CPP Root Domain", "v4.0.30319");
    if (!domain)
    {
        LOGE("mono_jit_init_version failed");
        return 0;
    }
    // The domain's base directory and configuration file, as Unity's desktop player sets them. Without
    // a config path System.Configuration throws "The 'ExeConfigFilename' argument cannot be null", and
    // everything that reads configuration fails with it: XmlSerializer (NL Facial Animation drew no
    // faces), mods writing settings or caches (TrueTerrainColors). The file need not exist: a missing
    // one reads as empty. BepInEx (bepinex.c) may set its own afterwards.
    {
        const char* base_dir = il2mono_cfg.cwd[0] ? il2mono_cfg.cwd : il2mono_cfg.primary_managed_dir;
        char config[1100];
        snprintf(config, sizeof(config), "%s/RimWorldLinux.exe.config", base_dir);
        p_mono_domain_set_config(domain, base_dir, config);
        LOGI("domain config %s (base %s)", config, base_dir);
    }

    pthread_mutex_lock(&g_icall_lock);
    for (int i = 0; i < g_pending_count; i++)
    {
        p_mono_add_internal_call(g_pending[i].name, g_pending[i].method);
        free(g_pending[i].name);
    }
    LOGI("registered %d internal calls queued before init", g_pending_count);
    free(g_pending);
    g_pending = NULL;
    g_pending_count = g_pending_cap = 0;
    il2mono_domain = domain;
    pthread_mutex_unlock(&g_icall_lock);

    il2mono_register_input_icalls();
    if (g_exception_policy >= 0)
        p_mono_runtime_unhandled_exception_policy_set(g_exception_policy);
    if (g_argc > 0)
        p_mono_runtime_set_main_args(g_argc, g_argv);
    il2mono_bepinex_boot(domain);

    LOGI("Mono domain up: %p", domain);
    return 1;
}
