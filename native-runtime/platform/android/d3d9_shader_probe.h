#pragma once

#include "gpu_probe.h"

namespace chusan {

GpuProbeResult run_d3d9_shader_probe(PFN_vkGetInstanceProcAddr gipa, VkInstance instance,
                                    VkPhysicalDevice physical, VkDevice device,
                                    uint32_t queue_family, VkQueue queue);

}
