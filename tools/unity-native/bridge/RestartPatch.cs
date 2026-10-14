using System;
using UnityEngine;

namespace RimDroid
{
    // The game's Restart (after a mod list change, or its own button) re-executes its PC binary:
    // Process.Start fails here ("Cannot find the specified file") and the game shows an error. As a
    // Harmony prefix on Verse.GenCommandLine.Restart: the launcher restarts the instance instead
    // (NativeUnityActivity.restartInstance) and this process ends. Nothing is lost: the game wrote
    // ModsConfig before asking.
    static class RestartPatch
    {
        public static bool BeforeRestart()
        {
            try
            {
                using (var activity = new AndroidJavaClass("com.rimdroid.nativeunity.NativeUnityActivity"))
                    activity.CallStatic("restartInstance");
                Debug.Log("RIMDROID bridge: restart handed to the launcher");
                return false;
            }
            catch (Exception e)
            {
                Debug.LogWarning("RIMDROID bridge: restart via the launcher failed, the game's own path runs: " + e.GetBaseException().Message);
                return true;
            }
        }
    }
}
