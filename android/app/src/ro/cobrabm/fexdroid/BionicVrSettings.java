package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.content.Context;
import android.widget.Switch;

/** Persisted opt-in OpenXR virtual screen mode for Meta Quest memory savings. */
final class BionicVrSettings {
    static final String KEY = "vr_openxr_mode";
    static boolean enabled(Context context) {
        return context.getSharedPreferences("iss", 0).getBoolean(KEY, false);
    }
    static Switch toggle(Activity activity) {
        Switch control = new Switch(activity);
        control.setText(AppLanguage.text(
            "VR Mode / Virtual Screen (RAM Saver)",
            "Modo VR / Tela Virtual (Economia de RAM)"));
        control.setTextColor(0xfffff1e9);
        control.setChecked(enabled(activity));
        control.setOnCheckedChangeListener((button, checked) ->
            activity.getSharedPreferences("iss", 0).edit().putBoolean(KEY, checked).apply());
        return control;
    }
}
