#pragma once
#include <unordered_map>

#include "SceneTexture.h"
#include "SceneRenderTarget.h"
#include "SceneNode.h"
#include "SceneLight.hpp"

#include "Core/NoCopyMove.hpp"

namespace wallpaper
{
class ParticleSystem;
class IShaderValueUpdater;
class IImageParser;

namespace fs
{
class VFS;
}
class Scene : NoCopy, NoMove {
public:
    Scene();
    ~Scene();

    std::unordered_map<std::string, SceneTexture>      textures;
    std::unordered_map<std::string, SceneRenderTarget> renderTargets;

    // static-pass caching: whether a render target is written only by passes
    // whose output never changes after the first frame. Filled during prepare
    // (topological order). cache_passes gates the whole optimization.
    bool                                  cache_passes { true };
    std::unordered_map<std::string, bool> rt_frame_static;
    // Real writers per render-target name. Graph versions of one name still
    // share a single GPU image, so a name written more than once cannot be
    // cached: a later writer overwrites it and a skipped producer never redraws.
    // Filled by sceneToRenderGraph; cleared at the start of each build.
    std::unordered_map<std::string, int> rt_write_count;

    std::unordered_map<std::string, std::shared_ptr<SceneCamera>> cameras;
    std::unordered_map<std::string, std::vector<std::string>>     linkedCameras;

    std::vector<std::unique_ptr<SceneLight>> lights;

    std::shared_ptr<SceneNode>           sceneGraph;
    std::unique_ptr<IShaderValueUpdater> shaderValueUpdater;
    std::unique_ptr<IImageParser>        imageParser;
    std::unique_ptr<fs::VFS>             vfs;

    std::string scene_id { "unknown_id" };

    bool first_frame_ok { false };

    SceneMesh default_effect_mesh;

    std::unique_ptr<ParticleSystem> paritileSys;

    SceneCamera* activeCamera;

    i32 ortho[2] { 1920, 1080 }; // w, h
    // Raw scene zoom. Ortho sizing goes through ZoomedExtent; fill mode reads
    // this so it does not replace the parsed crop with the full ortho size.
    float                camera_zoom { 1.0f };
    std::array<float, 3> clearColor { 1.0f, 1.0f, 1.0f };

    double elapsingTime { 0.0f }, frameTime { 0.0f };
    void   PassFrameTime(double t) {
        frameTime = t;
        elapsingTime += t;
    }

    void UpdateLinkedCamera(const std::string& name) {
        if (linkedCameras.count(name) != 0) {
            auto& cams = linkedCameras.at(name);
            for (auto& cam : cams) {
                if (cameras.count(cam) != 0) {
                    cameras.at(cam)->Clone(*cameras.at(name));
                    cameras.at(cam)->Update();
                }
            }
        }
    }
};
} // namespace wallpaper
