/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <jni.h>

#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/looper.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <jni.h>
#include <array>
#include <memory>
#include <sys/prctl.h>

#include "xenia/app/emulator_window.h"
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/null/null_graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/hid/nop/nop_hid.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/menu_item.h"
#include "xenia/ui/surface_android.h"
#include "xenia/vfs/devices/host_path_device.h"

#include "emulator.h"
#include "emulator_ax360e.h"

#include "xe_android_hid.h"
#include "xe_android_input_driver.h"
#include "xe_opensles_audio_system.h"
#include "xe_aaudio_audio_system.h"
#include "document_file.h"

#include "ax360e_emu.h"
#include "turnip_env.h"
#include "ax360e_perf_log.h"
#include "adreno_driver.h"
#include <fstream>
//#include "nlohmann/json.hpp"

#define LOG_TAG "ax360e_native"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG,__VA_ARGS__);
#define LOGD(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG,__VA_ARGS__);

DEFINE_string(apu, "aaudio", "Audio system. Use: [any, nop, opensles, aaudio]", "APU");
DEFINE_string(gpu, "vulkan", "Graphics system. Use: [vulkan, null]",
              "GPU");
DEFINE_string(hid, "android", "Input system. Use: [android, nop]. Multiple drivers can be chained via comma, for example: android,nop",
              "HID");

DEFINE_path(
        storage_root, "",
        "Root path for persistent internal data storage (config, etc.), or empty "
        "to use the path preferred for the OS, such as the documents folder, or "
        "the emulator executable directory if portable.txt is present in it.",
        "Storage");
DEFINE_path(
        content_root, "",
        "Root path for guest content storage (saves, etc.), or empty to use the "
        "content folder under the storage root.",
        "Storage");
DEFINE_path(
        cache_root, "",
        "Root path for files used to speed up certain parts of the emulator or the "
        "game. These files may be persistent, but they can be deleted without "
        "major side effects such as progress loss. If empty, the cache folder "
        "under the storage root, or, if available, the cache directory preferred "
        "for the OS, will be used.",
        "Storage");

DEFINE_bool(mount_scratch, false, "Enable scratch mount", "Storage");
DEFINE_bool(mount_cache, false, "Enable cache mount", "Storage");

DEFINE_transient_path(target, "",
                      "Specifies the target .xex or .iso to execute.",
                      "General");
DEFINE_transient_bool(portable, false,
                      "Specifies if Xenia should run in portable mode.",
                      "General");

DECLARE_bool(debug);

DEFINE_bool(discord, false, "Enable Discord rich presence", "General");

extern JavaVM* g_jvm;
namespace ae{
    extern std::unique_ptr<xe::ui::WindowedApp> g_windowed_app;
}
void AndroidWindowedAppContext::NotifyUILoopOfPendingFunctions() {
assert(WindowedAppContext::ui_thread_id_!=std::this_thread::get_id());
pthread_mutex_lock(&mutex);
event=EVENT_EXECUTE_PENDING_FUNCTIONS;
while(event){
pthread_cond_wait(&cond, &mutex);
}
pthread_mutex_unlock(&mutex);
}

void AndroidWindowedAppContext::PlatformQuitFromUIThread() {
pthread_mutex_lock(&mutex);
event=EVENT_QUIT;
while ( event){
pthread_cond_wait(&cond, &mutex);
}
pthread_mutex_unlock(&mutex);
}

void AndroidWindowedAppContext::request_paint() {
    paint_requested.store(true, std::memory_order_release);
}

void AndroidWindowedAppContext::setup_ui_thr_id(std::thread::id id){
    WindowedAppContext::ui_thread_id_=id;
}

void paint();
void AndroidWindowedAppContext::main_loop(){
    assert(WindowedAppContext::ui_thread_id_==std::this_thread::get_id());
    LOGD("main_loop: entered");
    XELOGI("main_loop: entered, ui_thread running");
    int loop_count = 0;
    while(!WindowedAppContext::HasQuitFromUIThread()){
        loop_count++;
        if(loop_count == 1) {
            LOGD("main_loop: first iteration, paint_requested=%d, event=%d",
                 (int)paint_requested.load(std::memory_order_relaxed), (int)event);
        }
        if(loop_count == 1000) {
            LOGD("main_loop: 1000 iterations, paint_requested=%d, event=%d",
                 (int)paint_requested.load(std::memory_order_relaxed), (int)event);
            XELOGI("main_loop: 1000 iterations, paint_requested={}, event={}",
                   paint_requested.load(std::memory_order_relaxed), (int)event);
        }
        if(event==0){
            if(paint_requested.exchange(false, std::memory_order_acquire)){
                paint();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        else if(event==EVENT_EXECUTE_PENDING_FUNCTIONS){
            pthread_mutex_lock(&mutex);
            WindowedAppContext::ExecutePendingFunctionsFromUIThread();
            event=0;
            pthread_cond_signal(&cond);
            pthread_mutex_unlock(&mutex);
            // Process any paint requests triggered during pending functions
            if(paint_requested.exchange(false, std::memory_order_acquire)){
                paint();
            }
        }
        else if(event==EVENT_QUIT){
            pthread_mutex_lock(&mutex);
            WindowedAppContext::QuitFromUIThread();
            event=0;
            pthread_cond_signal(&cond);
            pthread_mutex_unlock(&mutex);
            return;
        }
    }
}

AndroidWindowedAppContext::AndroidWindowedAppContext() {
    pthread_mutex_init(&mutex, nullptr);
    pthread_cond_init(&cond, nullptr);
}

AndroidWindowedAppContext::~AndroidWindowedAppContext(){
    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&mutex);
}

namespace xe {
    namespace ui {

        class AndroidWindow final : public Window {
        public:
            AndroidWindow(ui::WindowedAppContext& app_context, const std::string_view title,
                          uint32_t desired_logical_width, uint32_t desired_logical_height)
                    : Window(app_context, title, desired_logical_width,
                             desired_logical_height) {}

        protected:
            bool OpenImpl() override {
                XELOGI("Opening Android window...");
                return true;
            }

            void RequestCloseImpl() override {
                XELOGI("Requesting Android window close...");
            }

            std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override {
                LOGD("CreateSurfaceImpl: ae::window=%p, allowed_types=%u",
                       (void*)ae::window, (uint32_t)allowed_types);
                XELOGI("Creating Android surface... ae::window={}, allowed_types={}",
                       (void*)ae::window, (uint32_t)allowed_types);
                if(allowed_types&Surface::kTypeFlag_AndroidNativeWindow) {
                    ANativeWindow *window = ae::window;
                    if(!window) {
                        LOGD("CreateSurfaceImpl: FAILED ae::window is null!");
                        XELOGE("ae::window is null, cannot create surface!");
                        return nullptr;
                    }
                    LOGD("CreateSurfaceImpl: SUCCESS creating AndroidNativeWindowSurface");
                    return std::make_unique<AndroidNativeWindowSurface>(window);
                }
                LOGD("CreateSurfaceImpl: FAILED no AndroidNativeWindow type in allowed_types");
                return nullptr;
            }

            void RequestPaintImpl() override {
                static int rpi_count = 0;
                static ax360e::perf::FrameTimeTracker g_frame_tracker(120);
                rpi_count++;
                if(rpi_count <= 5) {
                    LOGD("RequestPaintImpl called, count=%d", rpi_count);
                    XELOGI("RequestPaintImpl called, count={}", rpi_count);
                }
                g_frame_tracker.Tick();
                AndroidWindowedAppContext* context=static_cast<AndroidWindowedAppContext*>(&app_context());
                context->request_paint();
            }

        public:
            void UpdateSurface(){
                OnSurfaceChanged(true);
            }
            void DoPaint(){
                OnPaint(true);
            }
        };

        std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                               const std::string_view title,
                                               uint32_t desired_logical_width,
                                               uint32_t desired_logical_height) {
            return std::make_unique<AndroidWindow>(
                    app_context, title, desired_logical_width, desired_logical_height);
        }

        class android_menu_item final : public MenuItem {
        public:
            android_menu_item(Type type, const std::string& text, const std::string& hotkey,
                              std::function<void()> callback)
                    : MenuItem(type, text, hotkey, callback) {
                LOGW("android_menu_item: %d %s %s",static_cast<int>(type),text.c_str(),hotkey.c_str());
            }
        };

        std::unique_ptr<ui::MenuItem> MenuItem::Create(Type type,
                                                       const std::string& text,
                                                       const std::string& hotkey,
                                                       std::function<void()> callback) {
            return std::make_unique<android_menu_item>(type, text, hotkey, callback);
        }
    }

    namespace app {
        class EmulatorApp final : public ui::WindowedApp {
        public:
            static std::unique_ptr<WindowedApp> create(ui::WindowedAppContext& app_context) {
                return std::unique_ptr<WindowedApp>(new EmulatorApp(app_context));
            }

            std::unique_ptr<Emulator> emu;
            std::unique_ptr<EmulatorWindow> emu_window;
            std::unique_ptr<xe::threading::Event> emu_thr_event;

            std::atomic<bool> emu_thr_quit_requested;
            std::thread emu_thr;

            template <typename T, typename... Args>
            class Factory {
            private:
                struct Creator {
                    std::string name;
                    std::function<bool()> is_available;
                    std::function<std::unique_ptr<T>(Args...)> instantiate;
                };

                std::vector<Creator> creators_;

            public:
                void Add(const std::string_view name, std::function<bool()> is_available,
                         std::function<std::unique_ptr<T>(Args...)> instantiate) {
                    creators_.push_back({std::string(name), is_available, instantiate});
                }

                void Add(const std::string_view name,
                         std::function<std::unique_ptr<T>(Args...)> instantiate) {
                    auto always_available = []() { return true; };
                    Add(name, always_available, instantiate);
                }

                template <typename DT>
                void Add(const std::string_view name) {
                    Add(name, DT::IsAvailable, [](Args... args) {
                        return std::make_unique<DT>(std::forward<Args>(args)...);
                    });
                }

                std::unique_ptr<T> Create(const std::string_view name, Args... args) {
                    if (!name.empty() && name != "any") {
                        auto it = std::find_if(
                                creators_.cbegin(), creators_.cend(),
                                [&name](const auto& f) { return name.compare(f.name) == 0; });
                        if (it != creators_.cend() && (*it).is_available()) {
                            return (*it).instantiate(std::forward<Args>(args)...);
                        }
                        return nullptr;
                    } else {
                        for (const auto& creator : creators_) {
                            if (!creator.is_available()) continue;
                            auto instance = creator.instantiate(std::forward<Args>(args)...);
                            if (!instance) continue;
                            return instance;
                        }
                        return nullptr;
                    }
                }

                std::vector<std::unique_ptr<T>> CreateAll(const std::string_view name,
                                                          Args... args) {
                    std::vector<std::unique_ptr<T>> instances;
                    if (!name.empty() && name != "any") {
                        auto it = std::find_if(
                                creators_.cbegin(), creators_.cend(),
                                [&name](const auto& f) { return name.compare(f.name) == 0; });
                        if (it != creators_.cend() && (*it).is_available()) {
                            auto instance = (*it).instantiate(std::forward<Args>(args)...);
                            if (instance) {
                                instances.emplace_back(std::move(instance));
                            }
                        }
                    } else {
                        for (const auto& creator : creators_) {
                            if (!creator.is_available()) continue;
                            auto instance = creator.instantiate(std::forward<Args>(args)...);
                            if (instance) {
                                instances.emplace_back(std::move(instance));
                            }
                        }
                    }
                    return instances;
                }
            };

            EmulatorApp(ui::WindowedAppContext& app_context)
                    : WindowedApp(app_context,"ax36e") {
            }
            bool OnInitialize() override {

                Profiler::Initialize();
                Profiler::ThreadEnter("Main");

                // === Custom Adreno Driver Loading (libadrenotools path) ===
                // This must happen VERY EARLY, before any Vulkan code (including Xenia's
                // VulkanGraphicsSystem) tries to load libvulkan.so.
                {
                    const char* driver_dir = std::getenv("CUSTOM_DRIVER_DIR");
                    const char* driver_path = std::getenv("CUSTOM_DRIVER_PATH");

                    if (driver_dir && driver_dir[0] != '\0') {
                        // Prefer directory + infer common Turnip name if needed
                        std::string dir(driver_dir);
                        std::string name = "libvulkan_freedreno.so"; // common name

                        // If we have a full path, extract the filename
                        if (driver_path && driver_path[0] != '\0') {
                            size_t last_slash = std::string(driver_path).find_last_of("/\\");
                            if (last_slash != std::string::npos) {
                                name = std::string(driver_path).substr(last_slash + 1);
                            }
                        }

                        LOGD("Attempting early custom driver load via adreno_driver: dir=%s, name=%s", 
                             dir.c_str(), name.c_str());

                        bool loaded = load_custom_adreno_driver(dir, name, true);
                        if (loaded) {
                            LOGD("Custom Adreno driver loaded successfully via new loader (pre-Vulkan)");
                        } else {
                            LOGW("Custom driver load failed or fell back to legacy");
                        }
                    }
                }

                std::filesystem::path storage_root=cvars::storage_root;

                storage_root = std::filesystem::absolute(storage_root);
                XELOGI("Storage root: {}", storage_root.c_str());

                config::SetupConfig(storage_root);

                std::filesystem::path content_root = cvars::content_root;
                if (content_root.empty()) {
                    content_root = storage_root / "content";
                } else {
                    // If content root isn't an absolute path, then it should be relative to the
                    // storage root.
                    if (!content_root.is_absolute()) {
                        content_root = storage_root / content_root;
                    }
                }
                content_root = std::filesystem::absolute(content_root);
                XELOGI("Content root: {}", content_root.c_str());

                std::filesystem::path cache_root = cvars::cache_root;
                if (cache_root.empty()) {
                    cache_root = storage_root / "cache";
                    // TODO(Triang3l): Point to the app's external storage "cache" directory on
                    // Android.
                } else {
                    // If content root isn't an absolute path, then it should be relative to the
                    // storage root.
                    if (!cache_root.is_absolute()) {
                        cache_root = storage_root / cache_root;
                    }
                }
                cache_root = std::filesystem::absolute(cache_root);
                XELOGI("Host cache root: {}", cache_root.c_str());

                // Create the emulator but don't initialize so we can setup the window.
                emu =
                        std::make_unique<Emulator>("", storage_root, content_root, cache_root);

                // Determine window size based on user setting.
                //auto res = xe::gpu::GraphicsSystem::GetInternalDisplayResolution();

                // Main emulator display window.
                emu_window = EmulatorWindow::Create(emu.get(), app_context()
                );

                if (!emu_window) {
                    XELOGE("Failed to create the main emulator window");
                    return false;
                }

                emu_thr_event = xe::threading::Event::CreateAutoResetEvent(false);
                assert_not_null(emu_thr_event);
                emu_thr_quit_requested.store(false, std::memory_order_relaxed);
                emu_thr = std::thread(&EmulatorApp::emu_thr_main, this);

                return true;
            }

            static std::unique_ptr<apu::AudioSystem> create_audio_system(
                    cpu::Processor* processor) {
                Factory<apu::AudioSystem, cpu::Processor*> factory;
                factory.Add<apu::nop::NopAudioSystem>("nop");
#if XE_PLATFORM_AX360E
                factory.Add<apu::opensles::OpenSLESAudioSystem>("opensles");
                factory.Add<apu::aaudio::AAudioAudioSystem>("aaudio");
#endif
                return factory.Create(cvars::apu, processor);
            }

            static std::unique_ptr<gpu::GraphicsSystem> create_graphics_system() {
                Factory<gpu::GraphicsSystem> factory;
                factory.Add<gpu::vulkan::VulkanGraphicsSystem>("vulkan");
                factory.Add<gpu::null::NullGraphicsSystem>("null");
                return factory.Create(cvars::gpu);
            }


            static std::vector<std::unique_ptr<hid::InputDriver>> create_input_drivers(
                    ui::Window* window) {
                std::vector<std::unique_ptr<hid::InputDriver>> drivers;
                Factory<hid::InputDriver, ui::Window *, size_t> factory;
                factory.Add("android", xe::hid::android::Create);
                factory.Add("nop", xe::hid::nop::Create);

                for (auto &driver: factory.CreateAll(cvars::hid, window,
                                                     EmulatorWindow::kZOrderHidInput)) {
                    if (driver && XSUCCEEDED(driver->Setup())) {
                        drivers.emplace_back(std::move(driver));
                    }
                }
                if (drivers.empty()) {
                    // Fallback to nop if none created.
                    drivers.emplace_back(
                            xe::hid::nop::Create(window, EmulatorWindow::kZOrderHidInput));
                }
                return drivers;
            }

            void emu_thr_main() {
                assert_not_null(emu_thr_event);

                JNIEnv *env;
                g_jvm->AttachCurrentThread(&env, nullptr);

                xe::threading::set_name("Emulator");
                Profiler::ThreadEnter("Emulator");

                LOGD("emu_thr_main: starting Setup...");
                // Setup and initialize all subsystems. If we can't do something
                // (unsupported system, memory issues, etc) this will fail early.
                X_STATUS result = emu->Setup(
                        emu_window->window(), emu_window->imgui_drawer(), true,
                        create_audio_system, create_graphics_system, create_input_drivers);
                if (XFAILED(result)) {
                    LOGD("emu_thr_main: Setup FAILED result=0x%08X", (unsigned)result);
                    XELOGE("Failed to setup emulator: {:08X}", result);
                    app_context().RequestDeferredQuit();
                    return;
                }
                LOGD("emu_thr_main: Setup succeeded, calling SetupGraphicsSystemPresenterPainting");
                XELOGI("emu_thr_main: Setup succeeded, calling SetupGraphicsSystemPresenterPainting on UI thread...");
                app_context().CallInUIThread(
                        [this]() { emu_window->SetupGraphicsSystemPresenterPainting(); });
                LOGD("emu_thr_main: SetupGraphicsSystemPresenterPainting dispatched");
                XELOGI("emu_thr_main: SetupGraphicsSystemPresenterPainting returned");

                if (cvars::mount_scratch) {
                    auto scratch_device = std::make_unique<xe::vfs::HostPathDevice>(
                            "\\SCRATCH", cvars::storage_root/"scratch", false);
                    if (!scratch_device->Initialize()) {
                        XELOGE("Unable to scan scratch path");
                    } else {
                        if (!emu->file_system()->RegisterDevice(
                                std::move(scratch_device))) {
                            XELOGE("Unable to register scratch path");
                        } else {
                            emu->file_system()->RegisterSymbolicLink("scratch:", "\\SCRATCH");
                        }
                    }
                }

                if (cvars::mount_cache) {

                    auto cache0_device =
                            std::make_unique<xe::vfs::HostPathDevice>("\\CACHE0", cvars::storage_root/"cache0", false);
                    if (!cache0_device->Initialize()) {
                        XELOGE("Unable to scan cache0 path");
                    } else {
                        if (!emu->file_system()->RegisterDevice(std::move(cache0_device))) {
                            XELOGE("Unable to register cache0 path");
                        } else {
                            emu->file_system()->RegisterSymbolicLink("cache0:", "\\CACHE0");
                        }
                    }

                    auto cache1_device =
                            std::make_unique<xe::vfs::HostPathDevice>("\\CACHE1", cvars::storage_root/"cache1", false);
                    if (!cache1_device->Initialize()) {
                        XELOGE("Unable to scan cache1 path");
                    } else {
                        if (!emu->file_system()->RegisterDevice(std::move(cache1_device))) {
                            XELOGE("Unable to register cache1 path");
                        } else {
                            emu->file_system()->RegisterSymbolicLink("cache1:", "\\CACHE1");
                        }
                    }

                    // Some (older?) games try accessing cache:\ too
                    // NOTE: this must be registered _after_ the cache0/cache1 devices, due to
                    // substring/start_with logic inside VirtualFileSystem::ResolvePath, else
                    // accesses to those devices will go here instead
                    auto cache_device =
                            std::make_unique<xe::vfs::HostPathDevice>("\\CACHE", cvars::storage_root/"cache", false);
                    if (!cache_device->Initialize()) {
                        XELOGE("Unable to scan cache path");
                    } else {
                        if (!emu->file_system()->RegisterDevice(std::move(cache_device))) {
                            XELOGE("Unable to register cache path");
                        } else {
                            emu->file_system()->RegisterSymbolicLink("cache:", "\\CACHE");
                        }
                    }
                }
#if 0
                emu->on_launch.AddListener([&](auto title_id, const auto& game_title) {
                    XELOGI("on_launch {}",
                           game_title.empty() ? "Unknown Title" : std::string(game_title));
                    app_context().CallInUIThread([this]() { emu_window->UpdateTitle(); });
                    emu_thr_event->Set();
                });
#else
                emu->on_launch.AddListener([&](auto title_id, const auto& game_title) {
                    /*nlohmann::json json;
                    if(std::filesystem::exists(g_uri_info_list_file_path)){
                        std::ifstream json_file(g_uri_info_list_file_path);
                        json = nlohmann::json::parse(json_file);
                        json_file.close();
                    }
                    if(!game_title.empty()){
                        nlohmann::json info;
                        info["name"] = game_title;

                        json[cvars::target.string()]=info;
                    }
                    std::ofstream json_file(g_uri_info_list_file_path);
                    json_file << json;
                    json_file.close();

                    emu_thr_event->Set();*/
                });
#endif
                emu->on_shader_storage_initialization.AddListener(
                        [this](bool initializing) {
                            XELOGI("Shader storage initialization: {}", initializing);
                            app_context().CallInUIThread([this, initializing]() {
                                emu_window->SetInitializingShaderStorage(initializing);

                            });

                        });

                /*emu->on_patch_apply.AddListener([this]() {
                    app_context().CallInUIThread([this]() { emu_window->UpdateTitle(); });
                });*/

                emu->on_terminate.AddListener([]() {
                    XELOGI("Emulator terminated");
                });

                // Enable emulator input now that the emulator is properly loaded.
                app_context().CallInUIThread(
                        [this]() { emu_window->OnEmulatorInitialized(); });

                // Grab path from the flag or unnamed argument.
                std::string path;
                if (!cvars::target.empty()) {
                    path = cvars::target;
                }

                if (!path.empty()) {
                    LOGD("emu_thr_main: launching game path=%s", path.c_str());
                    XELOGI("emu_thr_main: launching game path={}", path);
                    jclass uri_class = env->FindClass("android/net/Uri");
                    jmethodID parse_method = env->GetStaticMethodID(uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
                    jstring uri_string = env->NewStringUTF(path.c_str());
                    jobject uri = env->CallStaticObjectMethod(uri_class, parse_method, uri_string);

                    std::unique_ptr<DocumentFile> file =
                            DocumentFile::find(g_jvm,uri);

                    std::string name = file->getName();
                    if(name.ends_with(".xex")){
                        result = emu->LaunchXexFile(std::move(file));
                    }
                    else if(name.ends_with(".iso")){
                        result = emu->LaunchDiscImage(std::move(file));
                    }
                    else if(name.ends_with(".zar")){
                        result = emu->LaunchDiscArchive(std::move(file));
                    }
                    else{
                        std::string data_dir = path+".data";
                        jstring data_dir_str = env->NewStringUTF(data_dir.c_str());
                        jobject data_dir_uri = env->CallStaticObjectMethod(uri_class, parse_method, data_dir_str);

                        std::unique_ptr<DocumentFile> data_dir_file =
                                DocumentFile::find(g_jvm,data_dir_uri);

                        result = emu->LaunchStfsContainer(std::move(file), std::move(data_dir_file));
                    }

                    /*result = emu->LaunchPath(abs_path);*//*app_context().CallInUIThread(
                            [this, abs_path]() { return emu_window->RunTitle(abs_path); });*/
                    if (XFAILED(result)) {
                        LOGD("emu_thr_main: game launch FAILED result=0x%08X", (unsigned)result);
                        xe::FatalError(fmt::format("Failed to launch target: {:08X}", result));
                        app_context().RequestDeferredQuit();
                        return;
                    }
                    LOGD("emu_thr_main: game launched OK result=0x%08X", (unsigned)result);
                    XELOGI("emu_thr_main: game launched successfully, result={:08X}", result);
                }

                /*auto xam = emu->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
                        "xam.xex");

                if (xam) {
                    xam->LoadLoaderData();

                    if (xam->loader_data().launch_data_present) {
                        const std::filesystem::path host_path = xam->loader_data().host_path;
                        app_context().CallInUIThread([this, host_path]() {
                            return emu_window->RunTitle(host_path);
                        });
                    }
                }*/

                // Now, we're going to use this thread to drive events related to emulation.
                /*while (!emu_thr_quit_requested.load(std::memory_order_relaxed)) {
                    xe::threading::Wait(emu_thr_event.get(), false);
                    emu->WaitUntilExit();
                }*/
                while (!emu_thr_quit_requested.load(std::memory_order_relaxed)) {
                    xe::threading::Wait(emu_thr_event.get(), false);
                    while (true) {
                        emu->WaitUntilExit();
                        if (emu->TitleRequested()) {
                            emu->LaunchNextTitle();
                        } else {
                            break;
                        }
                    }
                }

                XELOGI("QUIT");
                app_context().QuitFromUIThread();
            }
        };
    }
}

XE_DEFINE_WINDOWED_APP(ax36e,xe::app::EmulatorApp::create);

void paint(){
    auto  e=dynamic_cast<xe::app::EmulatorApp*>(ae::g_windowed_app.get());
    if(!e || !e->emu_window || !e->emu_window->window()) {
        static int paint_null = 0;
        if(paint_null++ < 5) {
            LOGD("paint(): null check failed e=%p emu_window=%d window=%d",
                 (void*)e,
                 e ? (e->emu_window ? 1 : 0) : -1,
                 (e && e->emu_window) ? (e->emu_window->window() ? 1 : 0) : -1);
        }
        return;
    }
    static int paint_count = 0;
    paint_count++;
    if(paint_count <= 10) {
        LOGD("paint() called, count=%d", paint_count);
        XELOGI("paint() called, count={}", paint_count);
    }
    static_cast<xe::ui::AndroidWindow*>(e->emu_window->window())->DoPaint();
}
namespace ae{

    int boot_type;

    std::string boot_game_path;
    int boot_game_fd;
    std::string boot_game_uri;

    ANativeWindow* window;
    int window_width;
    int window_height;

    std::string game_id;

     std::unique_ptr<xe::ui::WindowedApp> g_windowed_app;
     xe::app::EmulatorApp* g_windowed_app_ref;

    // n->[n]
    static std::array<xe::ui::VirtualKey,24> key_maps={
            xe::ui::VirtualKey::kXInputPadDpadLeft,
            xe::ui::VirtualKey::kXInputPadDpadUp,
            xe::ui::VirtualKey::kXInputPadDpadRight,
            xe::ui::VirtualKey::kXInputPadDpadDown,
            xe::ui::VirtualKey::kXInputPadA,
            xe::ui::VirtualKey::kXInputPadB,
            xe::ui::VirtualKey::kXInputPadX,
            xe::ui::VirtualKey::kXInputPadY,
            xe::ui::VirtualKey::kXInputPadBack,
            xe::ui::VirtualKey::kXInputPadStart,

            xe::ui::VirtualKey::kXInputPadLShoulder,
            xe::ui::VirtualKey::kXInputPadRShoulder,
            xe::ui::VirtualKey::kXInputPadLThumbPress,
            xe::ui::VirtualKey::kXInputPadRThumbPress,
            xe::ui::VirtualKey::kXInputPadLTrigger,
            xe::ui::VirtualKey::kXInputPadRTrigger,

            xe::ui::VirtualKey::kXInputPadLThumbLeft,
            xe::ui::VirtualKey::kXInputPadLThumbUp,
            xe::ui::VirtualKey::kXInputPadLThumbRight,
            xe::ui::VirtualKey::kXInputPadLThumbDown,
            xe::ui::VirtualKey::kXInputPadRThumbLeft,
            xe::ui::VirtualKey::kXInputPadRThumbUp,
            xe::ui::VirtualKey::kXInputPadRThumbRight,
            xe::ui::VirtualKey::kXInputPadRThumbDown
    };

    void main_thr(){

        std::string tid=[]{
            std::stringstream ss;
            ss<<std::this_thread::get_id();
            return ss.str();
        }();
        LOGW("new thr: %s",tid.c_str());

        prctl(PR_SET_TIMERSLACK,1,0,0,0);

        // Apply Turnip optimizations based on detected Adreno GPU model
        {
            LOGD("main_thr: Checking GPU model for Turnip optimizations...");
            std::string gpu_name;
            std::ifstream f("/sys/class/kgsl/kgsl-3d0/gpu_model");
            if (f.is_open()) {
                std::getline(f, gpu_name);
                LOGD("main_thr: Found GPU model: %s", gpu_name.c_str());
            } else {
                LOGD("main_thr: Could not open kgsl gpu_model (errno: %d)", errno);
                // Fallback for some kernels
                std::ifstream f2("/sys/class/kgsl/kgsl-3d0/devname");
                if (f2.is_open()) {
                    std::getline(f2, gpu_name);
                    LOGD("main_thr: Found GPU devname: %s", gpu_name.c_str());
                } else {
                    LOGD("main_thr: Could not open kgsl devname (errno: %d)", errno);
                }
            }
            if (!gpu_name.empty()) {
                turnip::ApplyRecommendedConfig(gpu_name);
            } else {
                LOGD("main_thr: No GPU model detected, applying safe Turnip defaults");
                turnip::SetUbwcFlagHint(true);
            }
        }

        LOGD("main_thr: creating AndroidWindowedAppContext");
        AndroidWindowedAppContext wnd_ctx;
        wnd_ctx.setup_ui_thr_id(std::this_thread::get_id());
        LOGD("main_thr: creating windowed app");
        g_windowed_app=xe::ui::GetWindowedAppCreator()(wnd_ctx);
        g_windowed_app_ref=dynamic_cast<xe::app::EmulatorApp*>(g_windowed_app.get());
        LOGD("main_thr: windowed app created, app_ref=%p", (void*)g_windowed_app_ref);

        std::vector<char*> args;
        args.push_back(NULL);
        for(auto& i:g_launch_args){
            args.push_back((char*)i.c_str());
        }
        static std::string boot_target=std::string("--target=")+ae::boot_game_uri;
        args.push_back((char*)boot_target.c_str());
        LOGD("main_thr: boot_target=%s", boot_target.c_str());

        int argc=args.size();
        char** argv=args.data();

        cvar::ParseLaunchArguments(argc, argv, g_windowed_app->GetPositionalOptionsUsage(),
                                   g_windowed_app->GetPositionalOptions());
        LOGD("main_thr: args parsed, initializing logging");
        xe::InitializeLogging(g_windowed_app->GetName());
        LOGD("main_thr: logging initialized, calling OnInitialize");
        if(g_windowed_app->OnInitialize()){
            LOGD("main_thr: OnInitialize succeeded, entering main_loop");
            wnd_ctx.main_loop();
        } else {
            LOGD("main_thr: OnInitialize FAILED");
        }
        LOGD("main_thr: exiting");
    }

    void key_event(int key_code,bool pressed,int value){
        if(!g_windowed_app_ref||!g_windowed_app_ref->emu)
            return;
        auto* input_system=g_windowed_app_ref->emu->input_system();
        if(!input_system)
            return;
        for(auto& driver_base:input_system->drivers_){
            auto* driver=dynamic_cast<xe::hid::android::AndroidInputDriver*>(driver_base.get());
            if(driver){
                driver->OnKey(key_code,pressed,value);
                return;
            }
        }
    }
    bool is_running(){
        return !g_windowed_app_ref->emu->is_paused();
    }
    bool is_paused(){
        return g_windowed_app_ref->emu->is_paused();
    }
    void pause(){
        //g_windowed_app_ref->emu->Pause();
        /*g_windowed_app_ref->app_context().CallInUIThread([]{
            g_windowed_app_ref->emu->Pause();
        });*/
    }
    void resume(){
        //g_windowed_app_ref->emu->Resume();
        /*g_windowed_app_ref->app_context().CallInUIThread([]{
            g_windowed_app_ref->emu->Resume();
        });*/
    }
    void quit(){
    }

    void notify_surface_changed(){
        if(!g_windowed_app_ref || !g_windowed_app_ref->emu_window ||
           !g_windowed_app_ref->emu_window->window())
            return;
        static_cast<xe::ui::AndroidWindow*>(
            g_windowed_app_ref->emu_window->window())->UpdateSurface();
    }

    void init(){
    }

}
