#pragma once
#include <string>
#include <cstdint>
#include <array>
#include <vector>
#include <memory>
#include <Eigen/Dense>

#include "WPPuppet.hpp"

namespace wallpaper
{

class WPShaderInfo;

namespace wpscene
{
class WPMaterial;
};
namespace fs
{
class VFS;
};

struct WPMdl {
    i32 mdlv { 13 };
    i32 mdls { 1 };
    i32 mdla { 1 };

    std::string mat_json_file;
    struct Vertex {
        std::array<float, 3>    position;
        std::array<uint32_t, 4> blend_indices;
        std::array<float, 4>    weight;
        std::array<float, 2>    texcoord;
    };
    std::vector<Vertex>                  vertexs;
    std::vector<std::array<uint16_t, 3>> indices;

    // std::vector<Eigen::Matrix<float, 3, 4>> bones;
    std::shared_ptr<WPPuppet> puppet;
    // Bone-index table from the MDLS skeleton ("has_index" section), retained
    // for reference. It is NOT used by GenPuppetMesh: an earlier attempt to
    // treat it as a blend-index permutation was disproved (affected models can
    // carry an identity table, and applying it freezes the lash/lid layers).
    // GenPuppetMesh derives its correction from animation scale tracks and
    // vertex-group bounding boxes instead.
    std::vector<uint32_t> bone_index_map;
    // combo
    // SKINNING = 1
    // BONECOUNT

    // input
    // uvec4 a_BlendIndices
    // vec4 a_BlendWeights
    // uniform mat4x3 g_Bones[BONECOUNT]
};

class SceneMesh;

class WPMdlParser {
public:
    static bool Parse(std::string_view path, fs::VFS&, WPMdl&);

    static void AddPuppetShaderInfo(WPShaderInfo& info, const WPMdl& mdl);
    static void AddPuppetMatInfo(wpscene::WPMaterial& mat, const WPMdl& mdl);

    static void GenPuppetMesh(SceneMesh& mesh, const WPMdl& mdl);
};

} // namespace wallpaper
