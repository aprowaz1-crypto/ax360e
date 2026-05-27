// SPDX-License-Identifier: WTFPL
// Turnip Driver Environment Variable Helpers
//
// Used to configure TU_DEBUG, FD_DEV_FEATURES, etc. for Mesa Turnip.
//
// Driver loading itself is handled by adreno_driver.cpp (libadrenotools).

#ifndef AX360E_TURNIP_ENV_H
#define AX360E_TURNIP_ENV_H

#include <cstdlib>
#include <string>
#include <android/log.h>

#define TURNIP_LOG_TAG "TurnipEnv"
#define TURNIP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, TURNIP_LOG_TAG, __VA_ARGS__)

namespace turnip {

// Environment variable helpers for Turnip driver optimization
// Reference: https://github.com/eden-emulator/Issue-Reports/pulls/3205
// Reference: Mesa Turnip documentation

inline void SetGmemMode(bool enable) {
    if (enable) {
        const char* existing = std::getenv("TU_DEBUG");
        if (existing && std::string(existing).find("gmem") == std::string::npos) {
            std::string combined = std::string(existing) + ",gmem";
            setenv("TU_DEBUG", combined.c_str(), 1);
        } else if (!existing) {
            setenv("TU_DEBUG", "gmem", 1);
        }
        TURNIP_LOGI("Enabled GMEM mode (recommended for Adreno 710/720)");
    }
}

inline void SetUbwcFlagHint(bool enable) {
    if (enable) {
        setenv("FD_DEV_FEATURES", "enable_tp_ubwc_flag_hint=1", 1);
        TURNIP_LOGI("Enabled UBWC flag hint (fixes OneUI graphical bugs)");
    }
}

inline void SetDebugLogging(bool enable) {
    if (enable) {
        setenv("FD_MESA_DEBUG", "1", 1);
        setenv("MESA_DEBUG", "1", 1);
        TURNIP_LOGI("Enabled Turnip debug logging");
    }
}

inline void DisableUbwc(bool disable) {
    if (disable) {
        const char* existing = std::getenv("TU_DEBUG");
        if (existing && std::string(existing).find("noubwc") == std::string::npos) {
            std::string combined = std::string(existing) + ",noubwc";
            setenv("TU_DEBUG", combined.c_str(), 1);
        } else if (!existing) {
            setenv("TU_DEBUG", "noubwc", 1);
        }
        TURNIP_LOGI("Disabled UBWC compression (debug mode)");
    }
}

// Force system memory rendering instead of GMEM
// Required for Adreno 830 where GMEM is broken and causes GPU hangs
inline void SetSysmemMode(bool enable) {
    if (enable) {
        const char* existing = std::getenv("TU_DEBUG");
        if (existing && std::string(existing).find("sysmem") == std::string::npos) {
            std::string combined = std::string(existing) + ",sysmem";
            setenv("TU_DEBUG", combined.c_str(), 1);
        } else if (!existing) {
            setenv("TU_DEBUG", "sysmem", 1);
        }
        TURNIP_LOGI("Enabled sysmem mode (required for Adreno 830)");
    }
}

// Disable Low Resolution Z feature
// Fixes Z-fighting and GPU hangs on some Adreno GPUs (especially A830)
inline void DisableLrz(bool disable) {
    if (disable) {
        const char* existing = std::getenv("TU_DEBUG");
        if (existing && std::string(existing).find("nolrz") == std::string::npos) {
            std::string combined = std::string(existing) + ",nolrz";
            setenv("TU_DEBUG", combined.c_str(), 1);
        } else if (!existing) {
            setenv("TU_DEBUG", "nolrz", 1);
        }
        TURNIP_LOGI("Disabled Low Resolution Z (fixes Z-fighting on some GPUs)");
    }
}

// Disable unnecessary flush operations for better throughput
// Reduces pipeline stalls between draw calls
inline void SetNoFlush(bool enable) {
    if (enable) {
        const char* existing = std::getenv("TU_DEBUG");
        if (existing && std::string(existing).find("noflush") == std::string::npos) {
            std::string combined = std::string(existing) + ",noflush";
            setenv("TU_DEBUG", combined.c_str(), 1);
        } else if (!existing) {
            setenv("TU_DEBUG", "noflush", 1);
        }
        TURNIP_LOGI("Disabled unnecessary flushes (perf optimization)");
    }
}

// Force binning pass to improve tile-based rendering performance
// Helps avoid fallback to direct rendering on complex scenes
inline void SetForceBinning(bool enable) {
    if (enable) {
        const char* existing = std::getenv("TU_DEBUG");
        if (existing && std::string(existing).find("forcebin") == std::string::npos) {
            std::string combined = std::string(existing) + ",forcebin";
            setenv("TU_DEBUG", combined.c_str(), 1);
        } else if (!existing) {
            setenv("TU_DEBUG", "forcebin", 1);
        }
        TURNIP_LOGI("Forced binning pass (TBDR optimization)");
    }
}

// Disable visibility stream optimization (fixes some rendering artifacts
// on certain Adreno GPUs where VPC/VS culling is too aggressive)
inline void SetNoVsc(bool enable) {
    if (enable) {
        const char* existing = std::getenv("TU_DEBUG");
        if (existing && std::string(existing).find("novsc") == std::string::npos) {
            std::string combined = std::string(existing) + ",novsc";
            setenv("TU_DEBUG", combined.c_str(), 1);
        } else if (!existing) {
            setenv("TU_DEBUG", "novsc", 1);
        }
        TURNIP_LOGI("Disabled visibility stream (fixes culling artifacts)");
    }
}

// Set Turnip performance level hint via FD_DEV_FEATURES
// max_freq=1 requests the GPU to run at maximum frequency
inline void SetMaxGpuFreq(bool enable) {
    if (enable) {
        const char* existing = std::getenv("FD_DEV_FEATURES");
        std::string features;
        if (existing) {
            features = std::string(existing);
            if (features.find("max_freq") == std::string::npos) {
                features += ",max_freq=1";
            }
        } else {
            features = "max_freq=1";
        }
        setenv("FD_DEV_FEATURES", features.c_str(), 1);
        TURNIP_LOGI("Requested max GPU frequency");
    }
}

// Helper to append a TU_DEBUG flag safely
inline void AppendTuDebugFlag(const char* flag) {
    const char* existing = std::getenv("TU_DEBUG");
    if (existing) {
        std::string existing_str(existing);
        if (existing_str.find(flag) == std::string::npos) {
            std::string combined = existing_str + "," + flag;
            setenv("TU_DEBUG", combined.c_str(), 1);
        }
    } else {
        setenv("TU_DEBUG", flag, 1);
    }
}

// Get current TU_DEBUG setting
inline const char* GetTuDebug() {
    return std::getenv("TU_DEBUG");
}

// Get current FD_DEV_FEATURES setting
inline const char* GetFdDevFeatures() {
    return std::getenv("FD_DEV_FEATURES");
}

// Check if Turnip environment is configured
inline bool IsTurnipConfigured() {
    return std::getenv("TU_DEBUG") != nullptr ||
           std::getenv("FD_DEV_FEATURES") != nullptr ||
           std::getenv("FD_MESA_DEBUG") != nullptr;
}

// Apply recommended performance defaults for a given Adreno GPU
// Call this before Vulkan instance creation
inline void ApplyRecommendedConfig(const std::string& gpu_model) {
    std::string lower = gpu_model;
    for (auto& c : lower) c = std::tolower(c);

    // Always enable UBWC flag hint (fixes Samsung OneUI artifacts)
    SetUbwcFlagHint(true);

    if (lower.find("830") != std::string::npos ||
        lower.find("a830") != std::string::npos) {
        // Adreno 830: GMEM has issues, use with caution
        // UBWC flag hint already set above
        TURNIP_LOGI("Adreno 830 detected - applying A830 config");
    } else if (lower.find("750") != std::string::npos ||
               lower.find("a750") != std::string::npos) {
        // Adreno 750: Good GMEM support, enable binning
        SetGmemMode(true);
        SetForceBinning(true);
        TURNIP_LOGI("Adreno 750 detected - GMEM + forced binning");
    } else if (lower.find("740") != std::string::npos ||
               lower.find("a740") != std::string::npos) {
        // Adreno 740: Good GMEM support
        SetGmemMode(true);
        SetForceBinning(true);
        TURNIP_LOGI("Adreno 740 detected - GMEM + forced binning");
    } else if (lower.find("730") != std::string::npos ||
               lower.find("a730") != std::string::npos) {
        SetGmemMode(true);
        TURNIP_LOGI("Adreno 730 detected - GMEM mode");
    } else if (lower.find("710") != std::string::npos ||
               lower.find("720") != std::string::npos ||
               lower.find("a710") != std::string::npos ||
               lower.find("a720") != std::string::npos) {
        SetGmemMode(true);
        TURNIP_LOGI("Adreno 710/720 detected - GMEM mode");
    } else if (lower.find("840") != std::string::npos ||
               lower.find("a840") != std::string::npos) {
        // Adreno 840 (Snapdragon 8 Elite): latest gen, full support
        SetGmemMode(true);
        SetForceBinning(true);
        TURNIP_LOGI("Adreno 840 detected - GMEM + forced binning");
    } else {
        // Unknown Adreno - apply safe defaults
        TURNIP_LOGI("Unknown Adreno model '%s' - using safe defaults", gpu_model.c_str());
    }

    const char* final_tu_debug = GetTuDebug();
    const char* final_fd_features = GetFdDevFeatures();
    TURNIP_LOGI("Final TU_DEBUG=%s", final_tu_debug ? final_tu_debug : "(none)");
    TURNIP_LOGI("Final FD_DEV_FEATURES=%s", final_fd_features ? final_fd_features : "(none)");
}

} // namespace turnip

#endif // AX360E_TURNIP_ENV_H
