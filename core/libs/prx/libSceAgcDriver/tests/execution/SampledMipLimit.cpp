#include "prx/libSceAgcDriver/Execution/include/BdaFeatures.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <SDL_loadso.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <bit>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

constexpr int Skipped = 77;

PFN_vkGetDeviceProcAddr realDeviceProc = nullptr;
PFN_vkCreateImage realCreateImage = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL CheckedCreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocation, VkImage* image) {
    const auto maximum = std::bit_width(std::max({info->extent.width, info->extent.height, info->extent.depth}));
    if (info->mipLevels > maximum) {
        std::fprintf(stderr, "Rejected invalid Vulkan image: %ux%ux%u with %u mips (maximum %u)\n", info->extent.width, info->extent.height, info->extent.depth, info->mipLevels, maximum);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    return realCreateImage(device, info, allocation, image);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CountingDeviceProc(VkDevice device, const char* name) {
    const auto function = realDeviceProc(device, name);
    if (function == nullptr) return nullptr;
    if (std::strcmp(name, "vkCreateImage") == 0) {
        realCreateImage = reinterpret_cast<PFN_vkCreateImage>(function);
        return reinterpret_cast<PFN_vkVoidFunction>(&CheckedCreateImage);
    }
    return function;
}

class Device {
public:
    Device() {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        Require(library != nullptr, "cannot load Vulkan");
        try {
            instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
            Require(instanceProc != nullptr, "missing Vulkan instance resolver");
            VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &application;
            Check(function<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance), "vkCreateInstance");
            std::uint32_t count = 0;
            const auto enumerate = function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
            Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
            Require(count != 0, "no Vulkan device");
            std::vector<VkPhysicalDevice> devices(count);
            Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
            context.physical = devices.front();
            const auto extensions = function<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
            Check(extensions(context.physical, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties");
            std::vector<VkExtensionProperties> available(count);
            Check(extensions(context.physical, nullptr, &count, available.data()), "vkEnumerateDeviceExtensionProperties");
            auto bytes = AgcDriver::QueryBdaByteFeatures(context.physical, function<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), available);
            auto address = AgcDriver::QueryBdaFeatures(context.physical, function<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), available);
            const auto queues = function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
            queues(context.physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            queues(context.physical, &count, families.data());
            constexpr VkQueueFlags needed = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            std::uint32_t family = 0;
            while (family < count && (families[family].queueFlags & needed) != needed) ++family;
            Require(family < count, "no Vulkan graphics and compute queue");
            const float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkPhysicalDeviceFeatures enabled{};
            enabled.shaderInt64 = VK_TRUE;
            enabled.textureCompressionBC = VK_TRUE;
            address.pNext = &bytes;
            const std::array<const char*, 2> extensionsEnabled{VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_8BIT_STORAGE_EXTENSION_NAME};
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &address};
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos = &queue;
            device.enabledExtensionCount = static_cast<std::uint32_t>(extensionsEnabled.size());
            device.ppEnabledExtensionNames = extensionsEnabled.data();
            device.pEnabledFeatures = &enabled;
            Check(function<PFN_vkCreateDevice>("vkCreateDevice")(context.physical, &device, nullptr, &context.device), "vkCreateDevice");
            realDeviceProc = function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
            context.deviceProc = &CountingDeviceProc;
            function<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(context.physical, &context.memory);
            VkPhysicalDeviceProperties properties{};
            function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &properties);
            context.limits = properties.limits;
            context.bufferDeviceAddress = true;
            context.formatProperties = function<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
            context.Function<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(context.device, family, 0, &context.queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = family;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &pool, nullptr, &context.pool), "vkCreateCommandPool");
        } catch (...) {
            release();
            throw;
        }
    }

    ~Device() { release(); }
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    const Context& GetContext() const { return context; }

private:
    template<typename TFunction>
    TFunction function(const char* name) const {
        const auto result = reinterpret_cast<TFunction>(instanceProc(instance, name));
        Require(result != nullptr, name);
        return result;
    }

    void release() noexcept {
        if (context.device != VK_NULL_HANDLE) ClearDepthSurfaces(context.device);
        if (context.pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, context.pool, nullptr);
        context.bufferPool.reset();
        if (context.device != VK_NULL_HANDLE) function<PFN_vkDestroyDevice>("vkDestroyDevice")(context.device, nullptr);
        if (instance != VK_NULL_HANDLE) function<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (library != nullptr) SDL_UnloadObject(library);
    }

    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    Context context{};
};

}
int main() {
    try {
        std::optional<Device> device;
        try {
            device.emplace();
        } catch (const std::exception& error) {
            if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) throw;
            std::printf("skipped, no usable Vulkan device: %s\n", error.what());
            return Skipped;
        }
        {
            auto context = device->GetContext();
            context.textureCompressionBC = true;
            TextureDetiler detiler(context);
            GuestTextureResource resource{};
            resource.baseAddress = 0x1000000000ull;
            resource.width = 32;
            resource.height = 1;
            resource.depthOrLastArray = 63;
            resource.dimension = TextureDimension::k2DArray;
            resource.tileMode = TextureTileMode::kStandard64KB;
            resource.mipCount = 10;
            resource.allocatedMipCount = 10;
            resource.lastLevel = 5;
            resource.dstSelX = 4;
            resource.dstSelY = 5;
            resource.dstSelZ = 6;
            resource.dstSelW = 7;
            const VkComponentMapping components{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            for (const auto format : {174u, 181u, 20u}) {
                resource.format = format;
                const auto geometry = DescribeSurface(resource);
                std::vector<std::byte> snapshot(static_cast<std::size_t>(geometry.guestBytes));
                Texture texture(context, detiler, resource, components, snapshot);
                const auto range = texture.SampledViewRange(false);
                Require(texture.View() != VK_NULL_HANDLE && range.levels == 6 && range.layers == 64, "the valid mip range of a surface with unused tail levels was not preserved");
            }
            resource.lastLevel = 9;
            const auto geometry = DescribeSurface(resource);
            std::vector<std::byte> snapshot(static_cast<std::size_t>(geometry.guestBytes));
            bool rejected = false;
            try {
                Texture texture(context, detiler, resource, components, snapshot);
            } catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("outside its dimensional chain") != std::string::npos;
            }
            Require(rejected, "a view requiring mip levels outside the dimensional chain was not rejected");
        }
        device.reset();
        std::puts("sampled mip limit tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
