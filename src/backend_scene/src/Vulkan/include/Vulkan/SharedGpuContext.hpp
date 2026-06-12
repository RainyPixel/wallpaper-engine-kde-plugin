#pragma once
#include "Instance.hpp"
#include "Device.hpp"

#include <functional>
#include <memory>

namespace wallpaper
{
namespace vulkan
{

struct GpuContextCreateInfo {
    std::span<const Extension>     inst_exts;
    std::span<const InstanceLayer> inst_layers;
    std::span<const Extension>     device_exts;
    VkExtent2D                     extent { 1, 1 };
    std::span<const std::uint8_t>  uuid;
    bool                           offscreen { true };
    bool                           share_enabled { true };
    bool                           enable_valid_layer { false };
    // Creates a surface on the freshly built instance. Empty for offscreen.
    std::function<VkResult(VkInstance, VkSurfaceKHR*)> create_surface;
};

// Owns a VkInstance + VkDevice (and its VMA allocator / asset cache). When GPU
// sharing is enabled, every offscreen renderer on the same physical GPU shares
// one instance. Held through a shared_ptr so the context tears down only after
// the last screen releases it.
class SharedGpuContext : NoCopy, NoMove {
public:
    SharedGpuContext() = default;
    ~SharedGpuContext();

    Instance& instance() { return m_instance; }
    Device&   device() { return m_device; }

    static std::shared_ptr<SharedGpuContext> Create(const GpuContextCreateInfo&);

private:
    Instance m_instance;
    Device   m_device;
    bool     m_device_ready { false };
};

// Returns a shared context for the given GPU UUID when sharing is enabled and
// the renderer is offscreen; otherwise returns a fresh, unshared context. The
// WP_SHARE_GPU env var overrides the request (set to 0 to force off).
std::shared_ptr<SharedGpuContext> AcquireGpuContext(const GpuContextCreateInfo&);

} // namespace vulkan
} // namespace wallpaper
