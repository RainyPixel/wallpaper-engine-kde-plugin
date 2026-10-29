#include <doctest.h>

#include "Utils/PpmWriter.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>
#include <sys/stat.h>

using utils::PixelOrder;
using utils::WritePpmAtomic;
namespace fsys = std::filesystem;

namespace
{
// Fresh, empty directory per test case; removed again on scope exit.
struct TempDir {
    fsys::path path;
    TempDir() {
        std::string tmpl = (fsys::temp_directory_path() / "wekde-ppm-XXXXXX").string();
        REQUIRE(::mkdtemp(tmpl.data()) != nullptr);
        path = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        fsys::permissions(path, fsys::perms::owner_all, fsys::perm_options::add, ec);
        fsys::remove_all(path, ec);
    }
};

std::string readFile(const fsys::path& p) {
    std::ifstream in(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

// Only the expected file may remain: no leftover temporaries.
std::size_t entryCount(const fsys::path& dir) {
    return (std::size_t)std::distance(fsys::directory_iterator(dir), fsys::directory_iterator());
}

// 2x1 image: one red pixel, one blue pixel, alpha 0x80.
const std::vector<std::uint8_t> kRgba { 0xff, 0x00, 0x00, 0x80, 0x00, 0x00, 0xff, 0x80 };
const std::vector<std::uint8_t> kBgra { 0x00, 0x00, 0xff, 0x80, 0xff, 0x00, 0x00, 0x80 };
const std::string               kExpected =
    std::string("P6\n2 1\n255\n") + std::string("\xff\x00\x00\x00\x00\xff", 6);
} // namespace

TEST_SUITE("PpmWriter") {
    TEST_CASE("fresh output is written completely") {
        TempDir    dir;
        const auto out = dir.path / "frame.ppm";
        CHECK(WritePpmAtomic(out.string(), 2, 1, kRgba.data(), kRgba.size(), PixelOrder::RGBA) ==
              "");
        CHECK(readFile(out) == kExpected);
        CHECK(entryCount(dir.path) == 1);
        struct stat st {};
        REQUIRE(::stat(out.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0600);
    }

    TEST_CASE("BGRA readback is swizzled to RGB") {
        TempDir    dir;
        const auto out = dir.path / "frame.ppm";
        CHECK(WritePpmAtomic(out.string(), 2, 1, kBgra.data(), kBgra.size(), PixelOrder::BGRA) ==
              "");
        CHECK(readFile(out) == kExpected);
    }

    TEST_CASE("existing output is replaced, not appended to or kept") {
        TempDir    dir;
        const auto out = dir.path / "frame.ppm";
        {
            std::ofstream stale(out, std::ios::binary);
            stale << std::string(4096, 'x');
        }
        CHECK(WritePpmAtomic(out.string(), 2, 1, kRgba.data(), kRgba.size(), PixelOrder::RGBA) ==
              "");
        CHECK(readFile(out) == kExpected);
        CHECK(entryCount(dir.path) == 1);
    }

    TEST_CASE("missing directory reports an error and creates nothing") {
        TempDir    dir;
        const auto out = dir.path / "no-such-dir" / "frame.ppm";
        CHECK(WritePpmAtomic(out.string(), 2, 1, kRgba.data(), kRgba.size(), PixelOrder::RGBA) !=
              "");
        CHECK_FALSE(fsys::exists(out));
        CHECK(entryCount(dir.path) == 0);
    }

    TEST_CASE("unwritable directory reports an error and keeps the old file") {
        if (::geteuid() == 0) return; // root ignores directory permissions
        TempDir    dir;
        const auto out = dir.path / "frame.ppm";
        {
            std::ofstream stale(out, std::ios::binary);
            stale << "old";
        }
        fsys::permissions(dir.path, fsys::perms::owner_read | fsys::perms::owner_exec);
        CHECK(WritePpmAtomic(out.string(), 2, 1, kRgba.data(), kRgba.size(), PixelOrder::RGBA) !=
              "");
        fsys::permissions(dir.path, fsys::perms::owner_all);
        CHECK(readFile(out) == "old");
        CHECK(entryCount(dir.path) == 1);
    }

    TEST_CASE("a directory in the way reports an error and leaves no temporary") {
        TempDir    dir;
        const auto out = dir.path / "frame.ppm";
        fsys::create_directory(out);
        CHECK(WritePpmAtomic(out.string(), 2, 1, kRgba.data(), kRgba.size(), PixelOrder::RGBA) !=
              "");
        CHECK(fsys::is_directory(out));
        CHECK(entryCount(dir.path) == 1);
    }

    TEST_CASE("invalid arguments are rejected") {
        TempDir    dir;
        const auto out = (dir.path / "frame.ppm").string();
        CHECK(WritePpmAtomic("", 2, 1, kRgba.data(), kRgba.size(), PixelOrder::RGBA) != "");
        CHECK(WritePpmAtomic(out, 0, 1, kRgba.data(), kRgba.size(), PixelOrder::RGBA) != "");
        CHECK(WritePpmAtomic(out, 2, 1, nullptr, 0, PixelOrder::RGBA) != "");
        CHECK(WritePpmAtomic(out, 4, 4, kRgba.data(), kRgba.size(), PixelOrder::RGBA) != "");
        CHECK(entryCount(dir.path) == 0);
    }
}
