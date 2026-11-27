#pragma once

#include <array>
#include <memory>

namespace wallpaper::audio
{

using Spectrum16 = std::array<float, 16>;

struct AudioSpectrum {
    std::array<float, 64> left {};
    std::array<float, 64> right {};
};

class AudioSpectrumMonitor {
public:
    // Scenes with audio-responsive properties share one desktop-output monitor.
    static std::shared_ptr<AudioSpectrumMonitor> Acquire();
    ~AudioSpectrumMonitor();

    AudioSpectrum latest() const;

private:
    AudioSpectrumMonitor();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wallpaper::audio
