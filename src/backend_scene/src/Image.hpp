#pragma once
#include <cstdint>
#include <algorithm>
#include <bit>
#include <limits>
#include <vector>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <functional>

#include "Core/Literals.hpp"
#include "Type.hpp"
#include "SpriteAnimation.hpp"
#include "Scene/SceneTexture.h"
#include "Core/NoCopyMove.hpp"

namespace wallpaper
{

union ImageExtra {
    int32_t val { 0 };
    char    str[125];
};

typedef std::unique_ptr<uint8_t, std::function<void(uint8_t*)>> ImageDataPtr;

struct ImageData {
    i32          width { 0 };
    i32          height { 0 };
    isize        size { 0 };
    ImageDataPtr data {};
    ImageData() = default;
};

// Where a video texture's payload lives. A Wallpaper Engine "video texture" is a
// .tex whose single mip holds an MP4/WebM file verbatim. WPTexImageParser is the
// only place that locates it; the renderer decodes it from the scene VFS.
struct EmbeddedVideo {
    std::string path;         // VFS path of the .tex
    i64         offset { 0 }; // first byte of the container inside that file
    i64         size { 0 };   // container size in bytes
};

struct ImageHeader {
    // these two size is not for tex, just come from we
    // using Slot's size for tex
    i32 width { 0 };
    i32 height { 0 };
    i32 mapWidth { 0 };
    i32 mapHeight { 0 };

    bool mipmap_larger { false };
    bool mipmap_pow2 { false };

    ImageType     type { ImageType::UNKNOWN };
    TextureFormat format { TextureFormat::RGBA8 };
    i32           count { 0 };

    bool          isSprite { false };
    TextureSample sample;

    SpriteAnimation spriteAnim;
    // for specific property
    std::unordered_map<std::string, ImageExtra> extraHeader;

    // Set only for video textures (see EmbeddedVideo). The image itself is then
    // a transparent placeholder, used whenever the video cannot be decoded.
    std::optional<EmbeddedVideo> video;
};

// slot is one singal image
struct Image : NoCopy, NoMove {
    struct Slot {
        i32 width { 0 };
        i32 height { 0 };

        std::vector<ImageData> mipmaps;

        operator bool() const { return width > 0 && height > 0 && ! mipmaps.empty(); }

        bool HasValidMipmaps(TextureFormat format) const {
            if (! *this || mipmaps.size() > std::bit_width((u32)std::max(width, height)))
                return false;
            for (usize level = 0; level < mipmaps.size(); ++level) {
                const auto& mip = mipmaps[level];
                if (mip.width != std::max(1, width >> level) ||
                    mip.height != std::max(1, height >> level) || ! mip.data || mip.size <= 0 ||
                    (u64)mip.size > std::numeric_limits<u32>::max())
                    return false;
                const u64 w = (u64)mip.width, h = (u64)mip.height;
                u64       bytes = 0;
                switch (format) {
                case TextureFormat::BC1: bytes = ((w + 3) / 4) * ((h + 3) / 4) * 8; break;
                case TextureFormat::BC2:
                case TextureFormat::BC3: bytes = ((w + 3) / 4) * ((h + 3) / 4) * 16; break;
                case TextureFormat::RGB8: bytes = w * h * 3; break;
                case TextureFormat::RGBA8: bytes = w * h * 4; break;
                case TextureFormat::RG8: bytes = w * h * 2; break;
                case TextureFormat::R8: bytes = w * h; break;
                default: return false;
                }
                if ((u64)mip.size < bytes) return false;
            }
            return true;
        }
    };
    ImageHeader       header;
    std::vector<Slot> slots;
    std::string       key;
};

} // namespace wallpaper
