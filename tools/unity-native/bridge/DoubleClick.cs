using UnityEngine;

namespace RimDroid
{
    // Android's mouse events have no desktop click count. Observe the shared IMGUI event before
    // RimWorld's Root (GUI.depth 50) and restore it for all consumers, including map selection and
    // double-clicking the colonist bar. This does not require Harmony or invoke selection directly.
    static class DoubleClick
    {
        static float lastDown = -1f;
        static Vector2 lastPosition;
        static bool released, eligible;

        internal static void Reset() { lastDown = -1f; released = eligible = false; }

        internal static void OnGUI()
        {
            GUI.depth = 10000; // Higher depth runs first; this observer draws nothing.
            Event e = Event.current;
            if (e == null || e.button != 0) return;
            Vector2 point = GUIUtility.GUIToScreenPoint(e.mousePosition);
            float tolerance = Mathf.Max(6f, Screen.height * 0.012f);
            bool nearby = (point - lastPosition).sqrMagnitude <= tolerance * tolerance;
            if (e.type == EventType.MouseDown)
            {
                float now = Time.realtimeSinceStartup;
                bool pair = eligible && released && nearby && now - lastDown <= 0.3f;
                if (e.clickCount < 2) e.clickCount = pair ? 2 : 1;
                eligible = e.clickCount == 1;
                released = false;
                lastDown = now;
                lastPosition = point;
            }
            else if (e.rawType == EventType.MouseUp)
                released = nearby && lastDown >= 0 && Time.realtimeSinceStartup - lastDown <= 0.3f;
            else if (e.type == EventType.MouseDrag && !nearby)
                eligible = false;
        }
    }
}
