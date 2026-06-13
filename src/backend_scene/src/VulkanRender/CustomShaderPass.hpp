#pragma once
#include "VulkanPass.hpp"
#include <string>
#include <vector>

#include "Vulkan/Device.hpp"
#include "Scene/Scene.h"
#include "Vulkan/StagingBuffer.hpp"
#include "Vulkan/GraphicsPipeline.hpp"
#include "SpriteAnimation.hpp"
#include "Interface/IShaderValueUpdater.h"

namespace wallpaper
{

namespace vulkan
{

class CustomShaderPass : public VulkanPass {
public:
    struct Desc {
        // in
        SceneNode*               node { nullptr };
        std::vector<std::string> textures;
        std::string              output;
        sprite_map_t             sprites_map;

        // -----prepared
        // vulkan texs
        std::vector<ImageSlotsRef> vk_textures;
        std::vector<i32>           vk_tex_binding;
        ImageParameters            vk_output;

        // bufs
        bool                          dyn_vertex { false };
        std::vector<StagingBufferRef> vertex_bufs;
        StagingBufferRef              index_buf;
        StagingBufferRef              ubo_buf;

        // pipeline
        VkClearValue       clear_value;
        bool               blending { false };
        vvk::Framebuffer   fb;
        PipelineParameters pipeline;
        u32                draw_count { 0 };

        // uniforms
        std::function<void()> update_op;
    };

    CustomShaderPass(const Desc&);
    virtual ~CustomShaderPass();

    void setDescTex(u32 index, std::string_view tex_key);

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

    // Returns true if this pass has no dynamic elements (vertices, sprites)
    bool isStatic() const override { return ! m_desc.dyn_vertex && m_desc.sprites_map.empty(); }

    // Returns true if shader uses time-based uniforms (g_Time, g_PointerPosition, etc.)
    bool usesTimeUniforms() const { return m_uses_time_uniforms; }

    // Pass is cacheable if static and doesn't use time-based uniforms
    bool isCacheable() const { return isStatic() && ! m_uses_time_uniforms; }

    // Check if output is already cached and valid
    bool isCached() const { return m_cached; }
    void invalidateCache() { m_cached = false; }
    void markCached() { m_cached = true; }

private:
    Desc m_desc;
    bool m_cached { false };
    bool m_uses_time_uniforms { false };
    // decided in prepare(): output never changes after the first frame, so
    // execute() may be skipped on subsequent frames (graph-aware, see prepare)
    bool m_frame_static { false };
};

} // namespace vulkan
} // namespace wallpaper
