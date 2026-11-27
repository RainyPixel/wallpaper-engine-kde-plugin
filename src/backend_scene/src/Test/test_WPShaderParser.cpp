#include <doctest.h>

#include "Fs/PhysicalFs.h"
#include "ParserTestFs.hpp"
#include "WPShaderParser.hpp"
#include "Vulkan/Shader.hpp"
#include <SPIRV-Reflect/spirv_reflect.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>

using namespace wallpaper;

namespace
{
bool CompileFragment(const std::string& source) {
    WPShaderParser::InitGlslang();
    fs::VFS                     vfs;
    WPShaderInfo                info;
    std::vector<ShaderCode>     codes;
    std::array<WPShaderUnit, 1> units { WPShaderUnit { ShaderType::FRAGMENT, source, {} } };
    const bool ok = WPShaderParser::CompileToSpv("test", units, codes, vfs, &info, {});
    WPShaderParser::FinalGlslang();
    return ok && codes.size() == 1 && ! codes[0].empty();
}

struct ShaderCache {
    std::filesystem::path path;
    ShaderCache() {
        char name[] = "/tmp/wekde-shader-cache-XXXXXX";
        REQUIRE(mkdtemp(name) != nullptr);
        path = name;
        WPShaderParser::InitGlslang();
    }
    ~ShaderCache() {
        WPShaderParser::FinalGlslang();
        std::filesystem::remove_all(path);
    }
    std::vector<ShaderCode> Compile(std::string_view scene_id) const {
        fs::VFS                     vfs;
        WPShaderInfo                info;
        std::vector<ShaderCode>     codes;
        std::array<WPShaderUnit, 1> units { WPShaderUnit {
            ShaderType::FRAGMENT, "void main() { gl_FragColor = vec4(1.0); }", {} } };
        if (! vfs.Mount("/cache", fs::CreatePhysicalFs(path.string()), "cache")) return {};
        if (! WPShaderParser::CompileToSpv(scene_id, units, codes, vfs, &info, {})) return {};
        return codes;
    }
};
} // namespace

TEST_SUITE("WPShaderParser") {
    TEST_CASE("sparse texture slots enable opacity only when a mask is supplied") {
        for (const bool has_mask : { false, true }) {
            INFO("mask supplied: " << has_mask);
            fs::VFS                            vfs;
            WPShaderInfo                       info;
            const std::vector<WPShaderTexInfo> textures { { true }, { has_mask }, { true } };
            const auto                         source = WPShaderParser::PreShaderSrc(vfs,
                                                                                     R"(
uniform sampler2D g_Texture0;
uniform sampler2D g_Texture1; // {"combo":"OPACITY"}
uniform sampler2D g_Texture2;
void main() {
    vec4 color = texSample2D(g_Texture0, vec2(0.0)) + texSample2D(g_Texture2, vec2(0.0));
#if OPACITY
    color *= texSample2D(g_Texture1, vec2(0.0));
#endif
    gl_FragColor = color;
}
)",
                                                                                     &info,
                                                                                     textures);
            CHECK(info.combos.at("OPACITY") == (has_mask ? "1" : "0"));
            WPShaderParser::InitGlslang();
            std::array<WPShaderUnit, 1> units { WPShaderUnit { ShaderType::FRAGMENT, source, {} } };
            std::vector<ShaderCode>     codes;
            const bool                  compiled =
                WPShaderParser::CompileToSpv("test", units, codes, vfs, &info, textures);
            CHECK(compiled);
            if (compiled) {
                std::vector<vulkan::Uni_ShaderSpv> shaders;
                vulkan::ShaderReflected            reflected;
                const bool reflected_ok = vulkan::GenReflect(codes, shaders, reflected);
                CHECK(reflected_ok);
                if (reflected_ok) {
                    CHECK(reflected.binding_map.count("g_Texture0") == 1);
                    CHECK(reflected.binding_map.count("g_Texture2") == 1);
                    CHECK(reflected.binding_map.count("g_Texture1") == (has_mask ? 1 : 0));
                }
            }
            WPShaderParser::FinalGlslang();
        }
    }

    TEST_CASE("texture combos outside the supplied slot range stay disabled") {
        fs::VFS      vfs;
        WPShaderInfo info;
        WPShaderParser::PreShaderSrc(
            vfs,
            "uniform sampler2D g_Texture3; // {\"combo\":\"MASK\"}\nvoid main() {}",
            &info,
            { { true }, { false }, { true } });
        CHECK(info.combos.at("MASK") == "0");
    }

    TEST_CASE("HLSL math overloads and scalar vector declarators compile") {
        CHECK(CompileFragment(R"(
void main() {
    vec2 a = 0.0, b = .5, c = -1e-2;
    vec3 x = pow(vec3(0.5), 2.0);
    vec3 y = max(x, 0.0) + max(0.0, x) + min(x, 1.0) + min(1.0, x);
    float z = max(0, log10(10.0)) + min(1, 0.5);
    gl_FragColor = vec4(y + vec3(a + b + c, z), 1.0);
}
)"));
    }

    TEST_CASE("texture and vector expressions truncate at the initializer boundary") {
        CHECK(CompileFragment(R"(
uniform sampler2D g_Texture0;
varying vec3 v_TexCoord;
void main() {
    vec3 albedo = texSample2D(g_Texture0, v_TexCoord.xy), second = texSample2D(g_Texture0, vec2(0.0));
    vec2 offset = v_TexCoord - CAST2(vec3(0.0));
    vec4 wide = vec4(albedo, 1.0);
    vec2 narrow = wide * 0.5;
    gl_FragColor = vec4(albedo + second + vec3(offset + narrow, 0.0), 1.0);
}
)"));
    }

    TEST_CASE("an authored pow overload keeps its definition") {
        CHECK(CompileFragment(R"(
float pow(float base, int exponent) { return base * exponent; }
void main() { gl_FragColor = vec4(pow(0.25, 2)); }
)"));
    }

    TEST_CASE("valid scalar swizzles and nested scalar assignments keep their types") {
        CHECK(CompileFragment(R"(
uniform sampler2D g_Texture0;
float selectValue(float a, float b) { return a + b; }
void main() {
    vec3 wide = vec3(1.0);
    struct Data { float wide; } data;
    data.wide = 0.5;
    vec2 field = data . wide * vec2(1.0);
    vec3 shadowed = vec3(0.0);
    { int shadowed = 1; field += shadowed * vec2(1.0); }
    vec3 a = wide * vec2(0.5).x;
    vec3 b = wide * (vec2(0.5)).x;
    vec3 projected = wide - vec2(0.5) * mat3x2(1.0);
    float s = 0.0;
    vec2 c = vec2(selectValue(s = 1.0, 2.0));
    vec3 d = texture(g_Texture0, vec2(0.0)).gba, e = vec3(1.0);
    gl_FragColor = vec4(a + b + projected + d + e + vec3(c + field, 0.0), 1.0);
}
)"));
    }

    TEST_CASE("CRLF directives and an EOF without newline compile") {
        CHECK(
            CompileFragment("void main() {\r\n#if 1\r\ngl_FragColor = vec4(1.0);\r\n#endif\r\n}"));
    }

    TEST_CASE("Workshop stray endif is ignored but failed preprocessing is rejected") {
        CHECK(CompileFragment(R"(
/*
#if 0
*/
void main() {
#if 1
  gl_FragColor = vec4(1.0);
#endif
#endif // Some Workshop sources carry a second endif.
}
)"));
        CHECK_FALSE(CompileFragment("void main() { gl_FragColor = vec4(1.0); }\n#error bad\n"));
    }

    TEST_CASE("numeric comparisons can participate in arithmetic but still work in control flow") {
        CHECK(CompileFragment(R"(
void main() {
    float ratio = 0.5;
    float threshold = 0.6;
    int counter = 0;
    if (ratio < threshold) ++counter;
    ratio *= (ratio < threshold) * 6.0;
    float value = 1 ? ratio : 0.0;
    gl_FragColor = vec4(value + float(counter));
}
)"));
    }

    TEST_CASE("Workshop sampler and scalar conversions compile without reducing cube coordinates") {
        CHECK(CompileFragment(R"(
uniform sampler2D g_Texture0;
uniform samplerCube g_TextureCube;
varying vec3 uvw;
void main() {
    float sampled = texSample2D(g_Texture0, uvw * 0.5);
    float lod = texSample2DLod(g_Texture0, uvw, 0.0);
    float lowered = uvw + vec3(0.5);
    float scalar = uvw.z * uvw.y;
    int index = scalar;
    vec2 offset = scalar;
    offset += uvw;
    vec3 mixed = lerp(sampled, uvw, 0.5);
    gl_FragColor = vec4(pow(mixed, 2) * lowered, lod + float(index) + offset.x)
                   + texture(g_TextureCube, uvw);
}
)"));
    }

    TEST_CASE(
        "known input arguments truncate while overloads and writable arguments retain types") {
        CHECK(CompileFragment(R"(
float consume(vec2 value) { return value.x; }
void update(out vec2 x, inout vec3 y) { x = vec2(1.0); y *= 2.0; }
float selected(vec2 value) { return value.x; }
mat2 selected(vec3 value) { return mat2(value.y); }
void main() {
    vec3 wide = vec3(0.5);
    vec2 narrow;
    update(narrow, wide);
    mat2 frame = selected(wide);
    gl_FragColor = vec4(consume(wide) + frame[0][0] + narrow.x);
}
)"));
    }

    TEST_CASE("a writable fragment input uses a local copy and still links to the vertex stage") {
        fs::VFS                     vfs;
        WPShaderInfo                info;
        std::vector<ShaderCode>     codes;
        std::array<WPShaderUnit, 2> units {
            WPShaderUnit { ShaderType::VERTEX,
                           R"(
varying vec2 coord;
void main() {
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    coord = vec2(0.25);
}
)",
                           {} },
            WPShaderUnit { ShaderType::FRAGMENT,
                           R"(
varying vec2 coord;
void main() {
    coord.x *= 2.0;
    gl_FragColor = vec4(coord, 0.0, 1.0);
}
)",
                           {} },
        };
        WPShaderParser::InitGlslang();
        CHECK(WPShaderParser::CompileToSpv("test", units, codes, vfs, &info, {}));
        WPShaderParser::FinalGlslang();
        CHECK(codes.size() == 2);
    }

    TEST_CASE("a narrower fragment input preserves the linked varying and its location") {
        fs::VFS                     vfs;
        WPShaderInfo                info;
        std::vector<ShaderCode>     codes;
        std::array<WPShaderUnit, 2> units {
            WPShaderUnit { ShaderType::VERTEX,
                           R"(
flat out vec4 coord;
void main() {
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    coord = vec4(0.25, 0.5, 0.75, 1.0);
}
)",
                           {} },
            WPShaderUnit { ShaderType::FRAGMENT,
                           R"(
flat in vec2 coord;
void main() {
    gl_FragColor = vec4(coord, 0.0, 1.0);
}
)",
                           {} },
        };
        WPShaderParser::InitGlslang();
        const bool compiled = WPShaderParser::CompileToSpv("test", units, codes, vfs, &info, {});
        WPShaderParser::FinalGlslang();
        REQUIRE(compiled);
        REQUIRE(codes.size() == 2);

        const spv_reflect::ShaderModule vertex(codes[0], SPV_REFLECT_MODULE_FLAG_NO_COPY);
        const spv_reflect::ShaderModule fragment(codes[1], SPV_REFLECT_MODULE_FLAG_NO_COPY);
        const auto*                     output = vertex.GetOutputVariableByLocation(0);
        const auto*                     input  = fragment.GetInputVariableByLocation(0);
        REQUIRE(output);
        REQUIRE(input);
        CHECK(std::string_view(output->name) == "coord");
        CHECK(std::string_view(input->name) == "coord");
        CHECK(output->format == SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT);
        CHECK(input->format == output->format);
    }

    TEST_CASE("different scenes share cache hits and damaged entries recompile") {
        ShaderCache cache;
        const auto  compiled = cache.Compile("one");
        REQUIRE(compiled.size() == 1);
        std::filesystem::path artifact;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(cache.path)) {
            if (! entry.is_regular_file()) continue;
            REQUIRE(artifact.empty());
            artifact = entry.path();
        }
        REQUIRE_FALSE(artifact.empty());

        const auto read = [&artifact] {
            std::ifstream file(artifact, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), {});
        };
        const auto valid = read();
        REQUIRE(valid.size() > 20);
        const auto old_time = std::filesystem::last_write_time(artifact) - std::chrono::hours(1);
        std::filesystem::last_write_time(artifact, old_time);
        const auto recorded_time = std::filesystem::last_write_time(artifact);
        CHECK(cache.Compile("two") == compiled);
        CHECK(std::filesystem::last_write_time(artifact) == recorded_time);

        auto wrong_version = valid;
        wrong_version[7]   = '1';
        auto no_stages     = valid;
        no_stages[8]       = 0;
        auto invalid_magic = valid;
        invalid_magic[16]  = 0;
        for (const auto& invalid : { std::string {},
                                     wrong_version,
                                     no_stages,
                                     valid.substr(0, valid.size() - 1),
                                     invalid_magic,
                                     valid + 'x' }) {
            std::ofstream(artifact, std::ios::binary) << invalid;
            CHECK(cache.Compile("two") == compiled);
            CHECK(read() == valid);
        }
    }

    TEST_CASE("simultaneous scenes can compile and reuse the same cache entry") {
        ShaderCache cache;
        auto        first    = std::async(std::launch::async, [&] {
            return cache.Compile("one");
        });
        auto        second   = std::async(std::launch::async, [&] {
            return cache.Compile("two");
        });
        const auto  compiled = first.get();
        REQUIRE(compiled.size() == 1);
        CHECK(second.get() == compiled);
        CHECK(cache.Compile("three") == compiled);
    }

    TEST_CASE("includes follow complete structs even with comment braces") {
        fs::VFS           vfs;
        auto              files           = std::make_unique<ParserTestFs>();
        const std::string include         = "float shade(float x) { return x; }\n";
        files->files["/shaders/helper.h"] = { include.begin(), include.end() };
        REQUIRE(vfs.Mount("/assets", std::move(files)));
        WPShaderInfo info;
        const auto   source = WPShaderParser::PreShaderSrc(vfs,
                                                           R"(
#include "helper.h"
#define UNUSED_BRACE {
// {
struct Grid
{
    // } is not the end of this struct.
    vec2 pos;
};
void main() { Grid g; g.pos = vec2(1.0); gl_FragColor = vec4(shade(g.pos.x)); }
)",
                                                           &info,
                                                           {});
        CHECK(CompileFragment(source));
    }
    TEST_CASE("terminal include without a newline is loaded and compiles") {
        fs::VFS           vfs;
        auto              files           = std::make_unique<ParserTestFs>();
        const std::string include         = "#include \"math.h\"";
        const std::string nested          = "float shade(float x) { return x; }";
        files->files["/shaders/helper.h"] = { include.begin(), include.end() };
        files->files["/shaders/math.h"]   = { nested.begin(), nested.end() };
        REQUIRE(vfs.Mount("/assets", std::move(files)));
        WPShaderInfo info;
        std::string  source;
        REQUIRE_NOTHROW(
            source = WPShaderParser::PreShaderSrc(
                vfs,
                "void main() { gl_FragColor = vec4(shade(1.0)); }\n#include \"helper.h\"",
                &info,
                {}));
        CHECK(CompileFragment(source));
    }
}
