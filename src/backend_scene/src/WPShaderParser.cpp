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
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <vector>

static constexpr std::string_view SHADER_PLACEHOLD { "__SHADER_PLACEHOLD__" };

// Invalidate shaders generated before the stage and conversion fixes.
#define SHADER_DIR    "spvs02"
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

#define float1 float
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define lerp mix
#define log10(x) (log2(x) * 0.30102999566398120)

// HLSL pow(vecN, scalar) broadcasts the exponent; GLSL wants matching genTypes.
vec2 pow(vec2 x, float y) { return pow(x, vec2(y)); }
vec3 pow(vec3 x, float y) { return pow(x, vec3(y)); }
vec4 pow(vec4 x, float y) { return pow(x, vec4(y)); }
// HLSL promotes an int exponent; GLSL has no int->float conversion.
float pow(float x, int y) { return pow(x, float(y)); }
vec2 pow(vec2 x, int y) { return pow(x, vec2(y)); }
vec3 pow(vec3 x, int y) { return pow(x, vec3(y)); }
vec4 pow(vec4 x, int y) { return pow(x, vec4(y)); }
// HLSL accepts the scalar in either position of max/min; GLSL only as the
// second argument. (An argument-swapping macro used to do this, but it broke
// the already-valid max(vecN, scalar) order.)
vec2 max(float x, vec2 y) { return max(y, x); }
vec3 max(float x, vec3 y) { return max(y, x); }
vec4 max(float x, vec4 y) { return max(y, x); }
vec2 min(float x, vec2 y) { return min(y, x); }
vec3 min(float x, vec3 y) { return min(y, x); }
vec4 min(float x, vec4 y) { return min(y, x); }
// HLSL also promotes int literals: max(0, x)
float max(int x, float y) { return max(float(x), y); }
float max(float x, int y) { return max(x, float(y)); }
float min(int x, float y) { return min(float(x), y); }
float min(float x, int y) { return min(x, float(y)); }

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
        auto lineEnd = input.find_first_of('\n', linePos);
        if (lineEnd == std::string::npos) lineEnd = input.size();
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
                        // Effect inputs can have empty slots between bound textures.
                        const bool enabled =
                            index >= 0 && index < texcount && texinfos[(usize)index].enabled;
                        if (! wput.combo.empty()) combos[wput.combo] = enabled ? "1" : "0";
                        if (enabled) {
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
        usize pos = std::string::npos;
        for (auto it = std::sregex_iterator(p.begin(), p.end(), re); it != std::sregex_iterator();
             ++it)
            pos = (usize)it->position();
        return pos;
    };
    auto nextLinePos = [](const std::string& p, usize pos) {
        auto end = p.find_first_of('\n', pos);
        return end == std::string::npos ? p.size() : end + 1;
    };

    usize mainPos  = src.find("void main(");
    bool  two_main = src.find("void main(", mainPos + 2) != std::string::npos;
    if (two_main) return 0;

    usize pos;
    {
        // Keep offsets while ignoring comments when locating a declaration's end.
        std::string      code = src;
        const std::regex comments(R"(//[^\n]*|/\*[\s\S]*?\*/)");
        for (auto it = std::sregex_iterator(src.begin(), src.end(), comments);
             it != std::sregex_iterator();
             ++it) {
            for (usize i = (usize)it->position(); i < (usize)(it->position() + it->length()); ++i)
                if (code[i] != '\n') code[i] = ' ';
        }
        const std::regex reAfters(R"((^|\n)[ \t]*(attribute|varying|uniform|struct)[ \t])");
        const usize      declarationPos = searchLast(code, reAfters);
        usize            afterPos       = declarationPos;
        if (declarationPos != std::string::npos) {
            afterPos         = nextLinePos(code, declarationPos + 1);
            const auto start = code.find_first_not_of(" \t\r\n", declarationPos);
            if (start != std::string::npos && code.compare(start, 6, "struct") == 0) {
                const auto open = code.find_first_of("{;", start);
                if (open != std::string::npos && code[open] == '{') {
                    int depth = 1;
                    for (usize i = open + 1; depth > 0 && i < code.size(); ++i) {
                        if (code[i] == '{') ++depth;
                        if (code[i] == '}' && --depth == 0) afterPos = nextLinePos(src, i);
                    }
                }
            }
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
    // Workshop shaders are Windows-authored (CRLF). glslang's preprocessor drops
    // whatever follows a CRLF-terminated directive at the end of the file
    // ("#endif\r\n}" loses the brace -> "unexpected end of file"), so normalise
    // line endings first and make sure the source ends with a newline.
    {
        std::string lf;
        lf.reserve(src.size());
        for (usize i = 0; i < src.size(); i++) {
            if (src[i] == '\r') {
                if (i + 1 < src.size() && src[i + 1] == '\n') continue;
                lf += '\n';
            } else {
                lf += src[i];
            }
        }
        src.swap(lf);
        if (! src.empty() && src.back() != '\n') src += '\n';
    }
    // Workshop shaders (and WE's own headers) sometimes carry a stray #endif or
    // #else at nesting depth zero. WE's preprocessor ignores it; glslang's
    // silently drops everything after it (losing main()'s closing brace ->
    // "unexpected end of file"). Blank such lines.
    {
        std::regex  re_open(R"(^\s*#\s*if(def|ndef)?\b)");
        std::regex  re_close(R"(^\s*#\s*endif\b)");
        std::regex  re_else(R"(^\s*#\s*(else|elif)\b)");
        std::string out;
        out.reserve(src.size());
        int   depth = 0;
        usize pos   = 0;
        while (pos < src.size()) {
            usize end = src.find('\n', pos);
            if (end == std::string::npos) end = src.size();
            std::string line = src.substr(pos, end - pos);
            if (std::regex_search(line, re_open)) {
                depth++;
            } else if (std::regex_search(line, re_close)) {
                if (depth == 0)
                    line = "// stray #endif removed";
                else
                    depth--;
            } else if (std::regex_search(line, re_else)) {
                if (depth == 0) line = "// stray #else removed";
            }
            out += line;
            out += '\n';
            pos = end + 1;
        }
        src.swap(out);
    }
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
    if (! shader.preprocess(&vulkan::DefaultTBuiltInResource,
                            110,
                            EProfile::ECoreProfile,
                            false,
                            false,
                            emsg,
                            &res,
                            includer)) {
        LOG_ERROR("glslang preprocess failed: %s", shader.getInfoLog());
    }

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

// --- HLSL call/argument conversions ---------------------------------------
//
// HLSL converts between widths freely: a wider argument is truncated, a scalar
// is broadcast, an int literal is promoted. GLSL does none of that, so a scene
// written for DirectX reaches glslang with calls it can never resolve.
//
// Every fix below fires only when the width of an operand is proven from the
// unit's own declarations; a construct that can't be proven is left untouched,
// because rewriting it on a guess is how a shader that compiles today stops
// compiling.

inline int TypeWidth(const std::string& type) {
    if (type == "float" || type == "int" || type == "uint" || type == "bool" || type == "double")
        return 1;
    if (type == "vec2") return 2;
    if (type == "vec3") return 3;
    if (type == "vec4") return 4;
    return 0;
}

// Swizzle that keeps the first width components.
inline std::string NarrowSwizzle(int width) {
    switch (width) {
    case 1: return ".x";
    case 2: return ".xy";
    case 3: return ".xyz";
    default: return ".xyzw";
    }
}

// std::regex (libstdc++) has no lookbehind assertion, so a token boundary is
// checked here: true when pos starts a token rather than continuing one.
inline bool AtTokenStart(const std::string& src, usize pos) {
    if (pos == 0) return true;
    const unsigned char c = (unsigned char)src[pos - 1];
    return ! (std::isalnum(c) || c == '_' || c == '.');
}

inline std::string TrimSpace(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}

// Declared width of each scalar/vector variable in the unit. A name declared
// with two different widths is stored as 0 and never used.
inline std::map<std::string, int> CollectDeclWidths(const std::string& src) {
    std::map<std::string, int> widths;
    const std::regex           re(R"((?:^|[^\w.])(float|int|uint|bool|double|vec2|vec3|vec4))"
                                  R"(\s+([A-Za-z_]\w*)(?=\s*[;,=)]))",
                                  std::regex::ECMAScript | std::regex::multiline);
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re); it != std::sregex_iterator();
         ++it) {
        const int width { TypeWidth((*it)[1].str()) };
        auto [pos, inserted] = widths.emplace((*it)[2].str(), width);
        if (! inserted && pos->second != width) pos->second = 0;
    }
    return widths;
}

// Index of the bracket closing src[open], npos when unbalanced.
inline usize FindMatch(const std::string& src, usize open) {
    if (open >= src.size()) return std::string::npos;
    const char o = src[open];
    const char c = o == '(' ? ')' : o == '[' ? ']' : o == '{' ? '}' : '\0';
    if (c == '\0') return std::string::npos;
    int depth = 0;
    for (usize i = open; i < src.size(); i++) {
        if (src[i] == o) {
            depth++;
        } else if (src[i] == c) {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

// Byte ranges of the comma separated parts of src at bracket depth zero.
inline std::vector<std::pair<usize, usize>> SplitTopLevel(const std::string& src) {
    std::vector<std::pair<usize, usize>> parts;
    usize                                begin { 0 };
    int                                  depth { 0 };
    for (usize i = 0; i < src.size(); i++) {
        const char c = src[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        if (c == ')' || c == ']' || c == '}') depth--;
        if (c == ',' && depth == 0) {
            parts.emplace_back(begin, i);
            begin = i + 1;
        }
    }
    parts.emplace_back(begin, src.size());
    return parts;
}

struct TextEdit {
    usize       begin;
    usize       end;
    std::string text;
};

// Replace [begin, end) of src for each edit; overlapping edits are dropped.
inline std::string ApplyEdits(const std::string& src, std::vector<TextEdit> edits) {
    std::sort(edits.begin(), edits.end(), [](const TextEdit& a, const TextEdit& b) {
        return a.begin < b.begin;
    });
    std::string out;
    usize       last { 0 };
    for (const auto& edit : edits) {
        if (edit.begin < last) continue;
        out.append(src, last, edit.begin - last);
        out += edit.text;
        last = edit.end;
    }
    out.append(src, last, std::string::npos);
    return out;
}

// Width declared by an in/out declaration line (0 when it can't be parsed).
inline int DeclWidth(const std::string& decl) {
    const std::regex re(R"(\b(float|int|uint|bool|double|vec2|vec3|vec4)\s+[A-Za-z_]\w*)");
    std::smatch      match;
    if (! std::regex_search(decl, match, re)) return 0;
    return TypeWidth(match[1].str());
}

struct ChainAtom {
    int   width { 0 };
    bool  swizzlable { false };
    usize begin { 0 };
    usize end { 0 };
};

struct ChainScan {
    bool                   ok { false };
    int                    width { 0 };
    std::vector<ChainAtom> atoms;
};

// Prove the width of a pure arithmetic chain: numbers, declared variables,
// swizzles, vector constructors and texture() samples joined by + - * /.
// Parentheses are transparent. A comparison, ternary, subscript, hex literal or
// a call with an unknown result leaves the chain unproven.
inline ChainScan ScanChain(const std::string& src, const std::map<std::string, int>& widths) {
    ChainScan scan;
    usize     pos { 0 };
    int       depth { 0 };
    bool      expect_operand { true };
    auto      skip_space = [&src](usize p) {
        while (p < src.size() && std::isspace((unsigned char)src[p])) p++;
        return p;
    };
    while (true) {
        pos = skip_space(pos);
        if (pos >= src.size()) break;
        const char c = src[pos];
        if (expect_operand) {
            if (c == '+' || c == '-') {
                pos++;
                continue;
            }
            if (c == '(') {
                depth++;
                pos++;
                continue;
            }
        } else {
            if (c == ')') {
                if (depth == 0) return {};
                depth--;
                pos++;
                continue;
            }
            if (c == '+' || c == '-' || c == '*' || c == '/') {
                pos++;
                expect_operand = true;
                continue;
            }
            return {}; // two operands in a row
        }
        const usize begin      = pos;
        int         width      = 0;
        bool        swizzlable = false;
        if (std::isdigit((unsigned char)c) ||
            (c == '.' && pos + 1 < src.size() && std::isdigit((unsigned char)src[pos + 1]))) {
            usize p   = pos;
            bool  dot = false;
            while (p < src.size() &&
                   (std::isdigit((unsigned char)src[p]) || (src[p] == '.' && ! dot))) {
                if (src[p] == '.') dot = true;
                p++;
            }
            if (p < src.size() && (src[p] == 'e' || src[p] == 'E')) {
                usize q = p + 1;
                if (q < src.size() && (src[q] == '+' || src[q] == '-')) q++;
                if (q < src.size() && std::isdigit((unsigned char)src[q])) {
                    while (q < src.size() && std::isdigit((unsigned char)src[q])) q++;
                    p = q;
                }
            }
            if (p < src.size() && std::strchr("fFuU", src[p]) != nullptr) p++;
            width = 1;
            pos   = p;
        } else if (std::isalpha((unsigned char)c) || c == '_') {
            usize p = pos;
            while (p < src.size() && (std::isalnum((unsigned char)src[p]) || src[p] == '_')) p++;
            const std::string name = src.substr(pos, p - pos);
            if (p < src.size() && src[p] == '(') {
                const usize close = FindMatch(src, p);
                if (close == std::string::npos) return {};
                width = TypeWidth(name);
                if (name == "texture" || name == "textureLod") width = 4;
                if (width == 0) return {};
                pos = close + 1;
            } else if (p < src.size() && src[p] == '.') {
                usize       q { p + 1 };
                std::string swizzle;
                while (q < src.size() && swizzle.size() < 4 &&
                       std::strchr("xyzwrgba", src[q]) != nullptr)
                    swizzle += src[q++];
                if (swizzle.empty()) return {};
                width = (int)swizzle.size();
                pos   = q;
            } else {
                const auto it = widths.find(name);
                if (it == widths.end() || it->second <= 0) return {};
                width      = it->second;
                swizzlable = true;
                pos        = p;
            }
        } else {
            return {};
        }
        scan.atoms.push_back({ width, swizzlable, begin, pos });
        scan.width     = std::max(scan.width, width);
        expect_operand = false;
    }
    if (expect_operand || depth != 0 || scan.atoms.empty()) return {};
    scan.ok = true;
    return scan;
}

// Parameter types of the unit's own single-line function definitions, keyed by
// name. An overloaded name is dropped, as is any signature with a type this
// pass does not model.
inline std::map<std::string, std::vector<std::string>>
CollectFunctionParams(const std::string& src, std::vector<std::pair<usize, usize>>& spans) {
    std::map<std::string, std::vector<std::string>> params;
    std::set<std::string>                           overloaded;
    const std::regex                                sig(
        R"((?:^|\n)[ \t]*(?:const[ \t]+)?(float|vec2|vec3|vec4|int|uint|bool|void)[ \t]+)"
        R"((\w+)[ \t]*\(([^(){};]*)\)\s*\{)",
        std::regex::ECMAScript | std::regex::multiline);
    for (auto it = std::sregex_iterator(src.begin(), src.end(), sig); it != std::sregex_iterator();
         ++it) {
        const usize begin = (usize)(*it).position();
        spans.emplace_back(begin, begin + (usize)(*it).length());
        const std::string name = (*it)[2].str();
        if (name == "main") continue;
        const std::string        source = (*it)[3].str();
        std::vector<std::string> types;
        bool                     usable { true };
        for (const auto& [pbegin, pend] : SplitTopLevel(source)) {
            const std::string param = TrimSpace(source.substr(pbegin, pend - pbegin));
            if (param.empty() || param == "void") continue;
            if (param.find('[') != std::string::npos) {
                usable = false;
                break;
            }
            std::string type;
            usize       pos { 0 };
            while (pos < param.size()) {
                const auto word_begin = param.find_first_not_of(" \t\r\n", pos);
                if (word_begin == std::string::npos) break;
                const auto        word_end = param.find_first_of(" \t\r\n", word_begin);
                const std::string word     = param.substr(word_begin, word_end - word_begin);
                pos                        = word_end;
                if (word == "in" || word == "out" || word == "inout" || word == "const" ||
                    word == "highp" || word == "mediump" || word == "lowp" || word == "flat" ||
                    word == "smooth" || word == "noperspective" || word == "centroid")
                    continue;
                type = TypeWidth(word) > 0 ? word : std::string {};
                break;
            }
            if (type.empty()) {
                usable = false; // struct, matrix or unknown parameter type
                break;
            }
            types.push_back(type);
        }
        if (! usable) {
            overloaded.insert(name);
            continue;
        }
        const auto [pos, inserted] = params.emplace(name, types);
        if (! inserted && pos->second != types) overloaded.insert(name);
    }
    for (const auto& name : overloaded) params.erase(name);
    return params;
}

inline std::string FixImplicitCalls(const std::string&           src,
                                    const std::set<std::string>& link_mismatch) {
    std::string result { src };
    auto        decl_widths = [&result] {
        return CollectDeclWidths(result);
    };

    // texture() takes a vec2 coordinate; HLSL truncates a wider one.
    {
        const auto            widths = decl_widths();
        std::vector<TextEdit> edits;
        const std::regex      re(R"((?:texture|textureLod)\s*\()");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
             it != std::sregex_iterator();
             ++it) {
            if (! AtTokenStart(result, (usize)(*it).position())) continue;
            const usize open  = (usize)(*it).position() + (usize)(*it).length() - 1;
            const usize close = FindMatch(result, open);
            if (close == std::string::npos) continue;
            const std::string args  = result.substr(open + 1, close - open - 1);
            const auto        parts = SplitTopLevel(args);
            if (parts.size() != 2) continue;
            const auto& [cbegin, cend] = parts[1];
            const std::string coord    = args.substr(cbegin, cend - cbegin);
            const ChainScan   scan     = ScanChain(coord, widths);
            if (! scan.ok || scan.width <= 2) continue;
            bool narrowable { true };
            for (const auto& atom : scan.atoms)
                if (atom.width > 2 && ! atom.swizzlable) narrowable = false;
            if (! narrowable) continue;
            std::vector<TextEdit> coord_edits;
            for (const auto& atom : scan.atoms) {
                if (atom.width > 2) coord_edits.push_back({ atom.end, atom.end, ".xy" });
            }
            edits.push_back({ open + 1 + cbegin, open + 1 + cend, ApplyEdits(coord, coord_edits) });
        }
        if (! edits.empty()) result = ApplyEdits(result, edits);
    }

    // A texture sample assigned to a scalar keeps its first component in HLSL.
    {
        std::vector<TextEdit> edits;
        const std::regex      re(R"((?:float|int|uint)\s+(\w+)\s*=\s*)"
                                 R"((texture|textureLod)\s*\()");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
             it != std::sregex_iterator();
             ++it) {
            if (! AtTokenStart(result, (usize)(*it).position())) continue;
            const usize call_begin = (usize)(*it).position(2);
            const usize open       = (usize)(*it).position() + (usize)(*it).length() - 1;
            const usize close      = FindMatch(result, open);
            if (close == std::string::npos) continue;
            usize tail = close + 1;
            while (tail < result.size() && std::isspace((unsigned char)result[tail])) tail++;
            if (tail >= result.size() || (result[tail] != ';' && result[tail] != ',')) continue;
            edits.push_back({ call_begin, call_begin, "(" });
            edits.push_back({ close + 1, close + 1, ").x" });
        }
        if (! edits.empty()) result = ApplyEdits(result, edits);
    }

    // A declaration converts its initialiser to the declared width: a scalar
    // initialiser is broadcast, a wider one is truncated.
    {
        const auto            widths = decl_widths();
        std::vector<TextEdit> edits;
        const std::regex      re(R"((float|vec2|vec3|vec4)\s+([A-Za-z_]\w*)\s*=\s*)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
             it != std::sregex_iterator();
             ++it) {
            if (! AtTokenStart(result, (usize)(*it).position())) continue;
            const int   target = TypeWidth((*it)[1].str());
            const usize begin  = (usize)(*it).position() + (usize)(*it).length();
            usize       end    = begin;
            int         depth { 0 };
            for (; end < result.size(); ++end) {
                const char c = result[end];
                if (depth == 0 && (c == ',' || c == ';')) break;
                if (c == '(' || c == '[') depth++;
                if (c == ')' || c == ']') depth--;
                if (depth < 0 || c == '{' || c == '}') break;
            }
            if (end >= result.size() || (result[end] != ',' && result[end] != ';') || depth != 0)
                continue;
            const std::string init = result.substr(begin, end - begin);
            const ChainScan   scan = ScanChain(init, widths);
            if (! scan.ok || scan.width == target) continue;
            if (scan.width < target) {
                if (scan.width != 1) continue;
                edits.push_back({ begin, begin, (*it)[1].str() + "(" });
                edits.push_back({ end, end, ")" });
                continue;
            }
            std::string narrowed;
            usize       last { 0 };
            bool        skip { false };
            for (const auto& atom : scan.atoms) {
                if (atom.width <= target) continue;
                if (! atom.swizzlable) {
                    skip = true;
                    break;
                }
                narrowed.append(init, last, atom.begin - last);
                narrowed.append(init, atom.begin, atom.end - atom.begin);
                narrowed += NarrowSwizzle(target);
                last = atom.end;
            }
            if (skip) continue;
            narrowed.append(init, last, std::string::npos);
            edits.push_back({ begin, end, narrowed });
        }
        if (! edits.empty()) result = ApplyEdits(result, edits);
    }

    // HLSL truncates a wider argument to a function's vector parameter.
    {
        std::vector<std::pair<usize, usize>> spans;
        const auto                           params = CollectFunctionParams(result, spans);
        if (! params.empty()) {
            const auto            widths = decl_widths();
            std::vector<TextEdit> edits;
            const std::regex      call(R"((\w+)\s*\()");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), call);
                 it != std::sregex_iterator();
                 ++it) {
                const auto found = params.find((*it)[1].str());
                if (found == params.end()) continue;
                const usize begin { (usize)(*it).position() };
                if (! AtTokenStart(result, begin)) continue;
                bool in_signature { false };
                for (const auto& span : spans) {
                    if (begin >= span.first && begin < span.second) in_signature = true;
                }
                if (in_signature) continue;
                const usize open  = begin + (usize)(*it).length() - 1;
                const usize close = FindMatch(result, open);
                if (close == std::string::npos) continue;
                const std::string args  = result.substr(open + 1, close - open - 1);
                const auto        parts = SplitTopLevel(args);
                if (parts.size() != found->second.size()) continue;
                std::vector<TextEdit> arg_edits;
                for (usize i = 0; i < parts.size(); i++) {
                    const auto& param = found->second[i];
                    const int   width = TypeWidth(param);
                    if (width < 2) continue;
                    const std::string arg =
                        TrimSpace(args.substr(parts[i].first, parts[i].second - parts[i].first));
                    const ChainScan scan = ScanChain(arg, widths);
                    if (! scan.ok || scan.width <= width) continue;
                    arg_edits.push_back(
                        { parts[i].first, parts[i].second, param + "(" + arg + ")" });
                }
                if (arg_edits.empty()) continue;
                edits.push_back({ open + 1, close, ApplyEdits(args, arg_edits) });
            }
            if (! edits.empty()) result = ApplyEdits(result, edits);
        }
    }

    // mix() wants both of its first arguments at one width; HLSL converts one.
    {
        const auto            widths = decl_widths();
        std::vector<TextEdit> edits;
        const std::regex      re(R"(mix\s*\()");
        const std::regex      ident(R"(^[A-Za-z_]\w*$)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
             it != std::sregex_iterator();
             ++it) {
            if (! AtTokenStart(result, (usize)(*it).position())) continue;
            const usize open  = (usize)(*it).position() + (usize)(*it).length() - 1;
            const usize close = FindMatch(result, open);
            if (close == std::string::npos) continue;
            const std::string args  = result.substr(open + 1, close - open - 1);
            const auto        parts = SplitTopLevel(args);
            if (parts.size() != 3) continue;
            const std::string first =
                TrimSpace(args.substr(parts[0].first, parts[0].second - parts[0].first));
            const std::string second =
                TrimSpace(args.substr(parts[1].first, parts[1].second - parts[1].first));
            if (! std::regex_match(first, ident) || ! std::regex_match(second, ident)) continue;
            const auto fit = widths.find(first);
            const auto sit = widths.find(second);
            if (fit == widths.end() || sit == widths.end()) continue;
            if (fit->second <= 0 || sit->second <= 0 || fit->second == sit->second) continue;
            std::vector<TextEdit> arg_edits;
            if (fit->second == 1) {
                arg_edits.push_back({ parts[0].first,
                                      parts[0].second,
                                      "vec" + std::to_string(sit->second) + "(" + first + ")" });
            } else if (sit->second == 1) {
                arg_edits.push_back({ parts[1].first,
                                      parts[1].second,
                                      "vec" + std::to_string(fit->second) + "(" + second + ")" });
            } else if (fit->second > sit->second) {
                arg_edits.push_back(
                    { parts[0].second, parts[0].second, NarrowSwizzle(sit->second) });
            } else {
                arg_edits.push_back(
                    { parts[1].second, parts[1].second, NarrowSwizzle(fit->second) });
            }
            edits.push_back({ open + 1, close, ApplyEdits(args, arg_edits) });
        }
        if (! edits.empty()) result = ApplyEdits(result, edits);
    }

    // A comparison is not a float in GLSL; HLSL converts it to 0.0/1.0.
    {
        const auto       widths = decl_widths();
        const std::regex number(R"(^(?:\d+\.?\d*|\.\d+)$)");
        auto             is_op = [](char c) {
            return c != '\0' && std::strchr("*/+-", c) != nullptr;
        };
        auto scalar = [&widths, &number](const std::string& token) {
            if (std::regex_match(token, number)) return true;
            const auto it = widths.find(token);
            return it != widths.end() && it->second == 1;
        };
        std::vector<TextEdit> edits;
        const std::regex      re(R"(\(([A-Za-z_]\w*|\d+\.?\d*|\.\d+)\s*(<=|>=|==|!=|<|>)\s*)"
                                 R"(([A-Za-z_]\w*|\d+\.?\d*|\.\d+)\))");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
             it != std::sregex_iterator();
             ++it) {
            if (! scalar((*it)[1].str()) || ! scalar((*it)[3].str())) continue;
            const usize begin = (usize)(*it).position();
            const usize end   = begin + (usize)(*it).length();
            usize       after = end;
            while (after < result.size() && std::isspace((unsigned char)result[after])) after++;
            usize before = begin;
            while (before > 0 && std::isspace((unsigned char)result[before - 1])) before--;
            const char afterc  = after < result.size() ? result[after] : '\0';
            const char beforec = before > 0 ? result[before - 1] : '\0';
            if (! is_op(afterc) && ! is_op(beforec)) continue;
            const std::string inner = (*it)[0].str().substr(1, (*it).length() - 2);
            edits.push_back({ begin, end, "float(" + inner + ")" });
        }
        if (! edits.empty()) result = ApplyEdits(result, edits);
    }

    // int i = <float>;  HLSL narrows implicitly, GLSL needs the conversion.
    {
        std::set<std::string> floats;
        const std::regex      fre(R"((?:^|[^\w.])float\s+([A-Za-z_]\w*)(?=\s*[;,=)]))",
                                  std::regex::ECMAScript | std::regex::multiline);
        for (auto it = std::sregex_iterator(result.begin(), result.end(), fre);
             it != std::sregex_iterator();
             ++it)
            floats.insert((*it)[1].str());
        std::vector<TextEdit> edits;
        const std::regex      re(R"(int\s+(\w+)\s*=\s*([A-Za-z_]\w*)\s*;)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
             it != std::sregex_iterator();
             ++it) {
            if (! AtTokenStart(result, (usize)(*it).position())) continue;
            if (floats.count((*it)[2].str()) == 0) continue;
            edits.push_back({ (usize)(*it).position(),
                              (usize)(*it).position() + (usize)(*it).length(),
                              "int " + (*it)[1].str() + " = int(" + (*it)[2].str() + ");" });
        }
        if (! edits.empty()) result = ApplyEdits(result, edits);
    }

    // An assignment converts its right side to the target width too.
    {
        const auto            widths = decl_widths();
        std::vector<TextEdit> edits;
        const std::regex      re(R"(([A-Za-z_]\w*)\s*(=(?!=)|\+=|-=|\*=|/=))");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
             it != std::sregex_iterator();
             ++it) {
            const auto found = widths.find((*it)[1].str());
            if (found == widths.end() || found->second < 2) continue;
            if (! AtTokenStart(result, (usize)(*it).position())) continue;
            const usize begin = (usize)(*it).position();
            // a declaration is handled above, not here
            usize before = begin;
            while (before > 0 && std::isspace((unsigned char)result[before - 1])) before--;
            usize word = before;
            while (word > 0 &&
                   (std::isalnum((unsigned char)result[word - 1]) || result[word - 1] == '_'))
                word--;
            if (word < before && TypeWidth(result.substr(word, before - word)) > 0) continue;
            const usize rhs_begin = (usize)(*it).position() + (usize)(*it).length();
            usize       end       = rhs_begin;
            int         depth { 0 };
            for (; end < result.size(); ++end) {
                const char c = result[end];
                if (depth == 0 && c == ';') break;
                if (c == '(' || c == '[') depth++;
                if (c == ')' || c == ']') depth--;
                if (depth < 0 || c == '{' || c == '}') break;
            }
            if (end >= result.size() || result[end] != ';' || depth != 0) continue;
            const std::string rhs  = result.substr(rhs_begin, end - rhs_begin);
            const ChainScan   scan = ScanChain(rhs, widths);
            if (! scan.ok || scan.width <= found->second) continue;
            std::vector<TextEdit> rhs_edits;
            bool                  skip { false };
            for (const auto& atom : scan.atoms) {
                if (atom.width <= found->second) continue;
                if (! atom.swizzlable) {
                    skip = true;
                    break;
                }
                rhs_edits.push_back({ atom.end, atom.end, NarrowSwizzle(found->second) });
            }
            if (skip) continue;
            edits.push_back({ rhs_begin, end, ApplyEdits(rhs, rhs_edits) });
        }
        if (! edits.empty()) result = ApplyEdits(result, edits);
    }

    // A bare number is not a bool; HLSL treats a non zero one as true.
    {
        std::vector<TextEdit> edits;
        const std::regex      re(R"((\d+(?:\.\d*)?)[ \t]*\?)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
             it != std::sregex_iterator();
             ++it) {
            usize k = (usize)(*it).position(1);
            while (k > 0 && std::isspace((unsigned char)result[k - 1])) k--;
            if (k > 0) {
                const char previous = result[k - 1];
                if (previous == '=') {
                    usize k2 = k - 1;
                    while (k2 > 0 && std::isspace((unsigned char)result[k2 - 1])) k2--;
                    if (k2 > 0 && std::strchr("<>!=", result[k2 - 1]) != nullptr) continue;
                } else if (std::strchr(";(,?:", previous) == nullptr) {
                    continue;
                }
            }
            const std::string literal = (*it)[1].str();
            edits.push_back({ (usize)(*it).position(1),
                              (usize)(*it).position(1) + (usize)(*it).length(1),
                              std::stod(literal) == 0.0 ? "false" : "true" });
        }
        if (! edits.empty()) result = ApplyEdits(result, edits);
    }

    // A shader input is a read only varying in GLSL but a writable copy in HLSL;
    // give main() a local copy when the varying is used only there.
    {
        const std::regex mainre(R"(void\s+main\s*\(\s*(?:void\s*)?\)\s*\{)");
        usize            main_count { 0 };
        usize            main_open { 0 };
        for (auto it = std::sregex_iterator(result.begin(), result.end(), mainre);
             it != std::sregex_iterator();
             ++it) {
            if (! AtTokenStart(result, (usize)(*it).position())) continue;
            main_count++;
            main_open = (usize)(*it).position() + (usize)(*it).length() - 1;
        }
        const usize main_close = main_count == 1 ? FindMatch(result, main_open) : std::string::npos;
        if (main_close != std::string::npos) {
            const std::regex      decl(R"((^|\n)[ \t]*(?:(?:smooth|flat|noperspective|centroid))"
                                       R"([ \t]+)*in[ \t]+(float|vec2|vec3|vec4|int|uint|bool))"
                                       R"([ \t]+([A-Za-z_]\w*)[ \t]*;)",
                                       std::regex::ECMAScript | std::regex::multiline);
            std::vector<TextEdit> edits;
            std::string           locals;
            for (auto it = std::sregex_iterator(result.begin(), result.end(), decl);
                 it != std::sregex_iterator();
                 ++it) {
                const std::string type = (*it)[2].str();
                const std::string name = (*it)[3].str();
                if (result.find(name + "_hlsl_in") != std::string::npos) continue;
                // every use but the declaration must be inside main
                bool             inside_only { true };
                usize            uses { 0 };
                const std::regex use(name + R"((?![\w]))");
                for (auto uit = std::sregex_iterator(result.begin(), result.end(), use);
                     uit != std::sregex_iterator();
                     ++uit) {
                    const usize pos = (usize)uit->position();
                    if (! AtTokenStart(result, pos)) continue;
                    if (pos >= (usize)(*it).position(3) &&
                        pos < (usize)(*it).position(3) + name.size())
                        continue;
                    uses++;
                    if (pos <= main_open || pos >= main_close) inside_only = false;
                }
                if (! inside_only || uses == 0) continue;
                // the name must not be declared anywhere else either (a local of
                // the same name would collide with the injected copy)
                const std::regex other(R"((?:float|int|uint|bool|double|vec[234])\s+)" + name +
                                       R"(\s*[;,=)])");
                usize            declared { 0 };
                for (auto oit = std::sregex_iterator(result.begin(), result.end(), other);
                     oit != std::sregex_iterator();
                     ++oit) {
                    if (! AtTokenStart(result, (usize)oit->position())) continue;
                    declared++;
                }
                if (declared != 1) continue;
                const std::regex write(R"((^|[^\w.]))" + name +
                                       R"(\s*(?:\.\s*[xyzwrgba]{1,4}\s*)?)"
                                       R"((?:=(?!=)|\+=|-=|\*=|/=|%=))");
                if (! std::regex_search(result, write) && link_mismatch.count(name) == 0) continue;
                edits.push_back({ (usize)(*it).position(3),
                                  (usize)(*it).position(3) + name.size(),
                                  name + "_hlsl_in" });
                locals += "\n    " + type + " " + name + " = " + name + "_hlsl_in;";
            }
            if (! locals.empty()) {
                edits.push_back({ main_open + 1, main_open + 1, locals });
                result = ApplyEdits(result, edits);
            }
        }
    }

    return result;
}

// Fix HLSL->GLSL implicit type conversion issues.
// HLSL allows implicit float<->int conversions, GLSL/SPIR-V does not.
inline std::string FixImplicitConversions(const std::string&           src,
                                          const std::set<std::string>& link_mismatch) {
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
        auto upgradeIfOutOfRange = [&result](const char* small_type,
                                             const char* big_type,
                                             const char* oob_pattern,
                                             const char* bare_swizzle) {
            std::vector<std::string> to_upgrade;
            std::regex               re_decl(std::string(R"(\b)") + small_type + R"(\s+(\w+)\s*;)");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_decl);
                 it != std::sregex_iterator();
                 ++it) {
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
                                  const char*                  swizzle) {
            for (const auto& d : dst) {
                for (const auto& s : src) {
                    if (d == s) continue;
                    std::regex re("\\b(" + d + ")\\s*=\\s*(" + s + ")\\s*;");
                    result =
                        std::regex_replace(result, re, "$1 = $2." + std::string(swizzle) + ";");
                }
            }
        };

        fixTrunc(vec2_vars, vec3_vars, "xy");
        fixTrunc(vec2_vars, vec4_vars, "xy");
        fixTrunc(vec3_vars, vec4_vars, "xyz");

        // A vec2 constructor may have a scalar swizzle after its closing parenthesis.
        // Only handle a terminal vec2 operand; following arithmetic may change its type.
        auto             non_vector_vars = collect("float");
        const std::regex non_vector_decl(
            R"(\b(?:double|int|uint|bool|mat[234](?:x[234])?)\s+(\w+)\b)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), non_vector_decl);
             it != std::sregex_iterator();
             ++it)
            non_vector_vars.insert((*it)[1].str());
        for (const auto* vars : { &vec3_vars, &vec4_vars }) {
            for (const auto& name : *vars) {
                if (non_vector_vars.count(name)) continue;
                const std::regex operand("\\b(" + name + R"()(\s*[-+*/]\s*)(\(*\s*vec2\s*\())");
                usize            pos = 0;
                std::smatch      match;
                while (std::regex_search(
                    result.cbegin() + (isize)pos, result.cend(), match, operand)) {
                    const usize begin = pos + (usize)match.position();
                    const usize open  = begin + (usize)match.length() - 1;
                    usize       end   = open + 1;
                    int         depth = 1;
                    for (; end < result.size() && depth > 0; ++end) {
                        if (result[end] == '(') ++depth;
                        if (result[end] == ')') --depth;
                    }
                    const auto prefix = match[3].str();
                    usize      groups = (usize)std::count(prefix.begin(), prefix.end(), '(') - 1;
                    while (end < result.size()) {
                        if (std::isspace((unsigned char)result[end])) {
                            ++end;
                            continue;
                        }
                        if (groups > 0 && result[end] == ')') {
                            --groups;
                            ++end;
                            continue;
                        }
                        break;
                    }
                    const auto before =
                        result.find_last_not_of(" \t\r\n", begin == 0 ? 0 : begin - 1);
                    if (depth == 0 && groups == 0 &&
                        (begin == 0 || before == std::string::npos || result[before] != '.') &&
                        (end == result.size() || result[end] == ';' || result[end] == ',' ||
                         result[end] == ')')) {
                        result.insert(begin + name.size(), ".xy");
                        end += 3;
                    }
                    pos = end;
                }
            }
        }

        // Fix: HLSL truncates a vector to a scalar implicitly (keeps the first
        // component), e.g. "float N = vec2_expr;". GLSL requires the swizzle, so
        // wrap the initializer. Only plain operand arithmetic is typed here: a
        // member selection's width is its swizzle length, so "V.z / V.w" stays
        // scalar, and call/ternary/index operands are left alone.
        // NOTE: scalar targets only, vecN truncation is handled above.
        {
            std::map<std::string, int> widths;
            auto add_width = [&widths](const std::set<std::string>& vars, int width) {
                for (const auto& name : vars) {
                    auto [it, inserted] = widths.emplace(name, width);
                    if (! inserted && it->second != width) it->second = -1;
                }
            };
            add_width(vec2_vars, 2);
            add_width(vec3_vars, 3);
            add_width(vec4_vars, 4);
            add_width(non_vector_vars, 1);

            const std::regex number(R"(^[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?[fFuU]?$)");
            const std::regex member(R"(^([A-Za-z_]\w*)\.([xyzw]{1,4}|[rgba]{1,4})$)");
            const std::regex ident(R"(^[A-Za-z_]\w*$)");

            auto trim = [](std::string s) {
                const auto begin = s.find_first_not_of(" \t\r\n");
                if (begin == std::string::npos) return std::string {};
                return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
            };
            // 0 marks an operand whose width can't be proven, the whole statement is
            // then skipped.
            auto operand_width = [&](const std::string& operand) {
                const auto op = trim(operand);
                if (op.empty() || std::regex_match(op, number)) return 1;
                std::smatch match;
                if (std::regex_match(op, match, member)) return (int)match[2].length();
                if (std::regex_match(op, ident)) {
                    const auto it = widths.find(op);
                    if (it != widths.end() && it->second > 0) return it->second;
                }
                return 0;
            };

            const std::regex decl(R"(\bfloat\s+(\w+)\s*=\s*([^;{}]*);)");
            std::string      replaced;
            usize            last      = 0;
            bool             rewritten = false;
            for (auto it = std::sregex_iterator(result.begin(), result.end(), decl);
                 it != std::sregex_iterator();
                 ++it) {
                const std::string initializer = (*it)[2].str();
                if (initializer.find_first_of("()?,[]") != std::string::npos) continue;
                int  width       = 1;
                bool known       = true;
                auto add_operand = [&](const std::string& op) {
                    const int w = operand_width(op);
                    if (w == 0) known = false;
                    width = std::max(width, w);
                };
                std::string operand;
                for (const char c : initializer) {
                    if (c == '+' || c == '-' || c == '*' || c == '/') {
                        add_operand(operand);
                        operand.clear();
                    } else {
                        operand += c;
                    }
                }
                add_operand(operand);
                if (! known || width <= 1) continue;
                const usize begin = (usize)(*it).position();
                replaced.append(result, last, begin - last);
                replaced += "float " + (*it)[1].str() + " = (" + trim(initializer) + ").x;";
                last      = begin + (usize)(*it).length();
                rewritten = true;
            }
            if (rewritten) {
                replaced.append(result, last, std::string::npos);
                result.swap(replaced);
            }
        }

        // Wrap complete initializers so mixed vector/matrix arithmetic keeps its type.
        const std::regex declaration(R"(\b(vec[234])\s+\w+\s*=\s*)");
        const std::regex next_declarator(R"(^\s*\w+\s*=\s*)");
        const std::regex scalar(R"(^\s*[-+]?\s*(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?[fF]?\s*$)");
        const std::regex texture(R"(^\s*texture(?:Lod)?\s*\()");
        const std::regex leading_variable(R"(^\s*(\w+)\s*[-+*/])");
        usize            pos = 0;
        std::smatch      declaration_match;
        while (std::regex_search(
            result.cbegin() + (isize)pos, result.cend(), declaration_match, declaration)) {
            const auto type = declaration_match[1].str();
            usize      begin =
                pos + (usize)declaration_match.position() + (usize)declaration_match.length();
            while (true) {
                usize end   = begin;
                int   depth = 0;
                for (; end < result.size(); ++end) {
                    const char c = result[end];
                    if (depth == 0 && (c == ',' || c == ';')) break;
                    if (c == '(' || c == '[') ++depth;
                    if (c == ')' || c == ']') --depth;
                    if (depth < 0 || c == '{' || c == '}') break;
                }
                if (end == result.size() || depth != 0 ||
                    (result[end] != ',' && result[end] != ';')) {
                    pos = end;
                    break;
                }
                const auto  initializer = result.substr(begin, end - begin);
                std::smatch variable;
                bool        wider_variable = false;
                if (std::regex_search(initializer, variable, leading_variable)) {
                    const auto name = variable[1].str();
                    wider_variable =
                        ! non_vector_vars.count(name) &&
                        (vec4_vars.count(name) || (type == "vec2" && vec3_vars.count(name)));
                }
                if (std::regex_match(initializer, scalar) ||
                    (type != "vec4" &&
                     (std::regex_search(initializer, texture) || wider_variable))) {
                    result.insert(end, ")");
                    result.insert(begin, type + "(");
                    end += type.size() + 2;
                }
                pos = end + 1;
                if (result[end] != ',') break;
                std::smatch next;
                if (! std::regex_search(
                        result.cbegin() + (isize)pos, result.cend(), next, next_declarator))
                    break;
                begin = pos + (usize)next.length();
            }
        }
    }

    // Fix: HLSL pow(scalar, vecN) broadcasts the scalar; GLSL requires matching genType.
    // When the pow() result is used inside a vecN() constructor, move the broadcast inside:
    //   vecN(pow(X, Y)) → pow(vecN(X), vecN(Y))
    // Wrapping an already-vecN arg in vecN() is a safe copy-constructor identity.
    // Only handles one level of nesting inside each pow argument (sufficient in practice).
    {
        std::regex re(R"(\b(vec[234])\s*\(\s*pow\s*\()"
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

    return FixImplicitCalls(result, link_mismatch);
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
        if (pos == std::string::npos) pos = src.size();
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
                                  WPShaderInfo*                    shader_info,
                                  std::span<const WPShaderTexInfo> texs) {
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
            // A varying that the two stages declare at different widths is
            // matched by name and rejected by glslang; give the fragment its own
            // copy so the stages match by location, which is what DirectX did.
            std::set<std::string> link_mismatch;
            if (pre_info != nullptr) {
                for (const auto& [name, vdecl] : pre_info->output) {
                    const auto found = unit.preprocess_info.input.find(name);
                    if (found == unit.preprocess_info.input.end()) continue;
                    const int vertex_width   = DeclWidth(vdecl);
                    const int fragment_width = DeclWidth(found->second);
                    if (vertex_width > 0 && fragment_width > 0 && vertex_width != fragment_width)
                        link_mismatch.insert(name);
                }
            }
            unit.src = FixImplicitConversions(unit.src, link_mismatch);

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
