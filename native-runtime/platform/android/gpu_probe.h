#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace chusan {
struct GpuProbeResult {
    bool verified = false;
    bool coherent_readback = false;
    uint32_t matching_pixels = 0;
    uint64_t elapsed_ms = 0;
    uint8_t rgba[4]{};
    std::string error;
};

GpuProbeResult run_gpu_probe(PFN_vkGetInstanceProcAddr gipa, VkInstance instance,
                             VkPhysicalDevice physical, VkDevice device,
                             uint32_t queue_family, VkQueue queue);
}
