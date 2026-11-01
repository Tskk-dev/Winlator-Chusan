#include "d3d9_shader_probe.h"

#include "gpu_probe.h"

#include "d3d9_glsl.h"
#include "d3d9_shader.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <cstring>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace chusan {
namespace {
constexpr uint32_t kPixelShader[] = {
    0xffff0101u,             // ps_1_1
    81u, 0xa00f0000u,         // def c0
    0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, // green
    1u, 0x800f0000u, 0xa0e40000u, // mov r0, c0
    0x0000ffffu,
};

constexpr char kVertexSource[] = R"GLSL(#version 450
layout(location = 0) out vec4 o_d0;
layout(location = 1) out vec4 o_d1;
layout(location = 2) out vec4 o_t[8];
layout(location = 10) out float o_fog;
layout(location = 11) out float o_fogz;
void main() {
    const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
    o_d0 = vec4(0.0);
    o_d1 = vec4(0.0);
    for (int i = 0; i < 8; ++i) o_t[i] = vec4(0.0);
    o_fog = 1.0;
    o_fogz = 0.0;
}
)GLSL";

struct ShaderDeleter {
    void operator()(glslang_shader_t* value) const { glslang_shader_delete(value); }
};
struct ProgramDeleter {
    void operator()(glslang_program_t* value) const { glslang_program_delete(value); }
};

std::vector<uint32_t> compile_glsl(const std::string& source, glslang_stage_t stage) {
    static std::once_flag init_once;
    static bool initialized = false;
    std::call_once(init_once, [] { initialized = glslang_initialize_process() != 0; });
    if (!initialized) throw std::runtime_error("Initialize glslang failed");

    glslang_input_t input{};
    input.language = GLSLANG_SOURCE_GLSL;
    input.stage = stage;
    input.client = GLSLANG_CLIENT_VULKAN;
    input.client_version = GLSLANG_TARGET_VULKAN_1_1;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_3;
    input.code = source.c_str();
    input.default_version = 450;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.messages = GLSLANG_MSG_DEFAULT_BIT;
    input.resource = glslang_default_resource();

    std::unique_ptr<glslang_shader_t, ShaderDeleter> shader(glslang_shader_create(&input));
    if (!shader) throw std::runtime_error("Create glslang shader failed");
    if (!glslang_shader_preprocess(shader.get(), &input) || !glslang_shader_parse(shader.get(), &input))
        throw std::runtime_error(glslang_shader_get_info_log(shader.get()));

    std::unique_ptr<glslang_program_t, ProgramDeleter> program(glslang_program_create());
    if (!program) throw std::runtime_error("Create glslang program failed");
    glslang_program_add_shader(program.get(), shader.get());
    if (!glslang_program_link(program.get(), GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))
        throw std::runtime_error(glslang_program_get_info_log(program.get()));

    glslang_program_SPIRV_generate(program.get(), stage);
    const size_t word_count = glslang_program_SPIRV_get_size(program.get());
    if (!word_count) throw std::runtime_error("glslang produced empty SPIR-V");
    std::vector<uint32_t> words(word_count);
    glslang_program_SPIRV_get(program.get(), words.data());
    return words;
}
}

GpuProbeResult run_d3d9_shader_probe(PFN_vkGetInstanceProcAddr gipa, VkInstance instance,
                                    VkPhysicalDevice physical, VkDevice device,
                                    uint32_t queue_family, VkQueue queue) {
    bool translated_shader_compiled = false;
    try {
        std::vector<uint8_t> bytecode(sizeof(kPixelShader));
        std::memcpy(bytecode.data(), kPixelShader, sizeof(kPixelShader));
        const d9sh::Program& program = d9sh::program_for(bytecode);
        if (!program.ok) throw std::runtime_error("Parse D3D9 pixel shader: " + program.why);

        std::string pixel_source, why;
        if (!d9glsl::pixel_source(program, {}, &pixel_source, &why))
            throw std::runtime_error("Translate D3D9 pixel shader: " + why);

        const auto vertex = compile_glsl(kVertexSource, GLSLANG_STAGE_VERTEX);
        const auto fragment = compile_glsl(pixel_source, GLSLANG_STAGE_FRAGMENT);
        translated_shader_compiled = true;
        std::vector<float> constants(d9glsl::constant_registers(program) * 4, 0.0f);
        if (constants.size() < 4) throw std::runtime_error("D3D9 pixel shader has no c0 register");
        constants[1] = 1.0f;
        constants[3] = 1.0f;
        struct PixelParams {
            float alpha_ref = 0.0f;
            int32_t alpha_func = 8; // D3DCMP_ALWAYS
            int32_t fog_mode = 0;
            int32_t pad0 = 0;
            float fog_color[4]{};
            float fog_start = 0.0f, fog_end = 1.0f, fog_density = 1.0f, pad1 = 0.0f;
        };
        static_assert(offsetof(PixelParams, fog_color) == 16 && sizeof(PixelParams) == 48,
                      "D3D9 pixel parameter block must match std140 offsets");
        const PixelParams params{};
        GpuProbeShaders shaders{};
        shaders.vertex_spirv = vertex.data();
        shaders.vertex_word_count = vertex.size();
        shaders.fragment_spirv = fragment.data();
        shaders.fragment_word_count = fragment.size();
        shaders.fragment_constants = constants.data();
        shaders.fragment_constants_size = constants.size() * sizeof(float);
        shaders.fragment_params = &params;
        shaders.fragment_params_size = sizeof(params);
        shaders.expected_rgba[0] = 0;
        shaders.expected_rgba[1] = 255;
        shaders.expected_rgba[2] = 0;
        shaders.expected_rgba[3] = 255;
        GpuProbeResult result = run_gpu_probe(gipa, instance, physical, device, queue_family, queue, &shaders);
        result.translated_shader_compiled = true;
        return result;
    } catch (const std::exception& error) {
        GpuProbeResult result{};
        result.translated_shader_compiled = translated_shader_compiled;
        result.error = error.what();
        return result;
    }
}
}
