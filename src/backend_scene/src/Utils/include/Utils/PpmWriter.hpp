#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace utils
{

// Byte order of one 4-byte pixel in a tightly packed readback buffer.
enum class PixelOrder
{
    RGBA,
    BGRA,
};

// Writes `width` x `height` tightly packed 4-byte pixels as a binary PPM (P6,
// alpha dropped) to `path`.
//
// The image goes to a temporary file in the same directory first and is then
// renamed over `path`, so an existing file is replaced only once the new image
// is complete, and a failed write never leaves a truncated file behind.
//
// Returns an empty string on success, otherwise a human-readable reason.
std::string WritePpmAtomic(const std::string& path, std::uint32_t width, std::uint32_t height,
                           const std::uint8_t* pixels, std::size_t pixels_size, PixelOrder order);

} // namespace utils
