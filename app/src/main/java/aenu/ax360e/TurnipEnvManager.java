package aenu.ax360e;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;
import androidx.preference.PreferenceManager;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Manages Turnip driver environment variables based on user preferences.
 * These environment variables control Mesa Turnip driver behavior for Adreno GPUs.
 */
public class TurnipEnvManager {
    private static final String TAG = "TurnipEnvManager";

    private final Context context;

    public TurnipEnvManager(Context context) {
        this.context = context;
    }

    /**
     * Build environment variable configuration based on preferences.
     * Returns a map-like structure that can be applied to the native emulator process.
     */
    public List<EnvVar> buildEnvironmentVariables() {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        Map<String, String> configValues = loadGlobalConfigValues();
        List<EnvVar> envVars = new ArrayList<>();

        // TU_DEBUG flags
        List<String> tuDebugFlags = new ArrayList<>();

        if (getBooleanSetting(configValues, prefs, "TurnipAdvanced|gmem_mode", false)) {
            tuDebugFlags.add("gmem");
        }

        if (getBooleanSetting(configValues, prefs, "TurnipAdvanced|debug_logging", false)) {
            tuDebugFlags.add("startup");
            tuDebugFlags.add("nir");
        }

        if (getBooleanSetting(configValues, prefs, "TurnipAdvanced|noubwc", false)) {
            tuDebugFlags.add("noubwc");
        }

        // Sysmem mode - required for Adreno 830 where GMEM is broken
        if (getBooleanSetting(configValues, prefs, "TurnipAdvanced|sysmem_mode", false)) {
            tuDebugFlags.add("sysmem");
        }

        // Disable Low Resolution Z - fixes Z-fighting and GPU hangs on some Adreno GPUs
        if (getBooleanSetting(configValues, prefs, "TurnipAdvanced|nolrz", false)) {
            tuDebugFlags.add("nolrz");
        }

        // Performance: Force binning pass for better TBDR tile utilization
        if (getBooleanSetting(configValues, prefs, "TurnipAdvanced|forcebin", false)) {
            tuDebugFlags.add("forcebin");
        }

        // Performance: Skip unnecessary GPU flushes between draw calls
        if (getBooleanSetting(configValues, prefs, "TurnipAdvanced|noflush", false)) {
            tuDebugFlags.add("noflush");
        }

        // Graphical fix: Disable visibility stream culling (fixes missing geometry)
        if (getBooleanSetting(configValues, prefs, "TurnipAdvanced|novsc", false)) {
            tuDebugFlags.add("novsc");
        }

        if (tuDebugFlags.isEmpty()) {
            String recommendedFlags = getRecommendedTuDebugForDevice();
            if (!recommendedFlags.isEmpty()) {
                for (String token : recommendedFlags.split(",")) {
                    String trimmed = token.trim();
                    if (!trimmed.isEmpty()) {
                        tuDebugFlags.add(trimmed);
                    }
                }
            }
        }

        if (!tuDebugFlags.isEmpty()) {
            envVars.add(new EnvVar("TU_DEBUG", String.join(",", tuDebugFlags)));
        }

        // FD_DEV_FEATURES flags
        if (getBooleanSetting(configValues, prefs, "TurnipAdvanced|ubwc_flag_hint", false)) {
            envVars.add(new EnvVar("FD_DEV_FEATURES", "enable_tp_ubwc_flag_hint=1"));
        }

        // MESA_DEBUG - general Mesa debugging
        if (getBooleanSetting(configValues, prefs, "TurnipAdvanced|debug_logging", false)) {
            envVars.add(new EnvVar("MESA_DEBUG", "1"));
        }

        return envVars;
    }

    private boolean getBooleanSetting(
            Map<String, String> configValues,
            SharedPreferences prefs,
            String key,
            boolean defaultValue) {
        String configValue = configValues.get(key);
        if (configValue != null) {
            return Boolean.parseBoolean(configValue);
        }
        return prefs.getBoolean(key, defaultValue);
    }

    private Map<String, String> loadGlobalConfigValues() {
        Map<String, String> values = new HashMap<>();
        File configFile = Application.get_global_config_file();
        if (!configFile.exists() || !configFile.canRead()) {
            return values;
        }

        String currentSection = null;
        try (BufferedReader reader = new BufferedReader(new FileReader(configFile))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String trimmed = stripInlineComment(line).trim();
                if (trimmed.isEmpty()) {
                    continue;
                }
                if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
                    currentSection = trimmed.substring(1, trimmed.length() - 1).trim();
                    continue;
                }

                int equalsIndex = trimmed.indexOf('=');
                if (equalsIndex <= 0 || currentSection == null) {
                    continue;
                }

                String key = trimmed.substring(0, equalsIndex).trim();
                String value = trimmed.substring(equalsIndex + 1).trim();
                if (value.length() >= 2 && value.startsWith("\"") && value.endsWith("\"")) {
                    value = value.substring(1, value.length() - 1);
                }
                values.put(currentSection + "|" + key, value);
            }
        } catch (IOException e) {
            Log.w(TAG, "Failed to read global config for Turnip settings", e);
        }
        return values;
    }

    private String stripInlineComment(String line) {
        boolean inString = false;
        for (int i = 0; i < line.length(); ++i) {
            char character = line.charAt(i);
            if (character == '"') {
                inString = !inString;
            } else if (character == '#' && !inString) {
                return line.substring(0, i);
            }
        }
        return line;
    }

    private String getRecommendedTuDebugForDevice() {
        return TurnipDriverInfo.getRecommendedTuDebug(detectGpuName());
    }

    private String detectGpuName() {
        String[] candidatePaths = {
                "/sys/class/kgsl/kgsl-3d0/gpu_model",
                "/sys/class/kgsl/kgsl-3d0/gpu_name",
                "/sys/devices/platform/kgsl-3d0.0/kgsl/kgsl-3d0/gpu_model"
        };
        for (String path : candidatePaths) {
            File file = new File(path);
            if (!file.exists() || !file.canRead()) {
                continue;
            }
            try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
                String line = reader.readLine();
                if (line != null && !line.trim().isEmpty()) {
                    return line.trim();
                }
            } catch (IOException ignored) {
                // Try the next location.
            }
        }
        return "";
    }

    /**
     * Apply performance preset configurations
     */
    public void applyPreset(String presetKey, Emulator.Config config) {
        switch (presetKey) {
            case "Presets|ultra_performance":
                applyUltraPerformancePreset(config);
                break;
            case "Presets|high_quality":
                applyHighQualityPreset(config);
                break;
            case "Presets|maximum_quality":
                applyMaximumQualityPreset(config);
                break;
            case "Presets|battery_optimized":
                applyBatteryOptimizedPreset(config);
                break;
        }
    }

    private void applyUltraPerformancePreset(Emulator.Config config) {
        // Minimize resolution for maximum FPS
        config.save_config_entry("Video|internal_display_resolution", "1x");
        config.save_config_entry("GPU|vsync", "false");
        config.save_config_entry("Display|postprocess_antialiasing", "disabled");
        config.save_config_entry("Display|postprocess_scaling_and_sharpening", "bilinear");

        // Enable dynamic resolution with low min
        config.save_config_entry("MobileGPU|dynamic_resolution", "true");
        config.save_config_entry("MobileGPU|resolution_scale_min", "50");
        config.save_config_entry("MobileGPU|resolution_scale_max", "100");

        // Power mode
        config.save_config_entry("PowerManagement|power_mode", "performance");
    }

    private void applyHighQualityPreset(Emulator.Config config) {
        // Balanced resolution and quality
        config.save_config_entry("Video|internal_display_resolution", "2x");
        config.save_config_entry("GPU|vsync", "true");
        config.save_config_entry("Display|postprocess_antialiasing", "fxaa");
        config.save_config_entry("Display|postprocess_scaling_and_sharpening", "cas");

        // Conservative dynamic resolution
        config.save_config_entry("MobileGPU|dynamic_resolution", "true");
        config.save_config_entry("MobileGPU|resolution_scale_min", "75");
        config.save_config_entry("MobileGPU|resolution_scale_max", "150");

        // Balanced power mode
        config.save_config_entry("PowerManagement|power_mode", "balanced");
    }

    private void applyMaximumQualityPreset(Emulator.Config config) {
        // Maximum resolution and quality
        config.save_config_entry("Video|internal_display_resolution", "3x");
        config.save_config_entry("GPU|vsync", "true");
        config.save_config_entry("Display|postprocess_antialiasing", "fxaa");
        config.save_config_entry("Display|postprocess_scaling_and_sharpening", "cas_sharpening_max");

        // Disable dynamic resolution
        config.save_config_entry("MobileGPU|dynamic_resolution", "false");
        config.save_config_entry("MobileGPU|resolution_scale_max", "200");

        // Performance power mode
        config.save_config_entry("PowerManagement|power_mode", "performance");
    }

    private void applyBatteryOptimizedPreset(Emulator.Config config) {
        // Minimum quality for battery saving
        config.save_config_entry("Video|internal_display_resolution", "1x");
        config.save_config_entry("GPU|vsync", "false");
        config.save_config_entry("Display|postprocess_antialiasing", "disabled");
        config.save_config_entry("Display|postprocess_scaling_and_sharpening", "bilinear");

        // Enable dynamic resolution with very low min
        config.save_config_entry("MobileGPU|dynamic_resolution", "true");
        config.save_config_entry("MobileGPU|resolution_scale_min", "50");
        config.save_config_entry("MobileGPU|resolution_scale_max", "75");

        // Enable all power saving features
        config.save_config_entry("PowerManagement|power_mode", "battery_saver");
        config.save_config_entry("PowerManagement|background_unload", "true");
        config.save_config_entry("PowerManagement|low_memory_mode", "true");
        config.save_config_entry("MobileGPU|thermal_aware", "true");
    }

    /**
     * Environment variable pair
     */
    public static class EnvVar {
        public final String name;
        public final String value;

        public EnvVar(String name, String value) {
            this.name = name;
            this.value = value;
        }

        @Override
        public String toString() {
            return name + "=" + value;
        }
    }
}
