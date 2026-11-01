#include "vulkan_driver.h"
#include "gpu_probe.h"
#include <jni.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static std::string quote(const std::string& value) {
    std::string result = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}

static std::string utf8(JNIEnv* env, jstring value) {
    const char* text = env->GetStringUTFChars(value, nullptr);
    if (!text) throw std::runtime_error("Cannot read driver configuration");
    std::string result(text);
    env->ReleaseStringUTFChars(value, text);
    return result;
}

template<class Function>
static Function entry(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* name) {
    auto address = gipa(instance, name);
    if (!address) throw std::runtime_error(std::string("Missing Vulkan function: ") + name);
    return reinterpret_cast<Function>(address);
}

static void require(VkResult result, const char* action) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(action) + ": VkResult " + std::to_string(result));
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chusan_runtimeprobe_NativeProbeActivity_probe(JNIEnv* env, jclass, jstring libraries,
                                                      jstring files, jstring cache, jstring custom) {
    std::string report;
    try {
        std::string requested = utf8(env, custom);
        chusan::VulkanDriver driver(utf8(env, libraries), utf8(env, files), utf8(env, cache), requested);
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(driver.symbol("vkGetInstanceProcAddr"));
        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "Chusan native runtime probe";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &application;
        VkInstance instance = VK_NULL_HANDLE;
        require(entry<PFN_vkCreateInstance>(gipa, VK_NULL_HANDLE, "vkCreateInstance")(&createInfo, nullptr, &instance), "vkCreateInstance");
        auto destroy = entry<PFN_vkDestroyInstance>(gipa, instance, "vkDestroyInstance");
        try {
            auto enumerate = entry<PFN_vkEnumeratePhysicalDevices>(gipa, instance, "vkEnumeratePhysicalDevices");
            uint32_t count = 0;
            require(enumerate(instance, &count, nullptr), "Count physical devices");
            if (!count) throw std::runtime_error("Loaded driver exposes no physical devices");
            std::vector<VkPhysicalDevice> physical(count);
            require(enumerate(instance, &count, physical.data()), "Enumerate physical devices");
            VkPhysicalDevice gpu = physical[0];
            auto extensions = entry<PFN_vkEnumerateDeviceExtensionProperties>(gipa, instance, "vkEnumerateDeviceExtensionProperties");
            uint32_t extensionCount = 0;
            require(extensions(gpu, nullptr, &extensionCount, nullptr), "Count extensions");
            std::vector<VkExtensionProperties> extensionProperties(extensionCount);
            require(extensions(gpu, nullptr, &extensionCount, extensionProperties.data()), "Enumerate extensions");
            std::vector<std::string> names;
            for (const auto& extension : extensionProperties) names.emplace_back(extension.extensionName);
            std::sort(names.begin(), names.end());
            VkPhysicalDeviceDriverProperties driverProperties{};
            driverProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            if (std::binary_search(names.begin(), names.end(), "VK_KHR_driver_properties"))
                properties.pNext = &driverProperties;
            entry<PFN_vkGetPhysicalDeviceProperties2>(gipa, instance, "vkGetPhysicalDeviceProperties2")(gpu, &properties);
            VkPhysicalDeviceFeatures features{};
            entry<PFN_vkGetPhysicalDeviceFeatures>(gipa, instance, "vkGetPhysicalDeviceFeatures")(gpu, &features);
            auto queues = entry<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
            uint32_t queueCount = 0;
            queues(gpu, &queueCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueProperties(queueCount);
            queues(gpu, &queueCount, queueProperties.data());
            uint32_t family = 0;
            while (family < queueCount && !(queueProperties[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
            if (family == queueCount) throw std::runtime_error("No graphics queue");
            float priority = 1;
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;
            VkDeviceCreateInfo deviceInfo{};
            deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            deviceInfo.queueCreateInfoCount = 1;
            deviceInfo.pQueueCreateInfos = &queueInfo;
            auto destroyDevice = entry<PFN_vkDestroyDevice>(gipa, instance, "vkDestroyDevice");
            VkDevice device = VK_NULL_HANDLE;
            require(entry<PFN_vkCreateDevice>(gipa, instance, "vkCreateDevice")(gpu, &deviceInfo, nullptr, &device), "vkCreateDevice");
            auto getQueue = entry<PFN_vkGetDeviceQueue>(gipa, instance, "vkGetDeviceQueue");
            VkQueue queue = VK_NULL_HANDLE;
            getQueue(device, family, 0, &queue);
            chusan::GpuProbeResult draw{};
            std::string draw_error;
            try {
                draw = chusan::run_gpu_probe(gipa, instance, gpu, device, family, queue);
            } catch (const std::exception& error) {
                draw_error = error.what();
            }
            if (!draw.error.empty()) draw_error = draw.error;
            destroyDevice(device, nullptr);
            uint32_t version = properties.properties.apiVersion;
            std::ostringstream out;
            out << "{\"custom_requested\":" << (requested.empty() ? "false" : "true")
                << ",\"requested_library\":" << quote(requested)
                << ",\"device\":" << quote(properties.properties.deviceName)
                << ",\"api_version\":" << quote(std::to_string(VK_VERSION_MAJOR(version)) + "." + std::to_string(VK_VERSION_MINOR(version)) + "." + std::to_string(VK_VERSION_PATCH(version)))
                << ",\"driver_id\":" << driverProperties.driverID
                << ",\"driver_version\":" << properties.properties.driverVersion
                << ",\"driver_name\":" << quote(driverProperties.driverName)
                << ",\"driver_info\":" << quote(driverProperties.driverInfo)
                << ",\"texture_compression_bc\":" << (features.textureCompressionBC ? "true" : "false")
                << ",\"logical_device_created\":true,\"draw_verified\":" << (draw.verified ? "true" : "false")
                << ",\"draw_width\":4,\"draw_height\":4,\"draw_matching_pixels\":" << draw.matching_pixels
                << ",\"draw_readback_rgba\":[" << static_cast<unsigned>(draw.rgba[0]) << ','
                << static_cast<unsigned>(draw.rgba[1]) << ',' << static_cast<unsigned>(draw.rgba[2]) << ','
                << static_cast<unsigned>(draw.rgba[3]) << ']'
                << ",\"draw_readback_coherent\":" << (draw.coherent_readback ? "true" : "false")
                << ",\"draw_elapsed_ms\":" << draw.elapsed_ms;
            if (!draw_error.empty()) out << ",\"draw_error\":" << quote(draw_error);
            out << ",\"extensions\":[";
            for (size_t i = 0; i < names.size(); ++i) out << (i ? "," : "") << quote(names[i]);
            out << "]}";
            report = out.str();
        } catch (...) {
            destroy(instance, nullptr);
            throw;
        }
        destroy(instance, nullptr);
    } catch (const std::exception& error) {
        report = "{\"error\":" + quote(error.what()) + "}";
    }
    return env->NewStringUTF(report.c_str());
}
