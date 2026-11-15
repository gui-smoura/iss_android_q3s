// Adapted from Bloodborne-Android 2e472653. Used only by the experimental Bionic launch path.
package ro.cobrabm.fexdroid;

import android.view.Surface;
import android.view.SurfaceHolder;

/**
 * The Bionic game process's frame path (tools/android/present/iss_present_proto.h): libisspresent.so
 * listens on a unix socket, receives AHardwareBuffers from the game and shows them on the
 * SurfaceView with an ASurfaceControl. The game stays connected while the surface comes and goes:
 * with no surface its frames are dropped, with a new one they show again.
 */
public final class IssPresent {
    static { System.loadLibrary("isspresent"); }
    private static native String nativeStart(String socketPath);
    private static native void nativeSetSurface(Surface surface);
    private static native long nativeFrames();
    private static native void nativeSetFullscreen(boolean enabled);
    private static native void nativeSetForceEgl(boolean enabled);
    private static native void nativeSetFrameRate(float rate);
    public static void frameRate(float rate) { nativeSetFrameRate(rate); }
    private static native void nativeShutdown();

    private static SurfaceHolder.Callback callback;
    private static SurfaceHolder attached;

    /** Listens on socketPath (once) and shows the game on this surface until it is destroyed. */
    public static synchronized String start(SurfaceHolder holder, String socketPath) {
        return start(holder, socketPath, false);
    }

    public static synchronized String start(SurfaceHolder holder, String socketPath, boolean fullscreen) {
        nativeSetFullscreen(fullscreen);
        nativeSetForceEgl(false);
        String result = nativeStart(socketPath);
        used = true;
        if (!result.equals("present running")) return result;
        if (attached != null && callback != null) attached.removeCallback(callback);
        attached = holder;
        callback = new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { setSurface(h.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int height) {}
            // The game keeps running: its frames are dropped until a surface is back.
            @Override public void surfaceDestroyed(SurfaceHolder h) { setSurface(null); }
        };
        holder.addCallback(callback);
        setSurface(holder.getSurface().isValid() ? holder.getSurface() : null);
        return "bridge running: bionic present on " + socketPath;
    }

    /** Listens on socketPath and presents frames onto an OpenXR/external Android Surface via EGL. */
    public static synchronized String start(Surface surface, String socketPath, boolean fullscreen) {
        nativeSetFullscreen(fullscreen);
        nativeSetForceEgl(true);
        String result = nativeStart(socketPath);
        used = true;
        if (!result.equals("present running")) return result;
        if (attached != null && callback != null) {
            attached.removeCallback(callback);
            attached = null;
            callback = null;
        }
        setSurface(surface != null && surface.isValid() ? surface : null);
        return "bridge running: bionic egl present on " + socketPath;
    }

    public static synchronized void setSurface(Surface surface) { nativeSetSurface(surface); }

    /** Frames the game has presented to the app so far. */
    public static long frames() { return nativeFrames(); }

    private static boolean used;
    /** frames() for the status poll: 0 until the Bionic path was started. */
    public static long framesIfLoaded() { return used ? nativeFrames() : 0; }

    /** Ends the listener and the display (the app is closing). */
    public static synchronized void stop() {
        if (attached != null && callback != null) {
            attached.removeCallback(callback);
            attached = null;
            callback = null;
        }
        nativeShutdown();
    }
}
