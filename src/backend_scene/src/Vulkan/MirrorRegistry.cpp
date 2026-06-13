#include "Vulkan/MirrorRegistry.hpp"

#include "Utils/Logging.h"

namespace wallpaper
{
namespace vulkan
{

MirrorRegistry& MirrorRegistry::Instance() {
    static MirrorRegistry s_instance;
    return s_instance;
}

std::pair<MirrorRole, std::shared_ptr<MirrorSlot>> MirrorRegistry::acquire(const std::string& key,
                                                                           ScreenToken token) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto                        it = m_slots.find(key);
    if (it != m_slots.end() && it->second->primary_alive.load()) {
        return { MirrorRole::Secondary, it->second };
    }
    auto slot           = std::make_shared<MirrorSlot>();
    slot->primary       = token;
    slot->primary_alive = true;
    m_slots[key]        = slot;
    LOG_INFO("mirror: screen %llu is primary for group %s", (unsigned long long)token, key.c_str());
    return { MirrorRole::Primary, slot };
}

void MirrorRegistry::publish(const std::shared_ptr<MirrorSlot>& slot,
                             std::shared_ptr<VulkanExSwapchain> sc,
                             std::shared_ptr<SharedGpuContext>  gpu) {
    std::lock_guard<std::mutex> lk(slot->mtx);
    slot->swapchain = std::move(sc);
    slot->gpu       = std::move(gpu);
}

void MirrorRegistry::release(const std::string& key, ScreenToken token) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto                        it = m_slots.find(key);
    if (it == m_slots.end() || it->second->primary != token) return;
    it->second->primary_alive.store(false);
    {
        std::lock_guard<std::mutex> slk(it->second->mtx);
        it->second->swapchain.reset();
    }
    m_slots.erase(it);
    LOG_INFO("mirror: primary %llu left group %s", (unsigned long long)token, key.c_str());
}

} // namespace vulkan
} // namespace wallpaper
