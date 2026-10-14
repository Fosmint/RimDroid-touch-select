// BepInEx (mods) on the native engine. BepInEx normally comes in through Doorstop, a preloaded library
// that hooks mono_jit_init_version; il2mono makes that call itself, so it does Doorstop's job: once the
// root domain exists, load BepInEx.Preloader.dll and run Doorstop.Entrypoint.Start(). The same as the
// box64 launch on native Mono (box64 wrappedlibmonobdwgc.c, rd_bepinex_boot), so both use the
// instance's one BepInEx folder, mods and configs.
//
// Off unless RIMDROID_BEPINEX_PRELOADER names the preloader DLL. The launcher also sets the
// DOORSTOP_* variables BepInEx reads (the game's executable path and Managed folder): there is no
// executable to derive them from here.
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "il2mono.h"

// dlopen() for managed code (Mono's config sends dlopen of "dl", "libdl" and "libdl.so.2" here: see
// NativeEngine.prepareMonoConfig). Linux code opens system libraries by their glibc names; MonoMod's
// DynDll does it for BepInEx (libc.so.6 for dup/fdopen), past Mono's dllmap. Bionic's names differ.
IL2MONO_API void* il2mono_dlopen(const char* name, int flags)
{
    static const char* const names[][2] = {
        { "libc.so.6", "libc.so" },
        { "libc", "libc.so" },
        { "libdl.so.2", "libdl.so" },
        { "libm.so.6", "libm.so" },
        { "libpthread.so.0", "libc.so" },
    };
    if (name)
    {
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        {
            if (!strcmp(name, names[i][0]))
            {
                name = names[i][1];
                break;
            }
        }
    }
    return dlopen(name, flags);
}

// Harmony (MonoMod) refuses to run on Android: its PlatformDetection reports OSKind.Android when both
// /data and /system/build.prop exist, and PlatformTriple throws NotImplementedException for that OS.
// File.Exists reaches the file system through System.Native's two stat entry points. Mono's config maps
// System.Native to "il2mono-native" (NativeEngine.prepareMonoConfig), a library that does not exist, so
// Mono's dlopen fails and asks the fallback registered here: it hands back the real libmono-native.so
// and resolves its symbols itself, with just those two replaced by versions that hide that one path.
// MonoMod then picks Linux + Arm64, which Harmony 2.4 and later support. The same mechanism as the
// box64 launch on native Mono (box64 wrappedlibmonobdwgc.c, rd_dl_fallback_symbol). A dllentry in the
// config would be simpler, but with one present Mono sent every other System.Native import to the
// entry's library (EntryPointNotFoundException for SystemNative_LChflagsCanSetHiddenFlag).
#define NATIVE_ALIAS "il2mono-native"

// The same for libc: MonoMod's Interop.Unix imports glibc's __errno_location (bionic: __errno), glibc's
// sysconf numbering and mmap with a 32-bit offset, so "libc" / "libc.so.6" map to this alias and the
// fallback serves bionic's libc.so with those three replaced (box64: rd_glibc_* in wrappedlibmonobdwgc.c).
#define LIBC_ALIAS "il2mono-libc"
static void* libc_handle;

static int* glibc_errno_location(void)
{
    return &errno;
}

// glibc numbers sysconf names differently from bionic; translate the ones desktop code asks for.
static long glibc_sysconf(int name)
{
    switch (name)
    {
        case 0: return sysconf(_SC_ARG_MAX);
        case 2: return sysconf(_SC_CLK_TCK);
        case 4: return sysconf(_SC_OPEN_MAX);
        case 30: return sysconf(_SC_PAGESIZE);
        case 31: return sysconf(_SC_RTSIG_MAX);
        case 83: return sysconf(_SC_NPROCESSORS_CONF);
        case 84: return sysconf(_SC_NPROCESSORS_ONLN);
        case 85: return sysconf(_SC_PHYS_PAGES);
        case 86: return sysconf(_SC_AVPHYS_PAGES);
    }
    LOGW("libc sysconf(%d): glibc name not translated, returning -1", name);
    errno = EINVAL;
    return -1;
}

// MonoMod declares the offset as a 32-bit int; make sure the upper half of the 64-bit off_t is clean.
static void* glibc_mmap(void* addr, size_t length, int prot, int flags, int fd, int offset)
{
    return mmap(addr, length, prot, flags, fd, (off_t)offset);
}

typedef int (*StatFn)(const char*, void*);
static StatFn real_stat2, real_lstat2;

static bool path_is_hidden(const char* path)
{
    static atomic_uchar reported;
    if (!path || strcmp(path, "/system/build.prop"))
        return false;
    if (!atomic_exchange(&reported, 1))
        LOGI("hiding /system/build.prop from managed code (MonoMod platform detection)");
    return true;
}

static int hidden_stat2(const char* path, void* output)
{
    if (path_is_hidden(path))
    {
        errno = ENOENT;
        return -1;
    }
    return real_stat2(path, output);
}

static int hidden_lstat2(const char* path, void* output)
{
    if (path_is_hidden(path))
    {
        errno = ENOENT;
        return -1;
    }
    return real_lstat2(path, output);
}

static void* native_handle;

// err stays NULL: Mono frees it with its own allocator, which Unity replaces.
static void* fallback_load(const char* name, int flags, char** err, void* user_data)
{
    (void)flags; (void)user_data;
    if (err) *err = NULL;
    if (!name)
        return NULL;
    if (strstr(name, LIBC_ALIAS))
    {
        if (!libc_handle)
        {
            libc_handle = dlopen("libc.so", RTLD_NOW);
            LOGI("libc served from bionic with glibc shims%s%s", libc_handle ? "" : " FAILED: ",
                 libc_handle ? "" : dlerror());
        }
        return libc_handle;
    }
    if (!strstr(name, NATIVE_ALIAS))
        return NULL;
    if (!native_handle)
    {
        native_handle = dlopen("libmono-native.so", RTLD_NOW);
        if (!native_handle)
        {
            LOGE("libmono-native.so: %s", dlerror());
            return NULL;
        }
        real_stat2 = (StatFn)dlsym(native_handle, "SystemNative_Stat2");
        real_lstat2 = (StatFn)dlsym(native_handle, "SystemNative_LStat2");
        LOGI("System.Native served from libmono-native.so (stat hooks %s)",
             real_stat2 && real_lstat2 ? "on" : "MISSING");
    }
    return native_handle;
}

static void* fallback_symbol(void* handle, const char* name, char** err, void* user_data)
{
    (void)user_data;
    if (err) *err = NULL;
    if (!handle || !name)
        return NULL;
    if (handle == libc_handle)
    {
        if (!strcmp(name, "__errno_location"))
            return (void*)glibc_errno_location;
        if (!strcmp(name, "sysconf"))
            return (void*)glibc_sysconf;
        if (!strcmp(name, "mmap"))
            return (void*)glibc_mmap;
        return dlsym(handle, name);
    }
    if (handle != native_handle)
        return NULL;
    if (real_stat2 && !strcmp(name, "SystemNative_Stat2"))
        return (void*)hidden_stat2;
    if (real_lstat2 && !strcmp(name, "SystemNative_LStat2"))
        return (void*)hidden_lstat2;
    return dlsym(handle, name);
}

static void* fallback_close(void* handle, void* user_data)
{
    (void)handle; (void)user_data;   // kept open: the hooks keep using it
    return NULL;
}

void il2mono_install_native_fallback(void)
{
    if (!p_mono_dl_fallback_register)
    {
        LOGE("mono_dl_fallback_register missing: System.Native cannot be served, Harmony will not run");
        return;
    }
    p_mono_dl_fallback_register((void*)fallback_load, (void*)fallback_symbol, (void*)fallback_close, NULL);
}

static void log_exception(void* exception)
{
    void* klass = p_mono_object_get_class(exception);
    LOGE("BepInEx: Start() threw %s.%s", klass ? p_mono_class_get_namespace(klass) : "?",
         klass ? p_mono_class_get_name(klass) : "?");
    // The type name alone says little (a TypeInitializationException hides the real failure in its
    // InnerException): log Exception.ToString() too.
    void* inner = NULL;
    void* text = p_mono_object_to_string(exception, &inner);
    char* utf8 = (text && !inner) ? p_mono_string_to_utf8(text) : NULL;
    if (utf8)
    {
        LOGE("BepInEx: %s", utf8);
        p_mono_free(utf8);
    }
}

void il2mono_bepinex_boot(void* domain)
{
    const char* dll = getenv("RIMDROID_BEPINEX_PRELOADER");
    if (!dll || !dll[0])
        return;
    if (!getenv("DOORSTOP_INVOKE_DLL_PATH"))
        setenv("DOORSTOP_INVOKE_DLL_PATH", dll, 1);
    if (!getenv("DOORSTOP_INITIALIZED"))
        setenv("DOORSTOP_INITIALIZED", "TRUE", 1);

    // The engine gives the domain its base directory and config file only after this point; BepInEx
    // needs them sooner (System.Configuration via Trace.Listeners throws "ExeConfigFilename cannot be
    // null"). Doorstop sets them to <exe dir> and <exe>.config; so do we.
    const char* exe = getenv("DOORSTOP_PROCESS_PATH");
    const char* slash = exe ? strrchr(exe, '/') : NULL;
    if (slash)
    {
        char base_dir[1024], config[1024];
        size_t dir_len = (size_t)(slash - exe);
        if (dir_len < sizeof(base_dir) && strlen(exe) + sizeof(".config") < sizeof(config))
        {
            memcpy(base_dir, exe, dir_len);
            base_dir[dir_len] = 0;
            snprintf(config, sizeof(config), "%s.config", exe);
            p_mono_domain_set_config(domain, base_dir, config);
            LOGI("BepInEx: domain config %s (base %s)", config, base_dir);
        }
    }

    LOGI("BepInEx: loading %s", dll);
    void* assembly = p_mono_domain_assembly_open(domain, dll);
    if (!assembly)
    {
        LOGE("BepInEx: cannot open the preloader assembly");
        return;
    }
    void* image = p_mono_assembly_get_image(assembly);
    void* klass = image ? p_mono_class_from_name(image, "Doorstop", "Entrypoint") : NULL;
    void* method = klass ? p_mono_class_get_method_from_name(klass, "Start", 0) : NULL;
    if (!method)
    {
        LOGE("BepInEx: Doorstop.Entrypoint.Start() not found");
        return;
    }
    void* exception = NULL;
    p_mono_runtime_invoke(method, NULL, NULL, &exception);
    if (exception)
    {
        log_exception(exception);
        return;
    }
    LOGI("BepInEx: preloader finished");
}
