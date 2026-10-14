package com.rimdroid.mods;

import android.util.Log;

import org.w3c.dom.Document;
import org.w3c.dom.Element;
import org.w3c.dom.Node;
import org.w3c.dom.NodeList;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

import javax.xml.parsers.DocumentBuilderFactory;
import javax.xml.transform.OutputKeys;
import javax.xml.transform.Transformer;
import javax.xml.transform.TransformerFactory;
import javax.xml.transform.dom.DOMSource;
import javax.xml.transform.stream.StreamResult;

/**
 * An instance's mods as RimWorld sees them: every folder with About/About.xml under Data/ (Core and
 * the DLC) and Mods/, and the load order in Config/ModsConfig.xml — a mod is enabled when its
 * packageId is in {@code <activeMods>}, and the order there is the load order. Reads and edits only
 * that list; the game rewrites ModsConfig.xml when it exits, so nothing here may run while it does
 * (the screen checks that).
 */
public final class ModList {
    private static final String TAG = "RimDroid/Mods";
    public static final String CORE = "ludeon.rimworld";
    /** RimDroid's own built-in mod: the launcher manages it, the list hides it. */
    public static final String BUILTIN = "rimdroid.controllerui";

    /** A required mod as About.xml names it (modDependencies). */
    public static final class Dependency {
        public final String packageId, displayName, workshopId;   // workshopId "" when not given
        Dependency(String packageId, String displayName, String workshopId) {
            this.packageId = packageId;
            this.displayName = displayName;
            this.workshopId = workshopId;
        }
    }

    public static final class Mod {
        public final File folder;
        public final String packageId;   // lower case, as ModsConfig compares them
        public final String name, author, description;
        public final List<String> supportedVersions;
        public final boolean official;   // Core or a DLC (Data/)
        // Load-order rules for this game version (plain lists + the ...ByVersion ones), lower case.
        public final List<Dependency> dependencies = new ArrayList<>();
        public final List<String> loadBefore = new ArrayList<>(), loadAfter = new ArrayList<>();
        public final List<String> incompatibleWith = new ArrayList<>();

        Mod(File folder, String packageId, String name, String author, String description,
            List<String> supportedVersions, boolean official) {
            this.folder = folder;
            this.packageId = packageId;
            this.name = name;
            this.author = author;
            this.description = description;
            this.supportedVersions = supportedVersions;
            this.official = official;
        }

        /** Core cannot be switched off; the DLC can, as in the game's own menu. */
        public boolean canDisable() { return !CORE.equals(packageId); }

        /** Only downloaded mods can be deleted, never the game's own content. */
        public boolean canDelete() { return !official; }

        /** False when About.xml lists game versions and this one is not among them. */
        public boolean supports(String gameVersion) {
            if (supportedVersions.isEmpty() || gameVersion.isEmpty()) return true;
            return supportedVersions.contains(gameVersion);
        }
    }

    public static File configFile(File instanceDir) {
        return new File(instanceDir, "unity3d/Ludeon Studios/RimWorld by Ludeon Studios/Config/ModsConfig.xml");
    }

    /** Every installed mod, official content first, then by name. */
    public static List<Mod> installed(File instanceDir) {
        List<Mod> mods = new ArrayList<>();
        String version = gameVersion(instanceDir);
        scan(new File(instanceDir, "Data"), true, version, mods);
        scan(new File(instanceDir, "Mods"), false, version, mods);
        Collections.sort(mods, (a, b) -> a.official != b.official ? (a.official ? -1 : 1)
                : a.name.compareToIgnoreCase(b.name));
        return mods;
    }

    private static void scan(File dir, boolean official, String version, List<Mod> out) {
        File[] folders = dir.listFiles();
        if (folders == null) return;
        for (File folder : folders) {
            File about = new File(folder, "About/About.xml");
            if (!folder.isDirectory() || !about.isFile()) continue;
            try {
                Element root = factory().newDocumentBuilder().parse(about).getDocumentElement();
                String id = text(root, "packageId");
                if (id.isEmpty()) continue;
                id = id.toLowerCase(Locale.ROOT);
                if (BUILTIN.equals(id)) continue;
                String name = text(root, "name");
                List<String> versions = new ArrayList<>();
                Element sv = child(root, "supportedVersions");
                if (sv != null) for (Element li : children(sv, "li")) versions.add(li.getTextContent().trim());
                Mod mod = new Mod(folder, id, name.isEmpty() ? folder.getName() : name, text(root, "author"),
                        text(root, "description"), versions, official);
                String v = "v" + version;
                // forceLoadBefore/After are the same edges; the game only treats them as stricter.
                for (String tag : new String[] { "loadBefore", "forceLoadBefore" }) mod.loadBefore.addAll(ids(root, tag, v));
                for (String tag : new String[] { "loadAfter", "forceLoadAfter" }) mod.loadAfter.addAll(ids(root, tag, v));
                mod.incompatibleWith.addAll(ids(root, "incompatibleWith", v));
                for (Element li : versioned(root, "modDependencies", v)) {
                    String dep = text(li, "packageId").toLowerCase(Locale.ROOT);
                    if (dep.isEmpty()) continue;
                    mod.dependencies.add(new Dependency(dep, text(li, "displayName"), workshopId(text(li, "steamWorkshopUrl"))));
                }
                out.add(mod);
            } catch (Exception e) {
                Log.w(TAG, "Unreadable " + about + ": " + e);
            }
        }
    }

    /** {@code <tag><li>..</li></tag>} and {@code <tagByVersion><v1.6><li>..</li></v1.6></tagByVersion>}. */
    private static List<Element> versioned(Element root, String tag, String version) {
        List<Element> out = new ArrayList<>();
        Element plain = child(root, tag);
        if (plain != null) out.addAll(children(plain, "li"));
        Element byVersion = child(root, tag + "ByVersion");
        Element forThis = byVersion == null ? null : child(byVersion, version);
        if (forThis != null) out.addAll(children(forThis, "li"));
        return out;
    }

    private static List<String> ids(Element root, String tag, String version) {
        List<String> out = new ArrayList<>();
        for (Element li : versioned(root, tag, version)) {
            String id = li.getTextContent().trim().toLowerCase(Locale.ROOT);
            if (!id.isEmpty()) out.add(id);
        }
        return out;
    }

    /** The number in ".../?id=123" or "steam://url/CommunityFilePage/123", "" otherwise. */
    private static String workshopId(String url) {
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("(?:id=|CommunityFilePage/)(\\d+)").matcher(url);
        return m.find() ? m.group(1) : "";
    }

    /** The active packageIds in load order (lower case); null when there is no ModsConfig.xml yet
     *  (the game writes it on its first start). */
    public static List<String> activeOrder(File instanceDir) {
        File config = configFile(instanceDir);
        if (!config.isFile()) return null;
        try {
            Element active = activeElement(factory().newDocumentBuilder().parse(config));
            List<String> ids = new ArrayList<>();
            if (active != null)
                for (Element li : children(active, "li")) ids.add(li.getTextContent().trim().toLowerCase(Locale.ROOT));
            return ids;
        } catch (Exception e) {
            Log.w(TAG, "ModsConfig.xml unreadable: " + e);
            return null;
        }
    }

    /** Enables (appended at the end of the load order, as the game does) or disables a mod. */
    public static boolean setActive(File instanceDir, String packageId, boolean on) {
        File config = configFile(instanceDir);
        try {
            Document doc = factory().newDocumentBuilder().parse(config);
            Element active = activeElement(doc);
            if (active == null) return false;
            boolean present = false;
            for (Element li : children(active, "li")) {
                if (li.getTextContent().trim().equalsIgnoreCase(packageId)) {
                    if (on) present = true;
                    else active.removeChild(li);
                }
            }
            if (on && !present) {
                Element li = doc.createElement("li");
                li.appendChild(doc.createTextNode(packageId));
                active.appendChild(li);
            }
            write(doc, config);
            return true;
        } catch (Exception e) {
            Log.w(TAG, "Cannot update ModsConfig.xml: " + e);
            return false;
        }
    }

    /** Replaces the whole load order (the packageIds as given, lower case). */
    public static boolean writeOrder(File instanceDir, List<String> order) {
        File config = configFile(instanceDir);
        try {
            Document doc = factory().newDocumentBuilder().parse(config);
            Element active = activeElement(doc);
            if (active == null) return false;
            for (Element li : children(active, "li")) active.removeChild(li);
            for (String id : order) {
                Element li = doc.createElement("li");
                li.appendChild(doc.createTextNode(id));
                active.appendChild(li);
            }
            write(doc, config);
            return true;
        } catch (Exception e) {
            Log.w(TAG, "Cannot write the load order: " + e);
            return false;
        }
    }

    /** Removes the mod's folder and its entry in the load order. */
    public static boolean delete(File instanceDir, Mod mod) {
        if (!mod.canDelete()) return false;
        if (configFile(instanceDir).isFile()) setActive(instanceDir, mod.packageId, false);
        return deleteTree(mod.folder);
    }

    private static boolean deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        return f.delete() || !f.exists();
    }

    /** "1.6" from the instance's Version.txt, "" when unreadable. */
    public static String gameVersion(File instanceDir) {
        try {
            String line = new String(Files.readAllBytes(new File(instanceDir, "Version.txt").toPath()),
                    java.nio.charset.StandardCharsets.UTF_8).trim();
            String[] parts = line.split("[ .]");
            return parts.length >= 2 ? parts[0] + "." + parts[1] : "";
        } catch (Exception e) {
            return "";
        }
    }

    /** packageId → mod, for the rows of the active list. */
    public static Map<String, Mod> byId(List<Mod> mods) {
        Map<String, Mod> map = new HashMap<>();
        for (Mod m : mods) if (!map.containsKey(m.packageId)) map.put(m.packageId, m);
        return map;
    }

    // ---- XML ----

    private static DocumentBuilderFactory factory() {
        DocumentBuilderFactory f = DocumentBuilderFactory.newInstance();
        f.setExpandEntityReferences(false);
        try { f.setFeature("http://apache.org/xml/features/disallow-doctype-decl", true); } catch (Throwable ignored) {}
        return f;
    }

    private static Element activeElement(Document doc) {
        NodeList list = doc.getElementsByTagName("activeMods");
        return list.getLength() == 0 ? null : (Element) list.item(0);
    }

    private static Element child(Element parent, String name) {
        for (Node n = parent.getFirstChild(); n != null; n = n.getNextSibling())
            if (n.getNodeType() == Node.ELEMENT_NODE && n.getNodeName().equalsIgnoreCase(name)) return (Element) n;
        return null;
    }

    private static List<Element> children(Element parent, String name) {
        List<Element> out = new ArrayList<>();
        for (Node n = parent.getFirstChild(); n != null; n = n.getNextSibling())
            if (n.getNodeType() == Node.ELEMENT_NODE && n.getNodeName().equals(name)) out.add((Element) n);
        return out;
    }

    private static String text(Element parent, String name) {
        Element e = child(parent, name);
        return e == null ? "" : e.getTextContent().trim();
    }

    private static void write(Document doc, File output) throws Exception {
        Transformer t = TransformerFactory.newInstance().newTransformer();
        t.setOutputProperty(OutputKeys.ENCODING, "utf-8");
        t.setOutputProperty(OutputKeys.INDENT, "yes");
        t.setOutputProperty("{http://xml.apache.org/xslt}indent-amount", "2");
        File tmp = new File(output.getPath() + ".rdtmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            t.transform(new DOMSource(doc), new StreamResult(out));
        }
        try {
            Files.move(tmp.toPath(), output.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } catch (AtomicMoveNotSupportedException e) {
            Files.move(tmp.toPath(), output.toPath(), StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private ModList() {}
}
