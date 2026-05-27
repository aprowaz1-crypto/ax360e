#pragma once

#include <string>

// Loads a custom Adreno Vulkan driver (typically Mesa Turnip / freedreno).
//
// This is the new recommended path using libadrenotools when available.
//
// Parameters:
//   driver_dir   : Directory containing the custom driver .so (e.g. internal app storage path)
//   driver_name  : Name of the .so file, e.g. "libvulkan_freedreno.so"
//   enable_redirection : Whether to enable file redirection (useful for shader dumps / turnip_config)
//
// Returns true on success.
bool load_custom_adreno_driver(const std::string& driver_dir,
                               const std::string& driver_name,
                               bool enable_redirection = true);

// Unloads the custom driver (best effort).
void unload_custom_adreno_driver();

// Returns true if we are currently using a custom driver loaded via adrenotools (or legacy path).
bool is_using_custom_adreno_driver();

// Returns a short human-readable status string (useful for diagnostics / UI).
std::string get_custom_driver_status();

// Returns true if the current driver was loaded through libadrenotools.
bool is_using_libadrenotools();

// Returns true if the *build* includes libadrenotools (compile-time HAS_LIBADRENOTOOLS).
// Used for About / info screens to surface build capability (p3-4 polish).
bool supports_libadrenotools_build();

// Returns a richer status string including runtime driver detection when possible.
std::string get_detailed_driver_status();