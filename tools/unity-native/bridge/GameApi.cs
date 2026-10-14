using System;
using System.Reflection;
using UnityEngine;

namespace RimDroid
{
    // Runtime bindings to the player's own unmodified RimWorld 1.6 assembly. Cached once, so no
    // dependency on proprietary game DLLs is needed in CI or in the APK's bridge assembly.
    sealed class GameApi
    {
        const BindingFlags Flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance;
        readonly PropertyInfo windowStack, currentMap, world, mapCamera, worldCamera, worldSelected, worldBackground;
        readonly PropertyInfo inputBlocked, programState, zoomSwitchLayer, selectedLayer;
        readonly FieldInfo screenWidth, screenHeight, layerIn, layerOut;
        readonly PropertyInfo extraAltitude;
        readonly MethodInfo windowAt;
        readonly CameraApi map, planet;
        internal readonly MethodInfo BeginScrollView;

        static Type GameType(string name) { return Type.GetType(name + ", Assembly-CSharp", true); }
        static FieldInfo Field(Type type, string name)
        {
            return type.GetField(name, Flags) ?? throw new MissingFieldException(type.FullName, name);
        }
        static PropertyInfo Property(Type type, string name)
        {
            return type.GetProperty(name, Flags) ?? throw new MissingMemberException(type.FullName, name);
        }
        static object Read(PropertyInfo p, object instance = null) { return p.GetValue(instance, null); }
        static float Number(FieldInfo f, object instance) { return (float)f.GetValue(instance); }
        static float Number(PropertyInfo p, object instance = null) { return (float)Read(p, instance); }

        internal GameApi()
        {
            Type find = GameType("Verse.Find"), ui = GameType("Verse.UI");
            windowStack = Property(find, "WindowStack");
            currentMap = Property(find, "CurrentMap");
            world = Property(find, "World");
            mapCamera = Property(find, "CameraDriver");
            worldCamera = Property(find, "WorldCameraDriver");
            screenWidth = Field(ui, "screenWidth");
            screenHeight = Field(ui, "screenHeight");
            windowAt = GameType("Verse.WindowStack").GetMethod("GetWindowAt", new[] { typeof(Vector2) });
            if (windowAt == null) throw new MissingMethodException("WindowStack.GetWindowAt");
            Type renderer = GameType("RimWorld.Planet.WorldRendererUtility");
            worldSelected = Property(renderer, "WorldSelected");
            worldBackground = Property(renderer, "WorldBackgroundNow");
            inputBlocked = Property(GameType("Verse.Mouse"), "IsInputBlockedNow");
            programState = Property(GameType("Verse.Current"), "ProgramState");
            zoomSwitchLayer = Property(GameType("Verse.Prefs"), "ZoomSwitchWorldLayer");
            Type layer = GameType("RimWorld.Planet.PlanetLayer");
            selectedLayer = Property(layer, "Selected");
            layerIn = Field(layer, "zoomInToLayer");
            layerOut = Field(layer, "zoomOutToLayer");
            extraAltitude = Property(layer, "ExtraCameraAltitude");
            map = new CameraApi(GameType("Verse.CameraDriver"), false);
            planet = new CameraApi(GameType("RimWorld.Planet.WorldCameraDriver"), true);
            BeginScrollView = GameType("Verse.Widgets").GetMethod("BeginScrollView", new[] {
                typeof(Rect), typeof(Vector2).MakeByRefType(), typeof(Rect), typeof(bool) });
            if (BeginScrollView == null) throw new MissingMethodException("Widgets.BeginScrollView");
        }

        internal bool InputBlocked() { return (bool)Read(inputBlocked); }

        internal void Zoom(float notches, float x, float y)
        {
            object windows = Read(windowStack);
            if (windows == null) return;
            var point = new Vector2(x * (int)screenWidth.GetValue(null), y * (int)screenHeight.GetValue(null));
            if (windowAt.Invoke(windows, new object[] { point }) != null) return;
            bool onWorld = (bool)Read(worldSelected);
            if (onWorld)
            {
                if (Read(world) == null || (bool)Read(worldBackground)) return;
            }
            else if (Read(currentMap) == null) return;
            CameraApi api = onWorld ? planet : map;
            object camera = Read(onWorld ? worldCamera : mapCamera);
            if (camera == null || (bool)Read(api.Blocked, camera)) return;
            object config = api.Config.GetValue(camera);

            // CameraDriverOnGUI: MapDragSensitivity affects drag only, not the scroll zoom rate.
            // One wheel click reaches the game's handler as Event.delta.y = 3 on a PC (and did on
            // box64 through X11), so a notch here is three units: without it one press of the
            // Zoom button was a third of a step ("micro steps", tester 2026-10-12).
            float rate = onWorld ? 0.1f : Number(api.WheelRate);
            float desired = Number(api.Desired, camera)
                - notches * 3f * rate * Number(api.ZoomSpeed, config) * Number(api.Size, camera) / (onWorld ? 12f : 35f);
            if (!onWorld)
            {
                object range = api.Range.GetValue(config);
                api.Desired.SetValue(camera, Mathf.Clamp(desired, Number(api.RangeMin, range), Number(api.RangeMax, range)));
                return;
            }

            float min = Number(api.MinAltitude);
            // RimWorld 1.6 can switch planet layers at the zoom limits. Keep its altitude continuity
            // and decay, as WorldCameraDriverOnGUI does, instead of trapping the user on one layer.
            if (Convert.ToInt32(Read(programState)) == 2 && (bool)Read(zoomSwitchLayer))
            {
                object layer = Read(selectedLayer);
                object target = layer == null ? null : desired < min ? layerIn.GetValue(layer)
                    : desired > 1100f ? layerOut.GetValue(layer) : null;
                if (target != null)
                {
                    float offset = Number(api.LayerOffset, camera), extra = Number(extraAltitude, target);
                    desired += offset - extra;
                    api.Altitude.SetValue(camera, desired);
                    api.Decay.SetValue(camera, extra - offset);
                    selectedLayer.SetValue(null, target, null);
                    api.PreviousLayer.SetValue(camera, target);
                }
            }
            api.Desired.SetValue(camera, Mathf.Clamp(desired, min, 1100f));
        }

        sealed class CameraApi
        {
            internal readonly PropertyInfo Blocked, Size, WheelRate, MinAltitude;
            internal readonly FieldInfo Config, ZoomSpeed, Desired, Range, RangeMin, RangeMax;
            internal readonly FieldInfo LayerOffset, Altitude, Decay, PreviousLayer;

            internal CameraApi(Type type, bool planet)
            {
                Blocked = Property(type, "AnythingPreventsCameraMotion");
                Size = Property(type, planet ? "TrueAltitude" : "RootSize");
                Desired = Field(type, planet ? "desiredAltitude" : "desiredSize");
                Config = Field(type, "config");
                ZoomSpeed = Field(Config.FieldType, "zoomSpeed");
                if (planet)
                {
                    MinAltitude = Property(type, "MinAltitude");
                    LayerOffset = Field(type, "layerAltitudeOffset");
                    Altitude = Field(type, "altitude");
                    Decay = Field(type, "altitudeDecay");
                    PreviousLayer = Field(type, "prevSelectedLayer");
                }
                else
                {
                    WheelRate = Property(type, "ScrollWheelZoomRate");
                    Range = Field(Config.FieldType, "sizeRange");
                    RangeMin = Field(Range.FieldType, "min");
                    RangeMax = Field(Range.FieldType, "max");
                }
            }
        }
    }
}
