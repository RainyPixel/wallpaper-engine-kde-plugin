#include <doctest.h>

#include "VulkanRender/PassCache.hpp"

#include <string>
#include <unordered_map>

using wallpaper::vulkan::CountRealTargetWrite;
using wallpaper::vulkan::LatchQueryPersist;
using wallpaper::vulkan::PassOutputFrameStatic;
using wallpaper::vulkan::RealTargetWriteCount;

TEST_SUITE("PassCache") {
    TEST_CASE("a static single-writer pass is cached") {
        CHECK(PassOutputFrameStatic(true, true, false, true, true, 1));
    }

    TEST_CASE("a second writer disables the cache") {
        // Ping-pong copies and later composites share the GPU image. Caching the
        // first writer leaves that image black after they overwrite it.
        CHECK_FALSE(PassOutputFrameStatic(true, true, false, true, true, 2));
        CHECK_FALSE(PassOutputFrameStatic(true, true, false, true, true, 0));
    }

    TEST_CASE("the other gates still apply") {
        CHECK_FALSE(PassOutputFrameStatic(false, true, false, true, true, 1));
        CHECK_FALSE(PassOutputFrameStatic(true, false, false, true, true, 1));
        CHECK_FALSE(PassOutputFrameStatic(true, true, true, true, true, 1));
        CHECK_FALSE(PassOutputFrameStatic(true, true, false, false, true, 1));
        CHECK_FALSE(PassOutputFrameStatic(true, true, false, true, false, 1));
    }

    TEST_CASE("writer counts ignore empty names and treat a missing name as zero") {
        std::unordered_map<std::string, int> counts;
        CountRealTargetWrite(counts, "");
        CHECK(counts.empty());
        CHECK(RealTargetWriteCount(counts, "_rt_blur") == 0);

        CountRealTargetWrite(counts, "_rt_blur");
        CHECK(RealTargetWriteCount(counts, "_rt_blur") == 1);
        CHECK(PassOutputFrameStatic(
            true, true, false, true, true, RealTargetWriteCount(counts, "_rt_blur")));
        CountRealTargetWrite(counts, "_rt_blur");
        CHECK(RealTargetWriteCount(counts, "_rt_blur") == 2);
        CHECK_FALSE(PassOutputFrameStatic(
            true, true, false, true, true, RealTargetWriteCount(counts, "_rt_blur")));
        CHECK(RealTargetWriteCount(counts, "_rt_only") == 0);
        CHECK_FALSE(PassOutputFrameStatic(
            true, true, false, true, true, RealTargetWriteCount(counts, "_rt_only")));
    }

    TEST_CASE("a later query of the same target keeps a frame-static pin") {
        // Reader Query of a reusable target asks not to pin. The image the
        // writer already pinned must stay pinned, or MarkShareReady recycles it.
        CHECK(LatchQueryPersist(true, false));
        CHECK(LatchQueryPersist(true, true));
        // The writer can still pin an image that was not pinned yet.
        CHECK(LatchQueryPersist(false, true));
        // Nobody pinned it, so a reader leaves it recyclable.
        CHECK_FALSE(LatchQueryPersist(false, false));
    }
}
