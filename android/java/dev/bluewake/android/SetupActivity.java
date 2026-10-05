package dev.bluewake.android;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.StatFs;
import android.provider.OpenableColumns;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * The app's entry. With the game's files in place it hands straight over to
 * the game (BlueWakeActivity). Otherwise the player chooses their disc image
 * here: the app copies it into its own folder (files/game/GZLE01.iso) and
 * prepares main.dol and the RELs from it (the iOS app's importer, through
 * android/src/disc_setup.c), with no PC or adb. The European disc for the
 * other languages can be added the same way, here or later from the options
 * menu (started with EXTRA_EUROPE). The chosen files are copied, never moved
 * or deleted.
 */
public class SetupActivity extends Activity {
    static final String EXTRA_EUROPE = "europe";
    /** Every complete GameCube disc image has this size. */
    private static final long DISC_SIZE = 1459978240L;
    private static final int PICK_USA = 1, PICK_EUROPE = 2;
    private static final int BACKGROUND = 0xFF0B1424, TEXT = 0xFFE8EEF8, DIM = 0xFF9AA8BD, ERROR = 0xFFFF8A75;

    private File gameDir;
    private boolean europeOnly;
    private volatile boolean busy;
    private TextView status;
    private ProgressBar bar;
    private Button usaButton, europeButton, playButton;

    /** Prepares game/main.dol and game/rels from the copied disc; null, or what went wrong. */
    private native String nativePrepareDisc(String iso, String outDir);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        gameDir = new File(getExternalFilesDir(null), "game");
        europeOnly = getIntent().getBooleanExtra(EXTRA_EUROPE, false);
        if (!europeOnly && ready()) {
            startGame();
            return;
        }
        buildUi();
    }

    private boolean ready() {
        String[] rels = new File(gameDir, "rels").list();
        return new File(gameDir, "main.dol").isFile() && rels != null && rels.length >= 415
                && new File(gameDir, "GZLE01.iso").isFile();
    }

    private void startGame() {
        startActivity(new Intent(this, BlueWakeActivity.class));
        finish();
    }

    // ---------------------------------------------------------------- the screen
    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private TextView text(LinearLayout parent, String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(6), 0, dp(6));
        parent.addView(t, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return t;
    }

    private Button button(LinearLayout parent, String s, View.OnClickListener click) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(click);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(4);
        parent.addView(b, lp);
        return b;
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BACKGROUND);
        scroll.setFillViewport(true);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(dp(32), dp(32), dp(32), dp(32));
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        root.addView(column, new LinearLayout.LayoutParams(Math.min(dp(560),
                getResources().getDisplayMetrics().widthPixels - dp(64)), ViewGroup.LayoutParams.WRAP_CONTENT));
        scroll.addView(root);

        text(column, "Wind Waker Recomp", 28, TEXT, true);
        if (!europeOnly) {
            text(column, "Choose your disc image of The Legend of Zelda: The Wind Waker for GameCube, USA version "
                    + "(GZLE01), as .iso or .gcm. The app copies it into its own folder (1.4 GB) and prepares "
                    + "the game from it. Your file stays where it is.", 15, DIM, false);
            usaButton = button(column, "Choose your disc image", v -> pick(PICK_USA));
        }
        text(column, europeOnly
                ? "Choose your European disc image (GZLP01). The app copies it into its own folder and reads "
                  + "German, French, Spanish and Italian from it; choose the language under "
                  + "Options › Gameplay › Language."
                : "Optional: your European disc (GZLP01) adds German, French, Spanish and Italian "
                  + "(Options › Gameplay › Language).", 15, DIM, false);
        europeButton = button(column, "Choose your European disc image", v -> pick(PICK_EUROPE));
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setVisibility(View.GONE);
        column.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(24)));
        status = text(column, "", 15, TEXT, false);
        playButton = button(column, europeOnly ? "Back to the game" : "Play", v -> {
            if (europeOnly) finish(); else startGame();
        });
        playButton.setVisibility(europeOnly ? View.VISIBLE : View.GONE);
        setContentView(scroll);
    }

    private void pick(int which) {
        if (busy) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, which);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        final boolean usa = request == PICK_USA;
        busy = true;
        setBusy(true);
        new Thread(() -> {
            String error = null;
            try {
                importDisc(uri, usa);
            } catch (Exception e) {
                error = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final String failed = error;
            runOnUiThread(() -> finished(usa, failed));
        }, "DiscImport").start();
    }

    private void setBusy(boolean b) {
        if (usaButton != null) usaButton.setEnabled(!b);
        europeButton.setEnabled(!b);
        playButton.setEnabled(!b);
        bar.setVisibility(b ? View.VISIBLE : View.GONE);
        bar.setProgress(0);
    }

    private void show(final String s, final double fraction) {
        runOnUiThread(() -> {
            status.setTextColor(TEXT);
            status.setText(s);
            if (fraction >= 0) bar.setProgress((int) (fraction * 1000));
        });
    }

    private void finished(boolean usa, String error) {
        busy = false;
        setBusy(false);
        if (error != null) {
            status.setTextColor(ERROR);
            status.setText(error);
            return;
        }
        if (usa) {
            status.setText("Ready to play.");
            playButton.setVisibility(View.VISIBLE);
            usaButton.setText("Choose another disc image");
        } else {
            status.setText("The European disc is in. Choose the language under Options › Gameplay › "
                    + "Language" + (europeOnly ? "; it applies when the game starts again." : "."));
            europeButton.setText("Choose another European disc image");
            if (ready()) playButton.setVisibility(View.VISIBLE);
        }
    }

    // ------------------------------------------------------------- the import
    /** Called from nativePrepareDisc while it writes main.dol and the RELs. */
    void onPrepareProgress(double fraction) {
        show("Preparing the game from your disc…", fraction);
    }

    private void importDisc(Uri uri, boolean usa) throws IOException {
        ContentResolver resolver = getContentResolver();
        long size = -1;
        try (Cursor c = resolver.query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) size = c.getLong(0);
        }
        byte[] head = new byte[0x220];
        try (InputStream in = resolver.openInputStream(uri)) {
            if (in == null || readFully(in, head) < head.length) throw new IOException("This file is too small to be a disc image.");
        }
        String magic4 = new String(head, 0, 4, StandardCharsets.ISO_8859_1);
        if (magic4.equals("RVZ\u0001") || magic4.equals("WIA\u0001")) {
            throw new IOException("This is a Dolphin .rvz/.wia file. Convert it to ISO in Dolphin first "
                    + "(right-click the game, Convert File, format ISO), then choose the .iso.");
        }
        if (new String(head, 0x200, 4, StandardCharsets.ISO_8859_1).equals("NKIT")) {
            throw new IOException("This is an NKit image, which leaves parts of the disc out. Convert it back "
                    + "to a full ISO (NKit's \"Recover to ISO\") and choose that.");
        }
        int magic = ((head[0x1C] & 0xFF) << 24) | ((head[0x1D] & 0xFF) << 16) | ((head[0x1E] & 0xFF) << 8)
                | (head[0x1F] & 0xFF);
        if (magic != 0xC2339F3D) throw new IOException("This is not a GameCube disc image.");
        String id = new String(head, 0, 6, StandardCharsets.ISO_8859_1);
        String want = usa ? "GZLE01" : "GZLP01";
        if (!id.equals(want)) {
            throw new IOException(usa
                    ? (id.equals("GZLP01")
                        ? "This is the European disc (GZLP01). The game needs the USA disc (GZLE01); the "
                          + "European one can be added below, for the other languages."
                        : "This disc is " + id + ", not The Wind Waker for GameCube, USA (GZLE01).")
                    : "This disc is " + id + ", not the European Wind Waker (GZLP01).");
        }
        if (usa && size > 0 && size != DISC_SIZE) {
            throw new IOException("This image is " + size + " bytes; a complete GameCube disc image is "
                    + DISC_SIZE + ". A trimmed or compressed image does not work.");
        }
        gameDir.mkdirs();
        long free = new StatFs(gameDir.getPath()).getAvailableBytes();
        long needed = (size > 0 ? size : DISC_SIZE) + (64L << 20);
        if (free < needed) {
            throw new IOException("Not enough free storage: the disc needs " + (needed >> 20) + " MB, "
                    + (free >> 20) + " MB are free.");
        }

        File target = new File(gameDir, usa ? "GZLE01.iso" : "GZLP01.iso");
        File part = new File(gameDir, target.getName() + ".part");
        try (InputStream in = resolver.openInputStream(uri); OutputStream out = new FileOutputStream(part)) {
            byte[] buf = new byte[1 << 20];
            long done = 0, shown = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                done += n;
                if (done - shown >= (16 << 20)) {
                    shown = done;
                    show(String.format(Locale.ROOT, "Copying the disc into the app: %d of %d MB", done >> 20,
                            (size > 0 ? size : DISC_SIZE) >> 20), size > 0 ? (double) done / size : -1);
                }
            }
            if (size > 0 && done != size) throw new IOException("The copy broke off; please try again.");
        } catch (IOException e) {
            part.delete();
            throw e;
        }
        target.delete();
        if (!part.renameTo(target)) throw new IOException("Cannot finish the copy in " + gameDir);

        if (usa) {
            show("Preparing the game from your disc…", 0);
            System.loadLibrary("main");
            String error = nativePrepareDisc(target.getPath(), gameDir.getPath());
            if (error != null) throw new IOException(error);
        } else if (!europeOnly) {
            chooseLanguageFromSystem();
        }
    }

    /** With the European disc added before the first game, start in the phone's language. */
    private void chooseLanguageFromSystem() {
        String lang = Locale.getDefault().getLanguage();
        if (!lang.equals("de") && !lang.equals("fr") && !lang.equals("es") && !lang.equals("it")) return;
        File settings = new File(getExternalFilesDir(null), "settings.ini");
        try {
            String old = settings.isFile()
                    ? new String(java.nio.file.Files.readAllBytes(settings.toPath()), StandardCharsets.UTF_8) : "";
            if (old.contains("BLUEWAKE_LANGUAGE=")) return;
            try (OutputStream out = new FileOutputStream(settings, true)) {
                out.write(((old.isEmpty() || old.endsWith("\n") ? "" : "\n") + "BLUEWAKE_LANGUAGE=" + lang + "\n")
                        .getBytes(StandardCharsets.UTF_8));
            }
        } catch (IOException ignored) {
        }
    }

    private static int readFully(InputStream in, byte[] buf) throws IOException {
        int got = 0, n;
        while (got < buf.length && (n = in.read(buf, got, buf.length - got)) > 0) got += n;
        return got;
    }

    @Override
    public void onBackPressed() {
        if (!busy) super.onBackPressed();
    }
}
