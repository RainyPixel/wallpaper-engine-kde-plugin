#include "PpmWriter.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

namespace utils
{

namespace
{
std::string errnoText(const char* what, const std::string& path) {
    return std::string(what) + " " + path + ": " + std::strerror(errno);
}
} // namespace

std::string WritePpmAtomic(const std::string& path, std::uint32_t width, std::uint32_t height,
                           const std::uint8_t* pixels, std::size_t pixels_size, PixelOrder order) {
    if (path.empty()) return "empty output path";
    if (width == 0 || height == 0) return "empty image";
    if (pixels == nullptr || pixels_size < (std::size_t)width * height * 4)
        return "pixel buffer smaller than the image";

    // mkstemp keeps the temporary in the destination directory, so the final
    // rename stays on one filesystem and is atomic.
    std::string tmpl = path + ".XXXXXX";
    const int   fd   = ::mkstemp(tmpl.data());
    if (fd < 0) return errnoText("cannot create temporary file next to", path);

    FILE* out = ::fdopen(fd, "wb");
    if (out == nullptr) {
        const std::string err = errnoText("cannot open", tmpl);
        ::close(fd);
        ::unlink(tmpl.c_str());
        return err;
    }

    const auto fail = [&](const char* what) {
        const std::string err = errnoText(what, tmpl);
        std::fclose(out);
        ::unlink(tmpl.c_str());
        return err;
    };

    if (std::fprintf(out, "P6\n%u %u\n255\n", width, height) < 0) return fail("cannot write");

    const bool                bgra = order == PixelOrder::BGRA;
    std::vector<std::uint8_t> row((std::size_t)width * 3);
    for (std::uint32_t y = 0; y < height; y++) {
        const std::uint8_t* src = pixels + (std::size_t)y * width * 4;
        for (std::uint32_t x = 0; x < width; x++, src += 4) {
            row[x * 3 + 0] = bgra ? src[2] : src[0];
            row[x * 3 + 1] = src[1];
            row[x * 3 + 2] = bgra ? src[0] : src[2];
        }
        if (std::fwrite(row.data(), 1, row.size(), out) != row.size()) return fail("cannot write");
    }

    if (std::fflush(out) != 0) return fail("cannot flush");
    if (std::fclose(out) != 0) {
        const std::string err = errnoText("cannot close", tmpl);
        ::unlink(tmpl.c_str());
        return err;
    }
    if (std::rename(tmpl.c_str(), path.c_str()) != 0) {
        const std::string err = errnoText("cannot rename the screenshot onto", path);
        ::unlink(tmpl.c_str());
        return err;
    }
    return {};
}

} // namespace utils
