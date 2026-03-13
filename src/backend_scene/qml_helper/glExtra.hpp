#pragma once
#include <memory>
#include <array>
#include <cstdint>

#include <span>
#include "ExSwapchain.hpp"

class QOpenGLContext;
class QOffscreenSurface;

class GlExtra {
public:
    GlExtra();
    ~GlExtra();
    bool init(void* get_proc_address(const char*));
    uint genExTexture(wallpaper::ExHandle&);
    void deleteTexture(uint);

    std::span<const std::uint8_t> uuid() const;
    wallpaper::TexTiling     tiling() const;

private:
    class impl;
    std::unique_ptr<impl> pImpl;

    bool inited { false };

    wallpaper::TexTiling m_tiling { wallpaper::TexTiling::OPTIMAL };

    // Shared GL 4.2+ context for external memory import
    QOpenGLContext*    m_shared_ctx { nullptr };
    QOffscreenSurface* m_surface { nullptr };
    bool               m_use_shared_ctx { false };
    bool               m_is_low_gl { false };
};
