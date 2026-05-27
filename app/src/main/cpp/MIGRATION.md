# Driver Loading Migration to libadrenotools

This project has completed migration from a custom, fragile Adreno driver loader to **libadrenotools**.

## What Changed

### Before (Removed)
- Complex manual loading in `vkapi.cpp` (memfd copies, namespace assault, manual stub preloading)
- `dummy_libs.cpp` + generation of many `android_stub_*` shared libraries at build time
- Java-side `ensureAndroidStubLibraries()` + copying system libraries into `android_stub/`
- Heavy runtime mutation of `LD_LIBRARY_PATH`
- Many environment variable hacks tied to the old loader

### After (Current)
- `adreno_driver.cpp` + `adreno_driver.h` as the single entry point
- Uses `adrenotools_open_libvulkan()` from libadrenotools when available
- Early injection in `ax360e_emu.cpp` `OnInitialize()` (before any Xenia Vulkan code runs)
- Clean support for `TU_DEBUG`, `FD_DEV_FEATURES`, file redirection, etc.
- Much more reliable on modern Android + Adreno 8xx devices

## Required Setup (Build Prerequisite)

**This is mandatory before any build will succeed.**

```powershell
# Windows PowerShell
cd aX360e/app/src/main/cpp
git submodule add https://github.com/bylaws/libadrenotools.git external/libadrenotools
git submodule update --init --recursive
```

```bash
# Git Bash / WSL / Linux
cd aX360e/app/src/main/cpp
git submodule add https://github.com/bylaws/libadrenotools.git external/libadrenotools
git submodule update --init --recursive
```

After adding:
- Clean stale build dirs (Android Studio: Build > Clean/Rebuild, or delete `.cxx` and `build` folders)
- The top-level `CMakeLists.txt` will now **FATAL_ERROR** (with the exact commands above) if the submodule is absent.

See `external/libadrenotools/README.md` for the full rationale.

## For Developers

- Use `load_custom_adreno_driver()` (C++) or the new JNI methods on `Emulator` for custom drivers.
- New diagnostics: `nativeIsUsingLibadrenotools()` and `is_using_libadrenotools()`.
- The legacy `vk_load(..., true)` path now explicitly rejects custom drivers.
- Old stub-related code has been aggressively deleted across CMake, Java, and native layers.
- `vkapi.cpp` / `vkapi.h` and `dummy_libs.cpp` have been **permanently deleted**.
- Symbol resolution lives in `vk_symbols.h` + `vk_symbols.cpp` (the `VKFN` macro pattern).
- `adreno_driver.cpp` is the sole owner of custom driver loading + provides compatibility shims for the old `vk_load`/`vk_is_loaded`/`vk_unload` API.
- Zero references to the old fragile loader remain in the build.

## Status

**Phase 1 (Final Decoupling) + Phase 2 (Nuclear Cleanup) completed.**

- `vkapi.cpp`, `vkapi.h`, and `dummy_libs.cpp` have been **permanently deleted**.
- All references to the old stub generation and fragile loading logic have been removed.
- `adreno_driver.cpp` + `vk_symbols.cpp` are now the sole owners of custom driver loading and symbol resolution.
- Compatibility shims for the old API are provided cleanly in the new code.
- Major comment and code hygiene pass completed.

**Phase 3 (Polish & UX) completed across 6 workstreams (p3-1 → p3-6).**

All Phase 3 UX improvements are complete, consistent, and documented. The driver experience is now polished, discoverable, and provides rich live feedback using `TurnipDriverInfo` + native libadrenotools queries.

### Workstream Summary

- **p3-1: Main Screen Polish** (MainActivity + activity_main.xml)
  - Toolbar subtitle shows "GPU: libadrenotools + Turnip ✓" (or custom state) when relevant; tappable for quick status.
  - Subtle driver status header line (AppBar, scrolls away, only for non-empty lists + installed driver).
  - Non-intrusive empty-state driver badge (11sp, low alpha; now conditional on installed driver only).
  - `updateAllDriverStatusIndicators()` + onResume() keep everything live.
  - Startup health check dialog/toast with rich actionable guidance when driver installed but inactive.

- **p3-2: Dialog Polish & Rich Reports** (TurnipDriverInfo.java + EmulatorSettings + MainActivity + AboutActivity)
  - `getRichDriverReport()`: emoji-sectioned, comprehensive (INSTALLATION / RUNTIME STATUS with clear fix steps / DEVICE COMPATIBILITY with GPU name + TU_DEBUG hints / LOADER DETAILS).
  - Consistent "Turnip Driver Information" / "GPU Driver Status" dialogs everywhere (scrollable selectable TextView + prominent 📋 Copy + Refresh + Close).
  - `getDeviceGpuName()` + `getDeviceCompatibilityReport()` + warnings integrated.
  - Quick status from toolbar reuses exact same rich flow.

- **p3-3: Error Feedback & Startup Warnings** (EmulatorSettings, MainActivity, TurnipDriverInfo, native)
  - Rich post-install success/failure toasts + dialogs (include native detailed status + numbered QUICK FIXES).
  - Startup warning surfaces `nativeGetDetailedDriverStatus()` + step-by-step advice + one-time "Driver Health Check" dialog.
  - `getRichDriverReport()` embeds contextual guidance (e.g., "CLEAR STEPS TO FIX / ACTIVATE").
  - Legacy vs modern path surfaced clearly with migration nudges.

- **p3-4: About Screen Deep Integration** (AboutActivity.java + activity_about.xml + strings + native)
  - Prominent top Driver Actions bar (Driver Info / Troubleshooting / Driver Settings buttons).
  - About text now includes full `TurnipDriverInfo` formatted + compatibility + `nativeGetDetailedDriverStatus()` + `nativeSupportsLibadrenotoolsBuild()`.
  - Troubleshooting dialog with common fixes + deep links to full report / settings.
  - New native `supports_libadrenotools_build()` / `nativeSupportsLibadrenotoolsBuild()` surfaced.

- **p3-5: Settings Integration Polish** (EmulatorSettings.java + emulator_settings.xml + strings)
  - Live `refreshDriverLiveState()` on create/resume/install/remove/Refresh (GPU summary + loader indicator + Driver Info summary all synchronized).
  - New non-selectable "Driver Loader Path" indicator (Modern libadrenotools ✓ vs legacy vs none, with build support note).
  - Quick actions: "View Full Driver Status" + "Refresh Driver State" (under GPU Driver category).
  - All summaries now reflect live runtime (`isUsingLibadrenotools`, `isActiveInProcess`, detailed status).
  - Organized GPU Driver (Turnip) category with modern indicators vs legacy.

- **p3-6: Documentation & Final Review** (this workstream)
  - MIGRATION.md fully updated with complete Phase 3 story + 6-workstream summary.
  - Added explicit in-app help text in Driver Information dialog (ℹ HELP section + tip) and improved hints in About/Settings.
  - Comprehensive consistency review performed across toolbar, dialogs (3 sites), About, Settings, startup warnings, error paths, main badges.
  - Fixed remaining polish: extracted ~20 badge/toolbar/summary strings to strings.xml (for i18n + no more drift), made empty-state badge conditional (no clutter when no driver), replaced all remaining hardcoded "Driver Info"/"Copy failed"/status labels with resources, added help footer.
  - Verified no missing strings, coherent UX flow ("tap anywhere relevant → rich live report with copy/refresh"), Phase 3 story complete.

### Key Files Changed in Phase 3
- Java: MainActivity.java, EmulatorSettings.java, AboutActivity.java, TurnipDriverInfo.java, Emulator.java
- Resources: strings.xml (many new p3-*-prefixed + help), activity_main.xml, activity_about.xml, emulator_settings.xml (comments + structure)
- Native: adreno_driver.cpp/.h (detailed status, build flag, p3-4 polish)
- Docs: MIGRATION.md (this section + status)

Phase 3 delivers a complete, user-friendly, self-diagnosing custom driver experience on top of the solid libadrenotools foundation from Phases 1-2.

See:
- `external/libadrenotools/README.md`
- `adreno_driver.cpp` + `ax360e_emu.cpp` (early loading)
- `TurnipDriverInfo.java` (the heart of p3 UX)
- UI strings and layouts for all indicators/dialogs

**Overall libadrenotools migration status: COMPLETE (Phases 1-3).**