package dev.bluewake.android;

import android.content.Context;
import android.os.StatFs;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Hypatia's "TLoZ: The Wind Waker HD Pack" (v2.0001a, the Android-Lite build),
 * downloaded on the player's request from the link its author published in
 * the pack's Dolphin forum thread. The pack is not part of the app: nothing
 * of it ships with the APK or lives in this repository. The native side
 * (android/src/hd_pack.c) unpacks its GZL folder and shows the progress in
 * the options menu.
 */
final class HdPack {
    /** Google Drive file id of the Android-Lite (3x) build, from the forum thread. */
    static final String DRIVE_ID = "1UJAArn45RuMvbXTbY_nHBE0fCI7wX831";
    static final String THREAD_URL =
            "https://forums.dolphin-emu.org/Thread-hypatia-s-tloz-the-wind-waker-hd-pack-v2-0001a";
    /** The archive (500 MB) and the unpacked GZL (530 MB) side by side, with a margin. */
    private static final long SPACE_NEEDED = 1200L << 20;

    // States, as android/src/hd_pack.h.
    static final int DOWNLOADING = 1, UNPACKING = 2, DONE = 3, FAILED = 4;

    static native void nativeProgress(int state, long done, long total, String message);

    /** Unpacks the archive's GZL into dest; the file count, or -1 (reported as FAILED). */
    static native int nativeInstall(String archive, String dest);

    private static Thread worker;

    private HdPack() {
    }

    static synchronized void start(Context context, String dest) {
        if (worker != null && worker.isAlive()) return;
        final Context app = context.getApplicationContext();
        worker = new Thread(() -> run(app, dest), "HdPack");
        worker.start();
    }

    private static void run(Context context, String dest) {
        File cache = context.getExternalCacheDir() != null ? context.getExternalCacheDir() : context.getCacheDir();
        File archive = new File(cache, "hypatia-wwhd-android-lite.7z");
        try {
            new File(dest).mkdirs();
            long free = new StatFs(dest).getAvailableBytes();
            if (free < SPACE_NEEDED) {
                throw new IOException("needs about 1.2 GB of free storage, " + (free >> 20) + " MB are free");
            }
            download(archive);
            int files = nativeInstall(archive.getPath(), dest);
            if (files >= 0) nativeProgress(DONE, files, files, "");
        } catch (Exception e) {
            nativeProgress(FAILED, 0, 0, e.getMessage() != null ? e.getMessage() : e.toString());
        } finally {
            archive.delete();
        }
    }

    private static void download(File out) throws IOException {
        String base = "https://drive.usercontent.google.com/download?id=" + DRIVE_ID + "&export=download";
        HttpURLConnection c = open(base + "&confirm=t");
        String type = c.getContentType();
        if (type != null && type.startsWith("text/html")) {
            // Large files: Drive may answer with its "can't scan for viruses" form first.
            String html = readAll(c.getInputStream());
            c.disconnect();
            Matcher m = Pattern.compile("name=\"uuid\" value=\"([^\"]+)\"").matcher(html);
            c = open(base + "&confirm=t" + (m.find() ? "&uuid=" + m.group(1) : ""));
            type = c.getContentType();
            if (type != null && type.startsWith("text/html")) {
                c.disconnect();
                throw new IOException("Google Drive did not hand out the file (its daily quota, or the link "
                        + "changed). Try later, or get it from " + THREAD_URL);
            }
        }
        long total = c.getContentLengthLong();
        try (InputStream in = c.getInputStream(); OutputStream o = new FileOutputStream(out)) {
            byte[] buf = new byte[1 << 16];
            long done = 0, reported = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                o.write(buf, 0, n);
                done += n;
                if (done - reported >= (1 << 20)) {
                    nativeProgress(DOWNLOADING, done, total, null);
                    reported = done;
                }
            }
            if (total > 0 && done != total) throw new IOException("the download broke off");
        } finally {
            c.disconnect();
        }
    }

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setInstanceFollowRedirects(true);
        c.setConnectTimeout(20000);
        c.setReadTimeout(60000);
        c.setRequestProperty("User-Agent", "Mozilla/5.0 (Android) BlueWake");
        int code = c.getResponseCode();
        if (code != 200) throw new IOException("HTTP " + code + " from Google Drive");
        return c;
    }

    private static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        return b.toString("UTF-8");
    }
}
