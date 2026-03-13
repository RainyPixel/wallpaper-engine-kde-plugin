#include "WPMdlParser.hpp"
#include "Fs/VFS.h"
#include "Fs/IBinaryStream.h"
#include "Fs/MemBinaryStream.h"
#include "WPCommon.hpp"
#include "Utils/Logging.h"
#include "Scene/SceneMesh.h"
#include "SpecTexs.hpp"
#include "wpscene/WPMaterial.h"
#include "WPShaderParser.hpp"

using namespace wallpaper;

namespace
{

WPPuppet::PlayMode ToPlayMode(std::string_view m) {
    if (m == "loop" || m.empty()) return WPPuppet::PlayMode::Loop;
    if (m == "mirror") return WPPuppet::PlayMode::Mirror;
    if (m == "single") return WPPuppet::PlayMode::Single;

    LOG_ERROR("unknown puppet animation play mode \"%s\"", m.data());
    assert(m == "loop");
    return WPPuppet::PlayMode::Loop;
}
} // namespace

// bytes * size
constexpr uint32_t singile_vertex  = 4 * (3 + 4 + 4 + 2);
constexpr uint32_t singile_indices = 2 * 3;
constexpr uint32_t std_format_vertex_size_herald_value = 0x01800009;

// number of bytes in an MDAT attachment after the attachment name
constexpr uint32_t mdat_attachment_data_byte_length = 64;

// alternative consts for alternative mdl format
constexpr uint32_t alt_singile_vertex = 4 * (3 + 4 + 4 + 2 + 7);
constexpr uint32_t alt_format_vertex_size_herald_value = 0x0180000F;

constexpr uint32_t singile_bone_frame = 4 * 9;

bool WPMdlParser::Parse(std::string_view path, fs::VFS& vfs, WPMdl& mdl) {
    auto str_path = std::string(path);
    auto pfile    = vfs.Open("/assets/" + str_path);
    auto memfile  = fs::MemBinaryStream(*pfile);
    if (! pfile) return false;
    auto& f = memfile;

    mdl.mdlv = ReadMDLVesion(f);

    int32_t mdl_flag = f.ReadInt32();
    if (mdl_flag == 9) {
        LOG_INFO("puppet '%s' is not complete, ignore", str_path.c_str());
        return false;
    };
    f.ReadInt32(); // unk, 1
    f.ReadInt32(); // unk, 1

    mdl.mat_json_file = f.ReadStr();
    // 0    
    f.ReadInt32();

    bool alt_mdl_format = false;
    uint32_t curr = f.ReadUint32();

    // if the uint at the normal vertex size position is 0, then this file
    // uses the alternative MDL format, therefore the actual vertex size is
    // located after the herald value, and we'll need to account for other differences later on.
    if(curr == 0){
        alt_mdl_format = true;
        while (curr != alt_format_vertex_size_herald_value){
            curr = f.ReadUint32();
        }
        curr = f.ReadUint32();
    }
    else if(curr == std_format_vertex_size_herald_value){
        curr = f.ReadUint32();
    }

    uint32_t vertex_size = curr;
    if (vertex_size % (alt_mdl_format? alt_singile_vertex : singile_vertex) != 0) {
        LOG_ERROR("unsupport mdl vertex size %d", vertex_size);
        return false;
    }

    // if using the alternative MDL format, vertexes contain 7 extra 32-bit chunks between
    // position and blend indices
    uint32_t vertex_num = vertex_size / (alt_mdl_format ? alt_singile_vertex : singile_vertex);
    mdl.vertexs.resize(vertex_num);
    for (auto& vert : mdl.vertexs) {
        for (auto& v : vert.position) v = f.ReadFloat();
        if(alt_mdl_format) {for (int i = 0; i < 7; i++) f.ReadUint32();}
        for (auto& v : vert.blend_indices) v = f.ReadUint32();
        for (auto& v : vert.weight) v = f.ReadFloat();
        for (auto& v : vert.texcoord) v = f.ReadFloat();
    }

    uint32_t indices_size = f.ReadUint32();
    if (indices_size % singile_indices != 0) {
        LOG_ERROR("unsupport mdl indices size %d", indices_size);
        return false;
    }

    uint32_t indices_num = indices_size / singile_indices;
    mdl.indices.resize(indices_num);
    for (auto& id : mdl.indices) {
        for (auto& v : id) v = f.ReadUint16();
    }

    // Newer MDL formats (MDLV >= 23 / PKGV0022+) insert extra sections between
    // the mesh data and the skeleton.  Scan forward byte-by-byte for the "MDLS"
    // tag rather than assuming it immediately follows the index data.
    {
        char ring[4] = { 0, 0, 0, 0 };
        char c;
        while (f.Read(&c, 1) == 1) {
            ring[0] = ring[1];
            ring[1] = ring[2];
            ring[2] = ring[3];
            ring[3] = c;
            if (ring[0] == 'M' && ring[1] == 'D' && ring[2] == 'L' && ring[3] == 'S') {
                // Read the remaining 5 bytes of the 9-byte version tag (4 digits + NUL).
                char ver_tail[6] = {};
                f.Read(ver_tail, 5);
                int v = 0;
                std::from_chars(ver_tail, ver_tail + 4, v);
                mdl.mdls = v;
                break;
            }
        }
    }

    size_t bones_file_end = f.ReadUint32();
    (void)bones_file_end;

    uint16_t bones_num = f.ReadUint16();
    // 1 byte
    f.ReadUint16(); // unk

    mdl.puppet  = std::make_shared<WPPuppet>();
    auto& bones = mdl.puppet->bones;
    auto& anims = mdl.puppet->anims;

    bones.resize(bones_num);
    for (uint i = 0; i < bones_num; i++) {
        auto&       bone = bones[i];
        std::string name = f.ReadStr();
        f.ReadInt32(); // unk

        bone.parent = f.ReadUint32();
        if (bone.parent >= i && ! bone.noParent()) {
            LOG_ERROR("mdl wrong bone parent index %d, treating as root bone", bone.parent);
            bone.parent = 0xFFFFFFFFu;
        }

        uint32_t size = f.ReadUint32();
        if (size != 64) {
            LOG_ERROR("mdl unsupport bones size: %d", size);
            return false;
        }
        for (auto row : bone.transform.matrix().colwise()) {
            for (auto& x : row) x = f.ReadFloat();
        }

        std::string bone_simulation_json = f.ReadStr();
        /*
        auto trans = bone.transform.translation();
        LOG_INFO("trans: %f %f %f", trans[0], trans[1], trans[2]);
        */
    }

    if (mdl.mdls > 1) {
        int16_t unk = f.ReadInt16();
        if (unk != 0) {
            LOG_INFO("puppet: one unk is not 0, may be wrong");
        }

        uint8_t has_trans = f.ReadUint8();
        if (has_trans) {
            for (uint i = 0; i < bones_num; i++)
                for (uint j = 0; j < 16; j++) f.ReadFloat(); // mat
        }
        uint32_t size_unk = f.ReadUint32();
        for (uint i = 0; i < size_unk; i++)
            for (int j = 0; j < 3; j++) f.ReadUint32();

        f.ReadUint32(); // unk

        uint8_t has_offset_trans = f.ReadUint8();
        if (has_offset_trans) {
            for (uint i = 0; i < bones_num; i++) {
                for (uint j = 0; j < 3; j++) f.ReadFloat();  // like pos
                for (uint j = 0; j < 16; j++) f.ReadFloat(); // mat
            }
        }

        uint8_t has_index = f.ReadUint8();
        if (has_index) {
            for (uint i = 0; i < bones_num; i++) {
                f.ReadUint32();
            }
        }
    }

    // sometimes there can be one or more zero bytes and/or MDAT sections containing
    // attachments before the MDLA section, so we need to skip them
    std::string mdType = "";
    std::string mdVersion;
    
    do {
        std::string mdPrefix = f.ReadStr();

        // sometimes there can be other garbage in this gap, so we need to 
        // skip over that as well
        if(mdPrefix.length() == 8){
            mdType = mdPrefix.substr(0, 4);
            mdVersion = mdPrefix.substr(4, 4);

            if(mdType == "MDAT"){
                f.ReadUint32(); // skip 4 bytes
                uint32_t num_attachments = f.ReadUint16(); // number of attachments in the MDAT section

                for(int i = 0; i < num_attachments; i++){
                    f.ReadUint16(); // skip 2 bytes
                    std::string attachment_name = f.ReadStr(); // attachment name
                    int bytesToRead = mdat_attachment_data_byte_length;
                    for(int j = 0; j < bytesToRead; j++){
                        f.ReadUint8();
                    }

                }
            }
        }
    } while (mdType != "MDLA");
    

    if(mdType == "MDLA" && mdVersion.length() > 0){
        mdl.mdla = std::stoi(mdVersion);
        if (mdl.mdla != 0) {
            uint end_size = f.ReadUint32();
            (void)end_size;

            uint anim_num = f.ReadUint32();
            anims.resize(anim_num);
            for (auto& anim : anims) {
                // there can be a variable number of 32-bit 0s between animations
                anim.id = 0;
                while(anim.id == 0){
                    anim.id = f.ReadInt32();
                }
    
                if (anim.id <= 0) {
                    LOG_ERROR("wrong anime id %d", anim.id);
                    return false;
                }
                f.ReadInt32();
                anim.name   = f.ReadStr();
                if(anim.name.empty()){
                    anim.name = f.ReadStr();
                }
                anim.mode   = ToPlayMode(f.ReadStr());
                anim.fps    = f.ReadFloat();
                anim.length = f.ReadInt32();
                f.ReadInt32();

                uint32_t b_num = f.ReadUint32();
                anim.bframes_array.resize(b_num);
                for (auto& bframes : anim.bframes_array) {
                    f.ReadInt32();
                    uint32_t byte_size = f.ReadUint32();
                    uint32_t num       = byte_size / singile_bone_frame;
                    if (byte_size % singile_bone_frame != 0) {
                        LOG_ERROR("wrong bone frame size %d", byte_size);
                        return false;
                    }
                    bframes.frames.resize(num);
                    for (auto& frame : bframes.frames) {
                        for (auto& v : frame.position) v = f.ReadFloat();
                        for (auto& v : frame.angle) v = f.ReadFloat();
                        for (auto& v : frame.scale) v = f.ReadFloat();
                    }
                }
                
                // in the alternative MDL format there are 2 empty bytes followed
                // by a variable number of 32-bit 0s between animations. We'll read
                // the two bytes now so that the cursor is aligned to read through the
                // 32-bit 0s in the next iteration
                if(alt_mdl_format)
                {
                    f.ReadUint8();
                    f.ReadUint8();    
                }
                else if(mdl.mdla == 3){
                    // In MDLA version 3 there is an extra 8-bit zero between animations.
                    // This will cause the parser to be misaligned moving forward if we don't handle it here.
                    f.ReadUint8();
                }
                else{
                    uint32_t unk_extra_uint = f.ReadUint32();
                    for (uint i = 0; i < unk_extra_uint; i++) {
                        f.ReadFloat();
                        // data is like: {"$$hashKey":"object:2110","frame":1,"name":"random_anim"}
                        std::string unk_extra = f.ReadStr();
                    }
                }
            }
        }
    }
    
    mdl.puppet->prepared();

    LOG_INFO("read puppet: mdlv: %d, nmdls: %d, mdla: %d, bones: %d, anims: %d",
             mdl.mdlv,
             mdl.mdls,
             mdl.mdla,
             mdl.puppet->bones.size(),
             mdl.puppet->anims.size());
    return true;
}

void WPMdlParser::GenPuppetMesh(SceneMesh& mesh, const WPMdl& mdl) {
    SceneVertexArray vertex({ { WE_IN_POSITION.data(), VertexType::FLOAT3 },
                              { WE_IN_BLENDINDICES.data(), VertexType::UINT4 },
                              { WE_IN_BLENDWEIGHTS.data(), VertexType::FLOAT4 },
                              { WE_IN_TEXCOORD.data(), VertexType::FLOAT2 } },
                            mdl.vertexs.size());

    // Clamp blend indices to [0, bone_count-1] to prevent OOB GPU array access in
    // g_Bones[a_BlendIndices.x] which causes VK_ERROR_DEVICE_LOST on strict GPU drivers.
    // Unused slots in the MDL may carry sentinel values (e.g. 0xFF) with weight=0.
    uint32_t bone_count = mdl.puppet ? (uint32_t)mdl.puppet->bones.size() : 1u;

    std::array<float, 16> one_vert;
    auto                  to_one = [bone_count](const WPMdl::Vertex& in, decltype(one_vert)& out) {
        uint offset = 0;
        memcpy(out.data() + 4 * (offset++), in.position.data(), sizeof(in.position));
        std::array<uint32_t, 4> safe_indices;
        for (int j = 0; j < 4; j++)
            safe_indices[j] = in.blend_indices[j] < bone_count ? in.blend_indices[j] : 0u;
        memcpy(out.data() + 4 * (offset++), safe_indices.data(), sizeof(safe_indices));
        memcpy(out.data() + 4 * (offset++), in.weight.data(), sizeof(in.weight));
        memcpy(out.data() + 4 * (offset++), in.texcoord.data(), sizeof(in.texcoord));
    };
    for (uint i = 0; i < mdl.vertexs.size(); i++) {
        auto& v = mdl.vertexs[i];
        to_one(v, one_vert);
        vertex.SetVertexs(i, one_vert);
    }
    std::vector<uint32_t> indices;
    size_t                u16_count = mdl.indices.size() * 3;
    indices.resize(u16_count / 2 + 1);
    memcpy(indices.data(), mdl.indices.data(), u16_count * sizeof(uint16_t));

    mesh.AddVertexArray(std::move(vertex));
    mesh.AddIndexArray(SceneIndexArray(indices));
}

void WPMdlParser::AddPuppetShaderInfo(WPShaderInfo& info, const WPMdl& mdl) {
    info.combos["SKINNING"]  = "1";
    info.combos["BONECOUNT"] = std::to_string(mdl.puppet->bones.size());
}

void WPMdlParser::AddPuppetMatInfo(wpscene::WPMaterial& mat, const WPMdl& mdl) {
    mat.combos["SKINNING"]  = 1;
    mat.combos["BONECOUNT"] = (i32)mdl.puppet->bones.size();
    mat.use_puppet          = true;
}
