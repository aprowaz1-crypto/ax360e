// SPDX-License-Identifier: WTFPL
package aenu.ax360e;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import androidx.documentfile.provider.DocumentFile;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.Base64;

public class Emulator extends aenu.emulator.Emulator{
    public static Emulator get=null;
    public static void load_library(){
        if(get!=null)
            return;
        get=new Emulator();
        System.loadLibrary("e");
    }

    /*public void key_event(int keycode,boolean pressed){
        throw new RuntimeException("Not implemented");
        final int unused=-1;
        super.key_event(keycode,pressed,unused);
    }*/

    public native void setup_context(Context ctx);
    public native void setup_document_file_tree(DocumentFile tree);
    public native void setup_launch_args(String[] args);
    public  native void setup_uri_info_list_file(String path);
    public native String simple_device_info();
    public native String generate_config_xml(String config_path);

    /**
     * Push performance metrics snapshot to native code.
     * Called periodically (every 1-2s) and on thermal state transitions.
     *
     * @param fps Current frames per second
     * @param frameTimeMs Average frame time in milliseconds
     * @param perfState Performance state (0=NORMAL, 1=PRESSURED, 2=THROTTLING, 3=CRITICAL)
     * @param memoryUsedMB Memory used in MB
     * @param memoryTotalMB Total memory in MB
     * @param temperature Device temperature in Celsius
     */
    public native void push_performance_metrics(
            float fps,
            float frameTimeMs,
            int perfState,
            float memoryUsedMB,
            float memoryTotalMB,
            float temperature
    );

    /**
     * Update memory pressure status for dynamic texture cache management.
     * Called periodically (every 5s) to adjust GPU memory limits based on system RAM pressure.
     *
     * @param pressureLevel Pressure level (0=NONE, 1=LOW, 2=MEDIUM, 3=HIGH, 4=CRITICAL)
     * @param availableMB Available RAM in MB
     * @param thermalLevel Thermal status (0-6, from PowerManager.THERMAL_STATUS_*)
     */
    public native void update_memory_pressure(
            int pressureLevel,
            long availableMB,
            int thermalLevel
    );

    /**
     * Returns a snapshot of CPU accuracy / diagnostic metrics.
     * Includes counts for unhandled guest instructions, reservation (lwarx/stwcx)
     * acquires/successes/failures (with success rate), timebase reads, and room for extension.
     * Useful for measuring accuracy on real devices.
     */
    public native String get_cpu_accuracy_metrics();

    /**
     * Triggers the 128B reservation stress + false-share debug harness (CAPTAIN DIRECT ORDER).
     * Wires Java/PerformanceMonitor/hidden dev setting into the native A64 backend
     * validation sequences for lwarx + crossing stores (X+64, X+127) + V128 at 128B granules.
     * Logs full research citations (128B granule, per-thread pairing errata, audio+physics
     * false sharing) and increments crossing_invalidation_tests / false_share_detected
     * (visible in get_cpu_accuracy_metrics and PERF_TAG logs).
     * Used to prove the ClearXenonReservationIfStoreOverlaps research-to-code on real Adreno.
     */
    public native void trigger_128b_reservation_stress_test();

    /**
     * R1 (original paired-single research author) + CAPTAIN: triggers the ps_* accuracy
     * validation harness (a64_ps_accuracy_stress).
     * Modeled directly on trigger_128b_reservation_stress_test.
     * Exercises ps_maddx (FMA highest priority), ps_addx/msubx, basic psq quant (GQR),
     * NaN/denorm edges, and psq_st 128B reservation interaction (explicit warning in R1
     * 55-tool report: psq_st stores must invalidate granules like normal float stores or
     * lockfree+quantized titles corrupt atomics).
     * Increments ps_arith_executed / ps_fma_cases / psq_load_store_count / ps_nan_denorm_edge_hits
     * (surfaced in get_cpu_accuracy_metrics + logcat PERF_TAG).
     * Activation also at A64Backend init. As fleet lands ps emitters, flip cvar on real
     * Adreno + trigger from dev UI to immediately see if results are correct.
     * Full R1 citations + 128B interaction notes in native RunPairedSingleAccuracyHarness.
     */
    public native void trigger_ps_accuracy_stress_test();

    // === New libadrenotools-based custom driver loading ===
    public static native boolean nativeLoadCustomAdrenoDriver(String driverDir, String driverName, boolean enableRedirection);
    public static native String nativeGetCustomDriverStatus();
    public static native boolean nativeIsUsingCustomAdrenoDriver();
    public static native boolean nativeIsUsingLibadrenotools();
    public static native String nativeGetDetailedDriverStatus();
    /**
     * Returns true if this build was compiled with libadrenotools support (HAS_LIBADRENOTOOLS).
     * Used by AboutActivity and info screens for accurate "driver support" reporting (p3-4).
     */
    public static native boolean nativeSupportsLibadrenotoolsBuild();

    public static int nc_open_uri_fd(Context ctx,Uri uri) {
        ParcelFileDescriptor pfd_ = null;
        try {
            pfd_ = ctx.getContentResolver().openFileDescriptor(uri, "r");
            if (pfd_ == null) {
                Log.e("ax360e", "openFileDescriptor returned null for: " + uri);
                return -1;
            }
            int game_fd = pfd_.detachFd();
            return game_fd;
        } catch (Exception e) {
            Log.e("ax360e", "Failed to open URI fd", e);
            return -1;
        } finally {
            if (pfd_ != null) {
                try {
                    pfd_.close();
                } catch (Exception e) {
                    Log.e("ax360e", "Failed to close ParcelFileDescriptor", e);
                }
            }
        }
    }


    public native GameInfo meta_info_from_god_game(Context ctx,String uri) throws RuntimeException;


    public static class GameInfo{

        public String uri;
        public String name;
        public int fd;
        public byte[] icon;


        static JSONObject to_json(GameInfo  info) throws JSONException {
            JSONObject json=new JSONObject();

            json.put("uri",info.uri);
            if(info.name!=null)
                json.put("name",info.name);

            if(info.icon!=null)
                json.put("icon", Base64.getEncoder().encodeToString(info.icon));
            return json;
        }

        static GameInfo from_json(JSONObject json) throws JSONException {
            GameInfo info=new GameInfo();
            info.uri=json.getString("uri");
            if(json.has("name"))
                info.name=json.getString("name");
            if(json.has("icon"))
                info.icon=Base64.getDecoder().decode(json.getString("icon"));

            return info;
        }
    }

    // === Robustness: error surfacing to Java UI layer from native A64 backend / FatalError ===
    // Called by native code (via JNI from ShowSimpleMessageBox / unhandled paths) to surface
    // problems (unhandled instrs, guest crashes, accuracy issues) as toasts/logs.
    // Native threads attach and call this; we post to UI if possible.
    public static void onNativeEmulatorError(String message, int severity) {
        Log.e("ax360e_native_error", "SEV" + severity + ": " + message);
        // Best-effort toast from any thread via Application context if available.
        try {
            android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());
            handler.post(() -> {
                try {
                    android.widget.Toast.makeText(
                        aenu.ax360e.Application.ctx,
                        "Emulator: " + (message != null ? message.substring(0, Math.min(120, message.length())) : "error"),
                        android.widget.Toast.LENGTH_LONG
                    ).show();
                } catch (Exception toastEx) {
                    Log.w("ax360e", "Toast failed in onNativeEmulatorError", toastEx);
                }
            });
        } catch (Exception e) {
            Log.e("ax360e", "Failed to post native error toast", e);
        }
    }
}
