#include "WPSceneText.hpp"

#include "Fs/VFS.h"
#include "Scene/SceneNode.h"
#include "SpecTexs.hpp"
#include "Utils/Logging.h"
#include "Vulkan/ShaderComp.hpp"
#include "WPJson.hpp"
#include "WPSceneScript.hpp"
#include "WPTexImageParser.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

using namespace wallpaper;

namespace
{
constexpr int   atlas_width      = 1024;
constexpr int   atlas_max_height = 2048;
constexpr usize max_glyphs       = (std::numeric_limits<uint16_t>::max() + 1u) / 4;

std::u32string Codepoints(std::string_view text) {
    // Scene JSON strings have already passed UTF-8 validation.
    std::u32string result;
    for (const unsigned char byte : text) {
        if (byte < 0x80)
            result.push_back(byte);
        else if (byte < 0xc0)
            result.back() = (result.back() << 6) | (byte & 0x3f);
        else if (byte < 0xe0)
            result.push_back(byte & 0x1f);
        else if (byte < 0xf0)
            result.push_back(byte & 0x0f);
        else
            result.push_back(byte & 0x07);
    }
    return result;
}

struct Glyph {
    float advance;
    float left;
    float top;
    float width;
    float height;
    float x;
    float y;
};

struct TextLayout {
    std::unordered_map<char32_t, Glyph> glyphs;
    float                               line_height;
    float                               ascent;
    float                               glyph_scale;
    float                               atlas_height;
};

std::shared_ptr<Image> GenerateAtlas(FT_Face face, std::u32string glyphs, TextLayout& layout) {
    glyphs += U"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz -";
    std::sort(glyphs.begin(), glyphs.end());
    glyphs.erase(std::unique(glyphs.begin(), glyphs.end()), glyphs.end());
    std::vector<uint8_t> pixels(atlas_width);
    int                  x = 1, y = 1, row_height = 0;
    for (const auto codepoint : glyphs) {
        if (codepoint == '\n' ||
            FT_Load_Char(face, codepoint, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP)) {
            continue;
        }
        const auto& slot   = *face->glyph;
        const auto& bitmap = slot.bitmap;
        if (bitmap.width >= atlas_width - 2 || bitmap.rows >= atlas_max_height - 2) continue;
        const auto width  = static_cast<int>(bitmap.width);
        const auto height = static_cast<int>(bitmap.rows);
        if (x + width + 1 >= atlas_width) {
            x = 1;
            y += row_height + 2;
            row_height = 0;
        }
        if (y + height + 1 >= atlas_max_height) break;
        if (width > 0 && height > 0) {
            if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) continue;
            const auto required = static_cast<usize>(y + height + 1) * atlas_width;
            if (pixels.size() < required) pixels.resize(required);
            for (int row = 0; row < height; ++row) {
                std::memcpy(pixels.data() + (y + row) * atlas_width + x,
                            bitmap.buffer + static_cast<isize>(row) * bitmap.pitch,
                            static_cast<usize>(width));
            }
            row_height = std::max(row_height, height);
        }
        layout.glyphs.emplace(codepoint,
                              Glyph { static_cast<float>(slot.advance.x) / 64.0f,
                                      static_cast<float>(slot.bitmap_left),
                                      static_cast<float>(slot.bitmap_top),
                                      static_cast<float>(width),
                                      static_cast<float>(height),
                                      static_cast<float>(x),
                                      static_cast<float>(y) });
        x += width + 2;
    }
    if (pixels.size() == atlas_width) return nullptr;

    auto       image    = std::make_shared<Image>();
    const auto height   = static_cast<i32>(pixels.size() / atlas_width);
    layout.atlas_height = static_cast<float>(height);
    const std::string_view data(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    image->key          = "wekde-text/" + std::to_string(std::hash<std::string_view> {}(data));
    image->header.width = image->header.mapWidth = atlas_width;
    image->header.height = image->header.mapHeight = height;
    image->header.count                            = 1;
    image->header.format                           = TextureFormat::R8;
    image->header.sample                           = { TextureWrap::CLAMP_TO_EDGE,
                                                       TextureWrap::CLAMP_TO_EDGE,
                                                       TextureFilter::LINEAR,
                                                       TextureFilter::LINEAR };
    auto& slot                                     = image->slots.emplace_back();
    slot.width                                     = atlas_width;
    slot.height                                    = height;
    auto& mip                                      = slot.mipmaps.emplace_back();
    mip.width                                      = atlas_width;
    mip.height                                     = height;
    mip.size                                       = static_cast<isize>(pixels.size());
    mip.data = ImageDataPtr(new uint8_t[pixels.size()], std::default_delete<uint8_t[]>());
    std::copy(pixels.begin(), pixels.end(), mip.data.get());
    return image;
}

std::shared_ptr<SceneShader> CompileTextShader() {
    const vulkan::ShaderCompUnit units[] {
        { EShLangVertex, R"(#version 450
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 tint;
layout(set = 0, binding = 0, std140) uniform TextUniforms {
    mat4 g_ModelViewProjectionMatrix;
    vec4 g_Color4;
};
void main() {
    gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);
    uv = a_TexCoord;
    tint = g_Color4;
}
)" },
        { EShLangFragment, R"(#version 450
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 tint;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 1) uniform sampler2D g_Texture0;
void main() {
    color = vec4(tint.rgb, tint.a * texture(g_Texture0, uv).r);
}
)" },
    };
    std::vector<vulkan::Uni_ShaderSpv> spvs(std::size(units));
    if (! vulkan::CompileAndLinkShaderUnits(
            units, { .client_ver = glslang::EShTargetVulkan_1_1 }, spvs)) {
        return nullptr;
    }
    auto shader  = std::make_shared<SceneShader>();
    shader->name = "wekde-text";
    for (auto& spv : spvs) shader->codes.push_back(std::move(spv->spirv));
    return shader;
}

std::shared_ptr<SceneMesh> CreateMesh(usize capacity) {
    auto mesh = std::make_shared<SceneMesh>(true);
    mesh->AddVertexArray(SceneVertexArray({ { WE_IN_POSITION.data(), VertexType::FLOAT3, false },
                                            { WE_IN_TEXCOORD.data(), VertexType::FLOAT2, false } },
                                          capacity * 4));
    SceneIndexArray indices(capacity);
    for (usize i = 0; i < capacity; ++i) {
        const auto                    first = static_cast<uint16_t>(i * 4);
        const std::array<uint16_t, 6> quad { first,
                                             static_cast<uint16_t>(first + 1),
                                             static_cast<uint16_t>(first + 2),
                                             static_cast<uint16_t>(first + 2),
                                             static_cast<uint16_t>(first + 1),
                                             static_cast<uint16_t>(first + 3) };
        indices.AssignHalf(i * 6, quad);
    }
    indices.SetRenderDataCount(0);
    mesh->AddIndexArray(std::move(indices));
    return mesh;
}

void LayoutText(SceneMesh& mesh, const TextLayout& layout, std::u32string_view text) {
    std::vector<float> vertices;
    const auto         rows = std::count(text.begin(), text.end(), U'\n') + 1;
    float              baseline =
        (static_cast<float>(rows) * 0.5f * layout.line_height - layout.ascent) * layout.glyph_scale;
    while (true) {
        const auto newline = text.find(U'\n');
        const auto line    = text.substr(0, newline);
        float      width { 0 };
        for (const auto codepoint : line) {
            const auto glyph = layout.glyphs.find(codepoint);
            if (glyph != layout.glyphs.end()) width += glyph->second.advance * layout.glyph_scale;
        }
        float cursor = -width * 0.5f;
        for (const auto codepoint : line) {
            const auto entry = layout.glyphs.find(codepoint);
            if (entry == layout.glyphs.end()) continue;
            const auto& glyph = entry->second;
            if (glyph.width > 0 && glyph.height > 0) {
                const float left   = cursor + glyph.left * layout.glyph_scale;
                const float right  = left + glyph.width * layout.glyph_scale;
                const float top    = baseline + glyph.top * layout.glyph_scale;
                const float bottom = top - glyph.height * layout.glyph_scale;
                const float u = glyph.x / atlas_width, v = glyph.y / layout.atlas_height;
                const float u_end = (glyph.x + glyph.width) / atlas_width;
                const float v_end = (glyph.y + glyph.height) / layout.atlas_height;
                const float quad[] { left,  bottom, 0, u,     v_end, left,  top, 0, u,     v,
                                     right, bottom, 0, u_end, v_end, right, top, 0, u_end, v };
                vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
            }
            cursor += glyph.advance * layout.glyph_scale;
        }
        if (newline == text.npos) break;
        text.remove_prefix(newline + 1);
        baseline -= layout.line_height * layout.glyph_scale;
    }
    mesh.GetVertexArray(0).SetVertexs(0, vertices);
    mesh.GetIndexArray(0).SetRenderDataCount(vertices.size() / 20 * 3);
    mesh.SetDirty();
}
} // namespace

std::function<void()> wallpaper::AddSceneText(SceneNode& node, const nlohmann::json& object,
                                              WPTextScript script, fs::VFS& vfs,
                                              WPTexImageParser&             textures,
                                              std::shared_ptr<SceneShader>& shader) {
    bool visible { true };
    GET_JSON_NAME_VALUE_NOWARN(object, "visible", visible);
    if (! visible) return {};

    std::string font;
    float       point_size { 32.0f };
    GET_JSON_NAME_VALUE_NOWARN(object, "font", font);
    GET_JSON_NAME_VALUE_NOWARN(object, "pointsize", point_size);
    if (! std::isfinite(point_size) || point_size <= 0) return {};
    const std::filesystem::path font_path(font);
    if (font.empty() || font.find('\0') != std::string::npos || font_path.is_absolute() ||
        std::any_of(font_path.begin(), font_path.end(), [](const auto& part) {
            return part == "..";
        })) {
        LOG_ERROR("invalid scene text font path");
        return {};
    }

    FT_Library library { nullptr };
    if (FT_Init_FreeType(&library)) return {};
    const std::unique_ptr<std::remove_pointer_t<FT_Library>, decltype(&FT_Done_FreeType)> free_type(
        library, FT_Done_FreeType);
    const auto font_data = fs::GetFileContent(vfs, "/assets/" + font);
    FT_Face    raw_face { nullptr };
    if (font_data.empty() || FT_New_Memory_Face(library,
                                                reinterpret_cast<const FT_Byte*>(font_data.data()),
                                                static_cast<FT_Long>(font_data.size()),
                                                0,
                                                &raw_face)) {
        LOG_ERROR("unable to load scene text font '%s'", font.c_str());
        return {};
    }
    const std::unique_ptr<std::remove_pointer_t<FT_Face>, decltype(&FT_Done_Face)> face(
        raw_face, FT_Done_Face);
    const float pixels = point_size * (96.0f / 72.0f);
    if (! std::isfinite(pixels)) return {};
    const auto raster_size = static_cast<FT_UInt>(std::round(std::clamp(pixels, 1.0f, 128.0f)));
    if (FT_Set_Pixel_Sizes(face.get(), 0, raster_size)) {
        LOG_ERROR("unable to load scene text font '%s'", font.c_str());
        return {};
    }

    auto       extra_glyphs = Codepoints(script.ExtraGlyphs());
    const auto capacity     = std::min(32 + 2 * extra_glyphs.size(), max_glyphs);
    TextLayout layout { {},
                        static_cast<float>(face->size->metrics.height) / 64.0f,
                        static_cast<float>(face->size->metrics.ascender) / 64.0f,
                        pixels / static_cast<float>(raster_size),
                        0 };
    auto       image = GenerateAtlas(face.get(), std::move(extra_glyphs), layout);
    if (! image) return {};
    if (! shader) shader = CompileTextShader();
    if (! shader) return {};

    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    float                alpha { 1.0f }, brightness { 1.0f };
    GET_JSON_NAME_VALUE_NOWARN(object, "color", color);
    GET_JSON_NAME_VALUE_NOWARN(object, "alpha", alpha);
    GET_JSON_NAME_VALUE_NOWARN(object, "brightness", brightness);
    SceneMaterial material;
    material.name     = "wekde-text";
    material.blenmode = BlendMode::Translucent;
    material.textures.push_back(image->key);
    material.customShader.shader = shader;
    material.customShader.constValues["g_Color4"] =
        std::array { color[0] * brightness, color[1] * brightness, color[2] * brightness, alpha };

    textures.AddGeneratedImage(std::move(image));
    auto mesh = CreateMesh(capacity);
    mesh->AddMaterial(std::move(material));
    node.AddMesh(mesh);

    auto update = [mesh,
                   script = std::move(script),
                   layout = std::move(layout),
                   capacity,
                   last_second = std::time_t(-1),
                   last_text   = std::string()]() mutable {
        const auto now = std::time(nullptr);
        if (now == last_second) return;
        last_second = now;
        std::tm local_time {};
        if (! localtime_r(&now, &local_time)) return;
        auto text = script.Format(local_time);
        if (text == last_text) return;
        last_text        = std::move(text);
        const auto chars = Codepoints(last_text);
        LayoutText(*mesh, layout, std::u32string_view(chars).substr(0, capacity));
    };
    update();
    return update;
}
