#include "WPShaderParser.hpp"

#include "Fs/IBinaryStream.h"
#include "Utils/Logging.h"
#include "WPJson.hpp"

#include "wpscene/WPUniform.h"
#include "Fs/VFS.h"
#include "Utils/Sha.hpp"
#include "Utils/String.h"
#include "WPCommon.hpp"

#include "Vulkan/ShaderComp.hpp"

#include <regex>
#include <stack>
#include <charconv>
#include <string>

static constexpr std::string_view SHADER_PLACEHOLD { "__SHADER_PLACEHOLD__" };

#define SHADER_DIR    "spvs01"
#define SHADER_SUFFIX "spvs"

using namespace wallpaper;

namespace
{

static constexpr const char* pre_shader_code = R"(#version 330
#define GLSL 1
#define HLSL 0
#define highp

#define CAST2(x) (vec2(x))
#define CAST3(x) (vec3(x))
#define CAST4(x) (vec4(x))
#define CAST3X3(x) (mat3(x))

#define texSample2D texture
#define texSample2DLod textureLod
#define mul(x, y) ((y) * (x))
#define frac fract
#define atan2 atan
#define fmod(x, y) (x-y*trunc(x/y))
#define ddx dFdx
#define ddy(x) dFdy(-(x))
#define saturate(x) (clamp(x, 0.0, 1.0))

#define max(x, y) max(y, x)

#define float1 float
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define lerp mix

__SHADER_PLACEHOLD__

)";

static constexpr const char* pre_shader_code_vert = R"(
#define attribute in
#define varying out

)";
static constexpr const char* pre_shader_code_frag = R"(
#define varying in
#define gl_FragColor glOutColor
out vec4 glOutColor;

)";

inline std::string LoadGlslInclude(fs::VFS& vfs, const std::string& input) {
    std::string::size_type pos = 0;
    std::string            output;
    std::string::size_type linePos = std::string::npos;

    while (linePos = input.find("#include", pos), linePos != std::string::npos) {
        auto lineEnd  = input.find_first_of('\n', linePos);
        auto lineSize = lineEnd - linePos;
        auto lineStr  = input.substr(linePos, lineSize);
        output.append(input.substr(pos, linePos - pos));

        auto inP         = lineStr.find_first_of('\"') + 1;
        auto inE         = lineStr.find_last_of('\"');
        auto includeName = lineStr.substr(inP, inE - inP);
        auto includeSrc  = fs::GetFileContent(vfs, "/assets/shaders/" + includeName);
        output.append("\n//-----include " + includeName + "\n");
        output.append(LoadGlslInclude(vfs, includeSrc));
        output.append("\n//-----include end\n");

        pos = lineEnd;
    }
    output.append(input.substr(pos));
    return output;
}

inline void ParseWPShader(const std::string& src, WPShaderInfo* pWPShaderInfo,
                          const std::vector<WPShaderTexInfo>& texinfos) {
    auto& combos       = pWPShaderInfo->combos;
    auto& wpAliasDict  = pWPShaderInfo->alias;
    auto& shadervalues = pWPShaderInfo->svs;
    auto& defTexs      = pWPShaderInfo->defTexs;
    idx   texcount     = std::ssize(texinfos);

    // pos start of line
    std::string::size_type pos = 0, lineEnd = std::string::npos;
    while ((lineEnd = src.find_first_of(('\n'), pos)), true) {
        const auto clineEnd = lineEnd;
        const auto line     = src.substr(pos, lineEnd - pos);

        /*
        if(line.find("attribute ") != std::string::npos || line.find("in ") != std::string::npos) {
            update_pos = true;
        }
        */
        if (line.find("// [COMBO]") != std::string::npos) {
            nlohmann::json combo_json;
            if (PARSE_JSON(line.substr(line.find_first_of('{')), combo_json)) {
                if (combo_json.contains("combo")) {
                    std::string name;
                    int32_t     value = 0;
                    GET_JSON_NAME_VALUE(combo_json, "combo", name);
                    GET_JSON_NAME_VALUE(combo_json, "default", value);
                    combos[name] = std::to_string(value);
                }
            }
        } else if (line.find("uniform ") != std::string::npos) {
            if (line.find("// {") != std::string::npos) {
                nlohmann::json sv_json;
                if (PARSE_JSON(line.substr(line.find_first_of('{')), sv_json)) {
                    std::vector<std::string> defines =
                        utils::SpliteString(line.substr(0, line.find_first_of(';')), ' ');

                    std::string material;
                    GET_JSON_NAME_VALUE_NOWARN(sv_json, "material", material);
                    if (! material.empty()) wpAliasDict[material] = defines.back();

                    ShaderValue sv;
                    std::string name  = defines.back();
                    bool        istex = name.compare(0, 9, "g_Texture") == 0;
                    if (istex) {
                        wpscene::WPUniformTex wput;
                        wput.FromJson(sv_json);
                        i32 index { 0 };
                        STRTONUM(name.substr(9), index);
                        if (! wput.default_.empty()) defTexs.push_back({ index, wput.default_ });
                        if (! wput.combo.empty()) {
                            if (index >= texcount)
                                combos[wput.combo] = "0";
                            else
                                combos[wput.combo] = "1";
                        }
                        if (index < texcount && texinfos[(usize)index].enabled) {
                            auto& compos = texinfos[(usize)index].composEnabled;

                            usize num = std::min(std::size(compos), std::size(wput.components));
                            for (usize i = 0; i < num; i++) {
                                if (compos[i]) combos[wput.components[i].combo] = "1";
                            }
                        }

                    } else {
                        if (sv_json.contains("default")) {
                            auto        value = sv_json.at("default");
                            ShaderValue sv;
                            name = defines.back();
                            if (value.is_string()) {
                                std::vector<float> v;
                                GET_JSON_VALUE(value, v);
                                sv = std::span<const float>(v);
                            } else if (value.is_number()) {
                                sv.setSize(1);
                                GET_JSON_VALUE(value, sv[0]);
                            }
                            shadervalues[name] = sv;
                        }
                        if (sv_json.contains("combo")) {
                            std::string name;
                            GET_JSON_NAME_VALUE(sv_json, "combo", name);
                            combos[name] = "1";
                        }
                    }
                    if (defines.back()[0] != 'g') {
                        LOG_INFO("PreShaderSrc User shadervalue not supported");
                    }
                }
            }
        }

        // end
        if (line.find("void main()") != std::string::npos || clineEnd == std::string::npos) {
            break;
        }
        pos = lineEnd + 1;
    }
}

inline usize FindIncludeInsertPos(const std::string& src, usize startPos) {
    /* rule:
    after attribute/varying/uniform/struct
    befor any func
    not in {}
    not in #if #endif
    */
    (void)startPos;

    auto NposToZero = [](usize p) {
        return p == std::string::npos ? 0 : p;
    };
    auto search = [](const std::string& p, usize pos, const auto& re) {
        auto        startpos = p.begin() + (isize)pos;
        std::smatch match;
        if (startpos < p.end() && std::regex_search(startpos, p.end(), match, re)) {
            return pos + (usize)match.position();
        }
        return std::string::npos;
    };
    auto searchLast = [](const std::string& p, const auto& re) {
        auto        startpos = p.begin();
        std::smatch match;
        while (startpos < p.end() && std::regex_search(startpos, p.end(), match, re)) {
            startpos++;
            startpos += match.position();
        }
        return startpos >= p.end() ? std::string::npos : usize(startpos - p.begin());
    };
    auto nextLinePos = [](const std::string& p, usize pos) {
        return p.find_first_of('\n', pos) + 1;
    };

    usize mainPos  = src.find("void main(");
    bool  two_main = src.find("void main(", mainPos + 2) != std::string::npos;
    if (two_main) return 0;

    usize pos;
    {
        const std::regex reAfters(R"(\n(attribute|varying|uniform|struct) )");
        usize            afterPos = searchLast(src, reAfters);
        if (afterPos != std::string::npos) {
            afterPos = nextLinePos(src, afterPos + 1);
        }
        pos = std::min({ NposToZero(afterPos), mainPos });
    }
    {
        std::stack<usize> ifStack;
        usize             nowPos { 0 };
        const std::regex  reIfs(R"((#if|#endif))");
        while (true) {
            auto p = search(src, nowPos + 1, reIfs);
            if (p > mainPos || p == std::string::npos) break;
            if (src.substr(p, 3) == "#if") {
                ifStack.push(p);
            } else {
                if (ifStack.empty()) break;
                usize ifp = ifStack.top();
                ifStack.pop();
                usize endp = p;
                if (pos > ifp && pos <= endp) {
                    pos = nextLinePos(src, endp + 1);
                }
            }
            nowPos = p;
        }
        pos = std::min({ pos, mainPos });
    }

    return NposToZero(pos);
}

inline EShLanguage ToGLSL(ShaderType type) {
    switch (type) {
    case ShaderType::VERTEX: return EShLangVertex;
    case ShaderType::FRAGMENT: return EShLangFragment;
    default: return EShLangVertex;
    }
}

inline std::string Preprocessor(const std::string& in_src, ShaderType type, const Combos& combos,
                                WPPreprocessorInfo& process_info) {
    std::string res;

    std::string src = wallpaper::WPShaderParser::PreShaderHeader(in_src, combos, type);

    // workaround #require directive
    {
        std::regex re_require("(^|\r?\n)#require (.+)(\r?\n)");
        src = std::regex_replace(src, re_require, "$1//#require $2$3");
    }

    glslang::TShader::ForbidIncluder includer;
    glslang::TShader                 shader(ToGLSL(type));
    const EShMessages emsg { (EShMessages)(EShMsgDefault | EShMsgSpvRules | EShMsgRelaxedErrors |
                                           EShMsgSuppressWarnings | EShMsgVulkanRules) };

    auto* data = src.c_str();
    shader.setStrings(&data, 1);
    shader.preprocess(&vulkan::DefaultTBuiltInResource,
                      110,
                      EProfile::ECoreProfile,
                      false,
                      false,
                      emsg,
                      &res,
                      includer);

    // (?:^|\s) lets us match "out vec2 foo;" at column 0 (no qualifier) as well as
    // "smooth out vec4 foo;" where the qualifier precedes the keyword.
    std::regex re_io(R"((?:^|\s)(in|out)\s[\s\w]+\s(\w+)\s*;)",
                     std::regex::ECMAScript | std::regex::multiline);
    for (auto it = std::sregex_iterator(res.begin(), res.end(), re_io);
         it != std::sregex_iterator();
         it++) {
        std::smatch mc = *it;
        if (mc[1] == "in") {
            process_info.input[mc[2]] = mc[0].str();
        } else {
            process_info.output[mc[2]] = mc[0].str();
        }
    }

    std::regex re_tex(R"(uniform\s+sampler2D\s+g_Texture(\d+))", std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(res.begin(), res.end(), re_tex);
         it != std::sregex_iterator();
         it++) {
        std::smatch mc  = *it;
        auto        str = mc[1].str();
        uint        slot;
        auto [ptr, ec] { std::from_chars(str.c_str(), str.c_str() + str.size(), slot) };
        if (ec != std::errc()) continue;
        process_info.active_tex_slots.insert(slot);
    }
    return res;
}

inline std::string Finalprocessor(const WPShaderUnit& unit, const WPPreprocessorInfo* pre,
                                  const WPPreprocessorInfo* next) {
    std::string insert_str {};
    auto&       cur = unit.preprocess_info;
    if (pre != nullptr) {
        for (auto& [k, v] : pre->output) {
            if (! exists(cur.input, k)) {
                auto n = std::regex_replace(v, std::regex(R"(\s*out\s)"), " in ");
                insert_str += n + '\n';
            }
        }
    }
    if (next != nullptr) {
        for (auto& [k, v] : next->input) {
            if (! exists(cur.output, k)) {
                auto n = std::regex_replace(v, std::regex(R"(\s*in\s)"), " out ");
                insert_str += n + '\n';
            }
        }
    }
    std::regex re_hold(SHADER_PLACEHOLD.data());

    // LOG_INFO("insert: %s", insert_str.c_str());
    // return std::regex_replace(
    //    std::regex_replace(cur.result, re_hold, insert_str), std::regex(R"(\s+\n)"), "\n");
    return std::regex_replace(unit.src, re_hold, insert_str);
}

// Fix HLSL->GLSL implicit type conversion issues.
// HLSL allows implicit float<->int conversions, GLSL/SPIR-V does not.
inline std::string FixImplicitConversions(const std::string& src) {
    std::string result = src;

    // Fix: "float VAR = int(EXPR)" -> "int VAR = int(EXPR)"
    // HLSL pattern where a float variable is assigned an int constructor result,
    // then used in int contexts (for loops)
    {
        std::regex re(R"((\bfloat\s+)(\w+)(\s*=\s*int\s*\())");
        result = std::regex_replace(result, re, "int $2$3");
    }

    // Fix: IDENTIFIER % int_literal -> int(IDENTIFIER) % int_literal
    // HLSL allows % on floats; GLSL requires integer operands for %.
    // When the result is assigned directly to a uint variable, additionally wrap in
    // uint() to avoid the secondary int→uint implicit conversion error.
    {
        // Case: uint VAR = (WORD OP int_lit) % N  e.g. "uint b = (a + 1) % 32;"
        // where WORD is uint: "uint + int" and "uint % int" are both GLSL errors.
        // Fix: uint VAR = uint((int(WORD) OP int_lit) % N)
        {
            std::regex re(R"(\buint\s+(\w+)\s*=\s*\((\w+)\s*([\+\-])\s*(\d+)\)\s*%\s*(\d+\b))");
            result = std::regex_replace(result, re, "uint $1 = uint((int($2) $3 $4) % $5)");
        }
        // Special case: uint VAR = EXPR % N;  →  uint VAR = uint(int(EXPR) % N);
        {
            std::regex re(R"(\buint\s+(\w+)\s*=\s*\b(\w+)\s*%\s*(\d+\b))");
            result = std::regex_replace(result, re, "uint $1 = uint(int($2) % $3)");
        }
        // General case: EXPR % N  →  int(EXPR) % N
        {
            std::regex re(R"(\b(\w+)\s*%\s*(\d+\b))");
            result = std::regex_replace(result, re, "int($1) % $2");
        }
    }

    // Fix: HLSL varyings declared as vecN but accessed with components beyond N.
    // In DirectX, texture-coordinate interpolator slots are always 4-wide regardless of
    // the declared float2/float3 type; HLSL shaders rely on this.  GLSL enforces the
    // declared width strictly, so "in vec2 v_TexCoord; ... v_TexCoord.zw" is an error.
    // Upgrade vec2 → vec4 when .z/.w (xyzw) or .b/.a (rgba) is accessed on it;
    // likewise vec3 → vec4 when .w/.a is accessed.
    // NOTE: must run before fixTrunc so that upgraded variables are not incorrectly
    // truncated (e.g. a vec2 upgraded to vec4 must not have its assignments cut to .xy).
    {
        auto upgradeIfOutOfRange = [&result](const char* small_type, const char* big_type,
                                             const char* oob_pattern,
                                             const char* bare_swizzle) {
            std::vector<std::string> to_upgrade;
            std::regex               re_decl(std::string(R"(\b)") + small_type + R"(\s+(\w+)\s*;)");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_decl);
                 it != std::sregex_iterator(); ++it) {
                std::string name = (*it)[1].str();
                if (std::regex_search(result, std::regex(R"(\b)" + name + oob_pattern)))
                    to_upgrade.push_back(std::move(name));
            }
            for (const auto& name : to_upgrade) {
                // Upgrade the declaration (vec2 → vec4, etc.)
                std::regex re(std::string(R"(\b)") + small_type + R"((\s+)" + name + R"(\s*;))");
                result = std::regex_replace(result, re, std::string(big_type) + "$1");
                // After upgrading, bare uses of this variable as a texture() coordinate
                // are now too wide (e.g. texture(sampler2D, vec4) is invalid).
                // Add a swizzle to bring it back to the original size.
                // Only matches bare NAME with no following dot (i.e. no existing swizzle).
                std::regex re_tex(R"(\btexture\s*\(\s*(\w+)\s*,\s*)" + name + R"(\s*\))");
                result = std::regex_replace(
                    result, re_tex, "texture($1, " + name + "." + bare_swizzle + ")");
            }
        };
        upgradeIfOutOfRange("vec2", "vec4", R"(\.[zwba])", "xy");
        upgradeIfOutOfRange("vec3", "vec4", R"(\.[wa])", "xyz");
    }

    // Fix: HLSL implicit vector truncation (vec4->vec2, vec4->vec3, vec3->vec2).
    // HLSL allows "vec2_var = vec4_expr" silently dropping the extra components;
    // GLSL requires an explicit swizzle.  We collect all declared variable names
    // per width and rewrite bare same-name assignments.
    // NOTE: runs after upgradeIfOutOfRange so that variables already upgraded to vec4
    // are not seen as vec2/vec3 targets and have their assignments incorrectly truncated.
    {
        auto collect = [&result](const char* type) {
            std::set<std::string> vars;
            std::regex            re(std::string(R"(\b)") + type + R"(\s+(\w+)\b)");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                 it != std::sregex_iterator();
                 ++it)
                vars.insert((*it)[1].str());
            return vars;
        };

        const auto vec2_vars = collect("vec2");
        const auto vec3_vars = collect("vec3");
        const auto vec4_vars = collect("vec4");

        auto fixTrunc = [&result](const std::set<std::string>& dst,
                                  const std::set<std::string>& src,
                                  const char*                   swizzle) {
            for (const auto& d : dst) {
                for (const auto& s : src) {
                    if (d == s) continue;
                    std::regex re("\\b(" + d + ")\\s*=\\s*(" + s + ")\\s*;");
                    result = std::regex_replace(result, re,
                                                "$1 = $2." + std::string(swizzle) + ";");
                }
            }
        };

        fixTrunc(vec2_vars, vec3_vars, "xy");
        fixTrunc(vec2_vars, vec4_vars, "xy");
        fixTrunc(vec3_vars, vec4_vars, "xyz");
    }

    // Fix: HLSL pow(scalar, vecN) broadcasts the scalar; GLSL requires matching genType.
    // When the pow() result is used inside a vecN() constructor, move the broadcast inside:
    //   vecN(pow(X, Y)) → pow(vecN(X), vecN(Y))
    // Wrapping an already-vecN arg in vecN() is a safe copy-constructor identity.
    // Only handles one level of nesting inside each pow argument (sufficient in practice).
    {
        std::regex re(
            R"(\b(vec[234])\s*\(\s*pow\s*\()"
            R"(([^(),]*(?:\([^)]*\)[^(),]*)*),\s*)"
            R"(([^()]*(?:\([^)]*\)[^()]*)*)\)\s*\))");
        result = std::regex_replace(result, re, "pow($1($2), $1($3))");
    }

    // Fix: "int VAR = step(EXPR)" → "float VAR = step(EXPR)"
    // step() returns genType (float); the variable is used in float arithmetic throughout
    // (bar *= step(...), bar * u_BarOpacity, etc.), so changing the type is correct.
    // HLSL allows int = float implicitly; GLSL requires matching types.
    {
        std::regex re(R"(\bint\s+(\w+)\s*=\s*(step\s*\([^;]*\))\s*;)");
        result = std::regex_replace(result, re, "float $1 = $2;");
    }

    return result;
}

inline std::string GenSha1(std::span<const WPShaderUnit> units) {
    std::string shas;
    for (auto& unit : units) {
        shas += utils::genSha1(unit.src);
    }
    return utils::genSha1(shas);
}
inline std::string GetCachePath(std::string_view scene_id, std::string_view filename) {
    return std::string("/cache/") + std::string(scene_id) + "/" SHADER_DIR "/" +
           std::string(filename) + "." SHADER_SUFFIX;
}

inline bool LoadShaderFromFile(std::vector<ShaderCode>& codes, fs::IBinaryStream& file) {
    codes.clear();
    i32 ver = ReadSPVVesion(file);

    usize count = file.ReadUint32();
    assert(count <= 16 && count >= 0);
    if (count > 16) return false;

    codes.resize(count);
    for (usize i = 0; i < count; i++) {
        auto& c = codes[i];

        u32 size = file.ReadUint32();
        assert(size % 4 == 0);
        if (size % 4 != 0) return false;

        c.resize(size / 4);
        file.Read((char*)c.data(), size);
    }
    return true;
}

inline void SaveShaderToFile(std::span<const ShaderCode> codes, fs::IBinaryStreamW& file) {
    char nop[256] { '\0' };

    WriteSPVVesion(file, 1);
    file.WriteUint32((u32)codes.size());
    for (const auto& c : codes) {
        u32 size = (u32)c.size() * 4;
        file.WriteUint32(size);
        file.Write((const char*)c.data(), size);
    }
    file.Write(nop, sizeof(nop));
}

} // namespace

std::string WPShaderParser::PreShaderSrc(fs::VFS& vfs, const std::string& src,
                                         WPShaderInfo*                       pWPShaderInfo,
                                         const std::vector<WPShaderTexInfo>& texinfos) {
    std::string            newsrc(src);
    std::string::size_type pos = 0;
    std::string            include;
    while (pos = src.find("#include", pos), pos != std::string::npos) {
        auto begin = pos;
        pos        = src.find_first_of('\n', pos);
        newsrc.replace(begin, pos - begin, pos - begin, ' ');
        include.append(src.substr(begin, pos - begin) + "\n");
    }
    include = LoadGlslInclude(vfs, include);

    ParseWPShader(include, pWPShaderInfo, texinfos);
    ParseWPShader(newsrc, pWPShaderInfo, texinfos);

    newsrc.insert(FindIncludeInsertPos(newsrc, 0), include);
    return newsrc;
}

std::string WPShaderParser::PreShaderHeader(const std::string& src, const Combos& combos,
                                            ShaderType type) {
    std::string pre(pre_shader_code);
    if (type == ShaderType::VERTEX) pre += pre_shader_code_vert;
    if (type == ShaderType::FRAGMENT) pre += pre_shader_code_frag;
    std::string header(pre);
    for (const auto& c : combos) {
        std::string cup(c.first);
        std::transform(c.first.begin(), c.first.end(), cup.begin(), ::toupper);
        if (c.second.empty()) {
            LOG_ERROR("combo '%s' can't be empty", cup.c_str());
            continue;
        }
        header.append("#define " + cup + " " + c.second + "\n");
    }
    return header + src;
}

void WPShaderParser::InitGlslang() { glslang::InitializeProcess(); }
void WPShaderParser::FinalGlslang() { glslang::FinalizeProcess(); }

bool WPShaderParser::CompileToSpv(std::string_view scene_id, std::span<WPShaderUnit> units,
                                  std::vector<ShaderCode>& codes, fs::VFS& vfs,
                                  WPShaderInfo* shader_info, std::span<const WPShaderTexInfo> texs) {
    (void)texs;

    std::for_each(units.begin(), units.end(), [shader_info](auto& unit) {
        unit.src = Preprocessor(unit.src, unit.stage, shader_info->combos, unit.preprocess_info);
    });

    auto compile = [](std::span<WPShaderUnit> units, std::vector<ShaderCode>& codes) {
        std::vector<vulkan::ShaderCompUnit> vunits(units.size());
        for (usize i = 0; i < units.size(); i++) {
            auto&               unit     = units[i];
            auto&               vunit    = vunits[i];
            WPPreprocessorInfo* pre_info = i >= 1 ? &units[i - 1].preprocess_info : nullptr;
            WPPreprocessorInfo* post_info =
                i + 1 < units.size() ? &units[i + 1].preprocess_info : nullptr;

            unit.src = Finalprocessor(unit, pre_info, post_info);
            unit.src = FixImplicitConversions(unit.src);

            vunit.src   = unit.src;
            vunit.stage = ToGLSL(unit.stage);
        }

        vulkan::ShaderCompOpt opt;
        opt.client_ver             = glslang::EShTargetVulkan_1_1;
        opt.auto_map_bindings      = true;
        opt.auto_map_locations     = true;
        opt.relaxed_errors_glsl    = true;
        opt.relaxed_rules_vulkan   = true;
        opt.suppress_warnings_glsl = true;

        std::vector<vulkan::Uni_ShaderSpv> spvs(units.size());

        if (! vulkan::CompileAndLinkShaderUnits(vunits, opt, spvs)) {
            return false;
        }

        codes.clear();
        for (auto& spv : spvs) {
            codes.emplace_back(std::move(spv->spirv));
        }
        return true;
    };

    bool has_cache_dir = vfs.IsMounted("cache");

    if (has_cache_dir) {
        std::string sha1            = GenSha1(units);
        std::string cache_file_path = GetCachePath(scene_id, sha1);

        if (vfs.Contains(cache_file_path)) {
            auto cache_file = vfs.Open(cache_file_path);
            if (! cache_file || ! ::LoadShaderFromFile(codes, *cache_file)) {
                LOG_ERROR("load shader from \'%s\' failed", cache_file_path.c_str());
                return false;
            }
        } else {
            if (! compile(units, codes)) return false;
            if (auto cache_file = vfs.OpenW(cache_file_path); cache_file) {
                ::SaveShaderToFile(codes, *cache_file);
            }
        }
        return true;

    } else {
        return compile(units, codes);
    }
}
