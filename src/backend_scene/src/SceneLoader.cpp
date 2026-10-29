#include "SceneLoader.hpp"

#include "Audio/SoundManager.h"
#include "Fs/PhysicalFs.h"
#include "Fs/VFS.h"
#include "WPSceneParser.hpp"
#include "WPPkgFs.hpp"

using namespace wallpaper;

std::shared_ptr<Scene> wallpaper::LoadScene(const std::string& source, const std::string& assets,
                                            const std::string&   cachePath,
                                            audio::SoundManager& sound,
                                            const std::string&   userProps) try {
    // mount assets dir
    std::unique_ptr<fs::VFS> pVfs = std::make_unique<fs::VFS>();
    auto&                    vfs  = *pVfs;
    if (! vfs.IsMounted("assets")) {
        bool sus = vfs.Mount("/assets", fs::CreatePhysicalFs(assets), "assets");
        if (! sus) {
            LOG_ERROR("Mount assets dir failed");
            return nullptr;
        }
    }
    std::filesystem::path pkgPath_fs { source };
    pkgPath_fs.replace_extension("pkg");
    std::string pkgPath  = pkgPath_fs.native();
    std::string pkgEntry = pkgPath_fs.filename().replace_extension("json").native();
    std::string pkgDir   = pkgPath_fs.parent_path().native();
    std::string scene_id = pkgPath_fs.parent_path().filename().native();

    // Workshop metadata lives beside scene.pkg, outside its asset namespace.
    vfs.Mount("/project", fs::CreatePhysicalFs(pkgDir));

    // load pkgfile
    if (! vfs.Mount("/assets", fs::WPPkgFs::CreatePkgFs(pkgPath))) {
        LOG_INFO("load pkg file %s failed, fallback to use dir", pkgPath.c_str());
        // load pkg dir
        if (! vfs.Mount("/assets", fs::CreatePhysicalFs(pkgDir))) {
            LOG_ERROR("can't load pkg directory: %s", pkgDir.c_str());
            return nullptr;
        }
    }
    if (! cachePath.empty()) {
        if (! vfs.Mount("/cache", fs::CreatePhysicalFs(cachePath, true), "cache")) {
            LOG_ERROR("can't load cache folder: %s", cachePath.c_str());
        } else {
            LOG_INFO("cache folder: %s", cachePath.c_str());
        }
    }

    // Stage streams without starting a device or touching the current wallpaper's audio.
    audio::SoundManager pendingSound;
    const auto          scene_src = fs::GetFileContent(vfs, "/assets/" + pkgEntry);
    if (scene_src.empty()) {
        LOG_ERROR("Not supported scene type");
        return nullptr;
    }
    WPSceneParser parser;
    auto          scene = parser.Parse(scene_id, scene_src, vfs, pendingSound, userProps);
    if (! scene) return nullptr;
    scene->vfs.swap(pVfs);
    sound.ReplaceStreams(pendingSound);
    return scene;
} catch (const std::filesystem::filesystem_error& e) {
    LOG_ERROR("Scene filesystem error: %s", e.what());
    return nullptr;
}
