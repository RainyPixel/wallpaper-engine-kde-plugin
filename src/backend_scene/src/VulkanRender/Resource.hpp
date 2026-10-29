#pragma once
#include "Core/NoCopyMove.hpp"
#include "Vulkan/StagingBuffer.hpp"
#include "Vulkan/TextureCache.hpp"
#include <memory>
#include <vector>

namespace wallpaper
{
namespace vulkan
{

class VideoTextures;

struct RenderingResources {
    vvk::CommandBuffer command;

    vvk::Semaphore              sem_swap_wait_image;
    std::vector<vvk::Semaphore> sem_swap_finish;
    vvk::Fence                  fence_frame;

    StagingBuffer* vertex_buf;
    StagingBuffer* dyn_buf;

    // Per-screen render-target pool and this screen's asset-cache token.
    TextureCache* rt_pool { nullptr };
    ScreenToken   screen_token { 0 };

    // This screen's live video textures.
    VideoTextures* videos { nullptr };
};
} // namespace vulkan
} // namespace wallpaper
