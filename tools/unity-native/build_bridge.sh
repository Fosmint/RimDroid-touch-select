#!/bin/sh
# Build the original bridge against Unity's Android Mono assemblies; no game or Harmony DLLs needed.
# usage: build_bridge.sh <unity-managed-dir> <out-dir>
# MONO_API: framework references (Mono's 4.5-api, or a local Editor's MonoBleedingEdge/lib/mono/4.5).
# CSC: compiler executable (csc or mono-csc). For Roslyn hosted by dotnet, set CSC_DLL and DOTNET.
set -eu
UNITY=${1:?usage: build_bridge.sh <unity-managed-dir> <out-dir>}
OUT=${2:?usage: build_bridge.sh <unity-managed-dir> <out-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
MONO_API=${MONO_API:-$HERE/monoref/4.5}
test -f "$MONO_API/mscorlib.dll" || { echo "MONO_API must contain mscorlib.dll: $MONO_API" >&2; exit 1; }
test -f "$UNITY/UnityEngine.CoreModule.dll" || { echo "missing Unity Android managed assemblies: $UNITY" >&2; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
REFS="$WORK/refs.rsp"
for f in "$MONO_API/mscorlib.dll" "$MONO_API/System.dll" "$MONO_API/System.Core.dll" \
         "$MONO_API"/Facades/*.dll "$UNITY"/UnityEngine*.dll; do
    [ -f "$f" ] && printf '%s\n' "-r:\"$f\"" >> "$REFS"
done
mkdir -p "$OUT"
set -- -nologo -noconfig -nostdlib+ -target:library -langversion:7.2 -optimize+ \
    "-out:$OUT/RimDroidBridge.dll" "@$REFS" "$HERE"/bridge/*.cs
if [ -n "${CSC_DLL:-}" ]; then
    "${DOTNET:-dotnet}" "$CSC_DLL" "$@"
else
    "${CSC:-csc}" "$@"
fi
echo "built $OUT/RimDroidBridge.dll"
