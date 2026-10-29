#pragma once
#include "Instance.hpp"
#include <span>

namespace wallpaper
{
namespace vulkan
{
struct ImageParameters;
struct VmaImageParameters;

class Device;
class Swapchain {
public:
    // `want_readback` adds VK_IMAGE_USAGE_TRANSFER_SRC_BIT to the images, but
    // only when the surface supports it; supportsReadback() reports the outcome.
    static bool Create(Device&, VkSurfaceKHR, VkExtent2D, Swapchain&, bool want_readback = false);
    const vvk::SwapchainKHR&         handle() const;
    VkFormat                         format() const;
    VkExtent2D                       extent() const;
    VkPresentModeKHR                 presentMode() const;
    std::span<const ImageParameters> images() const;
    bool                             supportsReadback() const;

private:
    vvk::SwapchainKHR            m_handle;
    VkSurfaceFormatKHR           m_format;
    VkExtent2D                   m_extent;
    VkPresentModeKHR             m_present_mode;
    bool                         m_readback { false };
    std::vector<ImageParameters> m_images;
    std::vector<vvk::ImageView>  m_imageviews;
};
} // namespace vulkan
} // namespace wallpaper
