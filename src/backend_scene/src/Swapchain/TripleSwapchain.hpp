#pragma once
#include <atomic>
#include <cstdint>
#include "Core/NoCopyMove.hpp"

namespace wallpaper
{

template<typename T>
class TripleSwapchain : NoCopy, NoMove {
public:
    virtual ~TripleSwapchain() = default;

    // Consumer side. Returns the latest published frame if it differs from the
    // caller's last_seen id, otherwise nullptr. Multiple consumers may call this
    // concurrently, each holding its own last_seen, so one rendered frame can be
    // displayed on several screens (mirroring). The producer never renders into
    // the published slot, so the returned handle stays valid while the next frame
    // renders into a different slot.
    T* eatFrame(uint64_t& last_seen) {
        uint64_t cur = m_pub_id.load(std::memory_order_acquire);
        if (cur == last_seen) return nullptr;
        last_seen = cur;
        return presented().load(std::memory_order_acquire);
    }

    // Producer side. Publishes the just-rendered (inprogress) slot and rotates the
    // previously published slot into a one-frame cooldown before it can be reused
    // as the next render target.
    void renderFrame() {
        T* just  = inprogress().load(std::memory_order_relaxed);
        T* spare = ready().load(std::memory_order_relaxed);
        T* pub   = presented().load(std::memory_order_relaxed);
        inprogress().store(spare, std::memory_order_relaxed);
        ready().store(pub, std::memory_order_relaxed);
        presented().store(just, std::memory_order_release);
        m_pub_id.fetch_add(1, std::memory_order_release);
    }
    T* getInprogress() { return inprogress().load(std::memory_order_relaxed); }

    virtual uint width() const  = 0;
    virtual uint height() const = 0;

protected:
    TripleSwapchain() = default;

    virtual std::atomic<T*>& presented()  = 0;
    virtual std::atomic<T*>& ready()      = 0;
    virtual std::atomic<T*>& inprogress() = 0;

private:
    std::atomic<uint64_t> m_pub_id { 0 };
};

} // namespace wallpaper
