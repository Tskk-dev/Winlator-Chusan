#include "vulkan_driver.h"

#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <limits.h>
#include <cstdlib>
#include <stdexcept>
#include <unistd.h>

namespace chusan {
VulkanDriver::VulkanDriver(const std::string& hooks, const std::string& files,
                           const std::string& temporary, const std::string& library) {
    if (library.empty()) {
        handle_ = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    } else {
        if (library == "." || library == ".." || library.find('/') != std::string::npos
                || library.find('\\') != std::string::npos || library.size() < 4
                || library.compare(library.size() - 3, 3, ".so") != 0)
            throw std::runtime_error("The custom driver name must be a .so basename");
        char root[PATH_MAX], resolved[PATH_MAX];
        const std::string directory = files + "/driver/";
        if (!realpath(files.c_str(), root) || !realpath((directory + library).c_str(), resolved)
                || std::string(resolved).rfind(std::string(root) + "/", 0) != 0)
            throw std::runtime_error("Custom driver must exist in app-private files/driver");
        for (const char* hook : {"/libmain_hook.so", "/libhook_impl.so"}) {
            if (access((hooks + hook).c_str(), R_OK) != 0)
                throw std::runtime_error("Missing extracted adrenotools hook libraries");
        }
        handle_ = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
                temporary.c_str(), hooks.c_str(), directory.c_str(), library.c_str(), nullptr, nullptr);
    }
    if (!handle_) {
        const char* detail = dlerror();
        throw std::runtime_error(detail ? detail : "Vulkan driver loading failed");
    }
}

VulkanDriver::~VulkanDriver() {
    if (handle_) dlclose(handle_);
}

void* VulkanDriver::symbol(const char* name) const {
    void* address = dlsym(handle_, name);
    if (!address) throw std::runtime_error(std::string("Missing Vulkan entry: ") + name);
    return address;
}
}
