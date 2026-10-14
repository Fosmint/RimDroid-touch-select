package com.rimdroid.fragments;

import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Spinner;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.rimdroid.InstanceSettings;
import com.rimdroid.R;
import com.rimdroid.game.GameInstance;
import com.rimdroid.game.GameInstanceManager;
import com.rimdroid.input.InputControlsView;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Import and export of an instance's on-screen controls layout, one screen from the drawer (as
 * Zomdroid's and ValDroid's "Import/Export Controls"): the instance at the top, then an import card
 * and an export card. A layout is the controls .json the drawer's old Export/Import items used.
 * Importing replaces the layout, so it is confirmed first.
 */
public class ControlsTransferFragment extends Fragment {

    private Spinner spInstance;
    private final List<GameInstance> instances = new ArrayList<>();
    private String exportFor;   // the instance an export in flight belongs to

    private final ActivityResultLauncher<String[]> pickImport =
            registerForActivityResult(new ActivityResultContracts.OpenDocument(), this::confirmImport);

    private final ActivityResultLauncher<String> pickExport =
            registerForActivityResult(new ActivityResultContracts.CreateDocument("application/json"), this::export);

    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
        return inflater.inflate(R.layout.fragment_controls_transfer, container, false);
    }

    @Override
    public void onViewCreated(@NonNull View v, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(v, savedInstanceState);
        spInstance = v.findViewById(R.id.sp_controls_transfer_instance);
        GameInstanceManager.requireSingleton().reload();
        instances.clear();
        instances.addAll(GameInstanceManager.requireSingleton().getInstances());
        List<String> names = new ArrayList<>();
        if (instances.isEmpty()) names.add(getString(R.string.no_instances));
        for (GameInstance gi : instances) names.add(gi.getName());
        ArrayAdapter<String> a = new ArrayAdapter<>(requireContext(), android.R.layout.simple_spinner_item, names);
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spInstance.setAdapter(a);

        v.findViewById(R.id.btn_controls_import).setOnClickListener(b -> {
            if (current() == null) { toast(getString(R.string.no_instances)); return; }
            pickImport.launch(new String[]{"application/json", "text/plain", "application/octet-stream"});
        });
        v.findViewById(R.id.btn_controls_export).setOnClickListener(b -> {
            GameInstance gi = current();
            if (gi == null) { toast(getString(R.string.no_instances)); return; }
            exportFor = gi.getName();
            pickExport.launch("rimdroid_controls_" + gi.getName().replaceAll("[^A-Za-z0-9._-]", "_") + "_"
                    + new java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US).format(new java.util.Date())
                    + ".json");
        });
    }

    @Nullable
    private GameInstance current() {
        int i = spInstance.getSelectedItemPosition();
        return (i >= 0 && i < instances.size()) ? instances.get(i) : null;
    }

    /** Importing replaces the instance's layout: asked first, a stray tap must not do it. */
    private void confirmImport(@Nullable Uri uri) {
        GameInstance gi = current();
        if (uri == null || gi == null || !isAdded()) return;
        String file = displayName(uri);
        new MaterialAlertDialogBuilder(requireContext())
                .setMessage(getString(R.string.controls_transfer_import_confirm, gi.getName(),
                        file != null ? file : uri.getLastPathSegment()))
                .setPositiveButton(R.string.controls_transfer_import, (d, w) -> importLayout(uri, gi.getName()))
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void importLayout(Uri uri, String instance) {
        final android.content.Context app = requireContext().getApplicationContext();
        new Thread(() -> {
            String msg;
            try (InputStream in = app.getContentResolver().openInputStream(uri)) {
                if (in == null) throw new java.io.IOException("cannot open the file");
                String json = new String(readAll(in), StandardCharsets.UTF_8);
                // Only a layout that parses is stored, so a wrong file cannot leave the instance empty.
                if (InputControlsView.parse(json) == null) throw new java.io.IOException("not a controls layout");
                new InstanceSettings(instance).setControlsJson(json);
                msg = app.getString(R.string.controls_imported, instance);
            } catch (Exception e) {
                msg = app.getString(R.string.controls_import_failed,
                        e.getMessage() == null ? "invalid layout file" : e.getMessage());
            }
            toastLater(msg);
        }, "controls-import").start();
    }

    private void export(@Nullable Uri uri) {
        final String instance = exportFor;
        if (uri == null || instance == null || !isAdded()) return;
        final android.content.Context app = requireContext().getApplicationContext();
        new Thread(() -> {
            String msg;
            try (OutputStream out = app.getContentResolver().openOutputStream(uri)) {
                if (out == null) throw new java.io.IOException("cannot open the file");
                // An instance with no layout of its own plays the bundled default: export that.
                String json = new InstanceSettings(instance).getControlsJson();
                if (json == null || json.trim().isEmpty()) {
                    try (InputStream in = app.getAssets().open(InputControlsView.DEFAULT_ASSET)) {
                        json = new String(readAll(in), StandardCharsets.UTF_8);
                    }
                }
                out.write(json.getBytes(StandardCharsets.UTF_8));
                msg = app.getString(R.string.controls_exported);
            } catch (Exception e) {
                msg = app.getString(R.string.controls_export_failed, String.valueOf(e.getMessage()));
            }
            toastLater(msg);
        }, "controls-export").start();
    }

    private static byte[] readAll(InputStream in) throws java.io.IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        byte[] b = new byte[8192];
        int n;
        while ((n = in.read(b)) > 0) bos.write(b, 0, n);
        return bos.toByteArray();
    }

    private void toast(String msg) {
        if (isAdded()) Toast.makeText(requireContext(), msg, Toast.LENGTH_SHORT).show();
    }

    private void toastLater(String msg) {
        android.app.Activity a = getActivity();
        if (a != null) a.runOnUiThread(() -> Toast.makeText(a, msg, Toast.LENGTH_SHORT).show());
    }

    @Nullable
    private String displayName(Uri uri) {
        try (Cursor c = requireContext().getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (i >= 0) return c.getString(i);
            }
        } catch (Exception ignored) {}
        return null;
    }
}
