#pragma once
#include "Instance.hpp"
#include "Swapchain.hpp"
#include "vk_mem_alloc.h"
#include "Parameters.hpp"
#include "TextureCache.hpp"

namespace wallpaper
{
namespace vulkan
{

class PipelineParameters;

class Device : NoCopy, NoMove {
public:
    Device();
    ~Device();

    static bool Create(Instance&, std::span<const Extension> exts, VkExtent2D extent, Device&);
    static bool CheckGPU(vvk::PhysicalDevice gpu, std::span<const Extension> exts,
                         VkSurfaceKHR surface);

    void Destroy();

    const auto& graphics_queue() const { return m_graphics_queue; }
    const auto& present_queue() const { return m_present_queue; }
    const auto& device() const { return m_device; }
    const auto& handle() const { return m_device; }
    const auto& gpu() const { return m_gpu; }
    const auto& limits() const { return m_limits; }
    const auto& vma_allocator() const { return *m_allocator; }
    const auto& cmd_pool() const { return m_command_pool; }
    const auto& swapchain() const { return m_swapchain; }

    bool supportExt(std::string_view) const;

    AssetCache& asset_cache() const { return *m_asset_cache; }

    VkDeviceSize GetUsage() const;

private:
    std::vector<VkDeviceQueueCreateInfo> ChooseDeviceQueue(VkSurfaceKHR = {});

    vvk::DeviceDispatch     dld;
    vvk::Device             m_device;
    vvk::PhysicalDevice     m_gpu;
    vvk::VmaAllocatorHandle m_allocator;

    VkPhysicalDeviceLimits m_limits;
    Set<std::string>       m_extensions;

    Swapchain m_swapchain;

    vvk::CommandPool m_command_pool;

    QueueParameters m_graphics_queue;
    QueueParameters m_present_queue;

    std::unique_ptr<AssetCache> m_asset_cache;
};

} // namespace vulkan
} // namespace wallpaper
