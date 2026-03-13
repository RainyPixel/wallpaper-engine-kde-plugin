#include "glExtra.hpp"
#include <glad/glad.h>
#include <vector>
#include "Utils/Logging.h"

#include <QtGui/QOpenGLContext>
#include <QtGui/QOffscreenSurface>
#include <QtGui/QSurfaceFormat>

using namespace wallpaper;

#define CHECK_GL_ERROR_IF_DEBUG() CheckGlError(__SHORT_FILE__, __FUNCTION__, __LINE__);

namespace
{
inline char const* const GLErrorToStr(GLenum const err) noexcept {
#define Enum_GLError(glerr) \
    case glerr: return #glerr;

    switch (err) {
        // opengl 2
        Enum_GLError(GL_NO_ERROR);
        Enum_GLError(GL_INVALID_ENUM);
        Enum_GLError(GL_INVALID_VALUE);
        Enum_GLError(GL_INVALID_OPERATION);
        Enum_GLError(GL_OUT_OF_MEMORY);
        // opengl 3 errors (1)
        Enum_GLError(GL_INVALID_FRAMEBUFFER_OPERATION);
    default: return "Unknown GLError";
    }
#undef Enum_GLError
}

inline void CheckGlError(const char* file, const char* func, int line) {
    int err = glGetError();
    if (err != 0) {
        WallpaperLog(LOGLEVEL_ERROR, file, line, "%s(%d) at %s", GLErrorToStr(err), err, func);
    }
}
} // namespace

class GlExtra::impl {
public:
    bool                                       test;
    std::array<std::uint8_t, GL_UUID_SIZE_EXT> uuid;
};

GlExtra::GlExtra(): pImpl(std::make_unique<impl>()) {}
GlExtra::~GlExtra() {
    delete m_surface;
    delete m_shared_ctx;
}

static std::array<std::uint8_t, GL_UUID_SIZE_EXT> getUUID() {
    int32_t num_device = 0;
    glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &num_device);

    GLubyte uuid[GL_UUID_SIZE_EXT] = { 0 };
    glGetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, 0, uuid);
    std::array<std::uint8_t, GL_UUID_SIZE_EXT> result;
    std::copy(std::begin(uuid), std::end(uuid), result.begin());
    return result;
}

bool GlExtra::init(void* get_proc_address(const char*)) {
    do {
        if (inited) break;
        if (! gladLoadGLLoader((GLADloadproc)get_proc_address)) {
            LOG_ERROR("gl: Failed to initialize GLAD");
            break;
        }
        LOG_INFO("gl: OpenGL version %d.%d loaded", GLVersion.major, GLVersion.minor);
        if (! (GLAD_GL_EXT_memory_object && GLAD_GL_EXT_semaphore)) {
            LOG_ERROR("gl: EXT_memory_object not available");
            break;
        }
        bool is_low_gl = ! GLAD_GL_VERSION_4_2 && ! GLAD_GL_ES_VERSION_3_0;
        if (is_low_gl) {
            LOG_INFO("gl: Context is GL %d.%d, attempting shared GL 4.2 context",
                     GLVersion.major, GLVersion.minor);

            // Try to create a shared GL 4.2+ context for external memory operations
            QOpenGLContext* current = QOpenGLContext::currentContext();
            if (current) {
                auto* ctx = new QOpenGLContext();
                // Use the parent context's format as base, just bump version
                QSurfaceFormat fmt = current->format();
                fmt.setVersion(4, 2);
                ctx->setFormat(fmt);
                ctx->setShareContext(current);
                if (ctx->create()) {
                    auto actual = ctx->format();
                    LOG_INFO("gl: Shared context created: GL %d.%d",
                             actual.majorVersion(), actual.minorVersion());

                    auto* surface = new QOffscreenSurface();
                    surface->setFormat(actual);
                    surface->create();

                    // Switch to new context to re-init GLAD with 4.2+ functions
                    if (ctx->makeCurrent(surface)) {
                        if (gladLoadGLLoader((GLADloadproc)get_proc_address)) {
                            LOG_INFO("gl: GLAD reloaded with GL %d.%d",
                                     GLVersion.major, GLVersion.minor);
                            m_shared_ctx    = ctx;
                            m_surface       = surface;
                            m_use_shared_ctx = true;
                            is_low_gl       = false;
                        } else {
                            LOG_ERROR("gl: Failed to reload GLAD on shared context");
                        }
                        // Switch back to Plasma context
                        current->makeCurrent(current->surface());
                    } else {
                        LOG_ERROR("gl: Failed to makeCurrent on shared context");
                    }

                    if (! m_use_shared_ctx) {
                        delete surface;
                        delete ctx;
                    }
                } else {
                    LOG_INFO("gl: Shared GL 4.2 context not available, using fallback");
                    delete ctx;
                }
            }
        }

        m_is_low_gl = is_low_gl;
        pImpl->uuid = getUUID();

        std::string gl_verdor_name { (const char*)glGetString(GL_VENDOR) };
        LOG_INFO("gl: OpenGL vendor string: %s", gl_verdor_name.c_str());

        if (! is_low_gl) {
            int              num { 0 };
            std::vector<int> tex_tilings;
            glGetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_NUM_TILING_TYPES_EXT, 1, &num);
            if (num <= 0) {
                LOG_ERROR("gl: can't get texture tiling support info");
                break;
            }
            num = std::min(num, 2);
            tex_tilings.resize(num);

            glGetInternalformativ(GL_TEXTURE_2D,
                                  GL_RGBA8,
                                  GL_TILING_TYPES_EXT,
                                  tex_tilings.size(),
                                  tex_tilings.data());
            CHECK_GL_ERROR_IF_DEBUG();

            bool support_optimal { false }, support_linear { false };
            for (auto& tiling : tex_tilings) {
                if (tiling == GL_OPTIMAL_TILING_EXT) {
                    support_optimal = true;
                } else if (tiling == GL_LINEAR_TILING_EXT) {
                    support_linear = true;
                }
            }
            if (! support_optimal && ! support_linear) {
                LOG_ERROR("gl: no supported tiling mode");
                break;
            }

            if (support_optimal) {
                m_tiling = wallpaper::TexTiling::OPTIMAL;
            } else if (support_linear) {
                m_tiling = wallpaper::TexTiling::LINEAR;
            }

            // linear, fix for amd
            // https://gitlab.freedesktop.org/mesa/mesa/-/issues/2456
            if (support_linear && gl_verdor_name.find("AMD") != std::string::npos) {
                m_tiling = wallpaper::TexTiling::LINEAR;
            }
        }
        if (m_tiling == wallpaper::TexTiling::OPTIMAL) {
            LOG_INFO("gl: external tex using optimal tiling");
        } else {
            LOG_INFO("gl: external tex using linear tiling");
        }

        inited = true;
    } while (false);

    // If we used the shared context for init, switch back to Plasma context
    if (m_use_shared_ctx) {
        QOpenGLContext* current = QOpenGLContext::currentContext();
        if (current != m_shared_ctx) {
            // Already on Plasma context
        } else if (auto* share = m_shared_ctx->shareContext()) {
            share->makeCurrent(share->surface());
        }
    }

    return inited;
}

std::span<const std::uint8_t> GlExtra::uuid() const { return pImpl->uuid; }

TexTiling GlExtra::tiling() const { return m_tiling; }

uint GlExtra::genExTexture(ExHandle& handle) {
    if (handle.fd < 0 || handle.size == 0) {
        LOG_ERROR("gl: invalid ExHandle (fd=%d, size=%zu)", handle.fd, handle.size);
        return 0;
    }

    QOpenGLContext* prev_ctx     = nullptr;
    QSurface*       prev_surface = nullptr;

    // Switch to shared 4.2 context if available
    if (m_use_shared_ctx && m_shared_ctx) {
        prev_ctx     = QOpenGLContext::currentContext();
        prev_surface = prev_ctx ? prev_ctx->surface() : nullptr;
        m_shared_ctx->makeCurrent(m_surface);
    }

    uint memobject, tex;
    glCreateMemoryObjectsEXT(1, &memobject);
    glImportMemoryFdEXT(memobject, handle.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, handle.fd);
    // NVIDIA generates spurious GL_INVALID_ENUM here on both GL 3.2 and 4.x contexts.
    // Subsequent checks after glTexParameteri/glTexStorageMem2DEXT catch real errors.
    glGetError();

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    // GL_TEXTURE_TILING_EXT requires GL 4.2+ — skip on low GL to avoid GL_INVALID_ENUM
    if (! m_is_low_gl) {
        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_TILING_EXT,
            (m_tiling == TexTiling::OPTIMAL ? GL_OPTIMAL_TILING_EXT : GL_LINEAR_TILING_EXT));
        CHECK_GL_ERROR_IF_DEBUG()
    }

    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, handle.width, handle.height, memobject, 0);
    CHECK_GL_ERROR_IF_DEBUG()

    glBindTexture(GL_TEXTURE_2D, 0);
    handle.fd = -1;

    // Switch back to Plasma context
    if (prev_ctx && prev_surface) {
        prev_ctx->makeCurrent(prev_surface);
    }

    return tex;
}

void GlExtra::deleteTexture(uint tex) {
    glDeleteTextures(1, &tex);
    CHECK_GL_ERROR_IF_DEBUG();
}
