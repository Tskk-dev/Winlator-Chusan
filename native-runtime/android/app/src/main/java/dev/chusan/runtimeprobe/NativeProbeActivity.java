package dev.chusan.runtimeprobe;

import android.app.Activity;
import android.os.Bundle;
import android.widget.ScrollView;
import android.widget.TextView;
import org.json.JSONObject;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Diagnostic entry point. Each driver test is launched in a fresh process over ADB. */
public final class NativeProbeActivity extends Activity {
    static { System.loadLibrary("chusan_driver_probe"); }

    private static native String probe(String nativeLibraries, String files, String cache, String customLibrary);

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setPadding(32, 32, 32, 32);
        text.setTextIsSelectable(true);
        text.setText("Checking Vulkan driver…");
        ScrollView scroll = new ScrollView(this);
        scroll.addView(text);
        setContentView(scroll);
        new Thread(() -> {
            String report;
            try {
                String library = "";
                if ("custom".equals(getIntent().getStringExtra("driver"))) {
                    File metadata = new File(getFilesDir(), "driver/meta.json");
                    library = new JSONObject(new String(Files.readAllBytes(metadata.toPath()), StandardCharsets.UTF_8))
                            .getString("libraryName");
                }
                report = probe(getApplicationInfo().nativeLibraryDir, getFilesDir().getAbsolutePath(),
                        getCacheDir().getAbsolutePath(), library);
            } catch (Exception error) {
                JSONObject failure = new JSONObject();
                try { failure.put("error", error.toString()); } catch (Exception ignored) { }
                report = failure.toString();
            }
            try {
                File temporary = new File(getFilesDir(), "vulkan-probe.tmp");
                Files.write(temporary.toPath(), report.getBytes(StandardCharsets.UTF_8));
                if (!temporary.renameTo(new File(getFilesDir(), "vulkan-probe.json")))
                    throw new java.io.IOException("Cannot publish probe report");
            } catch (Exception error) {
                report = "Cannot save probe report: " + error;
            }
            String result = report;
            runOnUiThread(() -> text.setText(result));
        }, "Vulkan probe").start();
    }
}
