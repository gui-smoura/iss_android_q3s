package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.view.Surface;

/**
 * Minimal zero-eye-buffer OpenXR quad session bridge backed by libissopenxr.so + libopenxr_loader.so.
 * Loaded lazily only when VR mode is started so standard 2D launches stay unaffected.
 */
public final class IssOpenXr {
    static final int STATE_UNKNOWN = 0;
    static final int STATE_IDLE = 1;
    static final int STATE_READY = 2;
    static final int STATE_SYNCHRONIZED = 3;
    static final int STATE_VISIBLE = 4;
    static final int STATE_FOCUSED = 5;
    static final int STATE_STOPPING = 6;
    static final int STATE_LOSS_PENDING = 7;
    static final int STATE_EXITING = 8;

    public interface StateListener {
        void onSessionState(int state);
    }

    private static boolean loaded;
    private static volatile StateListener listener;

    private static synchronized void ensureLoaded() {
        if (loaded) return;
        try { System.loadLibrary("c++_shared"); } catch (UnsatisfiedLinkError ignored) {}
        System.loadLibrary("openxr_loader");
        System.loadLibrary("issopenxr");
        loaded = true;
    }

    private static native String nativeStart(Activity activity, int width, int height, String logPath);
    private static native Surface nativeGetSurface();
    private static native void nativeStop();
    private static native long nativeFrames();
    private static native int nativeSessionState();
    private static native String nativeLastError();

    public static synchronized String start(Activity activity, int width, int height,
                                            String logPath, StateListener stateListener) {
        try {
            ensureLoaded();
        } catch (Throwable t) {
            return "Failed to load OpenXR native libraries: " + t.getMessage();
        }
        listener = stateListener;
        String res = nativeStart(activity, width, height, logPath);
        if (!"openxr running".equals(res)) {
            listener = null;
        }
        return res;
    }

    public static Surface getSurface() {
        return loaded ? nativeGetSurface() : null;
    }

    public static synchronized void stop() {
        listener = null;
        if (loaded) nativeStop();
    }

    public static long frames() {
        return loaded ? nativeFrames() : 0;
    }

    public static int sessionState() {
        return loaded ? nativeSessionState() : STATE_UNKNOWN;
    }

    public static String lastError() {
        return loaded ? nativeLastError() : "";
    }

    /** Called synchronously on OpenXR's worker thread so Surface detachment completes before xrEndSession. */
    private static void onNativeSessionState(int state) {
        StateListener current = listener;
        if (current != null) current.onSessionState(state);
    }

    private IssOpenXr() {}
}
