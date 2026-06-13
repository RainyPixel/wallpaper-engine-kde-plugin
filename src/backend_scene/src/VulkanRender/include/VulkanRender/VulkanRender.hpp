#pragma once

#include "RenderGraph/RenderGraph.hpp"
#include "SceneWallpaperSurface.hpp"
#include "Swapchain/ExSwapchain.hpp"
#include "Type.hpp"

#include <cstdio>
#include <memory>
#include <string>

namespace wallpaper
{
class Scene;

namespace vulkan
{
class FinPass;

class VulkanRender {
public:
    VulkanRender();
    ~VulkanRender();

    bool init(RenderInitInfo);

    void destroy();

    void drawFrame(Scene&);

    void clearLastRenderGraph();
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void UpdateCameraFillMode(Scene&, wallpaper::FillMode);

    // Decides this screen's frame source after the graph has been compiled. An
    // empty key renders independently. A non-empty key joins a mirror group: the
    // first screen becomes primary (renders + publishes its swapchain), later
    // screens become secondary (their just-built graph is freed, they display the
    // primary's frames). Returns true when this screen renders, false when it
    // mirrors.
    bool beginFrameSource(const std::string& mirror_key);
    bool isMirror() const;
    // True for a secondary whose primary has gone away; the caller should rebuild
    // and re-run beginFrameSource to re-elect.
    bool mirrorLost() const;
    void releaseMirror();

    ExSwapchain* exSwapchain() const;
    // Thread-safe shared_ptr snapshot of the live frame source (own or mirrored);
    // the consumer should hold this across eatFrame() to avoid a use-after-free if
    // the render thread re-elects mid-frame.
    std::shared_ptr<ExSwapchain> currentSwapchain() const;
    bool                         inited() const;

    // Synchronously stops the per-frame redraw callback from firing again. Called
    // from the QML render thread when the texture node is destroyed, so the render
    // loop never emits into a half-destroyed node during teardown.
    void clearRedrawCallback();

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
} // namespace vulkan
} // namespace wallpaper