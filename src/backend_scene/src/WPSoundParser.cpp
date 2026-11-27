#include "WPSoundParser.hpp"
#include "Audio/SoundManager.h"
#include "Fs/VFS.h"
#include "wpscene/WPSoundObject.h"
#include "Utils/Logging.h"

#include <string>
#include <string_view>
#include <algorithm>

using namespace wallpaper;

enum class PlaybackMode
{
    Random,
    Loop
};

static PlaybackMode ToPlaybackMode(std::string_view s) {
    if (s == "loop")
        return PlaybackMode::Loop;
    else if (s == "random")
        return PlaybackMode::Random;
    return PlaybackMode::Loop;
};

class WPSoundStream : public audio::SoundStream {
public:
    struct Config {
        float        maxtime { 10.0f };
        float        mintime { 0.0f };
        float        volume { 1.0f };
        PlaybackMode mode { PlaybackMode::Loop };
    };
    WPSoundStream(const std::vector<std::string>& paths, fs::VFS& vfs, Config c,
                  std::shared_ptr<std::atomic_bool> playing)
        : vfs(vfs), m_config(c), m_soundPaths(paths), m_playing(std::move(playing)) {}
    virtual ~WPSoundStream() = default;

    uint64_t NextPcmData(void* pData, uint32_t frameCount) override {
        if (! m_playing->load(std::memory_order_relaxed)) {
            std::fill_n(static_cast<float*>(pData),
                        static_cast<size_t>(frameCount) * m_desc.channels,
                        0.0f);
            return frameCount;
        }
        // first
        if (! m_curActive && ! Switch()) return 0;

        // loop
        uint64_t frameReads = m_curActive->NextPcmData(pData, frameCount);
        if (frameReads == 0) {
            if (! Switch()) return 0;
            frameReads = m_curActive->NextPcmData(pData, frameCount);
        }
        // volume
        {
            float*     pData_float = static_cast<float*>(pData);
            const auto num         = frameReads * m_desc.channels;
            for (uint i = 0; i < num; i++, pData_float++) {
                (*pData_float) *= m_config.volume;
            }
        }
        return frameReads;
    };
    void PassDesc(const Desc& d) override { m_desc = d; }
    bool Switch() {
        if (m_soundPaths.empty()) return false;
        std::string path = m_soundPaths[LoopIndex()];
        // LOG_INFO("Switch to audio file: %s", path.c_str());
        m_curActive = audio::CreateSoundStream(vfs.Open("/assets/" + path), m_desc);
        return m_curActive != nullptr;
    }
    uint32_t LoopIndex() {
        m_curIndex++;
        if (m_curIndex == m_soundPaths.size()) m_curIndex = 0;
        return m_curIndex;
    }

private:
    fs::VFS& vfs;
    Config   m_config;
    Desc     m_desc;
    uint32_t m_curIndex { 0 };

    const std::vector<std::string>    m_soundPaths;
    std::unique_ptr<SoundStream>      m_curActive;
    std::shared_ptr<std::atomic_bool> m_playing;
};

std::shared_ptr<std::atomic_bool> WPSoundParser::Parse(const wpscene::WPSoundObject& obj,
                                                       fs::VFS& vfs, audio::SoundManager& sm) {
    WPSoundStream::Config config { .maxtime = obj.maxtime,
                                   .mintime = obj.mintime,
                                   .volume  = obj.volume > 1.0f ? 1.0f : obj.volume,
                                   .mode    = ToPlaybackMode(obj.playbackmode) };

    auto playing = std::make_shared<std::atomic_bool>(! obj.startSilent);
    auto ss      = std::make_unique<WPSoundStream>(obj.sound, vfs, config, playing);
    sm.MountStream(std::move(ss));
    return playing;
}
