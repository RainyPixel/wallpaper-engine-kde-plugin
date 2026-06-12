#include "SharedGpuContext.hpp"

#include "Utils/Logging.h"
#include "Core/MapSet.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

using namespace wallpaper::vulkan;

namespace
{
std::string UuidHex(std::span<const std::uint8_t> uuid) {
    std::string out;
    out.reserve(uuid.size() * 2);
    char buf[3];
    for (auto b : uuid) {
        std::snprintf(buf, sizeof(buf), "%02x", (unsigned)b);
        out.append(buf, 2);
    }
    return out;
}
} // namespace

SharedGpuContext::~SharedGpuContext() {
    if (m_device_ready && m_device.handle()) {
        VVK_CHECK(m_device.handle().WaitIdle());
        m_device.Destroy();
    }
    m_instance.Destroy();
}

std::shared_ptr<SharedGpuContext> SharedGpuContext::Create(const GpuContextCreateInfo& ci) {
    auto ctx = std::make_shared<SharedGpuContext>();

    if (! Instance::Create(ctx->m_instance, ci.inst_exts, ci.inst_layers)) {
        LOG_ERROR("init vulkan failed");
        return nullptr;
    }
    if (ci.create_surface) {
        VkSurfaceKHR surface;
        VVK_CHECK_ACT(
            {
                LOG_ERROR("create vulkan surface failed");
                return nullptr;
            },
            ci.create_surface(*ctx->m_instance.inst(), &surface));
        ctx->m_instance.setSurface(VkSurfaceKHR(surface));
    }
    {
        auto       surface     = *ctx->m_instance.surface();
        auto       device_exts = ci.device_exts;
        const auto check_gpu   = [device_exts, surface](const vvk::PhysicalDevice& gpu) {
            return Device::CheckGPU(gpu, device_exts, surface);
        };
        if (! ctx->m_instance.ChoosePhysicalDevice(check_gpu, ci.uuid)) return nullptr;
    }
    if (! Device::Create(ctx->m_instance, ci.device_exts, ci.extent, ctx->m_device)) {
        LOG_ERROR("init vulkan device failed");
        return nullptr;
    }
    ctx->m_device_ready = true;
    return ctx;
}

bool wallpaper::vulkan::GpuSharingEnabled() {
    static const bool enabled = std::getenv("WP_SHARE_GPU") != nullptr;
    return enabled;
}

std::shared_ptr<SharedGpuContext>
wallpaper::vulkan::AcquireGpuContext(const GpuContextCreateInfo& ci) {
    // Surface renderers (the standalone viewer) and the disabled path keep their
    // own private context — never cached, so topology matches the unshared build.
    if (! ci.offscreen || ! GpuSharingEnabled() || ci.uuid.empty()) {
        return SharedGpuContext::Create(ci);
    }

    static std::mutex                                        mtx;
    static Map<std::string, std::weak_ptr<SharedGpuContext>> cache;

    std::lock_guard<std::mutex> lk(mtx);
    const std::string           key = UuidHex(ci.uuid);
    if (auto it = cache.find(key); it != cache.end()) {
        if (auto sp = it->second.lock()) {
            LOG_INFO("reuse shared gpu context for uuid %s", key.c_str());
            return sp;
        }
    }
    auto sp = SharedGpuContext::Create(ci);
    if (sp) cache[key] = sp;
    return sp;
}
