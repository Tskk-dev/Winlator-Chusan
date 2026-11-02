#pragma once

#include <string>

namespace chusan {
// All Vulkan objects must be destroyed before this library handle is released.
// Select a driver before creating an instance; change drivers in a fresh process.
class VulkanDriver {
public:
    VulkanDriver(const std::string& nativeLibraryDirectory, const std::string& privateFilesDirectory,
                 const std::string& temporaryDirectory, const std::string& customLibraryName);
    ~VulkanDriver();
    VulkanDriver(const VulkanDriver&) = delete;
    VulkanDriver& operator=(const VulkanDriver&) = delete;
    void* symbol(const char* name) const;

private:
    void* handle_ = nullptr;
};
}
