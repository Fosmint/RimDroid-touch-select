package com.rimdroid.mods;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.PriorityQueue;
import java.util.Set;

/**
 * Load-order sort from the rules mod authors declare in About.xml — the same rules RimWorld's own
 * auto-sort and RimSort start from: modDependencies (the dependency loads first), loadBefore /
 * loadAfter and their force- and ByVersion variants. Core and the DLC come right after anything
 * that asks to load before them (Harmony does), in their release order, ahead of every other mod.
 *
 * The sort is stable: among mods free to go next it always takes the one that stood earliest, so a
 * mod moves only when a rule makes it, and a list that is already right comes back unchanged.
 * Contradicting rules (A before B, B before A) cannot all hold: they are reported, and the mod that
 * stood earliest goes first. Rules naming mods that are not active are ignored for the order.
 */
public final class ModSorter {
    static final String[] OFFICIAL = { ModList.CORE, "ludeon.rimworld.royalty", "ludeon.rimworld.ideology",
            "ludeon.rimworld.biotech", "ludeon.rimworld.anomaly", "ludeon.rimworld.odyssey" };

    public static final class Result {
        public final List<String> order = new ArrayList<>();
        public int moved;
        /** Required but not installed: "Name (Workshop id)". */
        public final List<ModList.Dependency> missing = new ArrayList<>();
        /** Required, installed, but switched off: their mods. */
        public final List<ModList.Mod> inactive = new ArrayList<>();
        /** Mods whose rules contradict each other. */
        public final List<String> conflicts = new ArrayList<>();
        /** "A ↔ B": both on, one says it is incompatible with the other. */
        public final List<String> incompatible = new ArrayList<>();
    }

    public static Result sort(List<String> current, Map<String, ModList.Mod> installed) {
        Result r = new Result();
        List<String> ids = new ArrayList<>(new LinkedHashSet<>(current));
        Map<String, Integer> pos = new HashMap<>();
        for (int i = 0; i < ids.size(); i++) pos.put(ids.get(i), i);

        Map<String, Set<String>> after = new HashMap<>();   // id -> ids that must come after it
        Map<String, Integer> incoming = new HashMap<>();
        for (String id : ids) { after.put(id, new LinkedHashSet<>()); incoming.put(id, 0); }

        Set<String> beforeOfficial = new HashSet<>();   // mods that ask to load before Core or a DLC
        for (String id : ids) {
            ModList.Mod m = installed.get(id);
            if (m == null) continue;
            for (String b : m.loadBefore) {
                edge(id, b, after, incoming);
                for (String o : OFFICIAL) if (o.equals(b)) beforeOfficial.add(id);
            }
            for (String a : m.loadAfter) edge(a, id, after, incoming);
            for (ModList.Dependency d : m.dependencies) edge(d.packageId, id, after, incoming);
        }
        // Core, then the DLC in release order, ahead of every other mod not asking otherwise.
        String prev = null;
        for (String o : OFFICIAL) {
            if (!pos.containsKey(o)) continue;
            if (prev != null) edge(prev, o, after, incoming);
            prev = o;
        }
        if (prev != null) {
            for (String id : ids) {
                ModList.Mod m = installed.get(id);
                boolean official = m != null && m.official;
                if (!official && !beforeOfficial.contains(id) && !isOfficialId(id)) edge(prev, id, after, incoming);
            }
        }

        // Kahn's algorithm, always taking the earliest-standing ready mod.
        PriorityQueue<String> ready = new PriorityQueue<>((a, b) -> pos.get(a) - pos.get(b));
        for (String id : ids) if (incoming.get(id) == 0) ready.add(id);
        Set<String> placed = new HashSet<>();
        while (placed.size() < ids.size()) {
            if (ready.isEmpty()) {
                // A cycle: report what is left, place the earliest of it to break it.
                String first = null;
                for (String id : ids) if (!placed.contains(id) && (first == null || pos.get(id) < pos.get(first))) first = id;
                if (r.conflicts.isEmpty()) for (String id : ids) if (!placed.contains(id) && incoming.get(id) > 0) r.conflicts.add(id);
                incoming.put(first, 0);
                ready.add(first);
            }
            String id = ready.poll();
            if (placed.contains(id)) continue;
            placed.add(id);
            r.order.add(id);
            for (String next : after.get(id)) {
                if (placed.contains(next)) continue;
                int n = incoming.get(next) - 1;
                incoming.put(next, n);
                if (n == 0) ready.add(next);
            }
        }
        for (int i = 0; i < r.order.size(); i++) if (!r.order.get(i).equals(ids.get(i))) r.moved++;

        // Warnings: what the active mods need, and incompatible pairs.
        Set<String> active = new HashSet<>(ids), seenMissing = new HashSet<>(), seenInactive = new HashSet<>();
        for (String id : ids) {
            ModList.Mod m = installed.get(id);
            if (m == null) continue;
            for (ModList.Dependency d : m.dependencies) {
                if (active.contains(d.packageId)) continue;
                ModList.Mod dep = installed.get(d.packageId);
                if (dep == null) { if (seenMissing.add(d.packageId)) r.missing.add(d); }
                else if (seenInactive.add(d.packageId)) r.inactive.add(dep);
            }
            for (String other : m.incompatibleWith)
                if (active.contains(other)) {
                    ModList.Mod o = installed.get(other);
                    r.incompatible.add(m.name + " \u2194 " + (o != null ? o.name : other));
                }
        }
        return r;
    }

    private static boolean isOfficialId(String id) {
        for (String o : OFFICIAL) if (o.equals(id)) return true;
        return false;
    }

    /** "first loads before second", when both are in the list and the edge is new. */
    private static void edge(String first, String second, Map<String, Set<String>> after, Map<String, Integer> incoming) {
        if (first.equals(second) || !after.containsKey(first) || !after.containsKey(second)) return;
        if (after.get(first).add(second)) incoming.put(second, incoming.get(second) + 1);
    }

    private ModSorter() {}
}
