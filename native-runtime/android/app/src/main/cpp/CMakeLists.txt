cmake_minimum_required(VERSION 3.22.1)
project(chusan_driver_probe LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY JOB_POOLS chusan_probe_pool=1)
set(CMAKE_JOB_POOL_COMPILE chusan_probe_pool)
set(CMAKE_JOB_POOL_LINK chusan_probe_pool)

option(CHUSAN_D3D9_SHADER_PROBE "Build the pinned D3D9 pixel shader proof" OFF)
set(RECOMP_KIT_ROOT "" CACHE PATH "Path to the pinned recomp-kit checkout")

include(FetchContent)
FetchContent_Declare(adrenotools
    GIT_REPOSITORY https://github.com/Pipetto-crypto/libadrenotools.git
    GIT_TAG 8483dfdaa2abf97ee89ad0e5f337e7b508550c6b
    GIT_SUBMODULES lib/linkernsbypass
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(adrenotools)

set(CHUSAN_PROBE_SOURCES
    vulkan_probe.cpp
    ../../../../../platform/android/vulkan_driver.cpp
    ../../../../../platform/android/gpu_probe.cpp)
if(CHUSAN_D3D9_SHADER_PROBE)
    if(NOT EXISTS "${RECOMP_KIT_ROOT}/dx/d3d9_shader.cpp"
       OR NOT EXISTS "${RECOMP_KIT_ROOT}/dx/d3d9_shadergen.cpp")
        message(FATAL_ERROR "CHUSAN_D3D9_SHADER_PROBE requires RECOMP_KIT_ROOT with dx/d3d9_shader.cpp and d3d9_shadergen.cpp")
    endif()
    execute_process(COMMAND git -C "${RECOMP_KIT_ROOT}" rev-parse HEAD
        OUTPUT_VARIABLE RECOMP_KIT_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE RECOMP_KIT_GIT_RESULT)
    if(NOT RECOMP_KIT_GIT_RESULT EQUAL 0
       OR NOT RECOMP_KIT_REVISION STREQUAL "487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d")
        message(FATAL_ERROR "CHUSAN_D3D9_SHADER_PROBE requires recomp-kit commit 487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d")
    endif()
    execute_process(COMMAND git -C "${RECOMP_KIT_ROOT}" diff --quiet HEAD --
        RESULT_VARIABLE RECOMP_KIT_DIFF_RESULT)
    execute_process(COMMAND git -C "${RECOMP_KIT_ROOT}" ls-files --others --exclude-standard
        OUTPUT_VARIABLE RECOMP_KIT_UNTRACKED OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE RECOMP_KIT_UNTRACKED_RESULT)
    if(NOT RECOMP_KIT_DIFF_RESULT EQUAL 0
       OR NOT RECOMP_KIT_UNTRACKED_RESULT EQUAL 0
       OR RECOMP_KIT_UNTRACKED)
        message(FATAL_ERROR "CHUSAN_D3D9_SHADER_PROBE requires a clean recomp-kit checkout")
    endif()
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG e1b562a8bed273a02f30b59b66a5d499793cede5)
    FetchContent_MakeAvailable(glslang)
    list(APPEND CHUSAN_PROBE_SOURCES
        "../../../../../platform/android/d3d9_shader_probe.cpp"
        "${RECOMP_KIT_ROOT}/dx/d3d9_shader.cpp"
        "${RECOMP_KIT_ROOT}/dx/d3d9_shadergen.cpp")
    set_source_files_properties(
        "${RECOMP_KIT_ROOT}/dx/d3d9_shader.cpp"
        "${RECOMP_KIT_ROOT}/dx/d3d9_shadergen.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wno-error")
endif()

add_library(chusan_driver_probe SHARED ${CHUSAN_PROBE_SOURCES})
target_include_directories(chusan_driver_probe PRIVATE . ../../../../../platform/android)
target_compile_options(chusan_driver_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(chusan_driver_probe PRIVATE adrenotools dl log)
if(CHUSAN_D3D9_SHADER_PROBE)
    target_include_directories(chusan_driver_probe PRIVATE "${RECOMP_KIT_ROOT}/dx")
    target_compile_definitions(chusan_driver_probe PRIVATE CHUSAN_D3D9_SHADER_PROBE=1)
    target_link_libraries(chusan_driver_probe PRIVATE glslang::glslang glslang::glslang-default-resource-limits)
else()
    target_compile_definitions(chusan_driver_probe PRIVATE CHUSAN_D3D9_SHADER_PROBE=0)
endif()
