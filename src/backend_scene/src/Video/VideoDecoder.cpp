#include "VideoDecoder.hpp"

#include "Fs/IBinaryStream.h"
#include "Utils/Logging.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace wallpaper;
using namespace wallpaper::video;

namespace
{

std::string AvError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

// AVIO view of [base, base + size) of a VFS stream. Only the decoder thread
// touches it after Open().
struct SliceIo {
    std::shared_ptr<fs::IBinaryStream> stream;
    i64                                base { 0 };
    i64                                size { 0 };
    i64                                pos { 0 };

    static int Read(void* opaque, uint8_t* buf, int buf_size) {
        auto*     io   = static_cast<SliceIo*>(opaque);
        const i64 left = io->size - io->pos;
        if (left <= 0) return AVERROR_EOF;
        const i64 want = std::min<i64>(buf_size, left);
        if (! io->stream->SeekSet((idx)(io->base + io->pos))) return AVERROR(EIO);
        const usize got = io->stream->Read(buf, (usize)want);
        if (got == 0) return AVERROR_EOF;
        io->pos += (i64)got;
        return (int)got;
    }

    static int64_t Seek(void* opaque, int64_t offset, int whence) {
        auto* io = static_cast<SliceIo*>(opaque);
        if (whence & AVSEEK_SIZE) return io->size;
        i64 next = 0;
        switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: next = offset; break;
        case SEEK_CUR: next = io->pos + offset; break;
        case SEEK_END: next = io->size + offset; break;
        default: return AVERROR(EINVAL);
        }
        if (next < 0 || next > io->size) return AVERROR(EINVAL);
        io->pos = next;
        return next;
    }
};

enum class SlotState
{
    Free,  // may be written by the worker
    Ready, // holds a decoded frame not yet displayed
    Shown, // returned by Acquire(); must not be written
};

// Never skip more than this many frames in a row while catching up, so a
// decoder slower than real time still shows (late) frames instead of none.
constexpr int kMaxSkipRun = 12;
// Frames this far behind the clock are decoded but not converted.
constexpr double kSkipBehind = 0.25;

} // namespace

struct VideoDecoder::Impl {
    SliceIo          io;
    AVIOContext*     avio { nullptr };
    AVFormatContext* fmt { nullptr };
    AVCodecContext*  codec { nullptr };
    SwsContext*      sws { nullptr };
    AVPacket*        pkt { nullptr };
    AVFrame*         frame { nullptr };
    int              stream_index { -1 };
    AVRational       time_base { 1, 1 };
    i64              start_ts { 0 };
    double           nominal_frame { 1.0 / 30.0 };
    i32              width { 0 };
    i32              height { 0 };

    // worker state
    std::thread                worker;
    mutable std::mutex         mtx;
    std::condition_variable    cv;
    bool                       stop { false };
    std::vector<std::uint8_t*> slot_ptrs;
    std::vector<SlotState>     slot_state;
    std::vector<double>        slot_time;
    int                        shown { -1 };
    double                     target { 0.0 };
    std::atomic<double>        loop_length { 0.0 };
    std::atomic<bool>          failed { false };

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
        sws_freeContext(sws);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&codec);
        avformat_close_input(&fmt);
        if (avio) av_freep(&avio->buffer);
        avio_context_free(&avio);
    }

    // Next decoded frame into `frame`. Loops to the start at end of stream.
    // Returns false on an unrecoverable error.
    bool decodeNext(bool& looped) {
        looped = false;
        for (int attempts = 0; attempts < 2; attempts++) {
            while (true) {
                int r = avcodec_receive_frame(codec, frame);
                if (r == 0) return true;
                if (r == AVERROR_EOF) break;
                if (r != AVERROR(EAGAIN)) {
                    LOG_ERROR("video texture: decode failed: %s", AvError(r).c_str());
                    return false;
                }
                r = av_read_frame(fmt, pkt);
                if (r == AVERROR_EOF) {
                    avcodec_send_packet(codec, nullptr); // drain
                    continue;
                }
                if (r < 0) {
                    LOG_ERROR("video texture: read failed: %s", AvError(r).c_str());
                    return false;
                }
                if (pkt->stream_index == stream_index) {
                    r = avcodec_send_packet(codec, pkt);
                    if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_INVALIDDATA) {
                        av_packet_unref(pkt);
                        LOG_ERROR("video texture: send packet failed: %s", AvError(r).c_str());
                        return false;
                    }
                }
                av_packet_unref(pkt);
            }
            // End of stream: rewind and loop.
            looped = true;
            if (avformat_seek_file(fmt, stream_index, INT64_MIN, start_ts, start_ts, 0) < 0 &&
                av_seek_frame(fmt, stream_index, start_ts, AVSEEK_FLAG_BACKWARD) < 0) {
                LOG_ERROR("video texture: cannot rewind to loop");
                return false;
            }
            avcodec_flush_buffers(codec);
        }
        LOG_ERROR("video texture: stream yields no frames");
        return false;
    }

    bool convert(std::uint8_t* dst) {
        sws = sws_getCachedContext(sws,
                                   frame->width,
                                   frame->height,
                                   (AVPixelFormat)frame->format,
                                   width,
                                   height,
                                   AV_PIX_FMT_RGBA,
                                   SWS_BILINEAR,
                                   nullptr,
                                   nullptr,
                                   nullptr);
        if (sws == nullptr) return false;
        // Honour the stream's matrix and range (BT.709 HD video would come out
        // with shifted colours under swscale's BT.601 default).
        const int cs        = frame->colorspace == AVCOL_SPC_UNSPECIFIED
                                  ? (height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601)
                                  : (int)frame->colorspace;
        const int src_range = frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        sws_setColorspaceDetails(sws,
                                 sws_getCoefficients(cs),
                                 src_range,
                                 sws_getCoefficients(SWS_CS_DEFAULT),
                                 1,
                                 0,
                                 1 << 16,
                                 1 << 16);
        uint8_t* planes[4]  = { dst, nullptr, nullptr, nullptr };
        int      strides[4] = { width * 4, 0, 0, 0 };
        return sws_scale(sws, frame->data, frame->linesize, 0, frame->height, planes, strides) ==
               height;
    }

    int freeSlotLocked() const {
        for (usize i = 0; i < slot_state.size(); i++)
            if (slot_state[i] == SlotState::Free) return (int)i;
        return -1;
    }

    void run() {
        double loop_base = 0.0; // stream time where the current loop starts
        double loop_end  = 0.0; // end of the last frame seen in this loop
        int    skip_run  = 0;
        while (true) {
            int    slot = -1;
            double want = 0.0;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [&] {
                    return stop || freeSlotLocked() >= 0;
                });
                if (stop) return;
                slot = freeSlotLocked();
                want = target;
            }

            bool looped = false;
            if (! decodeNext(looped)) {
                failed = true;
                return;
            }
            if (looped) {
                if (loop_end <= 0.0) loop_end = nominal_frame;
                loop_base += loop_end;
                loop_length = loop_end;
                loop_end    = 0.0;
            }

            const i64    ts  = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                   ? frame->best_effort_timestamp
                                   : frame->pts;
            const double rel = ts == AV_NOPTS_VALUE
                                   ? loop_end
                                   : std::max(0.0, (double)(ts - start_ts) * av_q2d(time_base));
            const double dur =
                frame->duration > 0 ? (double)frame->duration * av_q2d(time_base) : nominal_frame;
            loop_end = std::max(loop_end, rel + dur);

            // Far behind the clock (e.g. a mirrored screen resumed): jump whole
            // loops instead of decoding through them.
            const double len = loop_length.load();
            if (len > 0.0 && want - (loop_base + rel) > len + kSkipBehind)
                loop_base += std::floor((want - (loop_base + rel)) / len) * len;

            const double at = loop_base + rel;
            if (at + dur < want - kSkipBehind && skip_run < kMaxSkipRun) {
                skip_run++;
                av_frame_unref(frame);
                continue;
            }
            skip_run = 0;

            const bool ok = convert(slot_ptrs[(usize)slot]);
            av_frame_unref(frame);
            if (! ok) {
                LOG_ERROR("video texture: pixel conversion failed");
                failed = true;
                return;
            }
            {
                std::lock_guard<std::mutex> lk(mtx);
                slot_state[(usize)slot] = SlotState::Ready;
                slot_time[(usize)slot]  = at;
            }
        }
    }
};

VideoDecoder::VideoDecoder(): m(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;

i32    VideoDecoder::Width() const { return m->width; }
i32    VideoDecoder::Height() const { return m->height; }
double VideoDecoder::LoopLength() const { return m->loop_length.load(); }
bool   VideoDecoder::Failed() const { return m->failed.load(); }

std::string VideoDecoder::Description() const {
    char buf[160];
    std::snprintf(buf,
                  sizeof(buf),
                  "%s %dx%d %s",
                  m->codec->codec ? m->codec->codec->name : "?",
                  m->width,
                  m->height,
                  m->fmt->iformat ? m->fmt->iformat->name : "?");
    return buf;
}

double VideoDecoder::SlotTime(int slot) const {
    std::lock_guard<std::mutex> lk(m->mtx);
    if (slot < 0 || (usize)slot >= m->slot_time.size()) return -1.0;
    return m->slot_time[(usize)slot];
}

std::unique_ptr<VideoDecoder> VideoDecoder::Open(std::shared_ptr<fs::IBinaryStream> stream,
                                                 i64 offset, i64 size, std::string* err) {
    auto fail = [err](std::string why) -> std::unique_ptr<VideoDecoder> {
        if (err) *err = std::move(why);
        return nullptr;
    };
    if (! stream) return fail("cannot open the texture file");
    if (offset < 0 || size <= 0 || offset + size > (i64)stream->Size())
        return fail("video payload lies outside the texture file");

    std::unique_ptr<VideoDecoder> dec(new VideoDecoder());
    auto&                         d = *dec->m;
    d.io                            = SliceIo { std::move(stream), offset, size, 0 };

    constexpr int io_buf_size = 64 * 1024;
    auto*         io_buf      = static_cast<unsigned char*>(av_malloc(io_buf_size));
    if (io_buf == nullptr) return fail("out of memory");
    d.avio =
        avio_alloc_context(io_buf, io_buf_size, 0, &d.io, &SliceIo::Read, nullptr, &SliceIo::Seek);
    if (d.avio == nullptr) {
        av_free(io_buf);
        return fail("out of memory");
    }

    d.fmt = avformat_alloc_context();
    if (d.fmt == nullptr) return fail("out of memory");
    d.fmt->pb = d.avio;
    d.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (int r = avformat_open_input(&d.fmt, nullptr, nullptr, nullptr); r < 0) {
        // avformat_open_input frees the context on failure
        return fail("not a readable video container: " + AvError(r));
    }
    if (int r = avformat_find_stream_info(d.fmt, nullptr); r < 0)
        return fail("cannot read stream info: " + AvError(r));

    const AVCodec* codec = nullptr;
    d.stream_index       = av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (d.stream_index < 0 || codec == nullptr)
        return fail("no decodable video stream: " + AvError(d.stream_index));
    // Only the picture is used; audio tracks are never decoded.
    for (unsigned i = 0; i < d.fmt->nb_streams; i++)
        if ((int)i != d.stream_index) d.fmt->streams[i]->discard = AVDISCARD_ALL;

    AVStream* st = d.fmt->streams[d.stream_index];
    d.time_base  = st->time_base;
    d.start_ts   = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
        d.nominal_frame = av_q2d(av_inv_q(st->avg_frame_rate));

    d.codec = avcodec_alloc_context3(codec);
    if (d.codec == nullptr) return fail("out of memory");
    if (int r = avcodec_parameters_to_context(d.codec, st->codecpar); r < 0)
        return fail("bad codec parameters: " + AvError(r));
    // A wallpaper should stay light: a few threads are plenty for one layer.
    d.codec->thread_count = (int)std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
    if (int r = avcodec_open2(d.codec, codec, nullptr); r < 0)
        return fail(std::string("cannot open the ") + codec->name + " decoder: " + AvError(r));

    d.width  = d.codec->width;
    d.height = d.codec->height;
    if (d.width <= 0 || d.height <= 0 || d.width > 8192 || d.height > 8192)
        return fail("unsupported video size " + std::to_string(d.width) + "x" +
                    std::to_string(d.height));

    d.pkt   = av_packet_alloc();
    d.frame = av_frame_alloc();
    if (d.pkt == nullptr || d.frame == nullptr) return fail("out of memory");
    return dec;
}

bool VideoDecoder::Start(std::vector<std::uint8_t*> slots) {
    if (m->worker.joinable() || slots.size() < 2) return false;
    for (auto* p : slots)
        if (p == nullptr) return false;
    m->slot_ptrs = std::move(slots);
    m->slot_state.assign(m->slot_ptrs.size(), SlotState::Free);
    m->slot_time.assign(m->slot_ptrs.size(), 0.0);
    m->worker = std::thread([this] {
        m->run();
    });
    return true;
}

int VideoDecoder::Acquire(double t) {
    // Tolerance for clocks that land a hair before a frame's timestamp.
    constexpr double eps = 1e-4;
    int              best;
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        m->target = t;
        best      = -1;
        for (usize i = 0; i < m->slot_state.size(); i++) {
            if (m->slot_state[i] != SlotState::Ready || m->slot_time[i] > t + eps) continue;
            if (best < 0 || m->slot_time[i] > m->slot_time[(usize)best]) best = (int)i;
        }
        if (best >= 0) {
            // Frames older than the chosen one will never be displayed.
            for (usize i = 0; i < m->slot_state.size(); i++)
                if (m->slot_state[i] == SlotState::Ready &&
                    m->slot_time[i] < m->slot_time[(usize)best])
                    m->slot_state[i] = SlotState::Free;
            if (m->shown >= 0) m->slot_state[(usize)m->shown] = SlotState::Free;
            m->slot_state[(usize)best] = SlotState::Shown;
            m->shown                   = best;
        }
    }
    m->cv.notify_all();
    return best;
}
