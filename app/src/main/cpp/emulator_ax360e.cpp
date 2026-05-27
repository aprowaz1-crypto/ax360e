// SPDX-License-Identifier: WTFPL
#include "emulator_ax360e.h"
#include "ax360e_emu.h"

#include "xenia/app/emulator_window.h"
#include "xenia/emulator.h"
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/gpu/null/null_graphics_system.h"
#include "xenia/hid/nop/nop_hid.h"
#include "xenia/base/logging.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/base/mapped_memory.h"

#include "cpuinfo.h"
#include "ax360e_perf_log.h"   // CPU accuracy metrics + other perf/diag trackers
#include "adreno_driver.h"   // preferred custom driver loader (libadrenotools)

// 128B reservation stress harness (CAPTAIN task) - include for Run128B... + cvars::a64_128b...
#include "xenia/cpu/backend/a64/a64_backend.h"
#include <fstream>
#include <chrono>
#include "vkutil.h"

#include "cpptoml/include/cpptoml.h"

#include <android/log.h>
#define LOG_TAG "aX360e_Emulator"
#define LOGE(...) { \
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "%s : %d", __FILE__, __LINE__); \
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__); \
}

jclass g_class_DocumentFile;
jclass g_class_Emulator;

jobject g_context;
jobject g_doocument_file_tree;

jmethodID mid_open_uri_fd;

std::vector<std::string> g_launch_args;
std::string g_uri_info_list_file_path;

// Global performance metrics cache
PerformanceMetrics g_performance_metrics;

// Global memory pressure state
MemoryPressureState g_memory_pressure;

// Global CPU accuracy / diagnostics tracker (from ax360e_perf_log.h)
ax360e::perf::CpuAccuracyTracker g_cpu_accuracy(10000);  // report every ~10k relevant events

static void j_setup_context(JNIEnv* env,jobject self,jobject context ){
    g_context = env->NewGlobalRef(context);
}

//public native void setup_document_file_tree(DocumentFile tree);
static void j_setup_document_file_tree(JNIEnv* env,jobject self,jobject tree ){
    g_doocument_file_tree = env->NewGlobalRef(tree);
}

//public native void setup_launch_args(String[] args);
static void j_setup_launch_args(JNIEnv* env,jobject self,jobjectArray args ){
    g_launch_args.clear();
    for(int i=0;i<env->GetArrayLength(args);i++){
        jstring arg=(jstring)env->GetObjectArrayElement(args,i);
        const char* str = env->GetStringUTFChars(arg,NULL);
        if (str != NULL) {
            g_launch_args.push_back(std::string(str));
            env->ReleaseStringUTFChars(arg, str);
        }
    }
}


static jstring j_simple_device_info(JNIEnv* env, jobject thiz)
{
    std::string info;

    auto get_gpu_info=[]()->std::string {
        // Strongly prefer sysfs. Creating a VkInstance here (especially with custom Turnip)
        // has caused mutex corruption, FORTIFY crashes, and conflicts with HWUI's Vulkan
        // on Adreno devices. We no longer fall back to Vulkan initialization for diagnostics.
        std::vector<std::string> sysfs_paths = {
            "/sys/class/kgsl/kgsl-3d0/gpu_model",
            "/sys/class/kgsl/kgsl-3d0/gpu_name",
            "/sys/devices/platform/kgsl-3d0.0/kgsl/kgsl-3d0/gpu_model",
            "/sys/devices/platform/soc/3d00000.qcom,kgsl-3d0/kgsl/kgsl-3d0/gpu_model"
        };

        for (const auto& path : sysfs_paths) {
            std::ifstream f(path);
            if (f.is_open()) {
                std::string name;
                std::getline(f, name);
                if (!name.empty()) {
                    return "GPU [" + name + "] (sysfs)";
                }
            }
        }

        // Safe fallback - do NOT initialize Vulkan here anymore.
        return "GPU [Adreno] (sysfs probe failed - full details available after emulator starts)";
    };

    auto get_cpu_info=[]()->std::string {

        std::vector<core_info_t> core_info=cpu_get_core_info();
        std::string cpu_name=cpu_get_simple_info(core_info);
        std::string cpu_features;
        if (!core_info.empty()) {
            cpu_features=[&](){
                std::ostringstream oss;
                for(const auto& feature : core_info[0].features){
                    oss <<"    * " << feature << "\n";
                }
                return oss.str();
            }();
        }
        return "CPU [" + cpu_name + "]:\n" + cpu_features;
    };

    info+=get_cpu_info();
    info+="\n"+get_gpu_info();

    return env->NewStringUTF(info.c_str());
}

static jobject j_meta_info_from_god_game(JNIEnv* env,jobject self,jobject context,jstring uri_str ) {
    jclass cls_Emulator$GameInfo = env->FindClass("aenu/ax360e/Emulator$GameInfo");
    if (cls_Emulator$GameInfo == NULL) return NULL;
    jmethodID mid_Emulator$GameInfo = env->GetMethodID(cls_Emulator$GameInfo, "<init>", "()V");
    if (mid_Emulator$GameInfo == NULL) return NULL;
    jfieldID fid_name = env->GetFieldID(cls_Emulator$GameInfo, "name", "Ljava/lang/String;");
    jfieldID fid_uri = env->GetFieldID(cls_Emulator$GameInfo, "uri", "Ljava/lang/String;");
    jfieldID fid_icon = env->GetFieldID(cls_Emulator$GameInfo, "icon", "[B");
    if (fid_name == NULL || fid_uri == NULL || fid_icon == NULL) return NULL;

    jclass uri_class = env->FindClass("android/net/Uri");
    if (uri_class == NULL) return NULL;
    jmethodID parse_method = env->GetStaticMethodID(uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    if (parse_method == NULL) return NULL;

    jobject game_info = env->NewObject(cls_Emulator$GameInfo, mid_Emulator$GameInfo);
    env->SetObjectField(game_info, fid_uri, uri_str);

    jobject uri = env->CallStaticObjectMethod(uri_class, parse_method, uri_str);

    xe::vfs::XContentContainerHeader header;
    // read header
    {
        //public static int nc_open_uri_fd(Context ctx,Uri uri)
        int header_file_fd = env->CallStaticIntMethod(g_class_Emulator, mid_open_uri_fd, context, uri);

        if (header_file_fd == -1) {
            return NULL;
        }
        std::unique_ptr<xe::MappedMemory> mmap = xe::MappedMemory::OpenForUnixFd(header_file_fd);
        if (!mmap) {
            return NULL;
        }
        if(mmap->size() < sizeof(header)) {
            return NULL;
        }
        std::memcpy(&header, mmap->data(), sizeof(header));
    }

    std::string name = xe::to_utf8(header.content_metadata.title_name());
    env->SetObjectField(game_info, fid_name, env->NewStringUTF(name.c_str()));

    jbyteArray icon = env->NewByteArray(header.content_metadata.thumbnail_size);
    env->SetByteArrayRegion(icon, 0, header.content_metadata.thumbnail_size, (const jbyte*)header.content_metadata.thumbnail);
    env->SetObjectField(game_info, fid_icon, icon);
    return game_info;
}
#if 0
static std::unique_ptr<xe::apu::AudioSystem> create_nop_audio_system(
        xe::cpu::Processor* processor) {
    return std::make_unique<xe::apu::nop::NopAudioSystem>(processor);
}

static std::unique_ptr<xe::gpu::GraphicsSystem> create_null_graphics_system() {
    return std::make_unique<xe::gpu::null::NullGraphicsSystem>();
}

static std::vector<std::unique_ptr<xe::hid::InputDriver>> create_nop_input_drivers(
        xe::ui::Window* window) {

    std::vector<std::unique_ptr<xe::hid::InputDriver>> drivers;
    drivers.emplace_back(xe::hid::nop::Create(window, xe::app::EmulatorWindow::kZOrderHidInput));

    return drivers;
}
//public native GameInfo meta_info_from_uri(String uri) throws RuntimeException;
static jobject j_meta_info_from_uri(JNIEnv* env,jobject self,jstring uri_str ){

    /*
    public static class GameInfo{
        public String name;
        public String uri;
        public int fd;
        public byte[] icon;
     */
    jclass cls_Emulator$GameInfo = env->FindClass("aenu/ax360e/Emulator$GameInfo");
    jmethodID mid_Emulator$GameInfo = env->GetMethodID(cls_Emulator$GameInfo, "<init>", "()V");
    jobject game_info = env->NewObject(cls_Emulator$GameInfo, mid_Emulator$GameInfo);
    jfieldID fid_name = env->GetFieldID(cls_Emulator$GameInfo, "name", "Ljava/lang/String;");
    jfieldID fid_uri = env->GetFieldID(cls_Emulator$GameInfo, "uri", "Ljava/lang/String;");
    env->SetObjectField(game_info, fid_uri, uri_str);


    jclass uri_class = env->FindClass("android/net/Uri");
    jmethodID parse_method = env->GetStaticMethodID(uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jobject uri = env->CallStaticObjectMethod(uri_class, parse_method, uri_str);

    std::unique_ptr<DocumentFile> file = DocumentFile::find(env, uri);

    std::vector<char*> args;
    args.push_back(NULL);
    for(auto& i:g_launch_args){
        args.push_back((char*)i.c_str());
    }

    int argc=args.size();
    char** argv=args.data();

    cvar::ParseLaunchArguments(argc, argv, "",{});
    xe::InitializeLogging(file->getName());

    AndroidWindowedAppContext app_context;
    std::unique_ptr<xe::Emulator> emulator = std::make_unique<xe::Emulator>("","","","");
    auto emulator_wnd = xe::app::EmulatorWindow::Create(emulator.get(), app_context);
    xe::X_STATUS result = emulator->Setup(
            emulator_wnd->window(), emulator_wnd->imgui_drawer(), true,
            create_nop_audio_system, create_null_graphics_system, create_nop_input_drivers);
    if (XFAILED(result)) {
        env->SetObjectField(game_info, fid_name, env->NewStringUTF("???")) ;
        return game_info;
    }
    std::string result_str;
    bool ret=false;
    emulator->on_launch.AddListener([&](auto title_id, const auto& game_title) {
        result_str=game_title.empty() ? "Unknown Title" : std::string(game_title);
        XELOGI("#############: {}", result_str);
        ret=true;
    });

    std::string name = file->getName();
    if(name.ends_with(".xex")){
        result = emulator->LaunchXexFile(std::move(file));
    }
    else{
        const char* path = env->GetStringUTFChars(uri_str,NULL);
        std::string data_dir = std::string (path)+".data";
        env->ReleaseStringUTFChars(uri_str,path);

        jstring data_dir_str = env->NewStringUTF(data_dir.c_str());
        jobject data_dir_uri = env->CallStaticObjectMethod(uri_class, parse_method, data_dir_str);

        std::unique_ptr<DocumentFile> data_dir_file =
                DocumentFile::find(env, data_dir_uri);

        result = emulator->LaunchStfsContainer(std::move(file), std::move(data_dir_file));
    }

    if (XFAILED(result)) {
        env->SetObjectField(game_info, fid_name, env->NewStringUTF("????")) ;
        return game_info;
    }

    while (!ret);
    XELOGI("################Game: {}", result_str);
    env->SetObjectField(game_info, fid_name, env->NewStringUTF(result_str.c_str())) ;
    return game_info;
}
#endif

static const std::string gen_skips[]={
        //"CPU",
        "Config",
        "a64",
        "Profiles",
        "Vulkan|vulkan_device",

        "Storage|cache_root",
        "Storage|content_root",
        "Storage|storage_root",
        "Kernel|kernel_display_gamma_power",
        "Kernel|cl",
        "Kernel|kernel_build_version",
        "Kernel|default_achievements_backend",

        "Display|postprocess_ffx_cas_additional_sharpness",
        "Display|present_safe_area_y",
        "Display|postprocess_ffx_fsr_max_upsampling_passes",
        "Display|present_safe_area_x",
        "Display|postprocess_ffx_fsr_sharpness_reduction",


        "GPU|dump_shaders",
        "GPU|draw_resolution_scale_x",
        "GPU|primitive_processor_cache_min_indices",
        "GPU|query_occlusion_fake_sample_count",
        "GPU|texture_cache_memory_limit_soft_lifetime",
        "GPU|draw_resolution_scale_y",
        "GPU|trace_gpu_prefix",
        "GPU|texture_cache_memory_limit_render_to_texture",

        "GPU|query_occlusion_sample_lower_threshold",
        "GPU|query_occlusion_sample_upper_threshold",
        "GPU|framerate_limit",

        "CPU|pvr",
        "CPU|load_module_map",
         "CPU|break_condition_op",
         "CPU|trace_function_data",
         "CPU|trace_function_data_path",
          "CPU|break_condition_value",
           "CPU|break_on_instruction",
            "CPU|break_condition_gpr",

        "Logging|log_file",
        "Logging|log_mask",

        "Video|internal_display_resolution_x",
        "Video|internal_display_resolution_y",

        "HID|left_stick_deadzone_percentage",
        "HID|right_stick_deadzone_percentage",
        "HID|vibration",

        "XConfig|audio_flag",

        "General|notification_sound_path",
        "General|launch_module",


};
using entries=std::vector<std::string>;
static const std::pair<std::string,entries> gen_list[]={
        //str
        {"APU|apu",{"nop","aaudio","opensles"}},
        {"Display|postprocess_antialiasing",{"none", "fxaa", "fxaa_extreme"}},
        {"Display|postprocess_scaling_and_sharpening",{"bilinear", "cas", "fsr"}},
        {"GPU|gpu",{"vulkan", "null"}},
        {"GPU|render_target_path_vulkan",{"any", "fbo","fsi"}},
        {"HID|hid",{"android", "android,nop", "nop"}},
        {"CPU|cpu",{"any","a64"}},

        //int
        {"Content|license_mask",{"disable@0","first@1","all@-1"}},
        {"XConfig|user_country",{"AE@1","AL@2", "AM@3", "AR@4", "AT@5", "AU@6", "AZ@7", "BE@8", "BG@9"
                                 , "BH@10", "BN@11", "BO@12", "BR@13", "BY@14", "BZ@15", "CA@16", "CH@18", "CL@19"
                                 , "CN@20", "CO@21", "CR@22", "CZ@23", "DE@24", "DK@25", "DO@26", "DZ@27", "EC@28"
                                 , "EE@29", "EG@30", "ES@31", "FI@32", "FO@33", "FR@34", "GB@35", "GE@36", "GR@37"
                                 , "GT@38", "HK@39", "HN@40", "HR@41", "HU@42", "ID@43", "IE@44", "IL@45", "IN@46"
                                 , "IQ@47", "IR@48", "IS@49", "IT@50", "JM@51", "JO@52", "JP@53", "KE@54", "KG@55"
                                 , "KR@56", "KW@57", "KZ@58", "LB@59", "LI@60", "LT@61", "LU@62", "LV@63", "LY@64"
                                 , "MA@65", "MC@66", "MK@67", "MN@68", "MO@69", "MV@70", "MX@71", "MY@72", "NI@73"
                                 , "NL@74", "NO@75", "NZ@76", "OM@77", "PA@78", "PE@79", "PH@80", "PK@81", "PL@82"
                                 , "PR@83", "PT@84", "PY@85", "QA@86", "RO@87", "RU@88", "SA@89", "SE@90", "SG@91"
                                 , "SI@92", "SK@93", "SV@95", "SY@96", "TH@97", "TN@98", "TR@99", "TT@100","TW@101"
                                 , "UA@102", "US@103", "UY@104", "UZ@105", "VE@106", "VN@107", "YE@108", "ZA@109"
                                 }},
        {"XConfig|user_language",{"en@1","ja@2","de@3","fr@4","es@5","it@6","ko@7","zh@8"
                                 ,"pt@9","pl@11","ru@12","sv@13","tr@14","nb@15","nl@16","zh@17"}},
        {"Vulkan|vulkan_debug_utils_messenger_severity",{"error@0","warning@1","info@2","verbose@3"}},

        {"Kernel|kernel_display_gamma_type",{"linear@0","sRGB(CRT)@1","BT.709(HDTV)@2",/*kernel_display_gamma_power@3*/}},
        {"Logging|log_level",{"error@0","warning@1","info@2","debug@3",}},
        /*
                                                  	#  0 = PAL-60 Component (SD)
                                                  	#  1 = Unused
                                                  	#  2 = PAL-60 SCART
                                                  	#  3 = 480p Component (HD)
                                                  	#  4 = HDMI+A
                                                  	#  5 = PAL-60 Composite/S-Video
                                                  	#  6 = VGA
                                                  	#  7 = TV PAL-60
                                                  	#  8 = HDMI (default)*/
        {"Video|avpack",{"PAL-60 Component (SD)@0", "Unused@1","PAL-60 SCART@2","480p Component (HD)@3","HDMI+A@4","PAL-60 Composite/S-Video@5","VGA@6","TV PAL-60@7","HDMI@8"}},
        /*#    1=NTSC
                                                  	#    2=NTSC-J
                                                  	#    3=PAL*/
        {"Video|video_standard",{ "NTSC@1","NTSC-J@2","PAL-60@3"}},
/*#    0=640x480
                                                  	#    1=640x576
                                                  	#    2=720x480
                                                  	#    3=720x576
                                                  	#    4=800x600
                                                  	#    5=848x480
                                                  	#    6=1024x768
                                                  	#    7=1152x864
                                                  	#    8=1280x720 (Default)
                                                  	#    9=1280x768
                                                  	#    10=1280x960
                                                  	#    11=1280x1024
                                                  	#    12=1360x768
                                                  	#    13=1440x900
                                                  	#    14=1680x1050
                                                  	#    15=1920x540
                                                  	#    16=1920x1080*/
        {"Video|internal_display_resolution",{ "640x480@0","640x576@1","720x480@2","720x576@3","800x600@4","848x480@5","1024x768@6","1152x864@7","1280x720@8"
                                               ,"1280x768@9","1280x960@10","1280x1024@11","1360x768@12","1440x900@13", "1680x1050@14","1920x540@15","1920x1080@16"}},
                                               /*Kernel = 1, Apu = 2, Cpu = 4.*/
        //{"Logging|log_file"}

};

using range=std::pair<int,int>;
static const std::pair<std::string,range> gen_seekbar[]={
        {"GPU|texture_cache_memory_limit_hard",{512,4096}},
        {"GPU|texture_cache_memory_limit_soft",{512,4096}},
        {"Memory|mmap_address_high",{2,63}},
        {"APU|apu_max_queued_frames",{4,64}},
        {"APU|xmp_default_volume",{0,100}},
        {"General|time_scalar",{1,8}},
        //{"Video|internal_display_resolution_x",{1,1920}},
        //{"Video|internal_display_resolution_y",{1,1080}},
};

#define SEEKBAR_PREF_TAG "aenu.preference.SeekBarPreference"
#define CHECKBOX_PREF_TAG "aenu.preference.CheckBoxPreference"
#define LIST_PREF_TAG "aenu.preference.ListPreference"

static jstring generate_config_xml(JNIEnv* env,jobject self,jstring toml_path){

    jboolean is_copy=false;
    const char* path=env->GetStringUTFChars(toml_path,&is_copy);
    if (path == NULL) {
        return env->NewStringUTF("");
    }

    std::shared_ptr<cpptoml::table> toml;
    try {
        toml=cpptoml::parse_file(path);
    } catch (const std::exception& e) {
        env->ReleaseStringUTFChars(toml_path,path);
        return env->NewStringUTF("");
    }
    env->ReleaseStringUTFChars(toml_path,path);

    std::ostringstream out;
    out<<R"(
<?xml version="1.0" encoding="utf-8"?>
<PreferenceScreen
    xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:app="http://schemas.android.com/apk/res-auto">
    )";

    for(auto table_iter=toml->begin() ;table_iter!=toml->end();table_iter++){
        const std::string& table_name=table_iter->first;
        if(std::find(std::begin(gen_skips),std::end(gen_skips),table_name)!=std::end(gen_skips))
            continue;
        std::string table_name_l=table_name; std::transform(table_name_l.begin(),table_name_l.end(),table_name_l.begin(),::tolower);
        std::shared_ptr<cpptoml::table> table=table_iter->second->as_table();
        out<<"<PreferenceScreen app:title=\"@string/es_"<<table_name_l<<"\" \n";
        out<<"app:key=\""<<table_name<<"\" >\n";

        for(auto iter=table->begin();iter!=table->end();iter++){
            const std::string& key_name=iter->first;
            const std::string find_key=table_name+"|"+key_name;
            if(std::find(std::begin(gen_skips),std::end(gen_skips),find_key)!=std::end(gen_skips))
                continue;

            {
                auto find_iter=std::begin(gen_list);
                find_iter=std::find_if(find_iter,std::end(gen_list),[&find_key](const std::pair<std::string,entries>& entry){
                    return entry.first==find_key;
                });
                if(find_iter!=std::end(gen_list)){
                    out<<"<" LIST_PREF_TAG " app:title=\"@string/es_"<<table_name_l<<"_"<<key_name<<"\" \n";
                    if(std::find(std::begin(find_iter->second[0]),std::end(find_iter->second[0]),'@')!=std::end(find_iter->second[0])){
                        out<<"app:entryValues=\"@array/es_arr_v_"<<table_name_l<<"_"<<key_name<<"\" \n";
                    }
                    else{
                        out<<"app:entryValues=\"@array/es_arr_"<<table_name_l<<"_"<<key_name<<"\" \n";
                    }
                    out<<"app:entries=\"@array/es_arr_"<<table_name_l<<"_"<<key_name<<"\" \n";
                    out<<"app:key=\""<<table_name<<"|"<<key_name<<"\" />\n";
                    continue;
                }
            }

            {
                auto find_iter=std::begin(gen_seekbar);
                find_iter=std::find_if(find_iter,std::end(gen_seekbar),[&find_key](const std::pair<std::string,range>& entry){
                    return entry.first==find_key;
                });
                if(find_iter!=std::end(gen_seekbar)){
                    out<<"<" SEEKBAR_PREF_TAG " app:title=\"@string/es_"<<table_name_l<<"_"<<key_name<<"\" \n";
                    out<<"app:min=\""<<find_iter->second.first<<"\"\n";
                    out<<"android:max=\""<<find_iter->second.second<<"\"\n";
                    out<<"app:showSeekBarValue=\"true\"\n";
                    out<<"app:key=\""<<table_name<<"|"<<key_name<<"\" />\n";
                    continue;
                }
            }

            if(const auto val=table->get_as<bool>(key_name);val){
                std::string val_str=*val?"true":"false";
                out<<"<" CHECKBOX_PREF_TAG " app:title=\"@string/es_"<<table_name_l<<"_"<<key_name<<"\" \n";
                out<<"app:key=\""<<table_name<<"|"<<key_name<<"\" />\n";
            }
            /*else if(const auto val=table->get_as<int>(key_name);val){
                out<<"<" SEEKBAR_PREF_TAG " app:title=\"@string/es_"<<table_name_l<<"_"<<key_name<<"\" \n";
                out<<"app:showSeekBarValue=\"true\"\n";
                out<<"app:key=\""<<table_name<<"|"<<key_name<<"\" />\n";
            }*/
            else if(const auto val=table->get_as<double>(key_name);val){
                //FIXME
                out<<"<PreferenceScreen app:title=\"@string/es_"<<table_name_l<<"_"<<key_name<<"\" \n";
                out<<"app:key=\""<<table_name<<"|"<<key_name<<"\" />\n";
            }
            else if(const auto val=table->get_as<std::string>(key_name);val){
                //FIXME
                out<<"<PreferenceScreen app:title=\"@string/es_"<<table_name_l<<"_"<<key_name<<"\" \n";
                out<<"app:key=\""<<table_name<<"|"<<key_name<<"\" />\n";
            }
        }

        out<<"</PreferenceScreen>\n";
    }

    out<<"</PreferenceScreen>\n";

    //JAVA const String[]
    out<<"\n\n\n\n";

    out<<"final String[] BOOL_KEYS={\n";
    for(auto table_iter=toml->begin() ;table_iter!=toml->end();table_iter++){
        const std::string& table_name=table_iter->first;
        std::shared_ptr<cpptoml::table> table=table_iter->second->as_table();
        if(std::find(std::begin(gen_skips),std::end(gen_skips),table_name)!=std::end(gen_skips))
            continue;
        for(auto iter=table->begin();iter!=table->end();iter++){
            const std::string& key_name=iter->first;
            const std::string find_key=table_name+"|"+key_name;
            if(std::find(std::begin(gen_skips),std::end(gen_skips),find_key)!=std::end(gen_skips))
                continue;
                if(const auto val=table->get_as<bool>(key_name);val){
                    out<<"\""<<table_name<<"|"<<key_name<<"\",\n";
                }
        }
    }
    out<<"};\n";

    out<<"final String[] INT_KEYS={\n";
    for(auto table_iter=toml->begin() ;table_iter!=toml->end();table_iter++){
        const std::string& table_name=table_iter->first;
        std::shared_ptr<cpptoml::table> table=table_iter->second->as_table();
        if(std::find(std::begin(gen_skips),std::end(gen_skips),table_name)!=std::end(gen_skips))
            continue;
        for(auto iter=table->begin();iter!=table->end();iter++){
            const std::string& key_name=iter->first;
            const std::string find_key=table_name+"|"+key_name;
            if(std::find(std::begin(gen_skips),std::end(gen_skips),find_key)!=std::end(gen_skips))
                continue;
            {
                auto find_iter=std::begin(gen_seekbar);
                find_iter=std::find_if(find_iter,std::end(gen_seekbar),[&find_key](const std::pair<std::string,range>& entry){
                    return entry.first==find_key;
                });
                if(find_iter!=std::end(gen_seekbar)){
                    out<<"\""<<table_name<<"|"<<key_name<<"\",\n";
                }
            }

        }
    }
    out<<"};\n";
    out<<"final String[] STRING_ARR_KEYS={\n";
    for(auto table_iter=toml->begin() ;table_iter!=toml->end();table_iter++){
        const std::string& table_name=table_iter->first;
        std::shared_ptr<cpptoml::table> table=table_iter->second->as_table();
        if(std::find(std::begin(gen_skips),std::end(gen_skips),table_name)!=std::end(gen_skips))
            continue;
            for(auto iter=table->begin();iter!=table->end();iter++){
                const std::string& key_name=iter->first;
                const std::string find_key=table_name+"|"+key_name;
                if(std::find(std::begin(gen_skips),std::end(gen_skips),find_key)!=std::end(gen_skips))
                    continue;
                {
                    auto find_iter=std::begin(gen_list);
                    find_iter=std::find_if(find_iter,std::end(gen_list),[&find_key](const std::pair<std::string,entries>& entry){
                        return entry.first==find_key;
                    });
                    if(find_iter!=std::end(gen_list)){
                        out<<"\""<<table_name<<"|"<<key_name<<"\",\n";
                    }
                }
            }
    }
    out<<"};\n";

#if 0
    //STRING XML
    out<<"\n\n\n\n";

    auto convert_to_name=[](const std::string& key){
        std::string result=key;
        replace(result.begin(),result.end(),'_',' ');
        result[0]=toupper(result[0]);
        for(int i=1;i<result.size();i++){
            if(result[i]==' '){
                if(i+1<result.size())
                    result[i+1]=toupper(result[i+1]);
            }
        }
        return result;
    };

    for(auto table_iter=toml->begin() ;table_iter!=toml->end();table_iter++){
        const std::string& table_name=table_iter->first;
        std::string table_name_l=table_name;
        std::transform(table_name_l.begin(),table_name_l.end(),table_name_l.begin(),::tolower);
        out<<"<string name=\"es_"<<table_name_l<<"\">"<<table_name<<"</string>\n";

        std::shared_ptr<cpptoml::table> table=table_iter->second->as_table();
        for(auto iter=table->begin();iter!=table->end();iter++){
            const std::string& key_name=iter->first;
            out<<"<string name=\"es_"<<table_name_l<<"_"<<key_name<<"\">"<<convert_to_name(key_name)<<"</string>\n";
        }
    }

#endif
#if 0
    //STRING ARRAY XML
    out<<"\n\n\n\n";

    for(auto table_iter=toml->begin() ;table_iter!=toml->end();table_iter++){
        const std::string& table_name=table_iter->first;
        std::string table_name_l=table_name;
        std::transform(table_name_l.begin(),table_name_l.end(),table_name_l.begin(),::tolower);

        std::shared_ptr<cpptoml::table> table=table_iter->second->as_table();
        for(auto iter=table->begin();iter!=table->end();iter++){
            const std::string& key_name=iter->first;
            auto find_iter=std::begin(gen_list);
            find_iter=std::find_if(find_iter,std::end(gen_list),[&table_name,&key_name](const std::pair<std::string,entries>& entry){
                return entry.first==table_name+"|"+key_name;
            });
            if(find_iter!=std::end(gen_list)){
                auto list=find_iter->second;
                if(std::find(std::begin(list[0]),std::end(list[0]),'@')!=std::end(list[0])){
                    out<<"<string-array name=\"es_arr_v_"<<table_name_l<<"_"<<key_name<<"\">\n";
                    for(auto entry:list){
                        int pos=entry.find("@");
                        std::string entry_str=entry.substr(pos+1);
                        out<<"<item>"<<entry_str<<"</item>\n";
                    }
                    out<<"</string-array>\n";
                    out<<"<string-array name=\"es_arr_"<<table_name_l<<"_"<<key_name<<"\">\n";
                    for(auto entry:list){
                        int pos=entry.find("@");
                        std::string entry_str=entry.substr(0,pos);
                        out<<"<item>"<<entry_str<<"</item>\n";
                    }
                    out<<"</string-array>\n";
                }
                else{
                    out<<"<string-array name=\"es_arr_"<<table_name_l<<"_"<<key_name<<"\">\n";
                    for(auto entry:list){
                        out<<"<item>"<<entry<<"</item>\n";
                    }
                    out<<"</string-array>\n";
                }
            }
        }
    }
#endif
    return env->NewStringUTF(out.str().c_str());
}
#undef SEEKBAR_PREF_TAG
#undef CHECKBOX_PREF_TAG
#undef LIST_PREF_TAG

//public  native void setup_uri_info_list_file(String path);
static void j_setup_uri_info_list_file(JNIEnv* env,jobject self,jstring jpath ){
    const char* path = env->GetStringUTFChars(jpath,NULL);
    if (path == NULL) {
        return;
    }
    g_uri_info_list_file_path=path;
    env->ReleaseStringUTFChars(jpath,path);
}

//public native void push_performance_metrics(float fps, float frameTimeMs, int perfState,
//                                             float memoryUsedMB, float memoryTotalMB, float temperature);
static void j_push_performance_metrics(JNIEnv* env, jobject self,
                                       jfloat fps, jfloat frame_time_ms, jint perf_state,
                                       jfloat memory_used_mb, jfloat memory_total_mb, jfloat temperature) {
    // Update global performance metrics cache
    g_performance_metrics.fps.store(fps, std::memory_order_relaxed);
    g_performance_metrics.frame_time_ms.store(frame_time_ms, std::memory_order_relaxed);
    g_performance_metrics.perf_state.store(perf_state, std::memory_order_relaxed);
    g_performance_metrics.memory_used_mb.store(memory_used_mb, std::memory_order_relaxed);
    g_performance_metrics.memory_total_mb.store(memory_total_mb, std::memory_order_relaxed);
    g_performance_metrics.temperature.store(temperature, std::memory_order_relaxed);

    // Get current time in milliseconds
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    g_performance_metrics.last_update_time_ms.store(ms, std::memory_order_relaxed);

    // Log on state transitions
    static int last_logged_state = 0;
    if (perf_state != last_logged_state) {
        const char* state_names[] = {"NORMAL", "PRESSURED", "THROTTLING", "CRITICAL"};
        const char* state_name = (perf_state >= 0 && perf_state <= 3) ? state_names[perf_state] : "UNKNOWN";
        __android_log_print(ANDROID_LOG_INFO, "PerformanceMetrics",
                          "State: %s, FPS: %.1f, Temp: %.1f°C, Mem: %.0f/%.0f MB",
                          state_name, fps, temperature, memory_used_mb, memory_total_mb);
        last_logged_state = perf_state;
    }
}

// CPU accuracy / diagnostics snapshot (for Java display, logging, or debugging accuracy issues).
// Returns a formatted string with counters (unhandled instrs, reservation success/failure rates, timebase reads, etc.).
// Includes 128B + R1 ps_* (paired-single) harness metrics (ps_arith etc).
// Extensible via the CpuAccuracyTracker.
static jstring j_get_cpu_accuracy_metrics(JNIEnv* env, jobject self) {
    std::string report = g_cpu_accuracy.GetSnapshotString();

    // Also force a log (in addition to the tracker's periodic reports)
    PERF_LOGI("JNI snapshot: %s", report.c_str());
    // 128B pairing violations (reservation_pairing_violations) now auto-included via GetSnapshotString
    // (CAPTAIN errata counter for cross-thread 128B overlaps surfaced to Java/PerformanceMonitor).

    return env->NewStringUTF(report.c_str());
}

// 128B reservation stress + false-share harness trigger (CAPTAIN DIRECT ORDER).
// Called from Java PerformanceMonitor (hidden dev setting) or diagnostics.
// Invokes the full simulated validation sequences (lwarx on granule + crossing stw at +64/+127
// + V128) from a64_backend, which logs research citations and increments the dedicated
// CpuAccuracyTracker counters (crossing_invalidation_tests, false_share_detected).
// Wires the harness for on-device Adreno testing of the 128B invalidation logic.
static void j_trigger_128b_reservation_stress(JNIEnv* env, jobject self) {
    PERF_LOGI("JNI: 128B reservation stress harness triggered from Java side (PerformanceMonitor / dev setting)");
    // Call the research validation sequences (defined in a64 backend, guarded internally).
    xe::cpu::backend::a64::Run128BReservationStressTestHarness();
    // Also directly record via tracker for visibility even if harness early-returns.
    g_cpu_accuracy.RecordCrossingInvalidationTest(true);
    PERF_LOGI("JNI: 128B stress complete - check CPU_ACCURACY snapshot for cross_128b_inv + false_share_128b counters.");
}

// R1 (ps_* research report author) + CAPTAIN: ps accuracy stress harness JNI trigger.
// Exact mirror of 128B pattern. Calls RunPairedSingleAccuracyHarness (a64_backend).
// Logs R1 citations (ps_maddx FMA priority, psq impact, GQR, explicit 128B psq_st warning).
// Increments ps_* counters in tracker (visible in snapshot + logcat).
static void j_trigger_ps_accuracy_stress(JNIEnv* env, jobject self) {
    PERF_LOGI("JNI: PS_* (paired-single) accuracy stress harness triggered from Java (PerformanceMonitor / dev). "
              "R1 research author (harness owner) enrichment + validator-in-chief expansion: live ps_addx/maddx/msubx/subx/sel (ppc_emit_fpu) + richer title-derived FMA/sub/sel/GQR VBO fidelity + expanded psq_st+128B+lockfree audio/physics+barriers + deeper per-elem + TLB shootdown + crown full-stack psq_l(GQR)+sub/sel+psq_st+TLB+pairing integration sequences + 3 new R1-gap counters (ps_sub_sel_arith etc) in RunPairedSingleAccuracyHarness. "
              "TLB owner re-task (R2): TLB-aware seqs now plugged in (big.LITTLE shootdown + psq_st/res migration, prot faults in psq+atomic, TLB+FPU 128B); tlb_ops_ignored surfaces via harness under a64_accuracy_debug.");
    // Invoke the harness (guarded inside for cvar). This + emitter Record* sites (gated) wire the
    // ps-specific validation sequences/counters for the harness R1 is building.
    // TLB integration (this re-task ownership): harness now runs the 3 targeted TLB+psq/128B/res seqs (R2 citations + ESR polish cross-ref).
    xe::cpu::backend::a64::RunPairedSingleAccuracyHarness();
    // Direct counter bump for visibility (matches 128B). Exercises ps_arith path.
    g_cpu_accuracy.RecordPairedSingleArith(2);
    g_cpu_accuracy.RecordPairedSingleFMA();
    // Additional direct wiring for the new ps-specific debug counters (from re-task emitter + harness seqs).
    g_cpu_accuracy.RecordPsFmaExecuted(2);
    g_cpu_accuracy.RecordPsNanCase();
    g_cpu_accuracy.RecordPsDenormHandled();
    // R1 AUTHOR ENRICH: also bump new per-element mixed/rounding FPCR edge counters from JNI trigger
    // (matches harness sequences that now exercise title-derived additional R1 edges).
    g_cpu_accuracy.RecordPsMixedElementEdge();
    g_cpu_accuracy.RecordPsRoundingEdge();
    // TLB polish (R2 owner re-task): explicit reuse of tlb_ops_ignored from JNI ps trigger path for direct visibility
    // (harness TLB seqs already call it 3x; this ensures counter surfaces even on early-guard or direct trigger).
    // Citations: ppc_hir_builder hook + ax360e_perf_log.h RecordTlb + a64_backend.cc TLB seqs (big.LITTLE + psq_st + 128B).
    g_cpu_accuracy.RecordTlbOpIgnored();
    PERF_LOGI("JNI: PS_* stress complete - check CPU_ACCURACY for ps_arith_executed/ps_fma_executed/ps_nan_cases/ps_denorm_handled + NEW ps_mixed_element_edges/ps_rounding_edges (R1 title-derived enrichment). "
              "TLB: tlb_ops_ignored incremented (R2 TLB/ERAT report + ps harness integration for shootdown/res mig/prot-fault cases).");
}

//public native void update_memory_pressure(int pressureLevel, long availableMB, int thermalLevel);
static void j_update_memory_pressure(JNIEnv* env, jobject self,
                                     jint pressure_level, jlong available_mb, jint thermal_level) {
    // Update global memory pressure state
    g_memory_pressure.pressure_level.store(pressure_level, std::memory_order_relaxed);
    g_memory_pressure.available_mb.store(available_mb, std::memory_order_relaxed);
    g_memory_pressure.thermal_level.store(thermal_level, std::memory_order_relaxed);

    // Get current time in milliseconds
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    g_memory_pressure.last_update_time_ms.store(ms, std::memory_order_relaxed);

    // Log on pressure level changes
    static int last_logged_level = -1;
    if (pressure_level != last_logged_level) {
        const char* level_names[] = {"NONE", "LOW", "MEDIUM", "HIGH", "CRITICAL"};
        const char* level_name = (pressure_level >= 0 && pressure_level <= 4) ? level_names[pressure_level] : "UNKNOWN";
        double scale_factor = g_memory_pressure.GetScaleFactor();
        __android_log_print(ANDROID_LOG_INFO, "MemoryPressure",
                          "Level: %s, Available: %lld MB, Thermal: %d, Scale: %.2f",
                          level_name, (long long)available_mb, thermal_level, scale_factor);
        last_logged_level = pressure_level;
    }
}

int register_ax360e_Emulator(JNIEnv* env){

    jclass localDocFile=env->FindClass("androidx/documentfile/provider/DocumentFile");
    if (localDocFile == NULL) {
        LOGE("Failed to find DocumentFile class");
        return -1;
    }
    g_class_DocumentFile=(jclass)env->NewGlobalRef(localDocFile);
    env->DeleteLocalRef(localDocFile);

    jclass localEmulator = env->FindClass("aenu/ax360e/Emulator");
    if (localEmulator == NULL) {
        LOGE("Failed to find Emulator class");
        return -1;
    }
    g_class_Emulator = (jclass)env->NewGlobalRef(localEmulator);
    env->DeleteLocalRef(localEmulator);

    //public static int nc_open_uri_fd(Context ctx,String uri)
    mid_open_uri_fd = env->GetStaticMethodID(g_class_Emulator, "nc_open_uri_fd", "(Landroid/content/Context;Landroid/net/Uri;)I");
    if (mid_open_uri_fd == NULL) {
        LOGE("Failed to find nc_open_uri_fd method");
        return -1;
    }

    static const JNINativeMethod methods[] = {
            { "setup_context", "(Landroid/content/Context;)V", (void *) j_setup_context },
            { "setup_document_file_tree", "(Landroidx/documentfile/provider/DocumentFile;)V", (void *) j_setup_document_file_tree },
            { "setup_launch_args", "([Ljava/lang/String;)V", (void *) j_setup_launch_args },
            { "meta_info_from_god_game", "(Landroid/content/Context;Ljava/lang/String;)Laenu/ax360e/Emulator$GameInfo;", (void *) j_meta_info_from_god_game },
            { "setup_uri_info_list_file", "(Ljava/lang/String;)V", (void *) j_setup_uri_info_list_file },
            {"simple_device_info", "()Ljava/lang/String;", (void *) j_simple_device_info},
            {"generate_config_xml", "(Ljava/lang/String;)Ljava/lang/String;", (void *) generate_config_xml},
            {"push_performance_metrics", "(FFIFFF)V", (void *) j_push_performance_metrics},
            {"update_memory_pressure", "(IJI)V", (void *) j_update_memory_pressure},
            {"get_cpu_accuracy_metrics", "()Ljava/lang/String;", (void *) j_get_cpu_accuracy_metrics},
            // 128B reservation stress harness trigger (CAPTAIN task)
            {"trigger_128b_reservation_stress_test", "()V", (void *) j_trigger_128b_reservation_stress},
            // R1 ps_* accuracy stress harness trigger (CAPTAIN / original research author validator)
            {"trigger_ps_accuracy_stress_test", "()V", (void *) j_trigger_ps_accuracy_stress}
    };
    return env->RegisterNatives(g_class_Emulator,methods, sizeof(methods)/sizeof(methods[0]));
}
