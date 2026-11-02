#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstddef>
#include <string>

namespace chusan {
struct GpuProbeResult {
    bool verified = false;
    bool translated_shader_compiled = false;
    bool coherent_readback = false;
    uint32_t matching_pixels = 0;
    uint64_t elapsed_ms = 0;
    uint8_t rgba[4]{};
    std::string error;
};

struct GpuProbeShaders {
    const uint32_t* vertex_spirv = nullptr;
    size_t vertex_word_count = 0;
    const uint32_t* fragment_spirv = nullptr;
    size_t fragment_word_count = 0;
    const void* fragment_constants = nullptr;
    size_t fragment_constants_size = 0;
    const void* fragment_params = nullptr;
    size_t fragment_params_size = 0;
    uint8_t expected_rgba[4]{};
};

GpuProbeResult run_gpu_probe(PFN_vkGetInstanceProcAddr gipa, VkInstance instance,
                             VkPhysicalDevice physical, VkDevice device,
                             uint32_t queue_family, VkQueue queue,
                             const GpuProbeShaders* shaders = nullptr);
}
