cmake_minimum_required(VERSION 3.22.1)
project(chusan_driver_probe LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(adrenotools
    GIT_REPOSITORY https://github.com/Pipetto-crypto/libadrenotools.git
    GIT_TAG 8483dfdaa2abf97ee89ad0e5f337e7b508550c6b
    GIT_SUBMODULES lib/linkernsbypass
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(adrenotools)

add_library(chusan_driver_probe SHARED vulkan_probe.cpp ../../../../../platform/android/vulkan_driver.cpp ../../../../../platform/android/gpu_probe.cpp)
target_include_directories(chusan_driver_probe PRIVATE . ../../../../../platform/android)
target_compile_options(chusan_driver_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(chusan_driver_probe PRIVATE adrenotools dl log)
