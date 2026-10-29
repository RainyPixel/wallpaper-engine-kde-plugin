#pragma once
#include "Core/MapSet.hpp"
#include "Core/NoCopyMove.hpp"
#include "Image.hpp"
#include "Vulkan/Parameters.hpp"

#include <memory>
#include <string>

namespace wallpaper
{
namespace fs
{
class VFS;
}
namespace vulkan
{
class Device;
class VideoTexture;

// The live video textures of one renderer (screen), keyed by texture name.
//
// A video texture is an ordinary sampled texture of its layer, so the layer
// keeps its geometry, transforms, effects and draw order. Its frames come from
// the embedded MP4/WebM, decoded on a worker thread and chosen by scene time:
// the video pauses, changes speed and restarts together with the scene, never
// plays audio, and is recreated whenever the render graph is rebuilt.
//
// Whenever a video cannot be decoded (no FFmpeg at build time, unsupported
// codec, corrupt payload) Acquire() returns nullptr and the layer keeps the
// transparent placeholder from WPTexImageParser. A later decode failure clears
// the displayed frame back to transparent until the graph is rebuilt.
class VideoTextures : NoCopy, NoMove {
public:
    explicit VideoTextures(const Device&);
    ~VideoTextures();

    // The texture for `name`, created and started on first use.
    const ImageSlotsRef* Acquire(const std::string& name, fs::VFS&, const EmbeddedVideo&,
                                 const TextureSample&);

    // Records uploads of the frames due at scene time `t`. Must precede every
    // pass that samples them in the same command buffer, and the previous
    // frame's command buffer must have completed (the frame fence was waited).
    void RecordUpdates(vvk::CommandBuffer&, double t);

    bool Empty() const;

    // Drops all textures. The device must be idle.
    void Clear();

private:
    const Device&                                   m_device;
    Map<std::string, std::unique_ptr<VideoTexture>> m_textures;
};

} // namespace vulkan
} // namespace wallpaper
