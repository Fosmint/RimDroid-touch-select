// Where il2mono finds the game's managed side, set by the launcher through the environment
// (NativeUnityActivity copies its intent's "rimdroid.env" into the process environment before the
// player loads). Without it, the experiment layout next to Unity's data dir is used:
// <files>/il2cpp -> <files>/mono/{Managed,etc,StreamingAssets}.
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "il2mono.h"

Il2MonoConfig il2mono_cfg;

static void copy_env(char* dst, size_t size, const char* name, const char* fallback)
{
    const char* v = getenv(name);
    snprintf(dst, size, "%s", v && *v ? v : (fallback ? fallback : ""));
}

static bool is_dir(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool il2mono_config_load(const char* data_dir)
{
    char root[512] = "";
    if (data_dir)
    {
        snprintf(root, sizeof(root), "%s", data_dir);
        char* slash = strrchr(root, '/');
        if (slash)
            *slash = 0;
        strncat(root, "/mono", sizeof(root) - strlen(root) - 1);
    }
    char def_managed[600], def_etc[600], def_streaming[600];
    snprintf(def_managed, sizeof(def_managed), "%s/Managed", root);
    snprintf(def_etc, sizeof(def_etc), "%s/etc", root);
    snprintf(def_streaming, sizeof(def_streaming), "%s/StreamingAssets", root);

    copy_env(il2mono_cfg.managed_dirs, sizeof(il2mono_cfg.managed_dirs), "RIMDROID_IL2MONO_MANAGED", def_managed);
    copy_env(il2mono_cfg.etc_dir, sizeof(il2mono_cfg.etc_dir), "RIMDROID_IL2MONO_ETC", def_etc);
    copy_env(il2mono_cfg.cwd, sizeof(il2mono_cfg.cwd), "RIMDROID_IL2MONO_CWD", root);
    copy_env(il2mono_cfg.streaming_assets, sizeof(il2mono_cfg.streaming_assets), "RIMDROID_IL2MONO_STREAMING_ASSETS", def_streaming);
    copy_env(il2mono_cfg.persistent_data, sizeof(il2mono_cfg.persistent_data), "RIMDROID_IL2MONO_PERSISTENT_DATA", NULL);
    copy_env(il2mono_cfg.data_path, sizeof(il2mono_cfg.data_path), "RIMDROID_IL2MONO_DATA_PATH", NULL);
    const char* mip = getenv("RIMDROID_TEXTURE_MIPMAP_LIMIT");
    il2mono_cfg.texture_mipmap_limit = mip ? atoi(mip) : 0;
    const char* size = getenv("RIMDROID_RENDER_SIZE");
    if (!size || sscanf(size, "%dx%d", &il2mono_cfg.render_width, &il2mono_cfg.render_height) != 2
        || il2mono_cfg.render_width <= 0 || il2mono_cfg.render_height <= 0)
        il2mono_cfg.render_width = il2mono_cfg.render_height = 0;

    // The first managed dir is where mscorlib must live for mono_set_dirs.
    char first[512];
    snprintf(first, sizeof(first), "%s", il2mono_cfg.managed_dirs);
    char* colon = strchr(first, ':');
    if (colon)
        *colon = 0;
    snprintf(il2mono_cfg.primary_managed_dir, sizeof(il2mono_cfg.primary_managed_dir), "%s", first);

    LOGI("config: managed=%s etc=%s cwd=%s streaming=%s persistent=%s", il2mono_cfg.managed_dirs,
         il2mono_cfg.etc_dir, il2mono_cfg.cwd, il2mono_cfg.streaming_assets,
         il2mono_cfg.persistent_data[0] ? il2mono_cfg.persistent_data : "(engine default)");
    if (!is_dir(il2mono_cfg.primary_managed_dir))
    {
        LOGE("Mono assemblies not found: %s", il2mono_cfg.primary_managed_dir);
        return false;
    }
    return true;
}

// Finds <name> in the managed dirs, first match wins. Returns false when no dir has it.
bool il2mono_find_assembly(const char* file_name, char* out, size_t out_size)
{
    char dirs[sizeof(il2mono_cfg.managed_dirs)];
    snprintf(dirs, sizeof(dirs), "%s", il2mono_cfg.managed_dirs);
    char* save = NULL;
    for (char* dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save))
    {
        snprintf(out, out_size, "%s/%s", dir, file_name);
        struct stat st;
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
            return true;
    }
    return false;
}
