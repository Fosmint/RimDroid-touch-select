#!/bin/sh
# Fill unity/prepared/ (git-ignored) with the Unity files the native player module needs.
# Nothing of Unity is committed to this repository: everything comes from Unity's own Android Support
# package, the rest is generated or built from source here. Run once per machine / Unity version; CI
# runs it too (.github/workflows/build.yml). The module is built only with nativeEngine=true
# (settings.gradle.kts).
#
# usage: prepare_unity_module.sh <android-support.pkg>
#   <android-support.pkg>   UnitySetup-Android-Support-for-Editor-2022.3.35f1.pkg (macOS package,
#                           https://download.unity3d.com/download_unity/011206c7a712/MacEditorTargetInstaller/)
# environment: MONO_API, CSC (or CSC_DLL and DOTNET) for build_bridge.sh; PYTHON, TAR optional.
#
# Result:
#   unity/prepared/jniLibs/arm64-v8a/libunity.so   full (unstripped) engine, build-target check patched
#   unity/prepared/jniLibs/arm64-v8a/libmain.so
#   unity/prepared/libs/unity-classes.jar
#   unity/prepared/swappy/                         Unity 6 only: Unity's Swappy wrapper source (frame
#                                                  pacing); 2022.3 links Swappy into libunity itself.
#                                                  by the module's CMake
#   unity/prepared/assets/bin/Data/                boot.config and unity_app_guid (generated),
#                                                  "unity default resources" (Data/ root), and an empty
#                                                  Managed/Resources/mscorlib.dll-resources.dat: the
#                                                  player only needs it to exist (IL2CPP's data;
#                                                  il2mono runs Mono)
#   unity/prepared/assets/rimdroid_native/Managed/  Android builds that replace the game's Linux copies:
#                                                  UnityEngine*.dll (Unity's Android Mono variation).
#                                                  RimDroidBridge.dll handles legacy IMGUI wheel input.
set -e
USAGE="usage: prepare_unity_module.sh <android-support.pkg>"
PKG=${1:?$USAGE}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$ROOT/unity/prepared
PY=${PYTHON:-python}
# Fail before replacing a previously prepared module if the bridge toolchain is not configured.
test -f "${MONO_API:-$HERE/monoref/4.5}/mscorlib.dll" || {
    echo "set MONO_API to Mono's framework reference directory (see bridge/README.md)" >&2; exit 1;
}
if [ -n "${CSC_DLL:-}" ]; then
    test -f "$CSC_DLL" && command -v "${DOTNET:-dotnet}" >/dev/null || exit 1
else
    command -v "${CSC:-csc}" >/dev/null || { echo "set CSC to csc or mono-csc" >&2; exit 1; }
fi
# The package payload is a gzip'ed cpio archive: needs bsdtar (Windows' own tar.exe is bsdtar;
# GNU tar from Git Bash cannot read cpio).
if [ -z "$TAR" ]; then
    if [ -x /c/Windows/System32/tar.exe ]; then TAR=/c/Windows/System32/tar.exe
    elif command -v bsdtar >/dev/null 2>&1; then TAR=bsdtar
    else TAR=tar; fi
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

VARIATION=Variations/il2cpp/Release
echo "extracting the Android player from $PKG"
"$PY" "$HERE/xar_extract.py" "$PKG" get TargetSupport.pkg.tmp/Payload "$WORK/Payload"
(cd "$WORK" && "$TAR" -xf Payload "./$VARIATION/Libs/arm64-v8a/libunity.so" "./$VARIATION/Libs/arm64-v8a/libmain.so" \
    "./$VARIATION/Classes/classes.jar" "./Variations/mono/Managed/*" "./Data/unity default resources")

rm -rf "$OUT"
mkdir -p "$OUT/jniLibs/arm64-v8a" "$OUT/libs"
"$PY" "$HERE/patch_libunity_target.py" "$WORK/$VARIATION/Libs/arm64-v8a/libunity.so" "$OUT/jniLibs/arm64-v8a/libunity.so"
cp "$WORK/$VARIATION/Libs/arm64-v8a/libmain.so" "$OUT/jniLibs/arm64-v8a/"
cp "$WORK/$VARIATION/Classes/classes.jar" "$OUT/libs/unity-classes.jar"

# Only the player's own files go into the APK: the game data comes from the archive the launcher
# builds from the instance (NativeEngine), and a game file left here would shadow it.
DATA="$OUT/assets/bin/Data"
mkdir -p "$DATA/Managed/Resources"
# The 2022.3 Android player opens "Data/unity default resources" at the data root, not in Resources/
# as the Linux player does; without it there are no built-in shaders and the player reports the
# device as unsupported.
cp "$WORK/Data/unity default resources" "$DATA/"
# What a Unity Android player build writes (Vulkan, multithreaded rendering, full screen); taken from
# ValDroid's 6000.0 set, still to be checked against a 2022.3.35f1 Android build.
# The ids are ours: the player reads unity_app_guid during start-up and does not start without it.
cat > "$DATA/boot.config" <<'EOF'
gfx-threading-mode=4
wait-for-native-debugger=0
hdr-display-enabled=0
gc-max-time-slice=3
androidStartInFullscreen=1
androidRenderOutsideSafeArea=1
build-guid=6f3c2d9a8b1e4c57a0d4e9b2c7f15a38
EOF
printf '%s' "0b7e5a1c-3d92-4f6e-8a41-c5d2e9f07b63" > "$DATA/unity_app_guid"
# The player extracts Managed/Resources on first start and reports a missing source as "Not enough
# storage space to install required resources"; the content is IL2CPP's and unused by il2mono.
: > "$DATA/Managed/Resources/mscorlib.dll-resources.dat"

# Managed assemblies that replace the game's Linux copies on the Android player (il2mono searches this
# dir before the game's Managed folder).
MANAGED="$OUT/assets/rimdroid_native/Managed"
mkdir -p "$MANAGED"
cp "$WORK/Variations/mono/Managed/"UnityEngine*.dll "$MANAGED/"
sh "$HERE/build_bridge.sh" "$MANAGED" "$MANAGED"
echo "prepared $OUT ($(ls "$MANAGED" | wc -l) managed overrides)"
