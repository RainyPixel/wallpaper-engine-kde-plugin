#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "Core/MapSet.hpp"
#include "Vulkan/VulkanExSwapchain.hpp"
#include "Vulkan/SharedGpuContext.hpp"

namespace wallpaper
{
namespace vulkan
{

using ScreenToken = std::uint64_t;

enum class MirrorRole
{
    Primary,
    Secondary
};

// One mirror group: the primary renders into `swapchain`, secondaries import it.
// Held through a shared_ptr by every member so it (and the swapchain) outlives the
// primary's renderer until the last secondary drops it.
struct MirrorSlot {
    std::mutex                         mtx;
    std::shared_ptr<VulkanExSwapchain> swapchain; // null until the primary publishes
    // The primary's GPU context owns the swapchain images/memory. Held here so it
    // outlives the primary renderer while a secondary still displays its frames
    // (matters when GPU sharing is off and the primary has a private device).
    std::shared_ptr<SharedGpuContext> gpu;
    ScreenToken                       primary { 0 };
    std::atomic<bool>                 primary_alive { true };
};

// Process-global registry of mirror groups keyed by a render-identity string
// (gpu uuid + scene + resolution + fillmode + speed + user props). Only built for
// scenes that are not mouse-dependent, so a single rendered frame is correct on
// every member screen.
class MirrorRegistry {
public:
    static MirrorRegistry& Instance();

    // Claims `key` for `token`. Becomes primary when the key is unclaimed or its
    // previous primary has died; otherwise secondary. Always returns the shared slot.
    std::pair<MirrorRole, std::shared_ptr<MirrorSlot>> acquire(const std::string& key,
                                                               ScreenToken        token);

    // Primary publishes its swapchain (and the context owning it) into the slot.
    void publish(const std::shared_ptr<MirrorSlot>& slot, std::shared_ptr<VulkanExSwapchain> sc,
                 std::shared_ptr<SharedGpuContext> gpu);

    // Primary leaving (teardown or scene change): mark the group dead and drop the
    // key so surviving secondaries re-elect a new primary on their next tick.
    void release(const std::string& key, ScreenToken token);

private:
    MirrorRegistry() = default;

    std::mutex                                    m_mtx;
    Map<std::string, std::shared_ptr<MirrorSlot>> m_slots;
};

} // namespace vulkan
} // namespace wallpaper
