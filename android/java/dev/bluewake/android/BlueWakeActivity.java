package dev.bluewake.android;

import android.content.Intent;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/**
 * SDL's activity, with SDL3 linked statically into libmain.so (the host,
 * GXRuntime, Aurora and Dawn): only that one library is loaded. The game
 * module, libgGZLE01_recomp.so, is opened by the host itself (dlopen).
 *
 * Adds the touch controls over SDL's surface. They hide while a game
 * controller is connected and come back when it goes.
 */
public class BlueWakeActivity extends SDLActivity implements InputManager.InputDeviceListener {
    private TouchControlsView touchControls;
    private InputManager inputManager;

    /** The options menu's "Download Hypatia's HD pack" (android/src/hd_pack.c). */
    public static void startHdPackDownload(String dest) {
        HdPack.start(getContext(), dest);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        applyLockScreenSwitch(getIntent());
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        applyRefreshRate();
        if (mLayout != null) {
            touchControls = new TouchControlsView(this);
            mLayout.addView(touchControls, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        inputManager = (InputManager) getSystemService(INPUT_SERVICE);
        if (inputManager != null)
            inputManager.registerInputDeviceListener(this, null);
        updateTouchVisibility();
    }

    // Development over adb only: `am start ... --ez showWhenLocked true` shows
    // the game over the lock screen, so a locked test phone can run it. The
    // switch counts only while files/launch.env exists: the builder and
    // install.py write that file over adb, and no other app can write in this
    // app's storage, so another app's intent cannot bring the game over the
    // lock screen. Every launch decides again: the activity is
    // singleInstance, so a later launch arrives through onNewIntent and an
    // ordinary one turns the switch off.
    private void applyLockScreenSwitch(Intent intent) {
        java.io.File files = getExternalFilesDir(null);
        boolean show = intent != null && intent.getBooleanExtra("showWhenLocked", false)
                && files != null && new java.io.File(files, "launch.env").isFile();
        setShowWhenLocked(show);
        setTurnScreenOn(show);
    }

    // The panel's rate, as chosen in the options menu (files/settings.ini,
    // BLUEWAKE_REFRESH: 60 when unset, 120 on a panel that has it). The game
    // draws 30 frames a second, 60 with Smooth Motion: at 60 the panel and
    // the compositor save power, and on a phone that power is heat, which is
    // what the system's thermal manager lowers the game's clocks for. At 120
    // Smooth Motion's 120-frames preset needs the panel to actually switch.
    private void applyRefreshRate() {
        int wanted = 60;
        final String prefix = "BLUEWAKE_REFRESH=";
        java.io.File settings = new java.io.File(getExternalFilesDir(null), "settings.ini");
        if (settings.isFile()) {
            try (java.io.BufferedReader lines = new java.io.BufferedReader(
                    new java.io.FileReader(settings))) {
                for (String line; (line = lines.readLine()) != null; ) {
                    if (!line.startsWith(prefix))
                        continue;
                    try {
                        wanted = Integer.parseInt(line.substring(prefix.length()).trim());
                    } catch (NumberFormatException ignored) {
                    }
                    break;
                }
            } catch (java.io.IOException ignored) {
            }
        }
        // Ask for a display mode, not a rate: a nonzero preferredRefreshRate
        // overrides the mode id, and only the id picks among the panel's
        // same-resolution 60 and 120 Hz modes. Nearest match, so a 60 Hz-only
        // panel stays at 60 however the file reads.
        android.view.Display display = getDisplay();
        if (display == null)
            return;
        android.view.Display.Mode current = display.getMode();
        android.view.Display.Mode chosen = null;
        for (android.view.Display.Mode mode : display.getSupportedModes())
            if (mode.getPhysicalWidth() == current.getPhysicalWidth()
                    && mode.getPhysicalHeight() == current.getPhysicalHeight()
                    && (chosen == null || Math.abs(mode.getRefreshRate() - wanted)
                                       < Math.abs(chosen.getRefreshRate() - wanted)))
                chosen = mode;
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredRefreshRate = 0f;
        attributes.preferredDisplayModeId = chosen != null ? chosen.getModeId() : 0;
        getWindow().setAttributes(attributes);
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyRefreshRate();  // the mode request does not survive pause and resume
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        applyLockScreenSwitch(intent);
    }

    @Override
    protected void onDestroy() {
        if (inputManager != null)
            inputManager.unregisterInputDeviceListener(this);
        super.onDestroy();
    }

    @Override
    protected void onPause() {
        if (touchControls != null)
            touchControls.reset();
        super.onPause();
    }

    private static boolean gamepadConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device == null || device.isVirtual())
                continue;
            int sources = device.getSources();
            if ((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                return true;
        }
        return false;
    }

    private void updateTouchVisibility() {
        if (touchControls == null)
            return;
        boolean show = !gamepadConnected();
        if (show != (touchControls.getVisibility() == View.VISIBLE)) {
            touchControls.reset();
            touchControls.setVisibility(show ? View.VISIBLE : View.GONE);
        }
    }

    @Override
    public void onInputDeviceAdded(int deviceId) { updateTouchVisibility(); }

    @Override
    public void onInputDeviceRemoved(int deviceId) { updateTouchVisibility(); }

    @Override
    public void onInputDeviceChanged(int deviceId) { updateTouchVisibility(); }
}
