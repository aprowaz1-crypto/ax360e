#include "adreno_driver.h"
#include "vk_symbols.h"   // For ResolveVulkanSymbols / ResetVulkanSymbols

#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#if defined(HAS_LIBADRENOTOOLS)
#include "adrenotools/driver.h"
#endif

#define LOG_TAG "AdrenoDriver"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

bool g_using_custom_driver = false;
std::string g_last_status;

void* g_libvulkan_handle = nullptr;

} // namespace

bool load_custom_adreno_driver(const std::string& driver_dir,
                               const std::string& driver_name,
                               bool enable_redirection)
{
    if (g_using_custom_driver) {
        LOGI("Custom driver already loaded");
        return true;
    }

    std::string full_driver_path = driver_dir;
    if (!full_driver_path.empty() && full_driver_path.back() != '/') {
        full_driver_path += '/';
    }
    full_driver_path += driver_name;

    LOGI("Loading custom Adreno driver: %s", full_driver_path.c_str());

    // Fast existence check for better error messages in UI status dialogs
    if (access(full_driver_path.c_str(), F_OK) != 0) {
        g_last_status = "ERROR: Driver file not found on disk: " + full_driver_path +
                        "\n\nMake sure the Turnip .so and vk_icd.json are installed in the custom driver folder.";
        LOGE("Custom driver file missing: %s", full_driver_path.c_str());
        return false;
    }

#if defined(HAS_LIBADRENOTOOLS)
    // === Preferred path: libadrenotools ===
    LOGI("Using libadrenotools for driver loading");

    int flags = ADRENOTOOLS_DRIVER_CUSTOM;
    if (enable_redirection) {
        flags |= ADRENOTOOLS_DRIVER_FILE_REDIRECT;
    }

    // Use the app's native library directory for the hook library
    // (this is where the adrenotools hook .so lives after packaging)
    const char* hook_lib_dir = nullptr; // let adrenotools figure it out when possible

    // Temporary directory for injected libraries (inside app data is fine)
    std::string tmp_dir = driver_dir + "/tmp/";
    // Best-effort ensure the tmp directory exists (adrenotools uses it for extracted hooks)
    mkdir(tmp_dir.c_str(), 0755);  // ignore return; if it fails adrenotools will surface a clear error

    void* user_mapping = nullptr;

    g_libvulkan_handle = adrenotools_open_libvulkan(
        RTLD_NOW | RTLD_LOCAL,
        flags,
        tmp_dir.c_str(),
        hook_lib_dir,
        driver_dir.c_str(),      // customDriverDir
        driver_name.c_str(),
        driver_dir.c_str(),      // fileRedirectDir (for turnip_config, shader dumps, etc.)
        &user_mapping
    );

    if (g_libvulkan_handle) {
        g_using_custom_driver = true;
        g_last_status = "Loaded via libadrenotools: " + driver_name;
        LOGI("libadrenotools successfully loaded custom driver");

        // Resolve all required Vulkan symbols from the newly loaded library.
        ResetVulkanSymbols();
        if (ResolveVulkanSymbols(g_libvulkan_handle)) {
            LOGI("Vulkan symbols resolved successfully via libadrenotools path");

            // Optional verification: try to query the actual physical device
            // to confirm we really got a Turnip/Mesa driver.
            if (vkEnumeratePhysicalDevices_ && vkGetPhysicalDeviceProperties_) {
                uint32_t deviceCount = 0;
                if (vkEnumeratePhysicalDevices_(nullptr, &deviceCount, nullptr) == VK_SUCCESS && deviceCount > 0) {
                    // We can't easily create an instance here without more context,
                    // but at least the pointers are valid. Real verification happens later
                    // when Xenia creates the real VkInstance.
                    LOGI("Symbol resolution looks healthy (device enumeration functions available)");
                }
            }
        } else {
            LOGE("Symbol resolution FAILED after libadrenotools load — custom driver will likely not work");
            g_last_status = "ERROR: Loaded library but failed to resolve Vulkan entry points.";
        }

        return true;
    } else {
        LOGE("adrenotools_open_libvulkan failed for %s", driver_name.c_str());
        g_last_status = "ERROR: Failed to load via libadrenotools.\nDriver: " + driver_name + "\nCheck logcat for details (AdrenoDriver tag).";
    }
#else
    LOGW("libadrenotools not available at build time - custom driver support disabled");
#endif

    // Fallback path (when libadrenotools is not compiled in)
    ResetVulkanSymbols();

    g_last_status = "ERROR: Custom driver support requires libadrenotools submodule.\nSee external/libadrenotools/README.md for setup instructions.";
    LOGE("[AdrenoDriver] Custom Turnip loading requires libadrenotools. Add the submodule and rebuild.");
    return false;
}

void unload_custom_adreno_driver() {
    if (!g_using_custom_driver) return;

#if defined(HAS_LIBADRENOTOOLS)
    // libadrenotools manages the library lifetime internally in most cases.
    LOGI("Custom driver unload requested (libadrenotools path)");
#endif

    g_using_custom_driver = false;
    g_libvulkan_handle = nullptr;
    g_last_status = "Driver unloaded";
}

bool is_using_custom_adreno_driver() {
    return g_using_custom_driver;
}

std::string get_custom_driver_status() {
    return g_last_status.empty() ? "No custom driver status" : g_last_status;
}

bool is_using_libadrenotools() {
#if defined(HAS_LIBADRENOTOOLS)
    return g_using_custom_driver;
#else
    return false;
#endif
}

bool supports_libadrenotools_build() {
#if defined(HAS_LIBADRENOTOOLS)
    return true;
#else
    return false;
#endif
}

std::string get_detailed_driver_status() {
    std::string status = get_custom_driver_status();

    // Always report build-time capability (deepened for About / info screens UX polish p3-4)
    bool buildSupportsAdrenotools = false;
#if defined(HAS_LIBADRENOTOOLS)
    buildSupportsAdrenotools = true;
    status = "BUILD: libadrenotools ENABLED\n" + status;
#else
    status = "BUILD: libadrenotools NOT COMPILED IN\n" + status;
#endif

    if (g_using_custom_driver) {
#if defined(HAS_LIBADRENOTOOLS)
        status = "RUNTIME: libadrenotools path active\n" + status;
        // Note: symbol resolution state is internal; loader success reflected in g_last_status
#else
        status = "RUNTIME: legacy/custom path (no adrenotools)\n" + status;
#endif
    } else if (status.find("ERROR") == std::string::npos && !status.empty() && status != "No custom driver status") {
        status = "RUNTIME: Driver installed on disk but not loaded in this process\n" + status;
    }

    return status;
}

// ============================================================================
// Compatibility shims for the old vkapi API
// These are always provided so that code using the old API doesn't break.
// When libadrenotools is active, they forward to the new implementation.
// ============================================================================

extern "C" {

void vk_load(const char* lib_path, bool is_adreno_custom) {
    if (is_adreno_custom) {
        // Forward to the new modern loader
        if (!lib_path || lib_path[0] == '\0') return;

        std::string path(lib_path);
        size_t last_slash = path.find_last_of("/\\");
        std::string dir  = (last_slash != std::string::npos) ? path.substr(0, last_slash) : ".";
        std::string name = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;

        load_custom_adreno_driver(dir, name, true);
    } else {
        // For system driver, we currently do nothing here.
        // The system Vulkan loader will be used normally.
    }
}

void vk_unload() {
    unload_custom_adreno_driver();
}

bool vk_is_loaded() {
    return is_using_custom_adreno_driver();
}

} // extern "C"

// ============================================================================
// JNI exports for Java side
// ============================================================================

extern "C" {

JNIEXPORT jboolean JNICALL
Java_aenu_ax360e_Emulator_nativeLoadCustomAdrenoDriver(JNIEnv* env, jclass clazz,
                                                       jstring driverDir,
                                                       jstring driverName,
                                                       jboolean enableRedirection) {
    const char* dir = env->GetStringUTFChars(driverDir, nullptr);
    const char* name = env->GetStringUTFChars(driverName, nullptr);

    bool result = load_custom_adreno_driver(dir ? dir : "", name ? name : "", enableRedirection);

    env->ReleaseStringUTFChars(driverDir, dir);
    env->ReleaseStringUTFChars(driverName, name);

    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_aenu_ax360e_Emulator_nativeGetCustomDriverStatus(JNIEnv* env, jclass clazz) {
    std::string status = get_custom_driver_status();
    return env->NewStringUTF(status.c_str());
}

JNIEXPORT jboolean JNICALL
Java_aenu_ax360e_Emulator_nativeIsUsingCustomAdrenoDriver(JNIEnv* env, jclass clazz) {
    return is_using_custom_adreno_driver() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_aenu_ax360e_Emulator_nativeIsUsingLibadrenotools(JNIEnv* env, jclass clazz) {
    return is_using_libadrenotools() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_aenu_ax360e_Emulator_nativeGetDetailedDriverStatus(JNIEnv* env, jclass clazz) {
    std::string status = get_detailed_driver_status();
    return env->NewStringUTF(status.c_str());
}

JNIEXPORT jboolean JNICALL
Java_aenu_ax360e_Emulator_nativeSupportsLibadrenotoolsBuild(JNIEnv* env, jclass clazz) {
    return supports_libadrenotools_build() ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"