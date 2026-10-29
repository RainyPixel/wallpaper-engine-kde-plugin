#pragma once

#include <memory>
#include <string>

namespace wallpaper
{
class Scene;
namespace audio
{
class SoundManager;
}

// Failed loads preserve the current streams; successful scenes own their mounted filesystems.
std::shared_ptr<Scene> LoadScene(const std::string& source, const std::string& assets,
                                 const std::string& cachePath, audio::SoundManager& sound,
                                 const std::string& userProps);
} // namespace wallpaper
