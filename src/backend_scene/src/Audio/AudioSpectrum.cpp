#include "AudioSpectrum.h"

#include <mutex>

#ifdef WEKDE_AUDIO_MONITOR
#    include "Utils/Logging.h"
#    include <pulse/pulseaudio.h>
#    include <unsupported/Eigen/FFT>
#    include <algorithm>
#    include <chrono>
#    include <cmath>
#    include <complex>
#    include <numbers>
#    include <string>
#endif

using namespace wallpaper::audio;

#ifdef WEKDE_AUDIO_MONITOR

struct AudioSpectrumMonitor::Impl {
    static constexpr unsigned rate        = 48000;
    static constexpr size_t   sampleCount = 2048;

    Impl() {
        fft.SetFlag(Eigen::FFT<float>::HalfSpectrum);
        for (size_t i = 0; i < sampleCount; ++i) {
            window[i] =
                0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(i) /
                                       static_cast<float>(sampleCount));
        }
        for (size_t i = 1; i < binBand.size(); ++i) {
            const float hz = static_cast<float>(i * rate) / static_cast<float>(sampleCount);
            const float logPosition = std::log(hz / 30.0f) / std::log(20000.0f / 30.0f);
            binBand[i] = static_cast<size_t>(std::clamp(logPosition * 64.0f, 0.0f, 63.0f));
        }

        loop = pa_threaded_mainloop_new();
        if (! loop) return;
        context = pa_context_new(pa_threaded_mainloop_get_api(loop), "Wallpaper Engine KDE");
        if (! context) return;
        pa_context_set_state_callback(context, onContextState, this);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0 ||
            pa_threaded_mainloop_start(loop) < 0) {
            LOG_ERROR("could not start desktop audio monitoring");
            return;
        }
        started = true;
    }

    ~Impl() {
        if (started) pa_threaded_mainloop_stop(loop);
        closeStream();
        if (context) pa_context_unref(context);
        if (loop) pa_threaded_mainloop_free(loop);
    }

    AudioSpectrum latest() const {
        std::lock_guard guard(lock);
        return std::chrono::steady_clock::now() - lastFrame < std::chrono::milliseconds(250)
                   ? bands
                   : AudioSpectrum {};
    }

    static void onContextState(pa_context* context, void* data) {
        auto& self = *static_cast<Impl*>(data);
        if (pa_context_get_state(context) == PA_CONTEXT_READY) {
            pa_context_set_subscribe_callback(context, onServerChanged, &self);
            if (auto* operation =
                    pa_context_subscribe(context, PA_SUBSCRIPTION_MASK_SERVER, nullptr, nullptr))
                pa_operation_unref(operation);
            self.requestDefaultSink();
        }
    }

    static void onServerChanged(pa_context*, pa_subscription_event_type_t, uint32_t, void* data) {
        static_cast<Impl*>(data)->requestDefaultSink();
    }

    void requestDefaultSink() {
        if (auto* operation = pa_context_get_server_info(context, onServerInfo, this))
            pa_operation_unref(operation);
    }

    static void onServerInfo(pa_context* context, const pa_server_info* info, void* data) {
        if (! info || ! info->default_sink_name) return;
        if (auto* operation = pa_context_get_sink_info_by_name(
                context, info->default_sink_name, onSinkInfo, data))
            pa_operation_unref(operation);
    }

    static void onSinkInfo(pa_context*, const pa_sink_info* info, int end, void* data) {
        if (end || ! info || ! info->monitor_source_name || ! *info->monitor_source_name) return;
        static_cast<Impl*>(data)->openMonitor(info->monitor_source_name);
    }

    void openMonitor(const char* sinkMonitor) {
        if (monitor == sinkMonitor && stream && PA_STREAM_IS_GOOD(pa_stream_get_state(stream)))
            return;

        closeStream();
        used = 0;
        const pa_sample_spec format { PA_SAMPLE_FLOAT32NE, rate, 2 };
        stream = pa_stream_new(context, "Wallpaper audio response", &format, nullptr);
        if (! stream) return;
        pa_stream_set_read_callback(stream, onRead, this);
        pa_buffer_attr buffers {};
        buffers.maxlength = buffers.tlength = buffers.prebuf = buffers.minreq =
            static_cast<uint32_t>(-1);
        buffers.fragsize = sampleCount * 2 * sizeof(float);
        // An explicit monitor source keeps capture on playback, independent of
        // which microphone or other input is configured as the default source.
        if (pa_stream_connect_record(stream, sinkMonitor, &buffers, PA_STREAM_DONT_MOVE) < 0) {
            LOG_ERROR("could not connect to the desktop audio output monitor");
            closeStream();
            return;
        }
        monitor = sinkMonitor;
    }

    void closeStream() {
        if (! stream) return;
        pa_stream_set_read_callback(stream, nullptr, nullptr);
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
        stream = nullptr;
    }

    static void onRead(pa_stream* stream, size_t, void* data) {
        auto&       self    = *static_cast<Impl*>(data);
        const void* samples = nullptr;
        size_t      bytes   = 0;
        if (pa_stream_peek(stream, &samples, &bytes) < 0 || ! bytes) return;
        const auto* stereo = static_cast<const float*>(samples);
        for (size_t i = 0; i + 1 < bytes / sizeof(float); i += 2) {
            const float gain      = self.window[self.used];
            self.left[self.used]  = stereo ? stereo[i] * gain : 0.0f;
            self.right[self.used] = stereo ? stereo[i + 1] * gain : 0.0f;
            if (++self.used == sampleCount) self.publish();
        }
        pa_stream_drop(stream);
    }

    std::array<float, 64> analyze(const std::array<float, sampleCount>& samples) {
        fft.fwd(frequency.data(), samples.data(), sampleCount);
        std::array<float, 64> result {};
        for (size_t i = 1; i < frequency.size(); ++i) {
            // Compensate for the Hann window and the negative half of the FFT.
            const float amplitude = std::abs(frequency[i]) * (4.0f / sampleCount);
            result[binBand[i]]    = std::max(result[binBand[i]], std::min(1.0f, amplitude));
        }
        return result;
    }

    void publish() {
        const AudioSpectrum next { analyze(left), analyze(right) };
        std::lock_guard     guard(lock);
        bands     = next;
        lastFrame = std::chrono::steady_clock::now();
        used      = 0;
    }

    pa_threaded_mainloop*                                loop { nullptr };
    pa_context*                                          context { nullptr };
    pa_stream*                                           stream { nullptr };
    bool                                                 started { false };
    std::string                                          monitor;
    size_t                                               used { 0 };
    Eigen::FFT<float>                                    fft;
    std::array<float, sampleCount>                       window {};
    std::array<float, sampleCount>                       left {};
    std::array<float, sampleCount>                       right {};
    std::array<size_t, sampleCount / 2 + 1>              binBand {};
    std::array<std::complex<float>, sampleCount / 2 + 1> frequency {};
    mutable std::mutex                                   lock;
    AudioSpectrum                                        bands;
    std::chrono::steady_clock::time_point                lastFrame {};
};

#else

struct AudioSpectrumMonitor::Impl {
    AudioSpectrum latest() const { return {}; }
};

#endif

AudioSpectrumMonitor::AudioSpectrumMonitor(): m_impl(std::make_unique<Impl>()) {}
AudioSpectrumMonitor::~AudioSpectrumMonitor() = default;
AudioSpectrum AudioSpectrumMonitor::latest() const { return m_impl->latest(); }

std::shared_ptr<AudioSpectrumMonitor> AudioSpectrumMonitor::Acquire() {
    static std::mutex                          mutex;
    static std::weak_ptr<AudioSpectrumMonitor> monitor;
    std::lock_guard                            guard(mutex);
    auto                                       shared = monitor.lock();
    if (! shared) {
        shared.reset(new AudioSpectrumMonitor);
        monitor = shared;
    }
    return shared;
}
