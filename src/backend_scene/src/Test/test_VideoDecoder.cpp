#include <doctest.h>

#include "Video/VideoDecoder.hpp"
#include "Fs/MemBinaryStream.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace wallpaper;
using wallpaper::video::VideoDecoder;

namespace
{
// 64x32 VP9, 8 fps, 1 s: frames 0-3 solid red, frames 4-7 solid blue.
std::vector<uint8_t> loadFixture() {
    std::ifstream in(WEKDE_TEST_DATA_DIR "/red-blue-1s.webm", std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

constexpr i64 kJunk = 100;

// The fixture between junk bytes, as a video texture's payload sits in a .tex.
std::shared_ptr<fs::IBinaryStream> wrapped(const std::vector<uint8_t>& video) {
    std::vector<uint8_t> bytes(kJunk, 0xAB);
    bytes.insert(bytes.end(), video.begin(), video.end());
    bytes.insert(bytes.end(), kJunk, 0xCD);
    return std::make_shared<fs::MemBinaryStream>(std::move(bytes));
}

struct Decoding {
    // Declared first so it is destroyed last: the decoder's worker writes into
    // these until the decoder is gone.
    std::vector<std::vector<uint8_t>> buffers;
    std::unique_ptr<VideoDecoder>     dec;

    explicit Decoding(std::unique_ptr<VideoDecoder> d): dec(std::move(d)) {
        std::vector<std::uint8_t*> ptrs;
        for (int i = 0; i < 3; i++) {
            buffers.emplace_back((size_t)dec->Width() * (size_t)dec->Height() * 4);
            ptrs.push_back(buffers.back().data());
        }
        REQUIRE(dec->Start(ptrs));
    }

    // Polls Acquire(t) until it hands out a frame stamped within one frame
    // (1/8 s) before t, or gives up after two seconds. Returns the slot.
    int frameAt(double t) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        int        last     = -1;
        while (std::chrono::steady_clock::now() < deadline) {
            if (int slot = dec->Acquire(t); slot >= 0) last = slot;
            if (last >= 0 && dec->SlotTime(last) > t - 0.125 - 1e-6) return last;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return -1;
    }

    // RGBA of the centre pixel of `slot`.
    std::array<int, 4> centre(int slot) const {
        const auto&  b = buffers[(size_t)slot];
        const size_t i =
            ((size_t)(dec->Height() / 2) * (size_t)dec->Width() + (size_t)(dec->Width() / 2)) * 4;
        return { b[i], b[i + 1], b[i + 2], b[i + 3] };
    }
};

bool isRed(std::array<int, 4> c) { return c[0] > 220 && c[1] < 40 && c[2] < 40 && c[3] == 255; }
bool isBlue(std::array<int, 4> c) { return c[0] < 40 && c[1] < 40 && c[2] > 220 && c[3] == 255; }
} // namespace

TEST_SUITE("VideoDecoder") {
    TEST_CASE("opens a container embedded in a larger file") {
        const auto video = loadFixture();
        REQUIRE(! video.empty());
        std::string err;
        auto        dec = VideoDecoder::Open(wrapped(video), kJunk, (i64)video.size(), &err);
        REQUIRE_MESSAGE(dec != nullptr, err);
        CHECK(dec->Width() == 64);
        CHECK(dec->Height() == 32);
    }

    TEST_CASE("frames follow the clock and loop") {
        const auto  video = loadFixture();
        std::string err;
        auto        dec = VideoDecoder::Open(wrapped(video), kJunk, (i64)video.size(), &err);
        REQUIRE_MESSAGE(dec != nullptr, err);
        Decoding d(std::move(dec));

        int slot = d.frameAt(0.0);
        REQUIRE(slot >= 0);
        CHECK(d.dec->SlotTime(slot) == doctest::Approx(0.0));
        CHECK(isRed(d.centre(slot)));

        slot = d.frameAt(0.6);
        REQUIRE(slot >= 0);
        CHECK(d.dec->SlotTime(slot) == doctest::Approx(0.5));
        CHECK(isBlue(d.centre(slot)));

        // past the end: the second loop starts red again
        slot = d.frameAt(1.05);
        REQUIRE(slot >= 0);
        CHECK(d.dec->SlotTime(slot) == doctest::Approx(1.0));
        CHECK(isRed(d.centre(slot)));
        CHECK(d.dec->LoopLength() == doctest::Approx(1.0));
        CHECK_FALSE(d.dec->Failed());
    }

    TEST_CASE("a stopped clock shows no new frames") {
        const auto  video = loadFixture();
        std::string err;
        auto        dec = VideoDecoder::Open(wrapped(video), kJunk, (i64)video.size(), &err);
        REQUIRE_MESSAGE(dec != nullptr, err);
        Decoding d(std::move(dec));

        const int slot = d.frameAt(0.3);
        REQUIRE(slot >= 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // same time again (paused scene): nothing changes, nothing is skipped
        CHECK(d.dec->Acquire(0.3) == -1);
        CHECK(d.dec->SlotTime(slot) == doctest::Approx(0.25));
    }

    TEST_CASE("a clock far ahead jumps whole loops instead of decoding through them") {
        const auto  video = loadFixture();
        std::string err;
        auto        dec = VideoDecoder::Open(wrapped(video), kJunk, (i64)video.size(), &err);
        REQUIRE_MESSAGE(dec != nullptr, err);
        Decoding d(std::move(dec));
        REQUIRE(d.frameAt(1.05) >= 0); // learn the loop length
        const int slot = d.frameAt(100.6);
        REQUIRE(slot >= 0);
        CHECK(isBlue(d.centre(slot)));
    }

    TEST_CASE("garbage and out-of-range payloads are rejected") {
        std::string          err;
        std::vector<uint8_t> junk(4096, 0x5A);
        CHECK(VideoDecoder::Open(std::make_shared<fs::MemBinaryStream>(std::vector<uint8_t>(junk)),
                                 0,
                                 (i64)junk.size(),
                                 &err) == nullptr);
        CHECK_FALSE(err.empty());

        const auto video = loadFixture();
        err.clear();
        CHECK(VideoDecoder::Open(wrapped(video), kJunk, (i64)video.size() + 2 * kJunk, &err) ==
              nullptr);
        CHECK_FALSE(err.empty());
        CHECK(VideoDecoder::Open(nullptr, 0, 1, &err) == nullptr);
    }
}
