#include "Parameters.hpp"

#include <unistd.h>

namespace wallpaper
{
namespace vulkan
{

VmaBufferParameters::VmaBufferParameters()  = default;
VmaBufferParameters::~VmaBufferParameters() = default;
VmaBufferParameters::VmaBufferParameters(VmaBufferParameters&& o) noexcept
    : handle(std::move(o.handle)), req_size(o.req_size) {};
VmaBufferParameters& VmaBufferParameters::operator=(VmaBufferParameters&& o) noexcept {
    handle   = std::move(o.handle);
    req_size = o.req_size;
    return *this;
};

VmaImageParameters::VmaImageParameters()  = default;
VmaImageParameters::~VmaImageParameters() = default;
VmaImageParameters::VmaImageParameters(VmaImageParameters&& o) noexcept
    : handle(std::move(o.handle)),
      view(std::move(o.view)),
      sampler(std::move(o.sampler)),
      extent(o.extent),
      mipmap_level(o.mipmap_level) {}
VmaImageParameters& VmaImageParameters::operator=(VmaImageParameters&& o) noexcept {
    handle       = std::move(o.handle);
    view         = std::move(o.view);
    sampler      = std::move(o.sampler);
    extent       = o.extent;
    mipmap_level = o.mipmap_level;
    return *this;
}

ExImageParameters::ExImageParameters() = default;
ExImageParameters::~ExImageParameters() {
    if (fd > 0) ::close(fd);
}
ExImageParameters::ExImageParameters(ExImageParameters&& o) noexcept
    : mem(std::move(o.mem)),
      mem_reqs(o.mem_reqs),
      handle(std::move(o.handle)),
      view(std::move(o.view)),
      sampler(std::move(o.sampler)),
      extent(o.extent),
      mipmap_level(o.mipmap_level),
      fd(std::exchange(o.fd, 0)) {}
ExImageParameters& ExImageParameters::operator=(ExImageParameters&& o) noexcept {
    mem          = std::move(o.mem);
    mem_reqs     = o.mem_reqs;
    handle       = std::move(o.handle);
    view         = std::move(o.view);
    sampler      = std::move(o.sampler);
    extent       = o.extent;
    mipmap_level = o.mipmap_level;
    fd           = std::exchange(o.fd, 0);
    return *this;
}

ImageSlots::ImageSlots()  = default;
ImageSlots::~ImageSlots() = default;
ImageSlots::ImageSlots(ImageSlots&& o) noexcept: slots(std::move(o.slots)) {}
ImageSlots& ImageSlots::operator=(ImageSlots&& o) noexcept {
    slots = std::move(o.slots);
    return *this;
}

ImageSlotsRef::ImageSlotsRef()  = default;
ImageSlotsRef::~ImageSlotsRef() = default;
ImageSlotsRef::ImageSlotsRef(const ImageSlots& o)
    : slots(std::vector<ImageParameters>(o.slots.begin(), o.slots.end())) {}

} // namespace vulkan
} // namespace wallpaper
