#pragma once
#include "Core/Literals.hpp"
#include "Core/NoCopyMove.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wallpaper
{
namespace fs
{
class IBinaryStream;
}

namespace video
{

// Decodes an embedded MP4/WebM (a Wallpaper Engine video texture) into RGBA
// frames on a worker thread, paced by the caller's clock.
//
// Lifecycle:
//   auto dec = VideoDecoder::Open(stream, offset, size, &err); // probe only
//   dec->Start({ slot pointers sized dec->Width() x dec->Height() x 4 });
//   per rendered frame: int slot = dec->Acquire(scene_time);
//   destroy: joins the worker thread.
//
// The decoder has no clock of its own. Frames are chosen by the time passed to
// Acquire(), so the video pauses, speeds up and restarts exactly with that
// clock (the scene time). It never decodes audio. It loops at the end of the
// stream. When the caller stops calling Acquire() (paused), the worker fills
// the free slots and then sleeps.
class VideoDecoder : NoCopy, NoMove {
public:
    // Opens the container stored at [offset, offset + size) of `stream` and
    // checks that its first video stream can be decoded. Returns nullptr and
    // sets *err otherwise; nothing is started then.
    static std::unique_ptr<VideoDecoder> Open(std::shared_ptr<fs::IBinaryStream> stream, i64 offset,
                                              i64 size, std::string* err);
    ~VideoDecoder();

    i32         Width() const;
    i32         Height() const;
    std::string Description() const; // codec and size, for logs

    // Begins decoding into caller-owned RGBA buffers of Width() x Height() x 4
    // bytes each. At least two are needed: one being displayed, one being
    // filled. The buffers must outlive the decoder.
    bool Start(std::vector<std::uint8_t*> slots);

    // Picks the frame to display at clock time `t` (seconds; wraps at the end
    // of the video). Returns the slot index when it differs from the one
    // returned last, else -1. The returned slot is not written again until a
    // later Acquire() returns another one, so the caller may read it (or let
    // the GPU copy from it) until then.
    int Acquire(double t);

    // Stream time of the frame in `slot` (loop-adjusted); for tests and logs.
    double SlotTime(int slot) const;
    // Length of one loop in seconds, or 0 until the first loop has been decoded.
    double LoopLength() const;
    // True once the worker hit an unrecoverable error; the renderer clears its image.
    bool Failed() const;

private:
    VideoDecoder();
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace video
} // namespace wallpaper
