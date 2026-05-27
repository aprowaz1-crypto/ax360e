package aenu.ax360e;

import android.content.Context;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;

public class TurnipDriverInfo {
    private static final String TAG = "TurnipDriverInfo";
    
    private String driverName = "Unknown";
    private String driverVersion = "Unknown";
    private String mesaVersion = "Unknown";
    private boolean isTurnip = false;
    private boolean isInstalled = false;

    // Runtime state (from native loader)
    private boolean isUsingLibadrenotools = false;
    private boolean isActiveInProcess = false;
    private String runtimeStatus = "";
    
    public static TurnipDriverInfo detect(Context context) {
        TurnipDriverInfo info = new TurnipDriverInfo();
        
        File driverDir = CustomDriverUtils.getDriverDirectory(context);
        File icdFile = new File(driverDir, "vk_icd.json");
        File metaFile = new File(driverDir, "meta.json");
        
        if (icdFile.exists()) {
            info.isInstalled = true;
            
            // Try meta.json first for better info
            if (metaFile.exists()) {
                try (BufferedReader reader = new BufferedReader(new FileReader(metaFile))) {
                    StringBuilder sb = new StringBuilder();
                    String line;
                    while ((line = reader.readLine()) != null) {
                        sb.append(line);
                    }
                    JSONObject meta = new JSONObject(sb.toString());
                    info.driverName = meta.optString("name", "Mesa Turnip");
                    info.mesaVersion = meta.optString("description", "Unknown");
                    info.driverVersion = meta.optString("driverVersion", "Unknown");
                    info.isTurnip = true;
                } catch (IOException | JSONException e) {
                    Log.w(TAG, "Failed to read meta.json", e);
                }
            }

            // Fallback to vk_icd.json if meta.json fails or doesn't exist
            if (!info.isTurnip) {
                try (BufferedReader reader = new BufferedReader(new FileReader(icdFile))) {
                    StringBuilder sb = new StringBuilder();
                    String line;
                    while ((line = reader.readLine()) != null) {
                        sb.append(line);
                    }
                    
                    JSONObject json = new JSONObject(sb.toString());
                    JSONObject icd = json.getJSONObject("ICD");
                    String libraryPath = icd.getString("library_path");
                    
                    // Check if it's Turnip driver
                    if (libraryPath.contains("turnip") || libraryPath.contains("Turnip")
                            || libraryPath.contains("freedreno") || libraryPath.contains("libvulkan_freedreno")) {
                        info.isTurnip = true;
                        if (info.driverName.equals("Unknown")) info.driverName = "Mesa Turnip";
                        
                        // Try to extract version from filename
                        File soFile = new File(libraryPath);
                        String filename = soFile.getName();
                        if (filename != null && !filename.isEmpty()) {
                            info.parseVersionFromFilename(filename);
                        }
                    }
                    
                    if (info.driverVersion.equals("Unknown")) {
                        info.driverVersion = icd.optString("api_version", "Unknown");
                    }
                } catch (IOException | JSONException e) {
                    Log.e(TAG, "Failed to read driver info", e);
                }
            }
        }

        // Query runtime state from the native loader (this is the key improvement)
        try {
            info.isUsingLibadrenotools = Emulator.nativeIsUsingLibadrenotools();
            info.isActiveInProcess = Emulator.nativeIsUsingCustomAdrenoDriver();
            // Prefer the detailed status when available
            String detailed = null;
            try {
                detailed = Emulator.nativeGetDetailedDriverStatus();
            } catch (Throwable ignored) {}
            info.runtimeStatus = (detailed != null && !detailed.isEmpty()) ? detailed : Emulator.nativeGetCustomDriverStatus();
        } catch (Throwable t) {
            Log.w(TAG, "Failed to query native driver runtime status", t);
        }
        
        return info;
    }
    
    private void parseVersionFromFilename(String filename) {
        // Common pattern: libvulkan_freedreno.so or turnip_26.0.0.so
        if (filename.contains("26.0") || filename.contains("26_0")) {
            mesaVersion = "Mesa 26.0.0";
        } else if (filename.contains("25.2") || filename.contains("25_2")) {
            mesaVersion = "Mesa 25.2.0";
        } else if (filename.contains("25.0") || filename.contains("25_0")) {
            mesaVersion = "Mesa 25.0.0";
        }
        
        // Try to extract R version (R7, R8, etc.)
        if (filename.toLowerCase().contains("r8")) {
            mesaVersion += " R8";
        } else if (filename.toLowerCase().contains("r7")) {
            mesaVersion += " R7";
        } else if (filename.toLowerCase().contains("r6")) {
            mesaVersion += " R6";
        }
    }
    
    public String getDriverName() {
        return driverName;
    }
    
    public String getDriverVersion() {
        return driverVersion;
    }
    
    public String getMesaVersion() {
        return mesaVersion;
    }
    
    public boolean isTurnip() {
        return isTurnip;
    }
    
    public boolean isInstalled() {
        return isInstalled;
    }

    public boolean isUsingLibadrenotools() {
        return isUsingLibadrenotools;
    }

    public boolean isActiveInProcess() {
        return isActiveInProcess;
    }

    public String getRuntimeStatus() {
        return runtimeStatus;
    }
    
    public String getFormattedInfo() {
        if (!isInstalled) {
            return "No custom driver installed";
        }
        
        StringBuilder sb = new StringBuilder();
        sb.append(driverName);
        if (!mesaVersion.equals("Unknown")) {
            sb.append("\n").append(mesaVersion);
        }
        sb.append("\nVulkan ").append(driverVersion);

        // Show runtime status when using the modern loader (p3-3 enhanced guidance)
        if (isUsingLibadrenotools) {
            sb.append("\n[Active via libadrenotools]");
            if (isActiveInProcess) {
                sb.append(" (loaded in process)");
            } else {
                sb.append(" (installed but NOT active - see steps below)");
            }
            if (runtimeStatus != null && !runtimeStatus.isEmpty() && !runtimeStatus.equals("No custom driver status")) {
                sb.append("\n").append(runtimeStatus);
            }
        } else if (isActiveInProcess) {
            sb.append("\n[Active via legacy loader]");
        } else if (isInstalled) {
            // Explicit inactive case for callers that use this method directly (About, etc.)
            sb.append("\n⚠ Installed but NOT active in current process");
            if (runtimeStatus != null && !runtimeStatus.isEmpty() && !runtimeStatus.equals("No custom driver status")) {
                sb.append("\n").append(runtimeStatus);
            }
        }
        
        return sb.toString();
    }
    
    public String getRecommendedGpu() {
        if (!isTurnip || mesaVersion.equals("Unknown")) {
            return "Unknown";
        }
        
        if (mesaVersion.contains("26.0")) {
            return "Recommended for Adreno 840, 750, 740";
        } else if (mesaVersion.contains("25.2")) {
            return "Recommended for Adreno 750";
        } else if (mesaVersion.contains("25.0")) {
            return "Recommended for Adreno 740";
        }
        
        return "Check compatibility";
    }

    /**
     * Returns a short warning if this driver version is known to be risky on the current GPU family.
     */
    public String getCompatibilityWarning(String gpuName) {
        if (!isTurnip || gpuName == null) return "";

        String lowerGpu = gpuName.toLowerCase();
        String lowerVersion = mesaVersion.toLowerCase();

        if (lowerGpu.contains("840") || lowerGpu.contains("a840")) {
            if (lowerVersion.contains("25.0") || lowerVersion.contains("25.2")) {
                return "Warning: Older Mesa 25.x builds often have stability issues on Adreno 840. Prefer 26.0+ R7/R8.";
            }
        }
        return "";
    }
    
    /**
     * Returns device-specific TU_DEBUG recommendations based on GPU model.
     * Research from Mesa Turnip documentation and community reports:
     * - Adreno 840: Latest gen, GMEM + forcebin for best TBDR performance
     * - Adreno 830: GMEM has issues on some firmware, UBWC flag hint needed
     * - Adreno 750/740/730: GMEM + forcebin for good TBDR performance
     * - Adreno 710/720: gmem mode recommended for better performance
     * - Samsung OneUI devices: UBWC flag hint recommended
     */
    public static String getRecommendedTuDebug(String gpuName) {
        if (gpuName == null) return "";
        String gpuLower = gpuName.toLowerCase();
        if (gpuLower.contains("840") || gpuLower.contains("a840")) {
            return "gmem,forcebin";
        } else if (gpuLower.contains("830") || gpuLower.contains("a830")) {
            return "";  // Use safe defaults, UBWC hint applied via native code
        } else if (gpuLower.contains("750") || gpuLower.contains("a750")
                || gpuLower.contains("740") || gpuLower.contains("a740")) {
            return "gmem,forcebin";
        } else if (gpuLower.contains("730") || gpuLower.contains("a730")) {
            return "gmem";
        } else if (gpuLower.contains("710") || gpuLower.contains("720")
                || gpuLower.contains("a710") || gpuLower.contains("a720")) {
            return "gmem";
        }
        return "";
    }

    /**
     * Returns the physical Vulkan GPU device name for the current device (Phase 3 p3-2).
     * Prefers cached value from Application for efficiency, falls back to native query.
     */
    public static String getDeviceGpuName() {
        try {
            String cached = aenu.ax360e.Application.gpu_device_name_vk;
            if (cached != null && !cached.isEmpty() && !cached.equals("Unknown")) {
                return cached;
            }
        } catch (Throwable ignored) {}
        try {
            String vkName = aenu.hardware.ProcessorInfo.gpu_get_physical_device_name_vk();
            if (vkName != null && !vkName.isEmpty()) {
                return vkName;
            }
        } catch (Throwable ignored) {}
        return "Unknown";
    }

    /**
     * Builds a detailed compatibility report for the current device GPU vs this driver.
     * Uses existing warning + recommendation logic + TU_DEBUG hints. (p3-2 enhancement)
     */
    public String getDeviceCompatibilityReport() {
        String gpu = getDeviceGpuName();
        if (gpu == null || gpu.equals("Unknown") || !isTurnip) {
            return "";
        }

        StringBuilder sb = new StringBuilder();
        sb.append("Device GPU: ").append(gpu).append("\n");

        String warning = getCompatibilityWarning(gpu);
        if (!warning.isEmpty()) {
            sb.append("⚠ ").append(warning).append("\n");
        } else {
            sb.append("✓ Compatible with detected GPU family\n");
        }

        // Check if driver version is recommended for this GPU (match against getRecommendedGpu text)
        String driverRec = getRecommendedGpu();
        String gLower = gpu.toLowerCase();
        boolean matchesRec = false;
        if (driverRec.contains("840") && (gLower.contains("840") || gLower.contains("a840"))) matchesRec = true;
        else if (driverRec.contains("750") && (gLower.contains("750") || gLower.contains("a750"))) matchesRec = true;
        else if (driverRec.contains("740") && (gLower.contains("740") || gLower.contains("a740"))) matchesRec = true;
        else if (driverRec.contains("730") && (gLower.contains("730") || gLower.contains("a730"))) matchesRec = true;
        else if (driverRec.contains("710") || driverRec.contains("720")) {
            if (gLower.contains("710") || gLower.contains("720") || gLower.contains("a710") || gLower.contains("a720")) matchesRec = true;
        }

        if (matchesRec) {
            sb.append("✓ ").append(driverRec).append(" (matches your device)\n");
        }

        String tuDebug = getRecommendedTuDebug(gpu);
        if (tuDebug != null && !tuDebug.isEmpty()) {
            sb.append("Suggested TU_DEBUG: ").append(tuDebug).append("\n");
        }

        return sb.toString();
    }

    /**
     * Returns a richly formatted, emoji-enhanced, sectioned driver information report.
     * Used by both the full Turnip Driver Information dialog and the quick status dialog (p3-2).
     * Includes installation, runtime, device compatibility, and loader details.
     * Always fresh on call (detect() caller provides latest state).
     */
    public String getRichDriverReport(Context context) {
        StringBuilder message = new StringBuilder();

        String deviceGpu = getDeviceGpuName();

        if (isInstalled()) {
            // Header / overall status
            message.append("📦 INSTALLATION STATUS\n");
            message.append("─────────────────────\n");
            message.append("Driver: ").append(getDriverName()).append("\n");
            message.append("Version: ").append(getDriverVersion()).append("\n");
            if (!getMesaVersion().equals("Unknown")) {
                message.append("Mesa: ").append(getMesaVersion()).append("\n");
            }
            message.append("\n");

            // Runtime / Loader status with prominent icons (p3-3: rich contextual guidance + fix steps)
            message.append("⚙ RUNTIME STATUS\n");
            message.append("────────────────\n");
            if (isUsingLibadrenotools()) {
                message.append("✓ Loaded via libadrenotools (recommended modern path)\n");
                if (isActiveInProcess()) {
                    message.append("✓ Currently ACTIVE in this process\n");
                } else {
                    message.append("⚠ Installed but NOT active in current process\n");
                    message.append("\n");
                    message.append("CLEAR STEPS TO FIX / ACTIVATE:\n");
                    message.append("• Force close the app from Android recents and relaunch\n");
                    message.append("• Check that a game profile uses \"Driver Selection = Default\"\n");
                    message.append("• If native status shows ERROR: Remove driver here then re-install the ZIP\n");
                    message.append("• Confirm libadrenotools support in the About screen\n");
                    message.append("• Advanced: inspect logcat for AdrenoDriver / dlopen failures\n");
                }
            } else if (isActiveInProcess()) {
                message.append("⚠ Active via legacy loader (consider migrating to libadrenotools build)\n");
            } else {
                message.append("⚠ Installed on disk but NOT currently loaded/active by the emulator.\n");
                message.append("\n");
                message.append("CLEAR STEPS TO FIX / ACTIVATE:\n");
                message.append("• Force-stop aX360e and restart it (most common fix)\n");
                message.append("• Open a game or return to main screen (triggers loader)\n");
                message.append("• Verify Driver Selection in game profiles is not set to FORCE_SYSTEM\n");
                message.append("• Re-install the driver package if you previously saw load errors\n");
                message.append("• Use the main screen toolbar subtitle or Settings → Driver Info for live updates\n");
            }
            message.append("\n");

            // Device compatibility (new p3-2 detail)
            message.append("🎮 DEVICE COMPATIBILITY\n");
            message.append("──────────────────────\n");
            String compat = getDeviceCompatibilityReport();
            if (compat != null && !compat.isEmpty()) {
                message.append(compat);
            } else if (deviceGpu != null && !deviceGpu.equals("Unknown")) {
                message.append("Device GPU: ").append(deviceGpu).append("\n");
                message.append("Compatibility could not be determined automatically.\n");
            } else {
                message.append("Device GPU: Unknown\n");
            }
            message.append("\n");

            // Additional details from formatted info
            String formatted = getFormattedInfo();
            if (formatted != null && !formatted.isEmpty() && !formatted.contains("No custom driver")) {
                message.append("📋 ADDITIONAL DETAILS\n");
                message.append("────────────────────\n");
                message.append(formatted).append("\n\n");
            }

            // Raw loader details if useful
            String runtime = getRuntimeStatus();
            if (runtime != null && !runtime.isEmpty() && !runtime.contains("No custom driver status")) {
                message.append("🔧 LOADER DETAILS\n");
                message.append("────────────────\n");
                message.append(runtime).append("\n");
            }
        } else {
            message.append("📦 INSTALLATION STATUS\n");
            message.append("─────────────────────\n");
            message.append("No custom Turnip driver installed.\n\n");
            message.append("📍 TO USE A MODERN TURNIP DRIVER:\n");
            message.append("1. Go to Settings → Custom Drivers → GPU Driver (Turnip ZIP)\n");
            message.append("2. Install a compatible Turnip driver package (ZIP)\n");
            message.append("3. Ensure the build supports libadrenotools\n\n");
            message.append("🎮 For best results on Adreno GPUs, Mesa 26.0+ (R7/R8) Turnip is recommended.\n");
        }

        // p3-6: Explicit help text in the Driver Information dialog (and quick status / About reuse)
        try {
            String help = context.getString(R.string.driver_help_tip);
            if (help != null && !help.isEmpty()) {
                message.append("\n");
                message.append("ℹ HELP\n");
                message.append("────\n");
                message.append(help).append("\n");
            }
        } catch (Exception ignored) {}

        return message.toString().trim();
    }
}
