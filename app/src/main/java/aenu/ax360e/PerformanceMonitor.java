package aenu.ax360e;

import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.IOException;

public class PerformanceMonitor {
    private static final String TAG = "PerformanceMonitor";

    /**
     * Performance state classification for native consumption.
     * Used to inform dynamic resolution and quality adjustments.
     */
    public enum PerformanceState {
        NORMAL(0),          // Everything running smoothly
        PRESSURED(1),       // Starting to show strain (low battery, high memory)
        THROTTLING(2),      // Thermal throttling active
        CRITICAL(3);        // Critical condition (very low battery + throttling + low memory)

        private final int value;

        PerformanceState(int value) {
            this.value = value;
        }

        public int getValue() {
            return value;
        }
    }

    private final Context context;
    
    // Performance metrics
    private float currentFps = 0;
    private float averageFps = 0;
    private final float cpuUsage = 0;
    private final float gpuUsage = 0;  // Estimated
    private long memoryUsed = 0;
    private long memoryTotal = 0;
    private float batteryLevel = 100;
    private float deviceTemperature = 0;
    private boolean isThermalThrottling = false;
    
    // Shader compilation tracking
    private int shadersCompiled = 0;
    private int shaderCacheHits = 0;
    private long shaderCompilationTime = 0;

    // eDRAM/GMEM tracking
    private long gmemFlushes = 0;
    private long fsiOverhead = 0;

    // Metrics push tracking
    private long lastMetricsPushTime = 0;
    private static final long METRICS_PUSH_INTERVAL_MS = 2000; // Push every 2 seconds
    private PerformanceState lastPushedState = PerformanceState.NORMAL;

    public PerformanceMonitor(Context context) {
        this.context = context;
    }
    
    public void updateMetrics() {
        updateMemoryUsage();
        updateBatteryLevel();
        updateThermalStatus();
        updateCpuAccuracyMetrics();  // pulls reservation/timebase/unhandled stats from native CPU backend
    }
    
    private void updateMemoryUsage() {
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        ActivityManager activityManager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        if (activityManager == null) return;
        activityManager.getMemoryInfo(mi);
        
        memoryTotal = mi.totalMem;
        memoryUsed = memoryTotal - mi.availMem;
        
        // Check for low memory condition
        if (mi.lowMemory) {
            Log.w(TAG, "Device is in low memory condition");
        }
    }
    
    private void updateBatteryLevel() {
        IntentFilter ifilter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        Intent batteryStatus = context.registerReceiver(null, ifilter);
        
        if (batteryStatus != null) {
            int level = batteryStatus.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
            int scale = batteryStatus.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
            batteryLevel = level * 100 / (float) scale;
        }
    }
    
    private void updateThermalStatus() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            PowerManager powerManager = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
            if (powerManager == null) return;
            int thermalStatus = powerManager.getCurrentThermalStatus();
            
            // THERMAL_STATUS_NONE = 0, THERMAL_STATUS_LIGHT = 1, 
            // THERMAL_STATUS_MODERATE = 2, THERMAL_STATUS_SEVERE = 3, etc.
            isThermalThrottling = thermalStatus >= PowerManager.THERMAL_STATUS_MODERATE;
            
            // Estimate temperature (rough approximation)
            deviceTemperature = 25 + (thermalStatus * 10); // 25°C + 10°C per level
        } else {
            // Fallback for older Android versions - try reading thermal zone
            try (BufferedReader reader = new BufferedReader(new FileReader("/sys/class/thermal/thermal_zone0/temp"))) {
                String temp = reader.readLine();
                deviceTemperature = Integer.parseInt(temp) / 1000.0f; // Convert from millidegrees
                isThermalThrottling = deviceTemperature > 60; // Threshold for throttling
            } catch (IOException | NumberFormatException e) {
                // Couldn't read temperature
                deviceTemperature = 0;
            }
        }
    }
    
    public void updateFps(float fps) {
        this.currentFps = fps;
        // Calculate rolling average
        this.averageFps = (averageFps * 0.9f) + (fps * 0.1f);
    }
    
    public void recordShaderCompilation(long timeMs, boolean cacheHit) {
        if (cacheHit) {
            shaderCacheHits++;
        } else {
            shadersCompiled++;
            shaderCompilationTime += timeMs;
        }
    }
    
    public void recordGmemFlush() {
        gmemFlushes++;
    }
    
    public void recordFsiOverhead(long timeNs) {
        fsiOverhead += timeNs;
    }
    
    // Getters
    public float getCurrentFps() { return currentFps; }
    public float getAverageFps() { return averageFps; }
    public float getCpuUsage() { return cpuUsage; }
    public float getGpuUsage() { return gpuUsage; }
    public long getMemoryUsed() { return memoryUsed; }
    public long getMemoryTotal() { return memoryTotal; }
    public float getBatteryLevel() { return batteryLevel; }
    public float getDeviceTemperature() { return deviceTemperature; }
    public boolean isThermalThrottling() { return isThermalThrottling; }
    
    public int getShadersCompiled() { return shadersCompiled; }
    public int getShaderCacheHits() { return shaderCacheHits; }
    public long getShaderCompilationTime() { return shaderCompilationTime; }
    public float getShaderCacheHitRate() {
        int total = shadersCompiled + shaderCacheHits;
        return total > 0 ? (shaderCacheHits * 100.0f / total) : 0;
    }
    
    public long getGmemFlushes() { return gmemFlushes; }
    public long getFsiOverhead() { return fsiOverhead; }

    // CPU accuracy metrics (populated on demand from native)
    private String lastCpuAccuracyReport = "CPU metrics not yet fetched";
    
    public String getFormattedMemoryUsage() {
        float usedMB = memoryUsed / (1024.0f * 1024.0f);
        float totalMB = memoryTotal / (1024.0f * 1024.0f);
        return String.format("%.0f / %.0f MB", usedMB, totalMB);
    }
    
    public String getFormattedThermalStatus() {
        if (deviceTemperature > 0) {
            return String.format("%.1f°C%s", deviceTemperature, 
                isThermalThrottling ? " (Throttling)" : "");
        }
        return "Unknown";
    }

    /**
     * Fetch latest CPU accuracy metrics from native (reservations, unhandled instrs, timebase, etc.).
     * Call periodically alongside other updates. The native side also logs periodically.
     */
    public void updateCpuAccuracyMetrics() {
        if (Emulator.get != null) {
            try {
                lastCpuAccuracyReport = Emulator.get.get_cpu_accuracy_metrics();
                Log.d(TAG, "CPU accuracy: " + lastCpuAccuracyReport);
            } catch (Exception e) {
                Log.e(TAG, "Failed to fetch CPU accuracy metrics", e);
            }
        }
    }

    public String getLastCpuAccuracyReport() {
        return lastCpuAccuracyReport;
    }
    
    public boolean shouldReduceQuality() {
        // Suggest quality reduction if:
        // - Thermal throttling is active
        // - Battery is low and we're not charging
        // - Memory is critically low
        return isThermalThrottling ||
               (batteryLevel < 20 && !isCharging()) ||
               (memoryUsed > memoryTotal * 0.9);
    }

    public PerformanceState getPerformanceState() {
        boolean lowBattery = batteryLevel < 20 && !isCharging();
        boolean highMemory = memoryUsed > memoryTotal * 0.85;

        // CRITICAL: multiple severe conditions
        if (isThermalThrottling && (lowBattery || highMemory)) {
            return PerformanceState.CRITICAL;
        }

        // THROTTLING: thermal issues
        if (isThermalThrottling) {
            return PerformanceState.THROTTLING;
        }

        // PRESSURED: starting to show strain
        if (lowBattery || highMemory || (memoryUsed > memoryTotal * 0.75)) {
            return PerformanceState.PRESSURED;
        }

        // NORMAL: everything OK
        return PerformanceState.NORMAL;
    }

    /**
     * Push metrics snapshot to native code.
     * Should be called after updateMetrics().
     * Pushes on fixed cadence (2s) and on thermal state transitions.
     */
    public void pushMetricsToNative() {
        if (Emulator.get == null) {
            return;
        }

        long now = SystemClock.elapsedRealtime();
        PerformanceState currentState = getPerformanceState();

        // Push on state change or at regular intervals
        boolean stateChanged = currentState != lastPushedState;
        boolean intervalElapsed = (now - lastMetricsPushTime) >= METRICS_PUSH_INTERVAL_MS;

        if (stateChanged || intervalElapsed) {
            float memUsedMB = memoryUsed / (1024.0f * 1024.0f);
            float memTotalMB = memoryTotal / (1024.0f * 1024.0f);
            float frameTimeMs = currentFps > 0 ? (1000.0f / currentFps) : 0;

            try {
                Emulator.get.push_performance_metrics(
                        currentFps,
                        frameTimeMs,
                        currentState.getValue(),
                        memUsedMB,
                        memTotalMB,
                        deviceTemperature
                );

                lastMetricsPushTime = now;
                lastPushedState = currentState;

                if (stateChanged) {
                    Log.i(TAG, "Performance state changed to: " + currentState);
                }
            } catch (Exception e) {
                Log.e(TAG, "Failed to push metrics to native", e);
            }
        }
    }

    /**
     * CAPTAIN DIRECT ORDER - trigger for 128B reservation stress + false-share test harness.
     * Exposes the cvar-driven debug validation sequences (from a64_backend / a64_seq_memory)
     * to Java side (PerformanceMonitor or hidden dev settings / diagnostics UI).
     *
     * Sequences exercised:
     *   - Thread A lwarx at X (128B granule)
     *   - Crossing ordinary stw to X+64 or X+127 (verifies ClearXenonReservationIfStoreOverlaps clears it)
     *   - V128 wide store analogs
     *   - Results: logs with exact research citations + increments crossing_invalidation_tests + false_share_detected
     *     (visible via getLastCpuAccuracyReport / PERF_TAG and get_cpu_accuracy_metrics).
     *
     * Research citations (embedded in native harness):
     *   Real Xenon 128B reservation granule + per-thread pairing errata.
     *   Normal stores must invalidate (the landed helper).
     *   False sharing at 128B boundaries real risk for audio + physics lock-free cross-core code.
     *
     * Call from dev UI / hidden setting when a64_accuracy_debug is desired on Adreno to
     * prove the 128B invalidation research-to-code is solid and trustworthy on device.
     * Lightweight, reuses existing metrics + cvar + logging infrastructure.
     */
    public void trigger128BReservationStressTest() {
        if (Emulator.get != null) {
            try {
                Emulator.get.trigger_128b_reservation_stress_test();
                Log.i(TAG, "128B reservation stress harness triggered (crossing stores / false share validation). "
                           + "See CPU_ACCURACY logs for cross_128b_inv + false_share_128b + research citations.");
                // Optionally refresh metrics snapshot immediately after harness run
                updateCpuAccuracyMetrics();
            } catch (Exception e) {
                Log.e(TAG, "Failed to trigger 128B reservation stress harness", e);
            }
        } else {
            Log.w(TAG, "Emulator not available - cannot trigger 128B stress harness");
        }
    }

    /**
     * R1 (original paired-single / ps_* research report author) + CAPTAIN DIRECT ORDER.
     * Java-side trigger for the lightweight ps accuracy validation harness (a64_ps_accuracy_stress).
     * Exact mirror of trigger128BReservationStressTest pattern.
     *
     * On call (from hidden dev setting / diagnostics UI):
     *   - Invokes native RunPairedSingleAccuracyHarness via JNI.
     *   - Exercises: ps_maddx (FMA on known values - highest priority per R1 for UE3/Forza/Halo
     *     vertex/skin/anim/physics), ps_addx/ps_msubx, basic psq quant roundtrips (GQR), NaN/denorm
     *     edges, and psq_st 128B reservation granule invalidation stress (the *explicit* warning
     *     in the 55-tool R1 report: psq_st stores must hit ClearXenon... or atomicity breaks in
     *     real titles mixing lockfree + quantized data).
     *   - R1 AUTHOR ENRICH (CAPTAIN RE-TASK): now also title-derived ps_madd skin/phys/vtx patterns,
     *     realistic GQR VBO roundtrips (real helpers from ppc_context), expanded psq_st+128B false-share
     *     + lock-free CAS + lwsync combos, + additional per-element NaN/denorm/rounding/FPCR edges
     *     (mixed ps0/ps1, SNaN payload, ties, FZ/DN, overflow) with 2 new counters.
     *   - Increments + surfaces: ps_arith_executed, ps_fma_cases, psq_load_store_count,
     *     ps_nan_denorm_edge_hits + NEW ps_mixed_element_edges/ps_rounding_edges in CPU_ACCURACY snapshot (get_cpu_accuracy_metrics) + PERF_TAG logcat.
     *
     * Also auto-runs at A64Backend init when cvar set (same as 128B).
     * Research anchors: R1 ps report priorities + GQR + 128B psq_st interaction notes (see
     * a64_backend.cc RunPairedSingle... + ppc_emit_fpu.cc plan + a64_seq_memory STORE_F* comments).
     *
     * Captain/devs: while ps_* fleet agents land emitters, flip a64_ps_accuracy_stress + a64_accuracy_debug
     * on real Adreno device and trigger here to immediately validate results.
     */
    public void triggerPSAccuracyStressTest() {
        if (Emulator.get != null) {
            try {
                Emulator.get.trigger_ps_accuracy_stress_test();
                Log.i(TAG, "PS_* (paired-single) accuracy stress harness triggered (R1 research author / harness owner). "
                           + "ENRICHED: title ps_madd + real GQR VBO + psq+lockfree+lwsync + extra per-elem FPCR edges (new counters). "
                           + "See CPU_ACCURACY for all ps_* + research citations.");
                updateCpuAccuracyMetrics();
            } catch (Exception e) {
                Log.e(TAG, "Failed to trigger PS accuracy stress harness", e);
            }
        } else {
            Log.w(TAG, "Emulator not available - cannot trigger PS accuracy stress harness");
        }
    }

    private boolean isCharging() {
        IntentFilter ifilter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        Intent batteryStatus = context.registerReceiver(null, ifilter);

        if (batteryStatus != null) {
            int status = batteryStatus.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
            return status == BatteryManager.BATTERY_STATUS_CHARGING ||
                   status == BatteryManager.BATTERY_STATUS_FULL;
        }
        return false;
    }

}
