#include "WPShaderParser.hpp"

#include "Fs/IBinaryStream.h"
#include "Utils/Logging.h"
#include "WPJson.hpp"

#include "wpscene/WPUniform.h"
#include "Fs/VFS.h"
#include "Utils/Sha.hpp"
#include "Utils/String.h"
#include "Vulkan/ShaderComp.hpp"

#include <charconv>
#include <filesystem>
#include <map>
#include <mutex>
#include <regex>
#include <stack>
#include <string>

static constexpr std::string_view SHADER_PLACEHOLD { "__SHADER_PLACEHOLD__" };

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

std::string MaskComments(const std::string& src) {
    std::string             code = src;
    static const std::regex comments(R"(//[^\n]*|/\*[\s\S]*?\*/)");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), comments);
         it != std::sregex_iterator();
         ++it) {
        const usize begin = it->position();
        const usize end   = begin + it->length();
        for (usize i = begin; i < end; ++i) {
            if (code[i] != '\n') code[i] = ' ';
        }
    }
    return code;
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
        const std::string code = MaskComments(src);
        const std::regex  reAfters(R"((^|\n)[ \t]*(attribute|varying|uniform|struct)[ \t])");
        const usize       declarationPos = searchLast(code, reAfters);
        usize             afterPos       = declarationPos;
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

void RemoveStrayEndif(std::string& source) {
    static const std::regex directive(R"(^[ \t]*(#[ \t]*(if|ifdef|ifndef|endif)\b)([^\n]*))",
                                      std::regex::multiline);
    const auto              code  = MaskComments(source);
    usize                   depth = 0;
    for (auto it = std::sregex_iterator(code.begin(), code.end(), directive);
         it != std::sregex_iterator();
         ++it) {
        const usize begin = it->position();
        if (begin >= 2 && code[begin - 2] == '\\') continue;
        if ((*it)[2] != "endif") {
            ++depth;
        } else if (depth > 0) {
            --depth;
        } else if ((*it)[3].str().find_first_not_of(" \t") == std::string::npos) {
            // Some Workshop shaders contain an extra #endif. Keep line numbers and comments.
            std::fill_n(source.begin() + it->position(1), it->length(1), ' ');
        }
    }
}

bool Preprocess(WPShaderUnit& unit, const Combos& combos) {
    std::string res;
    std::string src = WPShaderParser::PreShaderHeader(unit.src, combos, unit.stage);
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
    RemoveStrayEndif(src);
    // workaround #require directive
    {
        std::regex re_require("(^|\r?\n)#require (.+)(\r?\n)");
        src = std::regex_replace(src, re_require, "$1//#require $2$3");
    }

    glslang::TShader::ForbidIncluder includer;
    glslang::TShader                 shader(ToGLSL(unit.stage));
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
        return false;
    }

    auto& process_info = unit.preprocess_info;
    process_info       = {};
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
    unit.src = std::move(res);
    return true;
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

using VaryingTypes = std::map<std::string, std::string, std::less<>>;

VaryingTypes VertexOutputTypes(const std::string& vertex) {
    static const std::regex output(R"(\bout\s+(float|vec[234])\s+([A-Za-z_]\w*)\s*;)");
    VaryingTypes            types;
    for (auto it = std::sregex_iterator(vertex.begin(), vertex.end(), output);
         it != std::sregex_iterator();
         ++it) {
        const auto [entry, inserted] = types.emplace((*it)[2].str(), (*it)[1].str());
        if (! inserted) entry->second.clear();
    }
    return types;
}

// Only type the expressions needed for the Workshop conversions below. An
// unrecognized operand or conflicting declaration leaves the source untouched.
class ShaderExpressions {
public:
    struct Token {
        std::string_view text;
        usize            offset;
        usize            end;
    };

    explicit ShaderExpressions(const std::string& source): m_source(source) {
        static const std::regex token(
            R"([A-Za-z_]\w*|(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?[fFuU]?|[=!<>+\-*/]=|\+\+|--|[^\s])");
        std::vector<usize> openings;
        for (auto it = std::sregex_iterator(source.begin(), source.end(), token);
             it != std::sregex_iterator();
             ++it) {
            const usize begin = it->position();
            const auto  word  = std::string_view(source).substr(begin, it->length());
            const usize i     = m_tokens.size();
            m_tokens.push_back({ word, begin, i + 1 });
            if (word == "(" || word == "[" || word == "{") {
                openings.push_back(i);
            } else if ((word == ")" || word == "]" || word == "}") && ! openings.empty()) {
                m_tokens[openings.back()].end = i + 1;
                openings.pop_back();
            }
            if (i >= 1 && Text(i - 1) == "struct") m_struct_types.insert(word);
        }

        for (usize i = 0; i + 2 < m_tokens.size(); ++i) {
            const auto type = Text(i);
            if (! IsType(type) || ! IsIdentifier(Text(i + 1))) continue;
            const auto name = Text(i + 1);
            if (Text(i + 2) == "(" && End(i + 2) > i + 3) {
                const auto [entry, inserted] = m_functions.emplace(name, i + 2);
                if (! inserted) entry->second = 0;
                continue;
            }
            for (usize var = i + 1; IsIdentifier(Text(var));) {
                const auto next = Text(var + 1);
                if (next != "=" && next != "[" && next != "," && next != ";" && next != ")") break;
                const auto value_type        = next == "[" ? std::string_view {} : type;
                const auto [entry, inserted] = m_symbols.emplace(Text(var), Symbol { value_type });
                if (! inserted) {
                    entry->second.unique = false;
                    if (entry->second.type != value_type) entry->second.type = {};
                }
                const usize after = ArgumentEnd(var + 1, m_tokens.size());
                if (Text(after) != ",") break;
                var = after + 1;
            }
        }
    }

    std::string Rewrite(ShaderType stage, const VaryingTypes& vertex_outputs) {
        for (usize i = 0; i < m_tokens.size(); ++i) {
            const auto word = Text(i);
            if (Text(i + 1) == "(" && IsIdentifier(word) && (i == 0 || Text(i - 1) != ".")) {
                RewriteCall(i);
            } else if (word == "(") {
                RewriteComparison(i);
            } else if (word == "?" && i > 1 && IsNumber(Text(i - 1)) &&
                       (Text(i - 2) == "=" || Text(i - 2) == "(" || Text(i - 2) == "," ||
                        Text(i - 2) == "return" || Text(i - 2) == "?" || Text(i - 2) == ":")) {
                Wrap(i - 1, i, "bool");
            } else if ((word == "=" || word == "+=" || word == "-=" || word == "*=" ||
                        word == "/=") &&
                       i > 0 && IsIdentifier(Text(i - 1)) && (i < 2 || Text(i - 2) != ".")) {
                const auto type = TypeOf(Text(i - 1));
                const auto end  = ArgumentEnd(i + 1, m_tokens.size());
                if (end <= i + 1 || (Text(end) != "," && Text(end) != ";")) continue;
                const int  width  = Width(i + 1, end);
                const auto target = FloatWidth(type);
                if (width > target && target > 0) Wrap(i + 1, end, type);
                if (word == "=" && target > 1 && width == 1) Wrap(i + 1, end, type);
                if (word == "=" && type == "int" && end == i + 2 && TypeOf(Text(i + 1)) == "float")
                    Wrap(i + 1, end, type);
            }
        }
        if (stage == ShaderType::FRAGMENT) RewriteFragmentInputs(vertex_outputs);
        std::string result = m_source;
        for (auto it = m_edits.rbegin(); it != m_edits.rend(); ++it)
            result.replace(it->first, it->second.length, it->second.text);
        return result;
    }

private:
    struct Symbol {
        std::string_view type;
        bool             unique { true };
    };

    struct Edit {
        usize       length { 0 };
        std::string text;
    };

    static bool IsIdentifier(std::string_view text) {
        return ! text.empty() &&
               (std::isalpha(static_cast<unsigned char>(text.front())) || text.front() == '_');
    }
    static bool IsNumber(std::string_view text) {
        return ! text.empty() && (std::isdigit(static_cast<unsigned char>(text.front())) ||
                                  (text.front() == '.' && text.size() > 1));
    }
    bool IsType(std::string_view text) const {
        return FloatWidth(text) != 0 || text == "int" || text == "uint" || text == "bool" ||
               text == "double" || text == "void" || text.starts_with("mat") ||
               text.starts_with("sampler") || text.starts_with("isampler") ||
               text.starts_with("usampler") || text.starts_with("ivec") ||
               text.starts_with("uvec") || text.starts_with("bvec") || text.starts_with("dvec") ||
               m_struct_types.contains(text);
    }
    static int FloatWidth(std::string_view type) {
        if (type == "float") return 1;
        if (type == "vec2") return 2;
        if (type == "vec3") return 3;
        if (type == "vec4") return 4;
        return 0;
    }
    std::string_view Text(usize i) const {
        return i < m_tokens.size() ? m_tokens[i].text : std::string_view {};
    }
    usize            End(usize i) const { return m_tokens[i].end; }
    std::string_view TypeOf(std::string_view name) const {
        const auto it = m_symbols.find(name);
        return it == m_symbols.end() ? std::string_view {} : it->second.type;
    }
    usize ArgumentEnd(usize begin, usize limit) const {
        while (begin < limit && Text(begin) != "," && Text(begin) != ";" && Text(begin) != ")" &&
               Text(begin) != "]" && Text(begin) != "}" && Text(begin) != "{")
            begin = End(begin);
        return begin;
    }
    bool IsSampler2D(usize begin, usize end) const {
        return begin + 1 < end && Text(begin + 1) == "," && TypeOf(Text(begin)) == "sampler2D";
    }
    static bool Arithmetic(std::string_view text) {
        return text == "+" || text == "-" || text == "*" || text == "/";
    }

    int Width(usize begin, usize end) const {
        int width = 0;
        for (usize i = begin; i < end;) {
            if (Text(i) == "+" || Text(i) == "-") ++i;
            if (i >= end) return 0;

            const auto word    = Text(i);
            int        operand = 0;
            if (word == "(" && End(i) > i + 1 && End(i) <= end) {
                operand = Width(i + 1, End(i) - 1);
                i       = End(i);
            } else if (Text(i + 1) == "(" && End(i + 1) > i + 2 && End(i + 1) <= end) {
                operand = FloatWidth(word);
                if ((word == "texture" || word == "textureLod") && ! m_functions.contains(word) &&
                    IsSampler2D(i + 2, End(i + 1) - 1))
                    operand = 4;
                i = End(i + 1);
            } else {
                operand = FloatWidth(TypeOf(word));
                if (IsNumber(word)) {
                    // Integer literals are accepted by ordinary floating-point arithmetic.
                    operand = word.ends_with('u') || word.ends_with('U') ? 0 : 1;
                }
                ++i;
            }

            if (operand > 1 && Text(i) == "." && i + 1 < end) {
                const auto             selector = Text(i + 1);
                const std::string_view xyzw = "xyzw", rgba = "rgba", stpq = "stpq";
                if (selector.size() > 4 ||
                    (selector.find_first_not_of(xyzw.substr(0, operand)) !=
                         std::string_view::npos &&
                     selector.find_first_not_of(rgba.substr(0, operand)) !=
                         std::string_view::npos &&
                     selector.find_first_not_of(stpq.substr(0, operand)) != std::string_view::npos))
                    return 0;
                operand = selector.size();
                i += 2;
            }
            if (operand == 0 || (width > 1 && operand > 1 && width != operand)) return 0;
            width = std::max(width, operand);
            if (i == end) return width;
            if (! Arithmetic(Text(i)) || ++i == end) return 0;
        }
        return 0;
    }

    void Wrap(usize begin, usize end, std::string_view type) {
        const usize first = m_tokens[begin].offset;
        const auto& last  = m_tokens[end - 1];
        m_edits[first].text += std::string(type) + "(";
        m_edits[last.offset + last.text.size()].text.insert(0, ")");
    }

    void RewriteComparison(usize open) {
        // A parenthesized comparison can be an arithmetic operand. A control
        // statement or a call owns its parentheses, including "if (...) ++x".
        if (open == 0 || IsIdentifier(Text(open - 1)) || End(open) != open + 5) return;
        const auto op    = Text(open + 2);
        const auto after = Text(End(open));
        if (op != "<" && op != ">" && op != "<=" && op != ">=" && op != "==" && op != "!=") return;
        if ((Arithmetic(Text(open - 1)) || Arithmetic(after)) && Width(open + 1, open + 2) == 1 &&
            Width(open + 3, open + 4) == 1)
            Wrap(open, End(open), "float");
    }

    void RewriteCall(usize name_index) {
        const auto  name = Text(name_index);
        const usize open = name_index + 1;
        if (End(open) <= open + 1) return;
        const usize close    = End(open) - 1;
        const auto  function = m_functions.find(name);
        if (function != m_functions.end() && (function->second == 0 || function->second == open))
            return;
        if (function == m_functions.end() && name != "texture" && name != "textureLod" &&
            name != "mix")
            return;
        std::vector<std::pair<usize, usize>> args;
        for (usize start = open + 1; start < close;) {
            const usize end = ArgumentEnd(start, close);
            if (end == start || (end != close && Text(end) != ",")) return;
            args.emplace_back(start, end);
            start = end + 1;
        }
        if (function == m_functions.end() && (name == "texture" || name == "textureLod")) {
            const usize minimum = name == "texture" ? 2 : 3;
            if (args.size() >= minimum && args.size() <= 3 && IsSampler2D(open + 1, close)) {
                const auto [begin, end] = args[1];
                if (Width(begin, end) > 2) Wrap(begin, end, "vec2");
            }
        } else if (function == m_functions.end() && name == "mix" && args.size() == 3) {
            const auto [first, first_end]   = args[0];
            const auto [second, second_end] = args[1];
            const int a = Width(first, first_end), b = Width(second, second_end);
            // Keep the call's vector width when one operand is scalar.
            if (a == 1 && b > 1) Wrap(first, first_end, "vec" + std::to_string(b));
            if (b == 1 && a > 1) Wrap(second, second_end, "vec" + std::to_string(a));
        } else if (function != m_functions.end()) {
            usize                         param       = function->second + 1;
            const usize                   param_close = End(function->second) - 1;
            std::vector<std::string_view> input_types;
            while (param < param_close) {
                const usize end = ArgumentEnd(param, param_close);
                while (Text(param) == "in" || Text(param) == "const") ++param;
                // out/inout and any unmodeled signature parameter stay unchanged.
                input_types.push_back(end == param + 2 ? Text(param) : std::string_view {});
                param = end + 1;
            }
            if (input_types.size() != args.size()) return;
            for (usize j = 0; j < args.size(); ++j) {
                const int target        = FloatWidth(input_types[j]);
                const auto [begin, end] = args[j];
                if (target > 1 && Width(begin, end) > target) Wrap(begin, end, input_types[j]);
            }
        }
    }

    void RewriteFragmentInputs(const VaryingTypes& vertex_outputs) {
        const auto main = m_functions.find("main");
        if (main == m_functions.end() || main->second == 0) return;
        const usize body = End(main->second);
        if (Text(body) != "{" || End(body) <= body + 1) return;
        const usize body_end = End(body) - 1;
        for (usize i = 0; i < body; ++i) {
            const auto type = Text(i + 1);
            const auto name = Text(i + 2);
            if (Text(i) != "in" || FloatWidth(type) == 0 || Text(i + 3) != ";") continue;
            const auto symbol = m_symbols.find(name);
            if (symbol == m_symbols.end() || ! symbol->second.unique) continue;

            const auto vertex = vertex_outputs.find(name);
            const auto vertex_width =
                vertex == vertex_outputs.end() ? 0 : FloatWidth(vertex->second);
            const bool        truncate = vertex_width > FloatWidth(type);
            const std::string copy     = "wpLocal_" + std::string(name);
            if (m_source.find(copy) != std::string::npos) continue;
            bool               writable = false;
            std::vector<usize> uses;
            for (usize j = 1; j < m_tokens.size(); ++j) {
                if (j == i + 2 || Text(j) != name || Text(j - 1) == ".") continue;
                if (j <= body || j >= body_end) {
                    uses.clear();
                    break;
                }
                uses.push_back(j);
                const auto after  = Text(j + 1) == "." ? Text(j + 3) : Text(j + 1);
                const auto before = Text(j - 1);
                writable |= after == "=" || after == "+=" || after == "-=" || after == "*=" ||
                            after == "/=" || after == "++" || after == "--" || before == "++" ||
                            before == "--";
            }
            if ((! truncate && ! writable) || uses.empty()) continue;
            // Preserve the interface name and qualifiers. Truncate the matched
            // vertex output only in the fragment's local copy.
            std::string initializer(name);
            if (truncate) {
                const auto& input_type     = m_tokens[i + 1];
                m_edits[input_type.offset] = { input_type.text.size(), vertex->second };
                initializer                = std::string(type) + "(" + initializer + ")";
            }
            m_edits[m_tokens[body].offset + 1].text.insert(
                0, "\n" + std::string(type) + " " + copy + " = " + initializer + ";\n");
            for (const auto use : uses) m_edits[m_tokens[use].offset].text += "wpLocal_";
        }
    }

    const std::string&                 m_source;
    std::vector<Token>                 m_tokens;
    std::set<std::string_view>         m_struct_types;
    std::map<std::string_view, Symbol> m_symbols;
    std::map<std::string_view, usize>  m_functions;
    std::map<usize, Edit>              m_edits;
};

// Fix HLSL->GLSL implicit type conversion issues.
// HLSL allows implicit float<->int conversions, GLSL/SPIR-V does not.
inline std::string FixImplicitConversions(const std::string& src, ShaderType stage,
                                          const VaryingTypes& vertex_outputs) {
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

    return ShaderExpressions(result).Rewrite(stage, vertex_outputs);
}

// Bump this namespace when translation or compiler options change. Old per-scene
// entries lack this version and the compiler/stage identity in their key.
constexpr std::string_view shader_cache_dir    = "/cache/spvs03/";
constexpr std::string_view shader_cache_header = "SPVS0002";
constexpr u32              spirv_magic         = 0x07230203;

std::string ShaderCachePath(std::span<const WPShaderUnit> units) {
    const auto  version = glslang::GetVersion();
    std::string key = std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
                      std::to_string(version.patch) + version.flavor + '\n';
    for (const auto& unit : units) {
        key += std::to_string(ToGLSL(unit.stage)) + ":" + utils::genSha1(unit.src) + '\n';
    }
    return std::string(shader_cache_dir) + utils::genSha1(key) + ".spvs";
}

bool LoadShaderFromFile(std::vector<ShaderCode>& codes, fs::IBinaryStream& file,
                        usize stage_count) {
    const auto file_size = file.Size();
    char       header[shader_cache_header.size()];
    if (file.Read(header, sizeof(header)) != sizeof(header) ||
        std::string_view(header, sizeof(header)) != shader_cache_header ||
        file.ReadUint32() != stage_count) {
        return false;
    }

    std::vector<ShaderCode> cached(stage_count);
    for (auto& code : cached) {
        const auto size   = file.ReadUint32();
        const auto offset = file.Tell();
        if (offset < 0 || offset > file_size || size > static_cast<usize>(file_size - offset) ||
            size % sizeof(u32) != 0 || size <= 5 * sizeof(u32)) {
            return false;
        }
        code.resize(size / sizeof(u32));
        if (file.Read(code.data(), size) != size || code.front() != spirv_magic) return false;
    }
    if (file.Tell() != file_size) return false;
    codes = std::move(cached);
    return true;
}

void SaveShaderToFile(std::span<const ShaderCode> codes, fs::IBinaryStreamW& file) {
    file.Write(shader_cache_header.data(), shader_cache_header.size());
    file.WriteUint32(static_cast<u32>(codes.size()));
    for (const auto& code : codes) {
        const auto size = static_cast<u32>(code.size() * sizeof(u32));
        file.WriteUint32(size);
        file.Write(code.data(), size);
    }
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
    (void)scene_id;
    (void)texs;
    codes.clear();
    if (units.empty()) return false;

    for (auto& unit : units) {
        if (! Preprocess(unit, shader_info->combos)) return false;
    }

    auto compile = [](std::span<WPShaderUnit> units, std::vector<ShaderCode>& codes) {
        std::vector<vulkan::ShaderCompUnit> vunits(units.size());
        for (usize i = 0; i < units.size(); i++) {
            auto&               unit     = units[i];
            auto&               vunit    = vunits[i];
            WPPreprocessorInfo* pre_info = i >= 1 ? &units[i - 1].preprocess_info : nullptr;
            WPPreprocessorInfo* post_info =
                i + 1 < units.size() ? &units[i + 1].preprocess_info : nullptr;

            unit.src                  = Finalprocessor(unit, pre_info, post_info);
            const auto vertex_outputs = unit.stage == ShaderType::FRAGMENT && i > 0 &&
                                                units[i - 1].stage == ShaderType::VERTEX
                                            ? VertexOutputTypes(units[i - 1].src)
                                            : VaryingTypes {};
            unit.src = FixImplicitConversions(unit.src, unit.stage, vertex_outputs);

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

    if (! vfs.IsMounted("cache")) return compile(units, codes);

    // Plasma can render multiple screens in one process. Serialize cache I/O so
    // a reader never sees a half-written entry, without serializing compilation.
    static std::mutex cache_mutex;
    const auto        path = ShaderCachePath(units);
    try {
        const std::lock_guard lock(cache_mutex);
        if (vfs.Contains(path)) {
            const auto cache_file = vfs.Open(path);
            if (cache_file && LoadShaderFromFile(codes, *cache_file, units.size())) return true;
            LOG_ERROR("Invalid shader cache entry '%s', recompiling", path.c_str());
        }
    } catch (const std::filesystem::filesystem_error& error) {
        LOG_ERROR("Cannot read shader cache: %s", error.what());
    }

    if (! compile(units, codes)) return false;
    try {
        const std::lock_guard lock(cache_mutex);
        if (const auto cache_file = vfs.OpenW(path); cache_file) {
            SaveShaderToFile(codes, *cache_file);
        }
    } catch (const std::filesystem::filesystem_error& error) {
        LOG_ERROR("Cannot write shader cache: %s", error.what());
    }
    return true;
}
