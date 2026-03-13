#include <doctest.h>

#include "Core/StringHelper.hpp"
#include "Utils/Algorism.h"
#include "Utils/BitFlags.hpp"
#include "WPCommon.hpp"
#include "Fs/MemBinaryStream.h"

#include <cstdio>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::fs;

// ===========================================================================
// StringHelper
// ===========================================================================

TEST_SUITE("StringHelper") {

TEST_CASE("sstart_with") {
    CHECK(sstart_with("hello world", "hello") == true);
    CHECK(sstart_with("hello", "hello") == true);
    CHECK(sstart_with("hel", "hello") == false);
    CHECK(sstart_with("", "hello") == false);
    CHECK(sstart_with("hello", "") == true);
    CHECK(sstart_with("", "") == true);
    CHECK(sstart_with("_rt_default", "_rt_") == true);
    CHECK(sstart_with("_rt", "_rt_") == false);
}

TEST_CASE("send_with") {
    CHECK(send_with("hello world", "world") == true);
    CHECK(send_with("hello", "hello") == true);
    CHECK(send_with("lo", "hello") == false);
    CHECK(send_with("", "hello") == false);
    CHECK(send_with("hello", "") == true);
    CHECK(send_with("", "") == true);
    CHECK(send_with("image.tex", ".tex") == true);
    CHECK(send_with("image.png", ".tex") == false);
}

} // TEST_SUITE

// ===========================================================================
// Algorism
// ===========================================================================

TEST_SUITE("Algorism") {

TEST_CASE("IsPowOfTwo") {
    CHECK(algorism::IsPowOfTwo(0) == false);
    CHECK(algorism::IsPowOfTwo(1) == false);
    CHECK(algorism::IsPowOfTwo(2) == true);
    CHECK(algorism::IsPowOfTwo(3) == false);
    CHECK(algorism::IsPowOfTwo(4) == true);
    CHECK(algorism::IsPowOfTwo(8) == true);
    CHECK(algorism::IsPowOfTwo(16) == true);
    CHECK(algorism::IsPowOfTwo(15) == false);
    CHECK(algorism::IsPowOfTwo(256) == true);
    CHECK(algorism::IsPowOfTwo(1024) == true);
    CHECK(algorism::IsPowOfTwo(1023) == false);
}

TEST_CASE("PowOfTwo") {
    // Minimum returned value is 8
    CHECK(algorism::PowOfTwo(0) == 8);
    CHECK(algorism::PowOfTwo(1) == 8);
    CHECK(algorism::PowOfTwo(7) == 8);
    CHECK(algorism::PowOfTwo(8) == 8);
    CHECK(algorism::PowOfTwo(9) == 16);
    CHECK(algorism::PowOfTwo(16) == 16);
    CHECK(algorism::PowOfTwo(17) == 32);
    CHECK(algorism::PowOfTwo(100) == 128);
    CHECK(algorism::PowOfTwo(256) == 256);
    CHECK(algorism::PowOfTwo(257) == 512);
}

} // TEST_SUITE

// ===========================================================================
// BitFlags
// ===========================================================================

TEST_SUITE("BitFlags") {

enum class TestFlag : uint32_t
{
    A = 0,
    B = 1,
    C = 2,
    D = 31
};

TEST_CASE("Default construction — all cleared") {
    BitFlags<TestFlag> flags;
    CHECK(flags.none() == true);
    CHECK(flags.any() == false);
    CHECK(flags[TestFlag::A] == false);
    CHECK(flags[TestFlag::B] == false);
}

TEST_CASE("Construction from integer") {
    BitFlags<TestFlag> flags(0b101u); // bits 0 and 2
    CHECK(flags[TestFlag::A] == true);
    CHECK(flags[TestFlag::B] == false);
    CHECK(flags[TestFlag::C] == true);
}

TEST_CASE("Set and reset") {
    BitFlags<TestFlag> flags;
    flags.set(TestFlag::B);
    CHECK(flags[TestFlag::B] == true);
    CHECK(flags.count() == 1);

    flags.set(TestFlag::D);
    CHECK(flags[TestFlag::D] == true);
    CHECK(flags.count() == 2);

    flags.reset(TestFlag::B);
    CHECK(flags[TestFlag::B] == false);
    CHECK(flags.count() == 1);

    flags.reset();
    CHECK(flags.none() == true);
}

TEST_CASE("Index by underlying type") {
    BitFlags<TestFlag> flags(0b010u); // bit 1
    CHECK(flags[1u] == true);
    CHECK(flags[0u] == false);
}

TEST_CASE("WPTexFlags simulation") {
    // Simulate the flags used in WPTexImageParser
    enum class WPTexFlagEnum : uint32_t
    {
        noInterpolation = 0,
        clampUVs        = 1,
        sprite          = 2,
        compo1          = 20,
        compo2          = 21,
        compo3          = 22
    };

    // Sprite flag = bit 2 = value 4
    BitFlags<WPTexFlagEnum> flags(4u);
    CHECK(flags[WPTexFlagEnum::sprite] == true);
    CHECK(flags[WPTexFlagEnum::noInterpolation] == false);
    CHECK(flags[WPTexFlagEnum::clampUVs] == false);

    // All three flags
    BitFlags<WPTexFlagEnum> flags2(0b111u);
    CHECK(flags2[WPTexFlagEnum::noInterpolation] == true);
    CHECK(flags2[WPTexFlagEnum::clampUVs] == true);
    CHECK(flags2[WPTexFlagEnum::sprite] == true);
}

} // TEST_SUITE

// ===========================================================================
// WPCommon — ReadTexVesion / ReadVersion
// ===========================================================================

TEST_SUITE("WPCommon") {

namespace
{
// Helper: create a 9-byte version header in a MemBinaryStream
std::vector<uint8_t> makeVersionBytes(const char* prefix, int ver) {
    char buf[9] {};
    std::snprintf(buf, sizeof(buf), "%.4s%.4d", prefix, ver);
    return std::vector<uint8_t>(buf, buf + 9);
}
} // namespace

TEST_CASE("ReadTexVesion") {
    auto data = makeVersionBytes("TEX", 1);
    MemBinaryStream stream(std::move(data));
    CHECK(ReadTexVesion(stream) == 1);
}

TEST_CASE("ReadTexVesion version 2") {
    auto data = makeVersionBytes("TEX", 2);
    MemBinaryStream stream(std::move(data));
    CHECK(ReadTexVesion(stream) == 2);
}

TEST_CASE("ReadTexVesion version 3") {
    auto data = makeVersionBytes("TEX", 3);
    MemBinaryStream stream(std::move(data));
    CHECK(ReadTexVesion(stream) == 3);
}

TEST_CASE("ReadMDLVesion") {
    auto data = makeVersionBytes("MDL", 5);
    MemBinaryStream stream(std::move(data));
    CHECK(ReadMDLVesion(stream) == 5);
}

TEST_CASE("ReadVersion with wrong prefix returns 0") {
    auto data = makeVersionBytes("XXX", 1);
    MemBinaryStream stream(std::move(data));
    CHECK(ReadTexVesion(stream) == 0);
}

TEST_CASE("ReadVersion consecutive reads") {
    // Two version headers back to back
    auto v1 = makeVersionBytes("TEX", 1);
    auto v2 = makeVersionBytes("TEX", 2);
    std::vector<uint8_t> data;
    data.insert(data.end(), v1.begin(), v1.end());
    data.insert(data.end(), v2.begin(), v2.end());

    MemBinaryStream stream(std::move(data));
    CHECK(ReadTexVesion(stream) == 1);
    CHECK(ReadTexVesion(stream) == 2);
}

} // TEST_SUITE
