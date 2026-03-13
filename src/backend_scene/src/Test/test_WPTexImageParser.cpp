#include <doctest.h>

#include "WPTexImageParser.hpp"
#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"
#include "Type.hpp"

#include <cstring>
#include <lz4.h>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::fs;

// ---------------------------------------------------------------------------
// Mock filesystem: serves pre-built binary blobs keyed by VFS-internal path
// ---------------------------------------------------------------------------
class MockFs : public Fs {
public:
    void AddFile(std::string path, std::vector<uint8_t> data) {
        m_files[std::move(path)] = std::move(data);
    }

    bool Contains(std::string_view path) const override {
        return m_files.count(std::string(path)) > 0;
    }

    std::shared_ptr<IBinaryStream> Open(std::string_view path) override {
        auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        auto copy = it->second; // copy so stream owns its data
        return std::make_shared<MemBinaryStream>(std::move(copy));
    }

    std::shared_ptr<IBinaryStreamW> OpenW(std::string_view) override { return nullptr; }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

// ---------------------------------------------------------------------------
// Binary builder helpers
// ---------------------------------------------------------------------------
namespace
{

// Append raw bytes
void append(std::vector<uint8_t>& buf, const void* data, size_t n) {
    auto p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + n);
}

void appendInt32(std::vector<uint8_t>& buf, int32_t v) { append(buf, &v, 4); }
void appendUint32(std::vector<uint8_t>& buf, uint32_t v) { append(buf, &v, 4); }
void appendFloat(std::vector<uint8_t>& buf, float v) { append(buf, &v, 4); }

// Write a 9-byte TEX version header (matches WriteVersion format)
void appendTexVersion(std::vector<uint8_t>& buf, int ver) {
    char hdr[9] {};
    std::snprintf(hdr, sizeof(hdr), "%.4s%.4d", "TEX", ver);
    append(buf, hdr, 9);
}

// Build the common .tex header portion (TEXV+TEXI+fields+TEXB)
// Returns the buffer so callers can append image data / sprite data after it
std::vector<uint8_t> makeTexHeader(int texvVer, int texiVer, int texbVer, int32_t type,
                                   uint32_t flags, int32_t width, int32_t height,
                                   int32_t mapWidth, int32_t mapHeight, int32_t count) {
    std::vector<uint8_t> buf;
    buf.reserve(256);
    appendTexVersion(buf, texvVer); // TEXV
    appendTexVersion(buf, texiVer); // TEXI
    appendInt32(buf, type);         // texture type
    appendUint32(buf, flags);       // flags
    appendInt32(buf, width);
    appendInt32(buf, height);
    appendInt32(buf, mapWidth);
    appendInt32(buf, mapHeight);
    appendInt32(buf, 0); // unknown, skipped
    appendTexVersion(buf, texbVer); // TEXB
    appendInt32(buf, count);
    return buf;
}

// Append one mipmap's data for texb==1 (no LZ4 fields)
void appendMipmapV1(std::vector<uint8_t>& buf, int32_t w, int32_t h,
                    const std::vector<uint8_t>& pixels) {
    appendInt32(buf, w);
    appendInt32(buf, h);
    appendInt32(buf, static_cast<int32_t>(pixels.size()));
    append(buf, pixels.data(), pixels.size());
}

// Append one mipmap's data for texb==2 (with LZ4 fields)
void appendMipmapV2(std::vector<uint8_t>& buf, int32_t w, int32_t h,
                    const std::vector<uint8_t>& pixels, bool compress = false) {
    appendInt32(buf, w);
    appendInt32(buf, h);
    if (compress) {
        int maxDst = LZ4_compressBound(static_cast<int>(pixels.size()));
        std::vector<char> compressed(static_cast<size_t>(maxDst));
        int compressedSize =
            LZ4_compress_default(reinterpret_cast<const char*>(pixels.data()), compressed.data(),
                                 static_cast<int>(pixels.size()), maxDst);
        appendInt32(buf, 1); // LZ4_compressed = true
        appendInt32(buf, static_cast<int32_t>(pixels.size()));
        appendInt32(buf, compressedSize);
        append(buf, compressed.data(), static_cast<size_t>(compressedSize));
    } else {
        appendInt32(buf, 0); // LZ4_compressed = false
        appendInt32(buf, 0); // decompressed_size (unused when not compressed)
        appendInt32(buf, static_cast<int32_t>(pixels.size()));
        append(buf, pixels.data(), pixels.size());
    }
}

// Build a complete simple RGBA8 .tex file (1 image, 1 mipmap, texb=1, no sprite)
std::vector<uint8_t> makeSimpleRGBA8Tex(int32_t w, int32_t h) {
    auto buf = makeTexHeader(1, 1, 1, /*type=RGBA8*/ 0, /*flags=*/ 0, w, h, w, h, /*count=*/ 1);
    // image 0: 1 mipmap
    appendInt32(buf, 1); // mipmap_count
    std::vector<uint8_t> pixels(static_cast<size_t>(w * h * 4), 0xAB);
    appendMipmapV1(buf, w, h, pixels);
    return buf;
}

// Mount a .tex blob into VFS at the path WPTexImageParser expects
void mountTex(VFS& vfs, const std::string& name, std::vector<uint8_t> data) {
    // WPTexImageParser opens "/assets/materials/<name>.tex"
    // VFS mount at "/assets" strips to "/materials/<name>.tex"
    auto mockFs = std::make_unique<MockFs>();
    mockFs->AddFile("/materials/" + name + ".tex", std::move(data));
    vfs.Mount("/assets", std::move(mockFs));
}

} // namespace

// ===========================================================================
// Tests
// ===========================================================================

TEST_SUITE("WPTexImageParser") {

TEST_CASE("Valid RGBA8 texture — Parse") {
    VFS vfs;
    auto texData = makeSimpleRGBA8Tex(4, 4);
    mountTex(vfs, "test_rgba8", std::move(texData));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_rgba8");

    REQUIRE(img != nullptr);
    CHECK(img->key == "test_rgba8");
    CHECK(img->header.format == TextureFormat::RGBA8);
    CHECK(img->header.width == 4);
    CHECK(img->header.height == 4);
    CHECK(img->header.count == 1);
    CHECK(img->header.isSprite == false);
    REQUIRE(img->slots.size() == 1);
    REQUIRE(img->slots[0].mipmaps.size() == 1);
    CHECK(img->slots[0].mipmaps[0].width == 4);
    CHECK(img->slots[0].mipmaps[0].height == 4);
    CHECK(img->slots[0].mipmaps[0].size == 4 * 4 * 4);
    // Verify pixel data
    CHECK(img->slots[0].mipmaps[0].data.get()[0] == 0xAB);
}

TEST_CASE("Valid RGBA8 texture — ParseHeader") {
    VFS vfs;
    auto texData = makeSimpleRGBA8Tex(8, 8);
    mountTex(vfs, "test_hdr", std::move(texData));

    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("test_hdr");

    CHECK(header.format == TextureFormat::RGBA8);
    CHECK(header.width == 8);
    CHECK(header.height == 8);
    CHECK(header.count == 1);
    CHECK(header.isSprite == false);
}

TEST_CASE("BC1 (DXT1) format") {
    // type=7 → BC1
    auto buf = makeTexHeader(1, 1, 1, 7, 0, 4, 4, 4, 4, 1);
    // BC1: 8 bytes per 4x4 block = 8 bytes for a 4x4 texture
    appendInt32(buf, 1); // mipmap_count
    std::vector<uint8_t> blockData(8, 0);
    appendMipmapV1(buf, 4, 4, blockData);

    VFS vfs;
    mountTex(vfs, "test_bc1", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_bc1");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::BC1);
}

TEST_CASE("BC3 (DXT5) format") {
    auto buf = makeTexHeader(1, 1, 1, 4, 0, 4, 4, 4, 4, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> blockData(16, 0); // 16 bytes per 4x4 block
    appendMipmapV1(buf, 4, 4, blockData);

    VFS vfs;
    mountTex(vfs, "test_bc3", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_bc3");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::BC3);
}

TEST_CASE("BC2 (DXT3) format") {
    auto buf = makeTexHeader(1, 1, 1, 6, 0, 4, 4, 4, 4, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> blockData(16, 0);
    appendMipmapV1(buf, 4, 4, blockData);

    VFS vfs;
    mountTex(vfs, "test_bc2", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_bc2");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::BC2);
}

TEST_CASE("RG8 format") {
    auto buf = makeTexHeader(1, 1, 1, 8, 0, 2, 2, 2, 2, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(2 * 2 * 2, 0);
    appendMipmapV1(buf, 2, 2, pixels);

    VFS vfs;
    mountTex(vfs, "test_rg8", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_rg8");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::RG8);
}

TEST_CASE("R8 format") {
    auto buf = makeTexHeader(1, 1, 1, 9, 0, 2, 2, 2, 2, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(2 * 2, 0);
    appendMipmapV1(buf, 2, 2, pixels);

    VFS vfs;
    mountTex(vfs, "test_r8", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_r8");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::R8);
}

TEST_CASE("Unknown format type falls back to RGBA8") {
    auto buf = makeTexHeader(1, 1, 1, 99, 0, 2, 2, 2, 2, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(2 * 2 * 4, 0);
    appendMipmapV1(buf, 2, 2, pixels);

    VFS vfs;
    mountTex(vfs, "test_unk", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_unk");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::RGBA8);
}

TEST_CASE("Alias texture fallback — missing file") {
    VFS vfs;
    // Mount an empty filesystem so Contains returns false
    auto mockFs = std::make_unique<MockFs>();
    vfs.Mount("/assets", std::move(mockFs));

    WPTexImageParser parser(&vfs);

    SUBCASE("Parse returns 1x1 white fallback") {
        auto img = parser.Parse("_alias_missing");
        REQUIRE(img != nullptr);
        CHECK(img->key == "_alias_missing");
        CHECK(img->header.width == 1);
        CHECK(img->header.height == 1);
        CHECK(img->header.format == TextureFormat::RGBA8);
        REQUIRE(img->slots.size() == 1);
        REQUIRE(img->slots[0].mipmaps.size() == 1);
        auto* px = img->slots[0].mipmaps[0].data.get();
        CHECK(px[0] == 255);
        CHECK(px[1] == 255);
        CHECK(px[2] == 255);
        CHECK(px[3] == 255);
    }

    SUBCASE("ParseHeader returns 1x1 fallback header") {
        auto header = parser.ParseHeader("_alias_missing");
        CHECK(header.width == 1);
        CHECK(header.height == 1);
        CHECK(header.format == TextureFormat::RGBA8);
        CHECK(header.count == 1);
    }
}

TEST_CASE("Alias texture with existing file — parses normally") {
    VFS vfs;
    auto texData = makeSimpleRGBA8Tex(2, 2);
    mountTex(vfs, "_alias_exists", std::move(texData));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("_alias_exists");
    REQUIRE(img != nullptr);
    CHECK(img->header.width == 2);
    CHECK(img->header.height == 2);
}

TEST_CASE("Sprite with out-of-range imageId — disables sprite") {
    // Build a sprite texture: 1 image, sprite flag set
    uint32_t spriteFlag = (1u << 2); // WPTexFlagEnum::sprite = bit 2
    auto buf = makeTexHeader(1, 1, 2, 0, spriteFlag, 16, 16, 16, 16, 1);

    // Image 0: 1 mipmap (texb=2, so LZ4 fields present)
    appendInt32(buf, 1); // mipmap_count
    std::vector<uint8_t> pixels(16 * 16 * 4, 0);
    appendMipmapV2(buf, 16, 16, pixels);

    // Sprite section
    appendTexVersion(buf, 2); // texs version
    appendInt32(buf, 1);      // framecount = 1

    // Frame with out-of-range imageId (only 1 image, so imageId=1 is OOB)
    appendInt32(buf, 1);      // imageId = 1 (invalid, only index 0 exists)
    appendFloat(buf, 0.1f);   // frametime
    appendFloat(buf, 0.0f);   // x
    appendFloat(buf, 0.0f);   // y
    appendFloat(buf, 16.0f);  // xAxis[0]
    appendFloat(buf, 0.0f);   // xAxis[1]
    appendFloat(buf, 0.0f);   // yAxis[0]
    appendFloat(buf, 16.0f);  // yAxis[1]

    VFS vfs;
    mountTex(vfs, "sprite_oob", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("sprite_oob");
    // Should disable sprite instead of crashing
    CHECK(header.isSprite == false);
}

TEST_CASE("Sprite with negative imageId — disables sprite") {
    uint32_t spriteFlag = (1u << 2);
    auto buf = makeTexHeader(1, 1, 2, 0, spriteFlag, 8, 8, 8, 8, 1);

    appendInt32(buf, 1); // mipmap_count
    std::vector<uint8_t> pixels(8 * 8 * 4, 0);
    appendMipmapV2(buf, 8, 8, pixels);

    appendTexVersion(buf, 2); // texs
    appendInt32(buf, 1);      // framecount

    appendInt32(buf, -1);     // imageId = -1 (invalid)
    appendFloat(buf, 0.1f);
    appendFloat(buf, 0.0f);
    appendFloat(buf, 0.0f);
    appendFloat(buf, 8.0f);
    appendFloat(buf, 0.0f);
    appendFloat(buf, 0.0f);
    appendFloat(buf, 8.0f);

    VFS vfs;
    mountTex(vfs, "sprite_neg", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("sprite_neg");
    CHECK(header.isSprite == false);
}

TEST_CASE("Valid sprite — parsed correctly") {
    uint32_t spriteFlag = (1u << 2);
    auto buf = makeTexHeader(1, 1, 2, 0, spriteFlag, 16, 16, 16, 16, 1);

    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(16 * 16 * 4, 0);
    appendMipmapV2(buf, 16, 16, pixels);

    appendTexVersion(buf, 2); // texs
    appendInt32(buf, 1);      // framecount = 1

    appendInt32(buf, 0);      // imageId = 0 (valid)
    appendFloat(buf, 0.5f);   // frametime
    appendFloat(buf, 0.0f);   // x
    appendFloat(buf, 0.0f);   // y
    appendFloat(buf, 16.0f);  // xAxis[0]
    appendFloat(buf, 0.0f);   // xAxis[1]
    appendFloat(buf, 0.0f);   // yAxis[0]
    appendFloat(buf, 16.0f);  // yAxis[1]

    VFS vfs;
    mountTex(vfs, "sprite_ok", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("sprite_ok");
    CHECK(header.isSprite == true);
    CHECK(header.spriteAnim.numFrames() == 1);
    auto& frame = header.spriteAnim.GetCurFrame();
    CHECK(frame.imageId == 0);
    CHECK(frame.frametime == doctest::Approx(0.5f));
}

TEST_CASE("LZ4 compressed mipmap") {
    auto buf = makeTexHeader(1, 1, 2, 0, 0, 4, 4, 4, 4, 1);

    // Create pixel data and compress it
    std::vector<uint8_t> pixels(4 * 4 * 4);
    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i] = static_cast<uint8_t>(i & 0xFF);
    }

    appendInt32(buf, 1); // mipmap_count
    appendMipmapV2(buf, 4, 4, pixels, /*compress=*/true);

    VFS vfs;
    mountTex(vfs, "test_lz4", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_lz4");
    REQUIRE(img != nullptr);
    REQUIRE(img->slots.size() == 1);
    REQUIRE(img->slots[0].mipmaps.size() == 1);
    CHECK(img->slots[0].mipmaps[0].size == static_cast<isize>(pixels.size()));
    // Verify decompressed data matches original
    auto* data = img->slots[0].mipmaps[0].data.get();
    for (size_t i = 0; i < pixels.size(); i++) {
        CHECK(data[i] == pixels[i]);
    }
}

TEST_CASE("Zero src_size returns nullptr") {
    auto buf = makeTexHeader(1, 1, 1, 0, 0, 4, 4, 4, 4, 1);
    appendInt32(buf, 1);       // mipmap_count
    appendInt32(buf, 4);       // mip width
    appendInt32(buf, 4);       // mip height
    appendInt32(buf, 0);       // src_size = 0

    VFS vfs;
    mountTex(vfs, "test_zero", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_zero");
    CHECK(img == nullptr);
}

TEST_CASE("Negative width returns nullptr") {
    auto buf = makeTexHeader(1, 1, 1, 0, 0, -1, 4, 4, 4, 1);
    appendInt32(buf, 1);
    appendInt32(buf, -1);      // mip width < 0
    appendInt32(buf, 4);       // mip height
    appendInt32(buf, 64);      // src_size
    std::vector<uint8_t> pixels(64, 0);
    append(buf, pixels.data(), pixels.size());

    VFS vfs;
    mountTex(vfs, "test_negw", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_negw");
    CHECK(img == nullptr);
}

TEST_CASE("Negative count returns nullptr") {
    auto buf = makeTexHeader(1, 1, 1, 0, 0, 4, 4, 4, 4, -1);

    VFS vfs;
    mountTex(vfs, "test_negcount", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_negcount");
    CHECK(img == nullptr);
}

TEST_CASE("Non-existent file returns nullptr") {
    VFS vfs;
    auto mockFs = std::make_unique<MockFs>();
    vfs.Mount("/assets", std::move(mockFs));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("nonexistent");
    CHECK(img == nullptr);
}

TEST_CASE("Texture flags — clampUVs and noInterpolation") {
    // flags: bit 0 = noInterpolation, bit 1 = clampUVs
    uint32_t flags = (1u << 0) | (1u << 1);
    auto buf = makeTexHeader(1, 1, 1, 0, flags, 2, 2, 2, 2, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(2 * 2 * 4, 0);
    appendMipmapV1(buf, 2, 2, pixels);

    VFS vfs;
    mountTex(vfs, "test_flags", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_flags");
    REQUIRE(img != nullptr);
    CHECK(img->header.sample.wrapS == TextureWrap::CLAMP_TO_EDGE);
    CHECK(img->header.sample.wrapT == TextureWrap::CLAMP_TO_EDGE);
    CHECK(img->header.sample.magFilter == TextureFilter::NEAREST);
    CHECK(img->header.sample.minFilter == TextureFilter::NEAREST);
}

TEST_CASE("Multiple images") {
    auto buf = makeTexHeader(1, 1, 1, 0, 0, 2, 2, 2, 2, 2); // count=2
    for (int i = 0; i < 2; i++) {
        appendInt32(buf, 1); // mipmap_count
        std::vector<uint8_t> pixels(2 * 2 * 4, static_cast<uint8_t>(i + 1));
        appendMipmapV1(buf, 2, 2, pixels);
    }

    VFS vfs;
    mountTex(vfs, "test_multi", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_multi");
    REQUIRE(img != nullptr);
    CHECK(img->header.count == 2);
    REQUIRE(img->slots.size() == 2);
    CHECK(img->slots[0].mipmaps[0].data.get()[0] == 1);
    CHECK(img->slots[1].mipmaps[0].data.get()[0] == 2);
}

} // TEST_SUITE
