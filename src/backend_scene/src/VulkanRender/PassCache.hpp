#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace wallpaper
{
namespace vulkan
{

// Real GPU writers of one render-target name. An empty name is not a target.
// A missing name has zero writers, which is not enough to cache.
inline void CountRealTargetWrite(std::unordered_map<std::string, int>& counts,
                                 std::string_view                      name) {
    if (name.empty()) return;
    counts[std::string(name)] += 1;
}

inline int RealTargetWriteCount(const std::unordered_map<std::string, int>& counts,
                                std::string_view                            name) {
    const auto it = counts.find(std::string(name));
    if (it == counts.end()) return 0;
    return it->second;
}

// A pass may be skipped after its first frame only when nothing else writes its
// render target. Several graph versions of one name still share one GPU image,
// so a second writer (ping-pong copy, later effect pass) overwrites the pinned
// result and the skipped producer leaves that image empty.
inline bool PassOutputFrameStatic(bool cache_enabled, bool geometry_static, bool uses_time,
                                  bool inputs_static, bool reusable_target, int writer_count) {
    return cache_enabled && geometry_static && ! uses_time && inputs_static && reusable_target &&
           writer_count == 1;
}

// Existing-key TextureCache::Query. A reader passes false for a reusable target
// after the writer pinned the image. Dropping that pin lets MarkShareReady
// recycle the VkImage while the sole writer still skips.
inline bool LatchQueryPersist(bool held, bool requested) { return held || requested; }

} // namespace vulkan
} // namespace wallpaper
