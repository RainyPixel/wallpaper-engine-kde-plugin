#include "VideoTexture.hpp"

#include "Fs/VFS.h"
#include "Utils/Logging.h"
#include "Vulkan/Device.hpp"
#include "Vulkan/TextureCache.hpp"
#include "Core/ArrayHelper.hpp"

#if WEKDE_HAVE_FFMPEG
#    include "Video/VideoDecoder.hpp"
#endif

using namespace wallpaper;
using namespace wallpaper::vulkan;

namespace wallpaper::vulkan
{

#if WEKDE_HAVE_FFMPEG

class VideoTexture : NoCopy, NoMove {
public:
    // Builds the GPU image and staging slots and starts the decoder. Returns
    // nullptr with the reason in *err when the video cannot be played.
    static std::unique_ptr<VideoTexture> Create(const Device& device, fs::VFS& vfs,
                                                const EmbeddedVideo& src,
                                                const TextureSample& sample, std::string* err) {
        std::unique_ptr<VideoTexture> tex(new VideoTexture());
        tex->m_decoder = video::VideoDecoder::Open(vfs.Open(src.path), src.offset, src.size, err);
        if (! tex->m_decoder) return nullptr;
        const u32 w = (u32)tex->m_decoder->Width(), h = (u32)tex->m_decoder->Height();

        const VkSamplerCreateInfo sampler_info {
            .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext                   = nullptr,
            .magFilter               = ToVkType(sample.magFilter),
            .minFilter               = ToVkType(sample.minFilter),
            .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU            = ToVkType(sample.wrapS),
            .addressModeV            = ToVkType(sample.wrapT),
            .addressModeW            = ToVkType(sample.wrapT),
            .anisotropyEnable        = false,
            .maxAnisotropy           = 1.0f,
            .compareEnable           = false,
            .compareOp               = VK_COMPARE_OP_NEVER,
            .minLod                  = 0.0f,
            .maxLod                  = 0.0f,
            .borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
            .unnormalizedCoordinates = false,
        };
        auto image = CreateImage(device,
                                 VkExtent3D { w, h, 1 },
                                 1,
                                 VK_FORMAT_R8G8B8A8_UNORM,
                                 sampler_info,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (! image) {
            if (err) *err = "cannot allocate the video image";
            return nullptr;
        }
        tex->m_image = std::move(*image);

        // One slot on display, two being decoded ahead.
        constexpr usize            slot_count = 3;
        const VkDeviceSize         bytes      = (VkDeviceSize)w * h * 4;
        std::vector<std::uint8_t*> ptrs;
        for (usize i = 0; i < slot_count; i++) {
            VmaBufferParameters      buf;
            const VkBufferCreateInfo ci {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .size  = bytes,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            };
            VmaAllocationCreateInfo vma_info {};
            vma_info.usage = VMA_MEMORY_USAGE_CPU_ONLY; // host visible + coherent
            void* mapped   = nullptr;
            if (vvk::CreateBuffer(device.vma_allocator(), ci, vma_info, buf.handle) != VK_SUCCESS ||
                buf.handle.MapMemory(&mapped) != VK_SUCCESS || mapped == nullptr) {
                if (err) *err = "cannot allocate video staging memory";
                return nullptr;
            }
            buf.req_size = (std::size_t)bytes;
            ptrs.push_back(static_cast<std::uint8_t*>(mapped));
            tex->m_staging.push_back(std::move(buf));
        }
        if (! tex->m_decoder->Start(std::move(ptrs))) {
            if (err) *err = "cannot start the video decoder";
            return nullptr;
        }
        tex->m_slots.slots = { ImageParameters(tex->m_image) };
        LOG_INFO("video texture: playing %s", tex->m_decoder->Description().c_str());
        return tex;
    }

    ~VideoTexture() {
        // Stop the worker before its target memory is unmapped.
        m_decoder.reset();
        for (auto& buf : m_staging) buf.handle.UnMapMemory();
    }

    const ImageSlotsRef& Slots() const { return m_slots; }

    void RecordUpdate(vvk::CommandBuffer& cmd, double t) {
        if (m_failed) return;
        const bool failed = m_decoder->Failed();
        const int  slot   = failed ? -1 : m_decoder->Acquire(t);
        if (! failed && m_ready && slot < 0) return;

        const VkImageSubresourceRange range {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        };
        VkImageMemoryBarrier to_dst {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext         = nullptr,
            .srcAccessMask = m_ready ? (VkAccessFlags)VK_ACCESS_SHADER_READ_BIT : 0u,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout =
                m_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = *m_image.handle,
            .subresourceRange    = range,
        };
        cmd.PipelineBarrier(m_ready ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                    : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0,
                            {},
                            {},
                            spanone { to_dst });

        if (slot >= 0) {
            VkBufferImageCopy region {
                .bufferOffset      = 0,
                .bufferRowLength   = 0,
                .bufferImageHeight = 0,
                .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset       = { 0, 0, 0 },
                .imageExtent       = m_image.extent,
            };
            cmd.CopyBufferToImage(*m_staging[(usize)slot].handle,
                                  *m_image.handle,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  spanone { region });
        } else {
            // A failed decoder also clears any previously displayed frame.
            VkClearColorValue clear {};
            cmd.ClearColorImage(
                *m_image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, spanone { range });
        }

        VkImageMemoryBarrier to_read = to_dst;
        to_read.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
        to_read.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0,
                            {},
                            {},
                            spanone { to_read });
        m_ready  = true;
        m_failed = failed;
        if (failed) LOG_ERROR("video texture: decoding stopped; the layer is now transparent");
    }

private:
    VideoTexture() = default;

    std::unique_ptr<video::VideoDecoder> m_decoder;
    VmaImageParameters                   m_image;
    std::vector<VmaBufferParameters>     m_staging;
    ImageSlotsRef                        m_slots;
    bool                                 m_ready { false };
    bool                                 m_failed { false };
};

#else

// Built without FFmpeg: no VideoTexture is ever created (Acquire() stores
// nullptr), so these are never called; they only satisfy the shared code.
class VideoTexture {
public:
    const ImageSlotsRef& Slots() const { return m_slots; }
    void                 RecordUpdate(vvk::CommandBuffer&, double) {}

private:
    ImageSlotsRef m_slots;
};

#endif

} // namespace wallpaper::vulkan

VideoTextures::VideoTextures(const Device& device): m_device(device) {}
VideoTextures::~VideoTextures() = default;

bool VideoTextures::Empty() const { return m_textures.empty(); }

const ImageSlotsRef* VideoTextures::Acquire(const std::string& name, fs::VFS& vfs,
                                            const EmbeddedVideo& src, const TextureSample& sample) {
    if (auto it = m_textures.find(name); it != m_textures.end())
        return it->second ? &it->second->Slots() : nullptr;
#if WEKDE_HAVE_FFMPEG
    std::string err;
    auto        tex = VideoTexture::Create(m_device, vfs, src, sample, &err);
    if (! tex)
        LOG_ERROR(
            "video texture \"%s\": %s; the layer stays transparent", name.c_str(), err.c_str());
#else
    (void)vfs;
    (void)src;
    (void)sample;
    (void)m_device;
    std::unique_ptr<VideoTexture> tex;
    LOG_INFO("video texture \"%s\": built without FFmpeg; the layer stays transparent",
             name.c_str());
#endif
    // Failures are remembered too, so a broken video is reported once per graph.
    auto& slot = m_textures[name];
    slot       = std::move(tex);
    return slot ? &slot->Slots() : nullptr;
}

void VideoTextures::RecordUpdates(vvk::CommandBuffer& cmd, double t) {
    for (auto& [name, tex] : m_textures)
        if (tex) tex->RecordUpdate(cmd, t);
}

void VideoTextures::Clear() { m_textures.clear(); }
