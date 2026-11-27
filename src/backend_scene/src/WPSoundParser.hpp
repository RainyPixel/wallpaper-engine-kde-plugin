#pragma once
#include <atomic>
#include <memory>

namespace wallpaper
{

namespace audio
{
class SoundManager;
}
namespace fs
{
class VFS;
}
namespace wpscene
{
class WPSoundObject;
}
class WPSoundParser {
public:
    static std::shared_ptr<std::atomic_bool> Parse(const wpscene::WPSoundObject&, fs::VFS&,
                                                   audio::SoundManager&);
};
} // namespace wallpaper
