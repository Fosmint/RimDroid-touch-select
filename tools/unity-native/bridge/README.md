# Native mouse input

`RimDroidBridge.dll` is built from these original sources. It references Unity's Android managed
assemblies and a Mono framework profile, not RimWorld or Harmony. Nothing from Unity or the game
belongs in this directory. The result lives in the ignored `unity/prepared/` tree.

`NativeEngine` adds the assembly and its `BeforeSceneLoad` initializer to the two manifests **inside
the generated archive**. The source manifests and the game's assemblies are not modified. il2mono
loads the bridge from the managed overrides directory; the data archive also contains its DLL.

## Build

CI installs `mono-devel` and invokes module preparation with `CSC=mono-csc` and
`MONO_API=/usr/lib/mono/4.5-api` when `NATIVE_ENGINE=true`. Preparation calls `build_bridge.sh`.
To rebuild only the bridge after editing:

```sh
CSC=mono-csc MONO_API=/usr/lib/mono/4.5-api sh tools/unity-native/build_bridge.sh \
  unity/prepared/assets/rimdroid_native/Managed unity/prepared/assets/rimdroid_native/Managed
```

On Windows, set `MONO_API` to an external Mono `lib/mono/4.5` directory, `DOTNET` to the local
Unity Editor's `Data/NetCoreRuntime/dotnet.exe` and `CSC_DLL` to its `Data/DotNetSdkRoslyn/csc.dll`.
Use Git Bash and forward-slash absolute paths for the reference and output directories. Paths may
contain spaces. A full Editor is not needed in CI; its compiler is only a convenient local option.
Then build the APK with `gradlew :app:assembleDebug -PnativeEngine=true`.

## Routing

* JNI atomically submits signed wheel notches and viewport-relative cursor coordinates. Positive
  means up. It declines input until the managed pump reports ready, so Java can use the existing
  zoom-key fallback. Fractional physical wheel deltas are preserved.
* `Update` takes the accumulated input once. A Harmony prefix on `Widgets.BeginScrollView` consumes
  it during Repaint at the hovered, input-accepting list, using Unity's 20-point wheel step and bounds.
  `ScreenToGUIPoint` accounts for resolution, UI scale, window origins and scroll offsets. Layout
  passes cannot apply the same input again. A list at its limit still consumes the wheel.
* `WaitForEndOfFrame` gives every IMGUI pass priority before applying any remaining input to a camera.
  Window hit testing and the game's `AnythingPreventsCameraMotion` gates prevent zoom through UI.
  Without Harmony the zoom path still runs; windows still block zoom.
* The map formula matches RimWorld 1.6 `CameraDriverOnGUI`: subtract
  `notches * ScrollWheelZoomRate * config.zoomSpeed * RootSize / 35`, then clamp to `config.sizeRange`.
  `MapDragSensitivity` applies to drag, not zoom. The world formula uses `0.1 * zoomSpeed * TrueAltitude
  / 12`, clamps to `MinAltitude..1100`, and preserves the 1.6 planet-layer transition behavior.
* Only Harmony already loaded by enabled player mods is used. There is no bundled copy, no disk
  probing that loads disabled mods, and no hard Harmony reference. A failed optional patch does not
  disable zoom. Startup/patch failures are logged once rather than every frame.
* An early, invisible OnGUI observer reconstructs missing Android double-click counts. RimWorld
  performs its own selection unchanged. Two clicks must be nearby, released between presses, within
  300 ms, and not part of a drag. A native count of two or more is preserved. This is independent of
  Harmony. Pause/disable clears wheel and click state.

## Device checks

On a native RimWorld 1.6 instance, check both with and without an enabled `brrainz.harmony` mod:

1. Check `native_engine.log` for bridge initialization and `RimDroid scroll bridge: ready`; with
   Harmony, also `list scrolling enabled`. Missing Harmony is an informational wait, not an error.
2. USB wheel over the map zooms in/out. Repeat on the world map, including a planet-layer boundary.
3. With Harmony, hover a long mods/settings list and scroll in both directions, including its limits.
   The camera must not move underneath. Repeat with a non-default UI scale and a boxed render preset.
4. Pinch and the overlay's Zoom in/out buttons use the same route. Repeated Layout/Repaint passes
   must not multiply one notch. Fractional mouse/trackpad deltas should accumulate normally.
5. Without Harmony, map/world zoom still works; list wheel scrolling is unavailable. Wheel input
   over an open dialog must not zoom the camera behind it.
6. Double-click an item/plant to select matching visible objects; single click and drag selection
   still work. Check a slow second click, movement between clicks and the colonist bar as well.
7. Pause/resume does not replay old input. Check the log for bridge exceptions.
