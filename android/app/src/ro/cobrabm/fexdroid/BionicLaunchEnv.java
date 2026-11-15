package ro.cobrabm.fexdroid;

import android.content.Context;
import android.content.SharedPreferences;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Map;

/** Shared native runtime environment configuration for the Bionic game process. */
final class BionicLaunchEnv {
    static Process startGame(Context context, File filesDir, String gameDir, String... command) throws IOException {
        ProcessBuilder p = new ProcessBuilder(command).redirectErrorStream(true);
        Map<String, String> e = p.environment();
        String files = filesDir.getPath();
        String base = files + "/issport";
        SharedPreferences prefs = context.getSharedPreferences("iss", 0);

        e.put("FXD_FILES", files);
        e.put("ISS_GAME_DIR", gameDir);
        e.put("ISS_APP_LANGUAGE", AppLanguage.gameLanguage());
        e.put("ISS_BIONIC", "1");
        e.put("ISS_ANDROID_INPUT", "1");
        String selectedDriver = BionicDriverSettings.selection(context);
        e.put("ISS_BIONIC_DRIVER", selectedDriver);
        e.put("TLG_BIONIC_DRIVER", selectedDriver);
        e.put("ISS_CUSTOM_DRIVER", BionicDriverSettings.customLibrary(context).getPath());
        String outputResolution = BionicVideoSettings.outputResolution(prefs);
        String[] dimensions = outputResolution.split("x");
        e.put("ISS_SCREEN_W", dimensions[0]);
        e.put("ISS_SCREEN_H", dimensions[1]);
        e.put("ISS_PRESENT_SOCKET", base + "/present.sock");
        e.put("TLG_PAD_FILE", base + "/android-pad.state");
        e.put("TLG_SHOW_FPS", "0");
        e.put("TLG_ANDROID_FPS_ONLY", "1");
        ArrayList<String> patches = new ArrayList<>();
        e.put("ISS_PERFORMANCE_STATE_FILE", base + "/performance.state");
        BionicRendererSettings.environment(prefs, e);
        boolean fsr3 = prefs.getBoolean("fsr3", false);
        e.put("TLG_UPSCALER", fsr3 ? "fsr3" : "none");
        if (fsr3) {
            e.put("TLG_UPSCALE_PRESET", Integer.toString(BionicVideoSettings.fsrEnginePreset(prefs)));
            e.put("TLG_FSR_SHARPNESS", BionicVideoSettings.fsrSharpnessValue(prefs));
            e.put("TLG_FSR_SHARPEN", BionicVideoSettings.fsrSharpenEnabled(prefs) ? "1" : "0");
            e.put("TLG_REACTIVE", "0");
            e.put("TLG_JITTER", "1");
            e.put("TLG_RENDER_RES", BionicVideoSettings.fsrInputSize(prefs));
        }
        String resolution = BionicVideoSettings.effectiveResolutionPatch(prefs);
        String resolutionPatch = BionicVideoSettings.internalPatchName(resolution);
        if (resolutionPatch != null) patches.add(resolutionPatch);
        if (prefs.getBoolean("patch_motion_blur", false)) patches.add("Disable motion blur");
        if (prefs.getBoolean("patch_dof", false)) patches.add("Disable depth of field");
        if (prefs.getBoolean("patch_depth_blur", false)) patches.add("Disable depth blur");
        if (prefs.getBoolean("patch_shadows", false)) patches.add("Disable shadow maps");
        e.put("ISS_PATCHES", android.text.TextUtils.join(";", patches));
        e.put("TLG_BP09", "0");
        e.put("TLG_BC_TINY", "0");
        e.put("TLG_OUTPUT_RES", outputResolution);
        return p.start();
    }

    private BionicLaunchEnv() {}
}
