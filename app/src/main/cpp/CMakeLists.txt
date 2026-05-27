
# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)
# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project("ax360e")

add_compile_options(-march=armv8-a)
add_compile_options(-stdlib=libc++)
add_compile_options(-fPIC)

add_library(hardware_ProcessorInfo SHARED hardware_ProcessorInfo.cpp)
target_link_libraries(hardware_ProcessorInfo PRIVATE log)

add_compile_options(-fno-omit-frame-pointer)
add_compile_options(-funwind-tables)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_link_options(-Wl,-z,max-page-size=16384)

set(CMAKE_CXX_STANDARD 20)

project(ax360e LANGUAGES C CXX ASM)

include_directories(${CMAKE_SOURCE_DIR})

# =============================================================================
# libadrenotools integration (recommended path for custom Turnip loading)
#
# 1. Add the submodule:
#      git submodule add https://github.com/bylaws/libadrenotools.git external/libadrenotools
#
# 2. After adding, this block will include and link it.
#
# libadrenotools replaces most of the old fragile logic that used to be in vkapi.cpp.
# =============================================================================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/libadrenotools/CMakeLists.txt")
    message(STATUS "libadrenotools found - using it for custom Adreno driver loading")
    add_subdirectory(external/libadrenotools)
    set(HAS_LIBADRENOTOOLS ON)
else()
    message(FATAL_ERROR
        "================================================================================\n"
        "  libadrenotools submodule is MISSING.\n\n"
        "  This project HARD REQUIRES libadrenotools for custom Adreno (Turnip) driver support.\n"
        "  The old fragile custom loader (vkapi + stub libraries) has been completely removed.\n\n"
        "  REQUIRED SETUP (run from repo root or the cpp directory):\n\n"
        "    # PowerShell (Windows):\n"
        "    cd aX360e/app/src/main/cpp\n"
        "    git submodule add https://github.com/bylaws/libadrenotools.git external/libadrenotools\n"
        "    git submodule update --init --recursive\n\n"
        "    # Git Bash / WSL / Linux / macOS:\n"
        "    cd aX360e/app/src/main/cpp\n"
        "    git submodule add https://github.com/bylaws/libadrenotools.git external/libadrenotools\n"
        "    git submodule update --init --recursive\n\n"
        "  After the submodule is present, delete any stale build intermediates:\n"
        "    - In Android Studio: Build > Clean Project, then Build > Rebuild Project\n"
        "    - Or manually: rm -rf .cxx build\n\n"
        "  Then sync Gradle and build.\n"
        "================================================================================\n"
    )
endif()

add_library(emu SHARED)
set_target_properties(emu PROPERTIES OUTPUT_NAME "e")

target_link_options(emu PRIVATE -Wl,--exclude-libs,ALL)

#target_compile_options(emu PRIVATE -flto)
#target_link_options(emu PRIVATE -flto)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    #
else ()
set(CMAKE_BUILD_TYPE Release)
endif()

target_sources(emu
        PRIVATE
        aarch64_disasm.cpp
        ax360e.cpp
        ax360e_emu.cpp
        emulator.cpp
        emulator_ax360e.cpp
        document_file.cpp
        xe_a64_code_cache_posix.cpp
        xe_aaudio_audio_driver.cpp
        xe_aaudio_audio_system.cpp
        xe_android_hid.cpp
        xe_android_input_driver.cpp
        xe_android_util.cpp
        xe_opensles_audio_driver.cpp
        xe_opensles_audio_system.cpp
        xe_saf_disc_image_device.cpp
        xe_saf_disc_image_entry.cpp
        xe_saf_disc_image_file.cpp
        xe_saf_disc_archive_file.cpp
        xe_saf_disc_archive_device.cpp
        xe_saf_disc_archive_entry.cpp
        xe_saf_stfs_device.cpp
        xe_saf_stfs_entry.cpp
        xe_saf_stfs_file.cpp
        xe_saf_xex_device.cpp
        xe_saf_xex_entry.cpp
        xe_saf_xex_file.cpp
        xe_system_ax360e.cpp
        xenia/src/xenia/app/emulator_window.cc
        cpuinfo.cpp
        vkutil.cpp
        vulkan_test_jni.cpp
        adreno_driver.cpp
        vk_symbols.cpp   # Shared symbol resolution (replaced legacy vkapi.cpp)
)
# python xenia-build premake --target_os=android --devenv=cmake
#add_subdirectory(xenia/ab EXCLUDE_FROM_ALL)

# Remap Gradle's CMAKE_BUILD_TYPE to what premake generated
set(_ORIG_BUILD_TYPE ${CMAKE_BUILD_TYPE})
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_BUILD_TYPE "Android-ARM64-Debug")
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CMAKE_BUILD_TYPE "Android-ARM64-Release")
endif()

include(build/xenia-app-discord.cmake)
include(build/xenia-apu.cmake)
include(build/xenia-apu-nop.cmake)
include(build/xenia-base.cmake)
include(build/xenia-core.cmake)
include(build/xenia-cpu.cmake)
include(build/xenia-cpu-backend-a64.cmake)
include(build/xenia-cpu-backend-x64.cmake)
include(build/xenia-debug-ui.cmake)
include(build/xenia-gpu.cmake)
include(build/xenia-gpu-null.cmake)
#include(build/xenia-gpu-shader-compiler.cmake)
include(build/xenia-gpu-vulkan.cmake)
# Workaround: clang ICE on vulkan_render_target_cache.cc — change optimization and disable debug info
set_source_files_properties(
  xenia/src/xenia/gpu/vulkan/vulkan_render_target_cache.cc
  PROPERTIES COMPILE_FLAGS "-O1 -g0"
)
include(build/xenia-hid.cmake)
include(build/xenia-hid-nop.cmake)
include(build/xenia-hid-skylander.cmake)
include(build/xenia-kernel.cmake)
include(build/xenia-patcher.cmake)
include(build/xenia-ui.cmake)
include(build/xenia-ui-vulkan.cmake)
include(build/xenia-vfs.cmake)
include(build/aes_128.cmake)
include(build/capstone.cmake)
#include(build/cxxopts.cmake)
include(build/discord-rpc.cmake)
include(build/dxbc.cmake)
include(build/fmt.cmake)
include(build/glslang-spirv.cmake)
include(build/imgui.cmake)
include(build/libavcodec.cmake)
include(build/libavformat.cmake)
include(build/libavutil.cmake)
include(build/mspack.cmake)
include(build/pugixml.cmake)
include(build/snappy.cmake)
#include(build/tomlplusplus.cmake)
include(build/xxhash.cmake)
include(build/zarchive.cmake)
#include(build/zlib.cmake)
include(build/zstd.cmake)

# Restore original build type
set(CMAKE_BUILD_TYPE ${_ORIG_BUILD_TYPE})

# TODO -O2/-O3会导致XThread::Reenter抛出c++异常后无法正常展开
# 找到更好的解决方案后优化这里
if(CMAKE_BUILD_TYPE STREQUAL Release)
target_compile_options(xenia-kernel PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-O1>
)
target_compile_options(xenia-cpu PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-O1>
)
target_compile_options(xenia-cpu-backend-a64 PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-O1>
)
endif()

# Old android stub libraries (cutils, hardware, sync, etc.) have been completely removed.
# They were only required for the pre-libadrenotools manual loading strategy.
# libadrenotools handles dependency resolution internally.

target_include_directories(emu PRIVATE
        xenia
        xenia/src
        xenia/third_party
)

if(HAS_LIBADRENOTOOLS)
    target_include_directories(emu PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/libadrenotools/include
    )
    target_compile_definitions(emu PRIVATE HAS_LIBADRENOTOOLS=1)
endif()

target_link_libraries(emu
        PRIVATE
        xenia-apu
        xenia-apu-nop
        xenia-base
        xenia-core
        xenia-cpu
        xenia-cpu-backend-a64
        xenia-gpu
        xenia-gpu-null
        xenia-gpu-vulkan
        xenia-hid
        xenia-hid-nop
        xenia-kernel
        xenia-ui
        xenia-ui-vulkan
        xenia-vfs
        aes_128
        capstone
        fmt
        dxbc
        discord-rpc
        glslang-spirv
        imgui
        libavcodec
        libavutil
        libavformat
        mspack
        snappy
        xxhash
        android
        dl
        log
        OpenSLES
        aaudio
        z
        pugixml
        xenia-hid-skylander
        xenia-patcher

        # libadrenotools (for reliable custom Turnip driver loading)
        $<$<BOOL:${HAS_LIBADRENOTOOLS}>:adrenotools>
)

#xenia-patcher
