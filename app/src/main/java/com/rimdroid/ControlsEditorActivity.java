package com.rimdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

import com.rimdroid.input.Binding;
import com.rimdroid.input.ButtonElement;
import com.rimdroid.input.ControlElement;
import com.rimdroid.input.ControlElementDescription;
import com.rimdroid.input.InputControlsView;
import com.rimdroid.input.MouseStickElement;
import com.rimdroid.input.WasdStickElement;

import java.util.function.Consumer;
import java.util.function.IntConsumer;

/**
 * On-screen controls layout editor (Zomdroid-style). Tap an element to select it, drag
 * to move, use the side panel to resize (scale), set opacity, change bindings, and add /
 * delete elements. Changes are saved to prefs on Done / pause.
 */
public class ControlsEditorActivity extends Activity implements InputControlsView.EditorListener {

    /** Which instance's controls layout to edit (null → global). */
    public static final String EXTRA_INSTANCE_NAME = "instance_name";

    private InputControlsView controls;
    // Grid button modes (stored as "grid_mode" in the controls_editor prefs).
    private static final int GRID_OFF = 0, GRID_SMALL = 1, GRID_LARGE = 2;
    private int gridMode = GRID_OFF;
    private ScrollView panel;
    private LinearLayout panelContainer;
    private float density;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        // Immersive fullscreen (hide status + nav bars) so the editing area matches the
        // game's fullscreen surface — otherwise element positions would be off on devices
        // with a visible navigation bar.
        getWindow().setDecorFitsSystemWindows(false);
        setContentView(R.layout.activity_controls_editor);
        hideSystemBars();   // after setContentView — the decor view / insets controller now exist
        density = getResources().getDisplayMetrics().density;

        final String instanceName = getIntent().getStringExtra(EXTRA_INSTANCE_NAME);

        // Match the game's effective scale (stored value raised to the per-device floor) so edited
        // element positions line up with the running game. Per-instance when opened from an
        // instance's settings; global as a fallback.
        float renderScale = 0.72f;
        android.graphics.Rect b = getWindowManager().getCurrentWindowMetrics().getBounds();
        int sLong  = Math.max(b.width(), b.height());
        int sShort = Math.min(b.width(), b.height());
        if (instanceName != null) {
            renderScale = new com.rimdroid.InstanceSettings(instanceName).getEffectiveRenderScale(sLong, sShort);
        } else {
            LauncherPreferences lp = LauncherPreferences.getSingleton();
            if (lp != null) renderScale = lp.getEffectiveRenderScale(sLong, sShort);
        }

        FrameLayout host = findViewById(R.id.controls_host);
        controls = new InputControlsView(this, renderScale, instanceName);
        host.addView(controls, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        controls.setEditMode(true, this);

        panel = findViewById(R.id.panel);
        panelContainer = findViewById(R.id.panel_container);

        ((Button) findViewById(R.id.btn_add)).setOnClickListener(v -> showAddDialog());
        ((Button) findViewById(R.id.btn_reset)).setOnClickListener(v -> showResetDialog());
        ((Button) findViewById(R.id.btn_icons)).setOnClickListener(v -> showIconsDialog());
        ((Button) findViewById(R.id.btn_done)).setOnClickListener(v -> { controls.saveToPrefs(); finish(); });

        // Grid button cycles off -> small -> large -> off; remembered across editor sessions,
        // editor-only (no effect in game). The old on/off "snap_to_grid" maps to large, the step it had.
        final android.content.SharedPreferences editorPrefs =
                getSharedPreferences("controls_editor", MODE_PRIVATE);
        android.widget.ToggleButton grid = findViewById(R.id.btn_grid);
        int saved = editorPrefs.contains("grid_mode")
                ? editorPrefs.getInt("grid_mode", GRID_OFF)
                : (editorPrefs.getBoolean("snap_to_grid", false) ? GRID_LARGE : GRID_OFF);
        applyGridMode(grid, saved);
        grid.setOnClickListener(btn -> {
            int next = (gridMode + 1) % 3;
            applyGridMode(grid, next);
            editorPrefs.edit().putInt("grid_mode", next).apply();
        });

        // Reference background: pick a screenshot to place buttons against. Editor-only helper —
        // copied to a file so it survives reopening the editor, and cleared on demand. Never affects
        // the game itself.
        editorBg = findViewById(R.id.editor_bg);
        loadEditorBg();
        ((Button) findViewById(R.id.btn_bg_set)).setOnClickListener(v -> {
            android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_GET_CONTENT);
            i.setType("image/*");
            try { startActivityForResult(i, REQ_PICK_BG); } catch (Throwable ignored) {}
        });
        ((Button) findViewById(R.id.btn_bg_clear)).setOnClickListener(v -> {
            bgFile().delete();
            editorBg.setImageDrawable(null);
        });
    }

    private static final int REQ_PICK_BG = 4801;
    private static final int REQ_PICK_ICON_PACK = 4802;
    private android.widget.ImageView editorBg;

    @Override
    protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK_ICON_PACK && resultCode == RESULT_OK && data != null && data.getData() != null) {
            importIconPack(data.getData());
            return;
        }
        if (requestCode == REQ_PICK_BG && resultCode == RESULT_OK && data != null && data.getData() != null) {
            try (java.io.InputStream in = getContentResolver().openInputStream(data.getData());
                 java.io.FileOutputStream out = new java.io.FileOutputStream(bgFile())) {
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
            } catch (Throwable ignored) { return; }
            loadEditorBg();
        }
    }

    /** Editor background lives in app files, one per device (a design aid, not per-instance state). */
    private java.io.File bgFile() { return new java.io.File(getFilesDir(), "editor_bg.png"); }

    private void loadEditorBg() {
        if (editorBg == null) return;
        java.io.File f = bgFile();
        if (f.isFile()) {
            android.graphics.Bitmap bm = android.graphics.BitmapFactory.decodeFile(f.getAbsolutePath());
            if (bm != null) { editorBg.setImageBitmap(bm); return; }
        }
        editorBg.setImageDrawable(null);
    }

    private void hideSystemBars() {
        android.view.WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(android.view.WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(
                    android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @Override protected void onPause() { super.onPause(); if (controls != null) controls.saveToPrefs(); }

    // ===== EditorListener =====
    @Override public void onElementSelected(ControlElement el) {
        if (el == null) { panel.setVisibility(View.GONE); return; }
        buildPanel(el);
        panel.setVisibility(View.VISIBLE);
    }

    // ===== panel construction =====

    private void buildPanel(ControlElement el) {
        panelContainer.removeAllViews();
        addTitle(el.editorLabel());

        addSlider("Size", 40, 200, Math.round(el.getScale() * 100), "%",
                v -> { el.setScale(v / 100f); controls.invalidate(); });
        addSlider("Opacity", 1, 100, Math.round(el.getAlpha() / 2.55f), "%",
                v -> { el.setAlpha(Math.round(v * 2.55f)); controls.invalidate(); });
        // "Opacity → all": copy THIS element's opacity to every element at once
        // (Zomdroid-style), instead of adjusting each one separately.
        Button toAll = new Button(this);
        toAll.setText("Opacity → all elements");
        toAll.setOnClickListener(v -> {
            controls.applyAlphaToAll(el.getAlpha());
            android.widget.Toast.makeText(this, "Opacity applied to all elements",
                    android.widget.Toast.LENGTH_SHORT).show();
        });
        panelContainer.addView(toAll, rowParams());

        if (el instanceof ButtonElement) {
            ButtonElement b = (ButtonElement) el;
            addBindingSpinner("Action", b.getBinding(), binding -> {
                b.setBinding(binding);
                com.rimdroid.input.IconPacks.followPack(this, controls, b);   // the pack's icon for the new action
            });
            addText("Label", b.getText(), b::setText);
            addShapeChoice(b);
            addCheckbox("Toggle (latch on/off)", b.isToggle(), b::setToggle);
            addIconRow(b);
        } else if (el instanceof MouseStickElement) {
            MouseStickElement m = (MouseStickElement) el;
            addSlider("Sensitivity", 25, 800, Math.round(m.getSensitivity() * 100), "%",
                    v -> m.setSensitivity(v / 100f));
        } else if (el instanceof WasdStickElement) {
            WasdStickElement w = (WasdStickElement) el;
            final String[] names = {"Up", "Right", "Down", "Left"};
            for (int i = 0; i < 4; i++) {
                final int idx = i;
                addBindingSpinner(names[i], w.getDir(i), bnd -> w.setDir(idx, bnd));
            }
        }

        Button del = new Button(this);
        del.setText(R.string.editor_delete);
        del.setOnClickListener(v -> { controls.removeSelected(); panel.setVisibility(View.GONE); });
        LinearLayout.LayoutParams lp = rowParams();
        lp.topMargin = (int) (16 * density);
        panelContainer.addView(del, lp);
        smallerText(panelContainer);
    }

    /** The element panel's text, two sizes (sp) below the platform defaults: it covers the layout
     *  being edited, so it should take less room. Applied once to the freshly built panel. */
    private static final float PANEL_TEXT_SHRINK_SP = 2f;

    private void smallerText(View v) {
        if (v instanceof TextView) {
            TextView t = (TextView) v;
            float px = t.getTextSize() - PANEL_TEXT_SHRINK_SP * getResources().getDisplayMetrics().scaledDensity;
            t.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, px);
        } else if (v instanceof android.view.ViewGroup && !(v instanceof AdapterView)) {
            android.view.ViewGroup g = (android.view.ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) smallerText(g.getChildAt(i));
        }
    }

    private void addTitle(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(0xFFFFFFFF);
        t.setTextSize(18);
        t.setPadding(0, 0, 0, (int) (8 * density));
        panelContainer.addView(t, rowParams());
    }

    private void addSlider(String label, int min, int max, int value, String unit, IntConsumer onChange) {
        final TextView lbl = new TextView(this);
        lbl.setTextColor(0xFFDDE3EA);
        lbl.setText(label + ": " + value + unit);
        panelContainer.addView(lbl, rowParams());

        SeekBar sb = new SeekBar(this);
        sb.setMax(max - min);
        sb.setProgress(Math.max(0, Math.min(max - min, value - min)));
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int progress, boolean fromUser) {
                int v = progress + min;
                lbl.setText(label + ": " + v + unit);
                onChange.accept(v);
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        LinearLayout.LayoutParams lp = rowParams();
        lp.bottomMargin = (int) (10 * density);
        panelContainer.addView(sb, lp);
    }

    private void addBindingSpinner(String label, Binding selected, Consumer<Binding> onSel) {
        TextView lbl = new TextView(this);
        lbl.setTextColor(0xFFDDE3EA);
        lbl.setText(label);
        panelContainer.addView(lbl, rowParams());

        Spinner sp = new Spinner(this);
        final Binding[] values = Binding.values();
        ArrayAdapter<Binding> ad = new ArrayAdapter<Binding>(this, android.R.layout.simple_spinner_item, values) {
            @Override public View getView(int pos, View convert, android.view.ViewGroup parent) {
                View v = super.getView(pos, convert, parent);
                if (convert == null) smallerText(v);
                return v;
            }
            @Override public View getDropDownView(int pos, View convert, android.view.ViewGroup parent) {
                View v = super.getDropDownView(pos, convert, parent);
                if (convert == null) smallerText(v);
                return v;
            }
        };
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad);
        for (int i = 0; i < values.length; i++) if (values[i] == selected) { sp.setSelection(i); break; }
        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) { onSel.accept(values[pos]); }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });
        LinearLayout.LayoutParams lp = rowParams();
        lp.bottomMargin = (int) (10 * density);
        panelContainer.addView(sp, lp);
    }

    private void addText(String label, String value, Consumer<String> onChange) {
        TextView lbl = new TextView(this);
        lbl.setTextColor(0xFFDDE3EA);
        lbl.setText(label);
        panelContainer.addView(lbl, rowParams());

        EditText et = new EditText(this);
        et.setText(value);
        et.setTextColor(0xFFFFFFFF);
        et.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) { onChange.accept(s.toString()); controls.invalidate(); }
            @Override public void afterTextChanged(Editable s) {}
        });
        LinearLayout.LayoutParams lp = rowParams();
        lp.bottomMargin = (int) (10 * density);
        panelContainer.addView(et, lp);
    }

    /** Rectangle / Circle / Square for a button, one row of radio buttons. */
    private void addShapeChoice(ButtonElement b) {
        android.widget.RadioGroup group = new android.widget.RadioGroup(this);
        group.setOrientation(android.widget.RadioGroup.HORIZONTAL);
        String[] labels = { "Rectangle", "Circle", "Square" };
        ButtonElement.Shape[] shapes = { ButtonElement.Shape.RECT, ButtonElement.Shape.CIRCLE, ButtonElement.Shape.SQUARE };
        for (int i = 0; i < shapes.length; i++) {
            android.widget.RadioButton rb = new android.widget.RadioButton(this);
            rb.setId(View.generateViewId());
            rb.setText(labels[i]);
            rb.setTextColor(0xFFDDE3EA);
            group.addView(rb);
            if (b.getShape() == shapes[i]) rb.setChecked(true);
            final ButtonElement.Shape shape = shapes[i];
            rb.setOnCheckedChangeListener((v, on) -> {
                if (!on) return;
                b.setShape(shape);
                controls.invalidate();
            });
        }
        LinearLayout.LayoutParams lp = rowParams();
        lp.bottomMargin = (int) (10 * density);
        panelContainer.addView(group, lp);
    }

    private void addCheckbox(String label, boolean checked, Consumer<Boolean> onChange) {
        CheckBox cb = new CheckBox(this);
        cb.setText(label);
        cb.setTextColor(0xFFDDE3EA);
        cb.setChecked(checked);
        cb.setOnCheckedChangeListener((b, isChecked) -> onChange.accept(isChecked));
        LinearLayout.LayoutParams lp = rowParams();
        lp.bottomMargin = (int) (10 * density);
        panelContainer.addView(cb, lp);
    }

    private LinearLayout.LayoutParams rowParams() {
        return new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    // ===== toolbar actions =====

    private void showAddDialog() {
        final String[] items = { "Button", "Mouse-stick (cursor)", "Camera-stick (keys)" };
        new com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
                .setTitle(R.string.editor_add)
                .setItems(items, (d, which) -> {
                    ControlElementDescription desc;
                    switch (which) {
                        case 1:  desc = ControlElementDescription.mouseStick(0.5f, 0.5f); break;
                        case 2:  desc = ControlElementDescription.wasdStick(
                                Binding.KEY_UP, Binding.KEY_RIGHT, Binding.KEY_DOWN, Binding.KEY_LEFT, 0.5f, 0.5f); break;
                        default: desc = ControlElementDescription.button("BTN", Binding.MOUSE_LEFT, "CIRCLE", 0.5f, 0.5f); break;
                    }
                    controls.addElement(desc);
                    if (controls.getSelected() instanceof ButtonElement)
                        com.rimdroid.input.IconPacks.followPack(this, controls, (ButtonElement) controls.getSelected());
                })
                .show();
    }

    // ===== icon packs (IconPacks; ported from ValDroid) =====

    /** Button panel row: what the button shows, plus Choose / Remove. */
    private void addIconRow(ButtonElement b) {
        TextView lbl = new TextView(this);
        lbl.setTextColor(0xFFDDE3EA);
        lbl.setText(getString(R.string.editor_icon_label,
                b.getIcon() != null ? b.getIcon().replaceAll("^pack_|\\.png$", "") : getString(R.string.editor_icon_none)));
        panelContainer.addView(lbl, rowParams());

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        Button choose = new Button(this);
        choose.setText(R.string.editor_icon_choose);
        choose.setOnClickListener(v -> pickPackThen(pack -> pickIconFromPack(pack, b)));
        Button remove = new Button(this);
        remove.setText(R.string.editor_icon_remove);
        remove.setEnabled(b.getIcon() != null);
        remove.setOnClickListener(v -> { b.setIcon(null, false); controls.invalidate(); buildPanel(b); });
        LinearLayout.LayoutParams half = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        row.addView(choose, half);
        row.addView(remove, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        LinearLayout.LayoutParams lp = rowParams();
        lp.bottomMargin = (int) (10 * density);
        panelContainer.addView(row, lp);
    }

    /** Toolbar "Icons": apply a pack to every button, import a pack zip, delete an imported one. */
    private void showIconsDialog() {
        new com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
                .setTitle(R.string.editor_icons)
                .setItems(new String[]{
                        getString(R.string.editor_icons_apply),
                        getString(R.string.editor_icons_remove),
                        getString(R.string.editor_icons_import),
                        getString(R.string.editor_icons_delete)}, (d, which) -> {
                    if (which == 1) {
                        int n = com.rimdroid.input.IconPacks.removeAll(controls);
                        android.widget.Toast.makeText(this, getString(R.string.editor_icons_removed, n),
                                android.widget.Toast.LENGTH_SHORT).show();
                        ControlElement sel = controls.getSelected();
                        if (sel != null) buildPanel(sel);
                        return;
                    }
                    if (which > 1) which--;   // the items below keep their old indices
                    if (which == 0) {
                        pickPackThen(pack -> {
                            int n = com.rimdroid.input.IconPacks.apply(this, pack, controls);
                            android.widget.Toast.makeText(this, getString(R.string.editor_icons_applied, n),
                                    android.widget.Toast.LENGTH_SHORT).show();
                            ControlElement sel = controls.getSelected();
                            if (sel != null) buildPanel(sel);
                        });
                    } else if (which == 1) {
                        android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT);
                        i.addCategory(android.content.Intent.CATEGORY_OPENABLE);
                        i.setType("*/*");
                        i.putExtra(android.content.Intent.EXTRA_MIME_TYPES,
                                new String[]{"application/zip", "application/x-zip-compressed", "application/octet-stream"});
                        try { startActivityForResult(i, REQ_PICK_ICON_PACK); } catch (Throwable ignored) {}
                    } else {
                        deleteImportedPack();
                    }
                })
                .show();
    }

    /** Lets the player pick a pack; says how to get one when there are none. */
    private void pickPackThen(Consumer<com.rimdroid.input.IconPacks.Pack> then) {
        final java.util.List<com.rimdroid.input.IconPacks.Pack> packs = com.rimdroid.input.IconPacks.list(this);
        if (packs.isEmpty()) {
            new com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
                    .setTitle(R.string.editor_icons)
                    .setMessage(R.string.editor_icons_none_yet)
                    .setPositiveButton(android.R.string.ok, null)
                    .show();
            return;
        }
        String[] names = new String[packs.size()];
        for (int i = 0; i < names.length; i++) names[i] = packs.get(i).name;
        new com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
                .setTitle(R.string.editor_icons_pick_pack)
                .setItems(names, (d, which) -> then.accept(packs.get(which)))
                .show();
    }

    /** A grid of the pack's pictures with their names; tapping one puts it on the button. */
    private void pickIconFromPack(com.rimdroid.input.IconPacks.Pack pack, ButtonElement b) {
        final java.util.List<String> icons = pack.icons;
        android.widget.GridView grid = new android.widget.GridView(this);
        grid.setNumColumns(android.widget.GridView.AUTO_FIT);
        grid.setColumnWidth((int) (84 * density));
        grid.setVerticalSpacing((int) (8 * density));
        grid.setPadding((int) (12 * density), (int) (8 * density), (int) (12 * density), 0);
        final androidx.appcompat.app.AlertDialog[] dlg = new androidx.appcompat.app.AlertDialog[1];
        grid.setAdapter(new android.widget.BaseAdapter() {
            @Override public int getCount() { return icons.size(); }
            @Override public Object getItem(int i) { return icons.get(i); }
            @Override public long getItemId(int i) { return i; }
            @Override public View getView(int i, View convert, android.view.ViewGroup parent) {
                LinearLayout cell = new LinearLayout(ControlsEditorActivity.this);
                cell.setOrientation(LinearLayout.VERTICAL);
                cell.setGravity(Gravity.CENTER_HORIZONTAL);
                android.widget.ImageView img = new android.widget.ImageView(ControlsEditorActivity.this);
                img.setImageBitmap(com.rimdroid.input.IconPacks.load(ControlsEditorActivity.this, pack, icons.get(i)));
                // White pictures need a dark backdrop to be visible in a light dialog.
                img.setBackgroundColor(0xFF2B2F36);
                int pad = (int) (6 * density);
                img.setPadding(pad, pad, pad, pad);
                cell.addView(img, new LinearLayout.LayoutParams((int) (56 * density), (int) (56 * density)));
                TextView t = new TextView(ControlsEditorActivity.this);
                t.setText(icons.get(i));
                t.setTextSize(11);
                t.setGravity(Gravity.CENTER_HORIZONTAL);
                cell.addView(t);
                return cell;
            }
        });
        grid.setOnItemClickListener((p, v, pos, id) -> {
            com.rimdroid.input.IconPacks.setIcon(this, pack, icons.get(pos), b);
            controls.invalidate();
            buildPanel(b);
            if (dlg[0] != null) dlg[0].dismiss();
        });
        dlg[0] = new com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
                .setTitle(pack.name)
                .setView(grid)
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void importIconPack(android.net.Uri uri) {
        String name = null;
        try (android.database.Cursor c = getContentResolver().query(uri,
                new String[]{android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) name = c.getString(0);
        } catch (Throwable ignored) {}
        final String displayName = name;
        new Thread(() -> {
            String msg;
            try {
                com.rimdroid.input.IconPacks.Pack p = com.rimdroid.input.IconPacks.importZip(this, uri, displayName);
                msg = getString(R.string.editor_icons_imported, p.name, p.icons.size());
            } catch (Throwable t) {
                msg = getString(R.string.editor_icons_import_failed, String.valueOf(t.getMessage()));
            }
            final String m = msg;
            runOnUiThread(() -> android.widget.Toast.makeText(this, m, android.widget.Toast.LENGTH_LONG).show());
        }, "IconPackImport").start();
    }

    private void deleteImportedPack() {
        final java.util.List<com.rimdroid.input.IconPacks.Pack> imported = new java.util.ArrayList<>();
        for (com.rimdroid.input.IconPacks.Pack p : com.rimdroid.input.IconPacks.list(this))
            if (!p.builtIn) imported.add(p);
        if (imported.isEmpty()) {
            android.widget.Toast.makeText(this, R.string.editor_icons_no_imported, android.widget.Toast.LENGTH_SHORT).show();
            return;
        }
        String[] names = new String[imported.size()];
        for (int i = 0; i < names.length; i++) names[i] = imported.get(i).name;
        new com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
                .setTitle(R.string.editor_icons_delete)
                .setItems(names, (d, which) -> {
                    // Buttons keep their icons: those were copied into control_icons/ when applied.
                    com.rimdroid.input.IconPacks.delete(imported.get(which));
                    android.widget.Toast.makeText(this, getString(R.string.editor_icons_deleted, names[which]),
                            android.widget.Toast.LENGTH_SHORT).show();
                })
                .show();
    }

    /** Show a grid mode on the toolbar button (filled while any grid is on) and hand its step to the view. */
    private void applyGridMode(android.widget.ToggleButton btn, int mode) {
        gridMode = (mode == GRID_SMALL || mode == GRID_LARGE) ? mode : GRID_OFF;
        // ToggleButton rewrites its text from textOn/textOff on every setChecked, so set both first.
        String label = getString(gridMode == GRID_SMALL ? R.string.editor_grid_small
                : gridMode == GRID_LARGE ? R.string.editor_grid_large : R.string.editor_grid_off);
        btn.setTextOn(label);
        btn.setTextOff(label);
        btn.setChecked(gridMode != GRID_OFF);
        controls.setGrid(gridMode == GRID_SMALL ? InputControlsView.GRID_STEP_FINE_DP
                : gridMode == GRID_LARGE ? InputControlsView.GRID_STEP_COARSE_DP : 0f);
    }

    private void showResetDialog() {
        new com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
                .setTitle(R.string.editor_reset)
                .setMessage(R.string.editor_reset_confirm)
                .setPositiveButton(android.R.string.ok, (d, w) -> {
                    controls.loadDefault();
                    panel.setVisibility(View.GONE);
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }
}
