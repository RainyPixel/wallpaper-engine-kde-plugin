#pragma once

#include "Parameters.hpp"
#include "Type.hpp"
#include "Core/NoCopyMove.hpp"
#include "Core/MapSet.hpp"

#include <cstdint>
#include <mutex>

namespace wallpaper
{

class Image;

namespace vulkan
{

// Stable id identifying the renderer (screen) that owns a reference to a shared
// asset texture.
using ScreenToken = std::uint64_t;

VkFormat             ToVkType(TextureFormat);
VkSamplerAddressMode ToVkType(TextureWrap);
VkFilter             ToVkType(TextureFilter);

enum class TexUsage
{
    COLOR,
    DEPTH
};

using TexHash = std::size_t;

struct TextureKey {
    i32           width;
    i32           height;
    TexUsage      usage;
    TextureFormat format;
    TextureSample sample;
    uint          mipmap_level { 1 };

    static TexHash HashValue(const TextureKey&);
};

// Per-screen pool of render targets and external-memory swapchain images. These
// scale with resolution and are never shared between screens.
class TextureCache : NoCopy, NoMove {
public:
    // cmd_pool is the per-screen command pool used for transient layout/copy
    // commands, so screens sharing a device never touch the same pool.
    TextureCache(const Device&, const vvk::CommandPool& cmd_pool);
    ~TextureCache();

    void Clear();

    std::optional<ExImageParameters> CreateExTex(uint32_t witdh, uint32_t height, VkFormat,
                                                 VkImageTiling);

    std::optional<ImageParameters> Query(std::string_view key, TextureKey content_hash,
                                         bool persist = false);

    void MarkShareReady(std::string_view key);

    void RecGenerateMipmaps(vvk::CommandBuffer& cmd, const ImageParameters& image) const;

private:
    std::optional<VmaImageParameters> CreateTex(TextureKey);
    void                              allocateCmd();
    vvk::CommandBuffers               m_tex_cmds;
    vvk::CommandBuffer                m_tex_cmd;

    const Device&           m_device;
    const vvk::CommandPool& m_cmd_pool;

    struct QueryTex {
        idx                index { 0 };
        bool               share_ready { false };
        bool               persist { false };
        TexHash            content_hash;
        VmaImageParameters image;
        Set<std::string>   query_keys;
    };
    std::vector<std::unique_ptr<QueryTex>> m_query_texs;
    Map<std::string, QueryTex*>            m_query_map;
};

// Process-wide (per shared Device) cache of decoded asset textures keyed by
// image key. Reference-counted by screen so the textures survive as long as any
// screen still references them — this is what dedupes VRAM across monitors.
class AssetCache : NoCopy, NoMove {
public:
    AssetCache(const Device&);
    ~AssetCache();

    ImageSlotsRef CreateTexShared(Image&, ScreenToken, std::string_view ns);
    void          ReleaseScreen(ScreenToken);

private:
    void allocateCmd();

    const Device&       m_device;
    std::mutex          m_mutex;
    vvk::CommandBuffers m_tex_cmds;
    vvk::CommandBuffer  m_tex_cmd;

    struct Entry {
        ImageSlots       slots;
        Set<ScreenToken> holders;
    };
    Map<std::string, Entry> m_tex_map;
};

} // namespace vulkan
} // namespace wallpaper
