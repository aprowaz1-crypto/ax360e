
#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include "vkutil.h"
#include "adreno_driver.h"   // preferred loader (libadrenotools)
#include <atomic>
#include <cctype>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#define LOG_TAG "VulkanTestJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
std::atomic<bool> g_vulkan_test_cancel_requested{false};
}

extern "C"
JNIEXPORT jstring JNICALL
Java_aenu_ax360e_VulkanTestActivity_nRunVulkanTest(JNIEnv *env, jobject thiz, jobject surface) {
    g_vulkan_test_cancel_requested.store(false, std::memory_order_release);
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) return env->NewStringUTF("Failed to get native window");

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;

    auto cleanup = [&]() {
        if (pool != VK_NULL_HANDLE && vkDestroyCommandPool_) {
            vkDestroyCommandPool_(device, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
        if (swapchain != VK_NULL_HANDLE && vkDestroySwapchainKHR_) {
            vkDestroySwapchainKHR_(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
        if (device != VK_NULL_HANDLE) {
            vk_destroy_device(device);
            device = VK_NULL_HANDLE;
        }
        if (vk_surface != VK_NULL_HANDLE && vkDestroySurfaceKHR_) {
            vkDestroySurfaceKHR_(instance, vk_surface, nullptr);
            vk_surface = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vk_destroy_instance(instance);
            instance = VK_NULL_HANDLE;
        }
        if (window) {
            ANativeWindow_release(window);
            window = nullptr;
        }
    };
    auto cancel_requested = [&]() {
        return g_vulkan_test_cancel_requested.load(std::memory_order_acquire);
    };

    // Check if a custom driver should be loaded (new clean path)
    const char* custom_driver_path = std::getenv("CUSTOM_DRIVER_PATH");
    bool is_custom = (custom_driver_path != nullptr && custom_driver_path[0] != '\0');

    LOGI("Vulkan Test: is_custom=%d, CUSTOM_DRIVER_PATH=%s", is_custom, is_custom ? custom_driver_path : "NULL");

    bool driver_loaded = false;

    if (is_custom) {
        // Preferred path: new adreno_driver loader (libadrenotools)
        std::string path(custom_driver_path);
        size_t last_slash = path.find_last_of("/\\");
        std::string dir  = (last_slash != std::string::npos) ? path.substr(0, last_slash) : ".";
        std::string name = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;

        driver_loaded = load_custom_adreno_driver(dir, name, /*enable_redirection=*/true);
        LOGI("Custom driver load via new loader: %s", driver_loaded ? "success" : "failed");
    } else {
        // System driver path
        vk_load("libvulkan.so", /*is_adreno_custom=*/false);
        driver_loaded = vk_is_loaded();
    }

    if (!driver_loaded) {
        cleanup();
        return env->NewStringUTF("Failed to load Vulkan library");
    }

    auto instance_opt = vk_create_instance("VulkanTest");
    if (!instance_opt) {
        cleanup();
        LOGE("Failed to create Vulkan instance");
        return env->NewStringUTF("Failed to create Vulkan instance");
    }
    instance = *instance_opt;
    LOGI("Vulkan instance created");
    if (cancel_requested()) {
        LOGI("Vulkan test cancelled after instance creation");
        cleanup();
        return env->NewStringUTF("Cancelled");
    }

    VkAndroidSurfaceCreateInfoKHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surface_info.window = window;
    
    VkResult res = vkCreateAndroidSurfaceKHR_(instance, &surface_info, nullptr, &vk_surface);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateAndroidSurfaceKHR failed: %d", res);
        cleanup();
        return env->NewStringUTF(("Failed to create surface: " + std::to_string(res)).c_str());
    }
    LOGI("Vulkan surface created");
    if (cancel_requested()) {
        LOGI("Vulkan test cancelled after surface creation");
        cleanup();
        return env->NewStringUTF("Cancelled");
    }

    auto physical_device_opt = vk_get_physical_device(instance);
    if (!physical_device_opt) {
        LOGE("No physical device found");
        cleanup();
        return env->NewStringUTF("No physical device found");
    }
    VkPhysicalDevice pdev = *physical_device_opt;

    VkPhysicalDeviceProperties props = vk_get_physical_device_properties(pdev);
    LOGI("Testing on GPU: %s", props.deviceName);

    // Determine driver type for clear color
    std::string dev_name = props.deviceName;
    for (auto& c : dev_name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    
    bool is_turnip = (dev_name.find("turnip") != std::string::npos || 
                      dev_name.find("mesa") != std::string::npos || 
                      dev_name.find("freedreno") != std::string::npos);
    
    bool is_stock_adreno = (dev_name.find("adreno") != std::string::npos && !is_turnip);

    // Clear color: 
    // GREEN (0,1,0) = Mesa/Turnip (SUCCESS)
    // RED   (1,0,0) = Stock Adreno (FAIL - custom driver didn't load or was bypassed)
    // BLUE  (0,0,1) = Other (Unknown state)
    VkClearColorValue clear_color;
    if (is_turnip) {
        clear_color = VkClearColorValue{{0.0f, 1.0f, 0.0f, 1.0f}};
        LOGI("Test result: SUCCESS (Mesa/Turnip detected)");
    } else if (is_stock_adreno) {
        clear_color = VkClearColorValue{{1.0f, 0.0f, 0.0f, 1.0f}};
        LOGI("Test result: FAIL (Stock Adreno detected)");
    } else {
        clear_color = VkClearColorValue{{0.0f, 0.0f, 1.0f, 1.0f}};
        LOGI("Test result: UNKNOWN (Other GPU detected: %s)", props.deviceName);
    }

    // Get queue family
    uint32_t queue_count = vk_get_queue_family_properties_count(pdev);
    LOGI("Queue family count: %u", queue_count);
    uint32_t graphics_queue_family = 0xFFFFFFFF;
    
    for (uint32_t i = 0; i < queue_count; i++) {
        auto qprops = vk_get_queue_family_properties(pdev, i);
        if (qprops) {
            LOGI("Queue family %u: flags %u, count %u", i, qprops->queueFlags, qprops->queueCount);
            if (qprops->queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                VkBool32 present_support = false;
                vkGetPhysicalDeviceSurfaceSupportKHR_(pdev, i, vk_surface, &present_support);
                LOGI("Queue family %u: present support %d", i, present_support);
                if (present_support) {
                    graphics_queue_family = i;
                    break;
                }
            }
        }
    }

    if (graphics_queue_family == 0xFFFFFFFF) {
        LOGE("No suitable graphics/present queue family found");
        cleanup();
        return env->NewStringUTF("No suitable queue family found");
    }
    LOGI("Selected graphics queue family: %u", graphics_queue_family);

    // Create device
    auto qprops_opt = vk_get_queue_family_properties(pdev, graphics_queue_family);
    if (!qprops_opt) {
        LOGE("Failed to get queue family properties for family %u", graphics_queue_family);
        cleanup();
        return env->NewStringUTF("Failed to get queue family properties");
    }
    auto device_opt = vk_create_device(pdev, graphics_queue_family, *qprops_opt);
    if (!device_opt) {
        LOGE("Failed to create device");
        cleanup();
        return env->NewStringUTF("Failed to create device");
    }
    device = *device_opt;
    LOGI("Vulkan device created");
    if (cancel_requested()) {
        LOGI("Vulkan test cancelled after device creation");
        cleanup();
        return env->NewStringUTF("Cancelled");
    }

    VkQueue queue;
    vkGetDeviceQueue_(device, graphics_queue_family, 0, &queue);

    // Simplified swapchain creation
    VkSurfaceCapabilitiesKHR caps;
    res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR_(pdev, vk_surface, &caps);
    if (res != VK_SUCCESS) {
        LOGE("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: %d", res);
        cleanup();
        return env->NewStringUTF(("Failed to get surface capabilities: " +
                                  std::to_string(res))
                                     .c_str());
    }
    LOGI("Surface capabilities: minImageCount %u, maxImageCount %u", caps.minImageCount, caps.maxImageCount);
    
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) {
        extent.width = ANativeWindow_getWidth(window);
        extent.height = ANativeWindow_getHeight(window);
    }
    LOGI("Swapchain extent: %ux%u", extent.width, extent.height);

    VkSwapchainCreateInfoKHR swap_info = {};
    swap_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swap_info.surface = vk_surface;
    swap_info.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && swap_info.minImageCount > caps.maxImageCount) {
        swap_info.minImageCount = caps.maxImageCount;
    }
    swap_info.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    swap_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swap_info.imageExtent = extent;
    swap_info.imageArrayLayers = 1;
    swap_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swap_info.preTransform = caps.currentTransform;
    swap_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swap_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swap_info.clipped = VK_TRUE;

    res = vkCreateSwapchainKHR_(device, &swap_info, nullptr, &swapchain);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateSwapchainKHR failed: %d", res);
        cleanup();
        return env->NewStringUTF(("Failed to create swapchain: " + std::to_string(res)).c_str());
    }
    LOGI("Vulkan swapchain created");
    if (cancel_requested()) {
        LOGI("Vulkan test cancelled after swapchain creation");
        cleanup();
        return env->NewStringUTF("Cancelled");
    }

    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR_(device, swapchain, &image_count, nullptr);
    LOGI("Swapchain image count: %u", image_count);
    std::vector<VkImage> images(image_count);
    vkGetSwapchainImagesKHR_(device, swapchain, &image_count, images.data());

    // Command pool and buffer
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = graphics_queue_family;
    
    res = vkCreateCommandPool_(device, &pool_info, nullptr, &pool);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateCommandPool failed: %d", res);
        cleanup();
        return env->NewStringUTF("Failed to create command pool");
    }
    LOGI("Command pool created");

    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    res = vkAllocateCommandBuffers_(device, &alloc_info, &cmd);
    if (res != VK_SUCCESS) {
        LOGE("vkAllocateCommandBuffers failed: %d", res);
        cleanup();
        return env->NewStringUTF("Failed to allocate command buffer");
    }
    LOGI("Command buffer allocated");

    // Create a fence for vkAcquireNextImageKHR: spec requires at least one of
    // semaphore or fence to be non-null.
    VkFenceCreateInfo acquire_fence_info = {};
    acquire_fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence acquire_fence;
    res = vkCreateFence_(device, &acquire_fence_info, nullptr, &acquire_fence);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateFence failed: %d", res);
        cleanup();
        return env->NewStringUTF("Failed to create acquire fence");
    }

    uint32_t image_index;
    LOGI("Acquiring next image...");
    res = vkAcquireNextImageKHR_(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, acquire_fence, &image_index);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        LOGE("vkAcquireNextImageKHR failed: %d", res);
        vkDestroyFence_(device, acquire_fence, nullptr);
        cleanup();
        return env->NewStringUTF("Failed to acquire image");
    }
    vkWaitForFences_(device, 1, &acquire_fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence_(device, acquire_fence, nullptr);
    LOGI("Acquired image index: %u", image_index);

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    LOGI("Beginning command buffer...");
    res = vkBeginCommandBuffer_(cmd, &begin_info);
    if (res != VK_SUCCESS) {
        LOGE("vkBeginCommandBuffer failed: %d", res);
        cleanup();
        return env->NewStringUTF(("Error: vkBeginCommandBuffer failed: " + std::to_string(res)).c_str());
    }

    // Barrier to transition image to TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[image_index];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    LOGI("Setting image barrier...");
    vkCmdPipelineBarrier_(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    LOGI("Clearing color image...");
    vkCmdClearColorImage_(cmd, images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &range);

    // Barrier to transition image to PRESENT_SRC_KHR
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;

    LOGI("Setting presentation barrier...");
    vkCmdPipelineBarrier_(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    LOGI("Ending command buffer...");
    res = vkEndCommandBuffer_(cmd);
    if (res != VK_SUCCESS) {
        LOGE("vkEndCommandBuffer failed: %d", res);
        cleanup();
        return env->NewStringUTF(("Error: vkEndCommandBuffer failed: " + std::to_string(res)).c_str());
    }

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    LOGI("Submitting queue...");
    res = vkQueueSubmit_(queue, 1, &submit_info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        LOGE("vkQueueSubmit failed: %d", res);
        cleanup();
        return env->NewStringUTF(("Error: vkQueueSubmit failed: " + std::to_string(res)).c_str());
    }
    LOGI("Waiting for queue idle...");
    res = vkQueueWaitIdle_(queue);
    if (res != VK_SUCCESS) {
        LOGE("vkQueueWaitIdle failed: %d", res);
        cleanup();
        return env->NewStringUTF(("Error: vkQueueWaitIdle failed: " +
                                  std::to_string(res))
                                     .c_str());
    }

    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain;
    present_info.pImageIndices = &image_index;

    LOGI("Presenting...");
    res = vkQueuePresentKHR_(queue, &present_info);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        LOGE("vkQueuePresentKHR failed: %d", res);
        cleanup();
        return env->NewStringUTF(("Error: vkQueuePresentKHR failed: " + std::to_string(res)).c_str());
    }

    // Wait a bit to show the color before cleanup
    LOGI("Rendering successful, waiting 2 seconds...");
    for (int i = 0; i < 20; ++i) {
        if (cancel_requested()) {
            LOGI("Vulkan test cancelled during post-present wait");
            cleanup();
            return env->NewStringUTF("Cancelled");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOGI("Cleaning up...");
    cleanup();

    return env->NewStringUTF("Success: Rendered Clear Color!");
}

extern "C"
JNIEXPORT void JNICALL
Java_aenu_ax360e_VulkanTestActivity_nCancelVulkanTest(JNIEnv* env, jobject thiz) {
    g_vulkan_test_cancel_requested.store(true, std::memory_order_release);
}

extern "C"
JNIEXPORT jstring JNICALL
Java_aenu_ax360e_VulkanTestActivity_nGetVulkanDeviceInfo(JNIEnv *env, jobject thiz) {
    bool driverReady = is_using_custom_adreno_driver();

#if !HAS_LIBADRENOTOOLS
    if (!driverReady) {
        driverReady = vk_is_loaded();
    }
#endif

    if (!driverReady) {
        return env->NewStringUTF("Driver not loaded");
    }

    auto instance_opt = vk_create_instance("DeviceInfo");
    if (!instance_opt) return env->NewStringUTF("Instance failed");
    VkInstance instance = *instance_opt;

    auto pdev_opt = vk_get_physical_device(instance);
    if (!pdev_opt) {
        vk_destroy_instance(instance);
        return env->NewStringUTF("No GPU found");
    }

    VkPhysicalDeviceProperties props = vk_get_physical_device_properties(*pdev_opt);
    std::string info = std::string(props.deviceName) + " (Vulkan " + 
                       std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." +
                       std::to_string(VK_VERSION_MINOR(props.apiVersion)) + ")";

    vk_destroy_instance(instance);
    return env->NewStringUTF(info.c_str());
}
