package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Minimal immersive OpenXR virtual screen host for Bionic inFAMOUS on Meta Quest.
 * Presents game frames onto a single flat XrCompositionLayerQuad via Android Surface
 * without stereoscopic eye buffers so HorizonOS unloads the 3D Home/panels cleanly.
 */
public final class BionicVrGameActivity extends Activity {
    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(AppLanguage.attach(base));
    }

    private TextView status;
    private AlertDialog errorDialog;
    private PadBridge pad;
    private AudioBridge audio;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Object xrSurfaceLock = new Object();
    private volatile Process game;
    private volatile boolean destroyed;
    private Surface xrSurface;
    private boolean xrSurfaceAttached;
    private String files, base, gameDir;
    private boolean failed;
    private long lastFrames = -1;
    private long lastBackMs;

    private synchronized void record(String line) {
        try (FileOutputStream f = new FileOutputStream(new File(files, "iss.log"), true)) {
            f.write(("[Bionic-VR] " + line + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) {}
    }

    @Override
    public void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);

        files = getFilesDir().getPath();
        base = files + "/issport";
        new File(base).mkdirs();
        try (FileOutputStream out = new FileOutputStream(new File(files, "iss.log"), false)) {
            out.write("inFAMOUS Second Son — Daedalus 0.2 · Android Bionic (OpenXR VR)\n"
                .getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) {}
        new File(base, "framegen.stats").delete();

        gameDir = getIntent().getStringExtra("game_dir");
        if (gameDir == null || gameDir.isEmpty()) {
            gameDir = getSharedPreferences("iss", 0).getString("game_dir", "");
        }

        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(0xff000000);
        frame.setFocusable(true);
        frame.setFocusableInTouchMode(true);
        status = new TextView(this);
        status.setText(AppLanguage.text("Starting OpenXR virtual screen…", "Iniciando tela virtual OpenXR…"));
        status.setGravity(Gravity.CENTER);
        status.setTextColor(0xffe0c891);
        status.setTextSize(18);
        frame.addView(status);
        setContentView(frame);
        frame.requestFocus();

        if (gameDir == null || gameDir.isEmpty()
            || !new File(gameDir, "eboot.bin").canRead()
            || !new File(gameDir, "art").isDirectory()
            || !new File(gameDir, "art").canRead()) {
            fatalDialog("inFAMOUS Second Son",
                AppLanguage.text("Choose the inFAMOUS game folder containing eboot.bin and readable art/.",
                    "Escolha a pasta de inFAMOUS com eboot.bin e art/ acessível."));
            return;
        }

        SharedPreferences prefs = getSharedPreferences("iss", 0);
        pad = new PadBridge(new File(base, "android-pad.state"));
        pad.setSwapFaceButtons(prefs.getBoolean("swap_square_triangle", false));
        pad.setDpadSwipe(prefs.getBoolean("dpad_swipe", true));
        pad.setMobileControls(false);
        audio = new AudioBridge(new File(base, "audio.fifo"));
        record("VR Activity initialized");

        worker.execute(this::startVrSession);
    }

    private void startVrSession() {
        try {
            BionicRuntimeInstaller.install(this);
            FramegenSettings.write(this);
            BionicPerformanceSettings.write(this);
        } catch (Exception ex) {
            if (!destroyed) fatalDialog("inFAMOUS Second Son", ex.toString());
            return;
        }

        SharedPreferences prefs = getSharedPreferences("iss", 0);
        String outputResolution = BionicVideoSettings.outputResolution(prefs);
        String[] dimensions = outputResolution.split("x");
        int outW = 1280;
        int outH = 720;
        try {
            if (dimensions.length == 2) {
                outW = Integer.parseInt(dimensions[0]);
                outH = Integer.parseInt(dimensions[1]);
            }
        } catch (NumberFormatException ignored) {}

        IssPresent.frameRate(BionicPerformanceSettings.limit(prefs));

        String xrStatus = IssOpenXr.start(this, outW, outH, files + "/iss.log", this::onXrSessionState);
        record("OpenXR start (" + outW + "x" + outH + "): " + xrStatus);
        if (!"openxr running".equals(xrStatus)) {
            if (!destroyed) {
                fatalDialog(
                    AppLanguage.text("OpenXR virtual screen error", "Erro na tela virtual OpenXR"),
                    xrStatus);
            }
            return;
        }
        if (destroyed) {
            IssOpenXr.stop();
            return;
        }

        Surface surface = IssOpenXr.getSurface();
        if (surface == null || !surface.isValid()) {
            IssOpenXr.stop();
            if (!destroyed) {
                fatalDialog(
                    AppLanguage.text("OpenXR virtual screen error", "Erro na tela virtual OpenXR"),
                    "OpenXR swapchain Surface unavailable");
            }
            return;
        }

        boolean stretch = "stretch".equals(BionicVideoSettings.gameDisplayMode(prefs));
        String presentResult = IssPresent.start((Surface) null, base + "/present.sock", stretch);
        record(presentResult);
        if (!presentResult.startsWith("bridge running:")) {
            stopPresentAndXr();
            if (!destroyed) {
                fatalDialog(
                    AppLanguage.text("OpenXR virtual screen error", "Erro na tela virtual OpenXR"),
                    presentResult);
            }
            return;
        }

        synchronized (xrSurfaceLock) {
            xrSurface = surface;
            int st = IssOpenXr.sessionState();
            if (!destroyed
                && st != IssOpenXr.STATE_STOPPING
                && st != IssOpenXr.STATE_EXITING
                && st != IssOpenXr.STATE_LOSS_PENDING
                && surface.isValid()) {
                xrSurfaceAttached = true;
                IssPresent.setSurface(surface);
            }
        }

        audio.start();
        if (!audio.awaitReady(5000)) {
            stopPresentAndXr();
            if (!destroyed) {
                fatalDialog("inFAMOUS Second Son",
                    AppLanguage.text("Audio FIFO did not start", "O áudio FIFO não iniciou"));
            }
            return;
        }

        main.post(this::poll);

        try (BionicSession.Guard ignored = BionicSession.acquire(getFilesDir())) {
            BionicSaveArchive.recover(new File(base, "user/savedata"));
            synchronized (this) {
                if (destroyed) return;
                record("Executing native Bionic inFAMOUS (OpenXR VR)");
                game = BionicLaunchEnv.startGame(
                    this, getFilesDir(), gameDir, "/system/bin/sh", base + "/run-bionic.sh");
            }
            try {
                try (InputStream in = game.getInputStream();
                     FileOutputStream out = new FileOutputStream(new File(files, "iss.log"), true)) {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                }
                int exit = game.waitFor();
                record("Bionic VR game exited " + exit);
                if (!destroyed) {
                    stopPresentAndXr();
                    fatalDialog("inFAMOUS Second Son",
                        AppLanguage.text("Game stopped (", "O jogo parou (")
                            + exit
                            + AppLanguage.text("). Export log.", "). Exporte o log."));
                }
            } finally {
                if (game.isAlive()) game.destroyForcibly();
                boolean interrupted = false;
                for (;;) {
                    try {
                        game.waitFor();
                        break;
                    } catch (InterruptedException e) {
                        interrupted = true;
                    }
                }
                if (interrupted) Thread.currentThread().interrupt();
            }
        } catch (Exception ex) {
            if (!destroyed) {
                stopPresentAndXr();
                fatalDialog("inFAMOUS Second Son", ex.toString());
            }
        }
    }

    private void onXrSessionState(int state) {
        synchronized (xrSurfaceLock) {
            if (state == IssOpenXr.STATE_STOPPING
                || state == IssOpenXr.STATE_EXITING
                || state == IssOpenXr.STATE_LOSS_PENDING) {
                if (xrSurfaceAttached) {
                    xrSurfaceAttached = false;
                    IssPresent.setSurface(null);
                }
            } else if (state == IssOpenXr.STATE_READY
                || state == IssOpenXr.STATE_SYNCHRONIZED
                || state == IssOpenXr.STATE_VISIBLE
                || state == IssOpenXr.STATE_FOCUSED) {
                if (!destroyed && !xrSurfaceAttached && xrSurface != null && xrSurface.isValid()) {
                    xrSurfaceAttached = true;
                    IssPresent.setSurface(xrSurface);
                }
            }
        }
        if (state == IssOpenXr.STATE_EXITING || state == IssOpenXr.STATE_LOSS_PENDING) {
            record("OpenXR session ended (state=" + state + ")");
            main.post(() -> {
                if (!destroyed && !isFinishing()) finish();
            });
        }
    }

    private void stopPresentAndXr() {
        synchronized (xrSurfaceLock) {
            xrSurfaceAttached = false;
            xrSurface = null;
        }
        IssPresent.stop();
        IssOpenXr.stop();
    }

    private void poll() {
        if (destroyed) return;
        long n = IssPresent.frames();
        if (!failed && n > 0) status.setVisibility(android.view.View.GONE);
        if (n > 0 && lastFrames < 0) {
            record("OpenXR virtual screen frames active (ahb=" + n + ", xr=" + IssOpenXr.frames() + ")");
            lastFrames = n;
        }
        main.postDelayed(this::poll, 1000);
    }

    private void fatalDialog(String title, String message) {
        record("ERROR: " + message);
        main.post(() -> {
            if (destroyed || isFinishing()) return;
            failed = true;
            status.setText(message);
            status.setVisibility(android.view.View.VISIBLE);
            if (errorDialog != null) errorDialog.dismiss();
            errorDialog = new AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage(message)
                .setCancelable(false)
                .setPositiveButton("OK", (d, w) -> finish())
                .create();
            errorDialog.show();
        });
    }

    private void handleBackExit() {
        long now = SystemClock.uptimeMillis();
        if (now - lastBackMs <= 2000L) {
            finish();
        } else {
            lastBackMs = now;
            Toast.makeText(this,
                AppLanguage.text("Press Back again to exit VR mode.",
                    "Pressione Voltar novamente para sair do modo VR."),
                Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    public void onBackPressed() {
        handleBackExit();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        if (code == KeyEvent.KEYCODE_BACK) {
            if (e.getAction() == KeyEvent.ACTION_UP && !e.isCanceled()) handleBackExit();
            return true;
        }
        return (pad != null && pad.key(e)) || super.dispatchKeyEvent(e);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent e) {
        return (pad != null && pad.motion(e)) || super.onGenericMotionEvent(e);
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (pad != null) pad.reset();
        if (audio != null) audio.pause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        if (audio != null) audio.resume();
    }

    @Override
    protected void onDestroy() {
        synchronized (this) {
            destroyed = true;
            if (game != null) game.destroyForcibly();
        }
        main.removeCallbacksAndMessages(null);
        if (errorDialog != null) {
            errorDialog.dismiss();
            errorDialog = null;
        }
        stopPresentAndXr();
        if (audio != null) audio.stop();
        worker.shutdown();
        super.onDestroy();
    }
}
