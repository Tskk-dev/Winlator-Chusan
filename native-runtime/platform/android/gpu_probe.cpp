#include "gpu_probe.h"

#include "probe_shaders.h"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace chusan {
namespace {
constexpr uint32_t kWidth = 4;
constexpr uint32_t kHeight = 4;
constexpr uint8_t kExpected[4] = {255, 0, 0, 255};

template<class Function>
Function instance_entry(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* name) {
    auto address = gipa(instance, name);
    if (!address) throw std::runtime_error(std::string("Missing Vulkan function: ") + name);
    return reinterpret_cast<Function>(address);
}

template<class Function>
Function device_entry(PFN_vkGetDeviceProcAddr gdpa, VkDevice device, const char* name) {
    auto address = gdpa(device, name);
    if (!address) throw std::runtime_error(std::string("Missing Vulkan function: ") + name);
    return reinterpret_cast<Function>(address);
}

void require(VkResult result, const char* action) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(action) + ": VkResult " + std::to_string(result));
}

struct Resources {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDestroyShaderModule destroy_shader = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
    PFN_vkDestroyRenderPass destroy_render_pass = nullptr;
    PFN_vkDestroyFramebuffer destroy_framebuffer = nullptr;
    PFN_vkDestroyImageView destroy_image_view = nullptr;
    PFN_vkDestroyImage destroy_image = nullptr;
    PFN_vkDestroyBuffer destroy_buffer = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
    PFN_vkDestroyFence destroy_fence = nullptr;
    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule fragment_shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    ~Resources() {
        if (fence) destroy_fence(device, fence, nullptr);
        if (command_pool) destroy_command_pool(device, command_pool, nullptr);
        if (readback) destroy_buffer(device, readback, nullptr);
        if (readback_memory) free_memory(device, readback_memory, nullptr);
        if (framebuffer) destroy_framebuffer(device, framebuffer, nullptr);
        if (image_view) destroy_image_view(device, image_view, nullptr);
        if (image) destroy_image(device, image, nullptr);
        if (image_memory) free_memory(device, image_memory, nullptr);
        if (pipeline) destroy_pipeline(device, pipeline, nullptr);
        if (pipeline_layout) destroy_pipeline_layout(device, pipeline_layout, nullptr);
        if (fragment_shader) destroy_shader(device, fragment_shader, nullptr);
        if (vertex_shader) destroy_shader(device, vertex_shader, nullptr);
        if (render_pass) destroy_render_pass(device, render_pass, nullptr);
    }
};

uint32_t memory_type(const VkPhysicalDeviceMemoryProperties& properties, uint32_t allowed,
                    VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                    VkMemoryPropertyFlags& actual) {
    int fallback = -1;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if (!(allowed & (1u << i))) continue;
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if ((flags & preferred) == preferred) {
            actual = flags;
            return i;
        }
        if (fallback < 0) fallback = static_cast<int>(i);
    }
    if (fallback >= 0) {
        actual = properties.memoryTypes[fallback].propertyFlags;
        return static_cast<uint32_t>(fallback);
    }
    throw std::runtime_error("No compatible Vulkan memory type");
}

VkShaderModule shader_module(VkDevice device, PFN_vkCreateShaderModule create_shader,
                             const uint32_t* code, size_t word_count) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = word_count * sizeof(uint32_t);
    info.pCode = code;
    VkShaderModule result = VK_NULL_HANDLE;
    require(create_shader(device, &info, nullptr, &result), "Create probe shader");
    return result;
}
}

GpuProbeResult run_gpu_probe(PFN_vkGetInstanceProcAddr gipa, VkInstance instance,
                             VkPhysicalDevice physical, VkDevice device,
                             uint32_t queue_family, VkQueue queue) {
    auto gdpa = instance_entry<PFN_vkGetDeviceProcAddr>(gipa, instance, "vkGetDeviceProcAddr");
    auto get_memory_properties = instance_entry<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa, instance,
                                                                                         "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties memory_properties{};
    get_memory_properties(physical, &memory_properties);

    Resources r;
    r.device = device;
    auto create_shader = device_entry<PFN_vkCreateShaderModule>(gdpa, device, "vkCreateShaderModule");
    r.destroy_shader = device_entry<PFN_vkDestroyShaderModule>(gdpa, device, "vkDestroyShaderModule");
    auto create_render_pass = device_entry<PFN_vkCreateRenderPass>(gdpa, device, "vkCreateRenderPass");
    r.destroy_render_pass = device_entry<PFN_vkDestroyRenderPass>(gdpa, device, "vkDestroyRenderPass");
    auto create_image = device_entry<PFN_vkCreateImage>(gdpa, device, "vkCreateImage");
    r.destroy_image = device_entry<PFN_vkDestroyImage>(gdpa, device, "vkDestroyImage");
    auto get_image_requirements = device_entry<PFN_vkGetImageMemoryRequirements>(gdpa, device, "vkGetImageMemoryRequirements");
    auto allocate_memory = device_entry<PFN_vkAllocateMemory>(gdpa, device, "vkAllocateMemory");
    r.free_memory = device_entry<PFN_vkFreeMemory>(gdpa, device, "vkFreeMemory");
    auto bind_image_memory = device_entry<PFN_vkBindImageMemory>(gdpa, device, "vkBindImageMemory");
    auto create_image_view = device_entry<PFN_vkCreateImageView>(gdpa, device, "vkCreateImageView");
    r.destroy_image_view = device_entry<PFN_vkDestroyImageView>(gdpa, device, "vkDestroyImageView");
    r.destroy_buffer = device_entry<PFN_vkDestroyBuffer>(gdpa, device, "vkDestroyBuffer");
    r.destroy_framebuffer = device_entry<PFN_vkDestroyFramebuffer>(gdpa, device, "vkDestroyFramebuffer");
    r.destroy_pipeline = device_entry<PFN_vkDestroyPipeline>(gdpa, device, "vkDestroyPipeline");
    r.destroy_pipeline_layout = device_entry<PFN_vkDestroyPipelineLayout>(gdpa, device, "vkDestroyPipelineLayout");
    r.destroy_command_pool = device_entry<PFN_vkDestroyCommandPool>(gdpa, device, "vkDestroyCommandPool");
    r.destroy_fence = device_entry<PFN_vkDestroyFence>(gdpa, device, "vkDestroyFence");

    GpuProbeResult result{};
    auto started = std::chrono::steady_clock::now();
    r.vertex_shader = shader_module(device, create_shader, probe_shader::vertex,
                                    sizeof(probe_shader::vertex) / sizeof(uint32_t));
    r.fragment_shader = shader_module(device, create_shader, probe_shader::fragment,
                                      sizeof(probe_shader::fragment) / sizeof(uint32_t));

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 2;
    render_pass_info.pDependencies = dependencies;
    require(create_render_pass(device, &render_pass_info, nullptr, &r.render_pass), "Create probe render pass");

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {kWidth, kHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    require(create_image(device, &image_info, nullptr, &r.image), "Create probe image");
    VkMemoryRequirements image_requirements{};
    get_image_requirements(device, r.image, &image_requirements);
    VkMemoryPropertyFlags image_flags = 0;
    VkMemoryAllocateInfo image_memory_info{};
    image_memory_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_memory_info.allocationSize = image_requirements.size;
    image_memory_info.memoryTypeIndex = memory_type(memory_properties, image_requirements.memoryTypeBits, 0,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image_flags);
    (void)image_flags;
    require(allocate_memory(device, &image_memory_info, nullptr, &r.image_memory), "Allocate probe image memory");
    require(bind_image_memory(device, r.image, r.image_memory, 0), "Bind probe image memory");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = r.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    require(create_image_view(device, &view_info, nullptr, &r.image_view), "Create probe image view");

    auto create_framebuffer = device_entry<PFN_vkCreateFramebuffer>(gdpa, device, "vkCreateFramebuffer");
    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = r.render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &r.image_view;
    framebuffer_info.width = kWidth;
    framebuffer_info.height = kHeight;
    framebuffer_info.layers = 1;
    require(create_framebuffer(device, &framebuffer_info, nullptr, &r.framebuffer), "Create probe framebuffer");

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    auto create_pipeline_layout = device_entry<PFN_vkCreatePipelineLayout>(gdpa, device, "vkCreatePipelineLayout");
    require(create_pipeline_layout(device, &layout_info, nullptr, &r.pipeline_layout), "Create probe pipeline layout");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r.vertex_shader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r.fragment_shader;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = r.pipeline_layout;
    pipeline_info.renderPass = r.render_pass;
    pipeline_info.subpass = 0;
    auto create_pipelines = device_entry<PFN_vkCreateGraphicsPipelines>(gdpa, device, "vkCreateGraphicsPipelines");
    require(create_pipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &r.pipeline), "Create probe graphics pipeline");

    auto create_buffer = device_entry<PFN_vkCreateBuffer>(gdpa, device, "vkCreateBuffer");
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = kWidth * kHeight * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    require(create_buffer(device, &buffer_info, nullptr, &r.readback), "Create readback buffer");
    auto get_buffer_requirements = device_entry<PFN_vkGetBufferMemoryRequirements>(gdpa, device, "vkGetBufferMemoryRequirements");
    VkMemoryRequirements buffer_requirements{};
    get_buffer_requirements(device, r.readback, &buffer_requirements);
    VkMemoryPropertyFlags buffer_flags = 0;
    VkMemoryAllocateInfo buffer_memory_info{};
    buffer_memory_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buffer_memory_info.allocationSize = buffer_requirements.size;
    buffer_memory_info.memoryTypeIndex = memory_type(memory_properties, buffer_requirements.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer_flags);
    require(allocate_memory(device, &buffer_memory_info, nullptr, &r.readback_memory), "Allocate readback memory");
    require(device_entry<PFN_vkBindBufferMemory>(gdpa, device, "vkBindBufferMemory")(
                device, r.readback, r.readback_memory, 0), "Bind readback memory");

    auto create_command_pool = device_entry<PFN_vkCreateCommandPool>(gdpa, device, "vkCreateCommandPool");
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue_family;
    require(create_command_pool(device, &pool_info, nullptr, &r.command_pool), "Create probe command pool");
    auto allocate_commands = device_entry<PFN_vkAllocateCommandBuffers>(gdpa, device, "vkAllocateCommandBuffers");
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = r.command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    require(allocate_commands(device, &command_info, &command), "Allocate probe command buffer");
    auto begin_command = device_entry<PFN_vkBeginCommandBuffer>(gdpa, device, "vkBeginCommandBuffer");
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    require(begin_command(command, &begin_info), "Begin probe command buffer");
    VkClearValue clear{};
    clear.color.float32[2] = 1.0f;
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo render_begin{};
    render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_begin.renderPass = r.render_pass;
    render_begin.framebuffer = r.framebuffer;
    render_begin.renderArea.extent = {kWidth, kHeight};
    render_begin.clearValueCount = 1;
    render_begin.pClearValues = &clear;
    device_entry<PFN_vkCmdBeginRenderPass>(gdpa, device, "vkCmdBeginRenderPass")(
        command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    device_entry<PFN_vkCmdBindPipeline>(gdpa, device, "vkCmdBindPipeline")(
        command, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
    device_entry<PFN_vkCmdDraw>(gdpa, device, "vkCmdDraw")(command, 3, 1, 0, 0);
    device_entry<PFN_vkCmdEndRenderPass>(gdpa, device, "vkCmdEndRenderPass")(command);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {kWidth, kHeight, 1};
    device_entry<PFN_vkCmdCopyImageToBuffer>(gdpa, device, "vkCmdCopyImageToBuffer")(
        command, r.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.readback, 1, &copy);
    VkBufferMemoryBarrier host_barrier{};
    host_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.buffer = r.readback;
    host_barrier.size = VK_WHOLE_SIZE;
    device_entry<PFN_vkCmdPipelineBarrier>(gdpa, device, "vkCmdPipelineBarrier")(
        command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
        0, nullptr, 1, &host_barrier, 0, nullptr);
    require(device_entry<PFN_vkEndCommandBuffer>(gdpa, device, "vkEndCommandBuffer")(command), "End probe command buffer");

    auto create_fence = device_entry<PFN_vkCreateFence>(gdpa, device, "vkCreateFence");
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    require(create_fence(device, &fence_info, nullptr, &r.fence), "Create probe fence");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    require(device_entry<PFN_vkQueueSubmit>(gdpa, device, "vkQueueSubmit")(queue, 1, &submit, r.fence), "Submit probe draw");
    const VkResult wait_result = device_entry<PFN_vkWaitForFences>(gdpa, device, "vkWaitForFences")(
        device, 1, &r.fence, VK_TRUE, 10'000'000'000ull);
    if (wait_result != VK_SUCCESS) {
        const VkResult drain = device_entry<PFN_vkQueueWaitIdle>(gdpa, device, "vkQueueWaitIdle")(queue);
        if (drain != VK_SUCCESS) require(drain, "Drain probe queue after failed fence wait");
        require(wait_result, "Wait for probe draw (10 second timeout)");
    }
    result.elapsed_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());

    auto map_memory = device_entry<PFN_vkMapMemory>(gdpa, device, "vkMapMemory");
    void* mapped = nullptr;
    require(map_memory(device, r.readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "Map readback memory");
    auto unmap_memory = device_entry<PFN_vkUnmapMemory>(gdpa, device, "vkUnmapMemory");
    if (!(buffer_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = r.readback_memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        const VkResult invalidate = device_entry<PFN_vkInvalidateMappedMemoryRanges>(gdpa, device,
            "vkInvalidateMappedMemoryRanges")(device, 1, &range);
        if (invalidate != VK_SUCCESS) {
            unmap_memory(device, r.readback_memory);
            require(invalidate, "Invalidate readback memory");
        }
    }
    const auto* pixels = static_cast<const uint8_t*>(mapped);
    result.matching_pixels = 0;
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        if (std::memcmp(pixels + i * 4, kExpected, 4) == 0) ++result.matching_pixels;
    }
    std::memcpy(result.rgba, pixels + ((kHeight / 2) * kWidth + kWidth / 2) * 4, 4);
    unmap_memory(device, r.readback_memory);
    result.coherent_readback = (buffer_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    result.verified = result.matching_pixels == kWidth * kHeight;
    result.elapsed_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    if (!result.verified) result.error = "Readback did not match red fragment output across all 16 pixels";
    return result;
}
}
