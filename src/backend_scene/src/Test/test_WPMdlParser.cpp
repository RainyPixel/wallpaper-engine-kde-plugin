#include <doctest.h>
#include <bit>
#include <limits>

#include "ParserTestFs.hpp"
#include "Scene/SceneMesh.h"
#include "SpecTexs.hpp"
#include "WPMdlParser.hpp"

using namespace wallpaper;

namespace
{
void U32(std::vector<uint8_t>& data, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data.push_back((uint8_t)(value >> (8 * i)));
}
void Str(std::vector<uint8_t>& data, const std::string& value) {
    data.insert(data.end(), value.begin(), value.end());
    data.push_back(0);
}
std::vector<uint8_t> MeshPrefix() {
    std::vector<uint8_t> data;
    Str(data, "MDLV0013");
    U32(data, 1);
    U32(data, 1);
    U32(data, 1);
    Str(data, "material.json");
    U32(data, 0);
    return data;
}
std::vector<uint8_t> StaticModel() {
    auto data = MeshPrefix();
    U32(data, 52);
    data.resize(data.size() + 52);
    U32(data, 6);
    data.resize(data.size() + 6);
    Str(data, "MDLS0001");
    U32(data, 0);
    data.resize(data.size() + 4); // zero bones, unknown uint16
    return data;
}
std::vector<uint8_t> AnimatedModel(const std::string& mode, uint32_t frame_bytes,
                                   int32_t length = 1, float fps = 30.0f) {
    auto data = StaticModel();
    Str(data, "MDLA0001");
    U32(data, 0);
    U32(data, 1);
    U32(data, 1);
    U32(data, 0);
    Str(data, "test");
    Str(data, mode);
    U32(data, std::bit_cast<uint32_t>(fps));
    U32(data, (uint32_t)length);
    U32(data, 0);
    U32(data, 1);
    U32(data, 0);
    U32(data, frame_bytes);
    data.resize(data.size() + frame_bytes);
    U32(data, 0);
    return data;
}
bool ParseModel(std::vector<uint8_t> data, WPMdl& mdl) {
    fs::VFS vfs;
    auto    files             = std::make_unique<ParserTestFs>();
    files->files["/test.mdl"] = std::move(data);
    if (! vfs.Mount("/assets", std::move(files))) return false;
    return WPMdlParser::Parse("test.mdl", vfs, mdl);
}

void AddQuad(WPMdl& mdl, u32 bone, float x, float y, float width) {
    for (const float dx : { 0.0f, width }) {
        for (const float dy : { 0.0f, width }) {
            mdl.vertexs.push_back({
                { x + dx, y + dy, 0.0f },
                { bone, 0xff, 0xff, 0xff },
                { 1.0f },
                {},
            });
        }
    }
}

WPMdl BlinkModel() {
    WPMdl mdl;
    mdl.puppet = std::make_shared<WPPuppet>();
    mdl.puppet->bones.resize(6);
    for (usize i = 1; i < mdl.puppet->bones.size(); ++i) mdl.puppet->bones[i].parent = 0;
    AddQuad(mdl, 1, 0.0f, 0.0f, 2.0f);
    AddQuad(mdl, 2, 0.5f, 0.5f, 1.0f);
    AddQuad(mdl, 3, 3.0f, 0.0f, 2.0f);
    AddQuad(mdl, 4, 3.5f, 0.5f, 1.0f);
    AddQuad(mdl, 5, 0.0f, 3.0f, 1.0f);
    mdl.indices.push_back({ 0, 1, 2 });

    WPPuppet::Animation blink {
        .id     = 12,
        .fps    = 30.0,
        .length = 3,
        .mode   = WPPuppet::PlayMode::Loop,
    };
    blink.bframes_array.resize(mdl.puppet->bones.size());
    for (auto& track : blink.bframes_array) {
        track.frames.assign(3,
                            WPPuppet::BoneFrame { Eigen::Vector3f::Zero(),
                                                  Eigen::Vector3f::Zero(),
                                                  Eigen::Vector3f::Ones(),
                                                  Eigen::Quaterniond::Identity() });
    }
    blink.bframes_array[1].frames[1].scale.y() = 0.05f;
    blink.bframes_array[3].frames[1].scale.y() = 0.05f;
    mdl.puppet->anims.push_back(std::move(blink));
    return mdl;
}

std::vector<u32> MeshBoneIndices(const WPMdl&                                   mdl,
                                 std::span<const WPPuppetLayer::AnimationLayer> layers) {
    SceneMesh mesh;
    WPMdlParser::GenPuppetMesh(mesh, mdl, layers);
    const auto& vertices = mesh.GetVertexArray(0);
    const auto  offset =
        vertices.GetAttrOffsetMap().at(std::string(WE_IN_BLENDINDICES)).offset / sizeof(float);
    std::vector<u32> bones;
    for (usize i = 0; i < vertices.VertexCount(); i += 4) {
        const auto* indices = vertices.Data() + i * vertices.OneSize() + offset;
        bones.push_back(std::bit_cast<u32>(indices[0]));
        CHECK(std::bit_cast<u32>(indices[1]) == 0);
    }
    return bones;
}
} // namespace

TEST_SUITE("WPMdlParser") {
    TEST_CASE("static models may end after the skeleton") {
        WPMdl mdl;
        REQUIRE(ParseModel(StaticModel(), mdl));
        REQUIRE(mdl.puppet != nullptr);
        CHECK(mdl.vertexs.size() == 1);
        CHECK(mdl.indices.size() == 1);
        CHECK(mdl.puppet->anims.empty());
    }

    TEST_CASE("truncated fixed fields and missing alternate herald fail") {
        const auto valid = StaticModel();
        for (size_t size = 0; size < valid.size(); ++size) {
            INFO("prefix length: ", size);
            WPMdl mdl;
            CHECK_FALSE(ParseModel({ valid.begin(), valid.begin() + (isize)size }, mdl));
        }
        auto alternate = MeshPrefix();
        U32(alternate, 0);
        WPMdl mdl;
        CHECK_FALSE(ParseModel(std::move(alternate), mdl));
    }

    TEST_CASE("truncated animation ids and invalid versions fail") {
        auto data = StaticModel();
        Str(data, "MDLA0001");
        U32(data, 0);
        U32(data, 1);
        U32(data, 0); // Padding with no following animation id.
        WPMdl mdl;
        CHECK_FALSE(ParseModel(std::move(data), mdl));
        data = StaticModel();
        Str(data, "MDLAoops");
        bool parsed = true;
        CHECK_NOTHROW(parsed = ParseModel(std::move(data), mdl));
        CHECK_FALSE(parsed);
    }

    TEST_CASE("animation counts and bone frame sizes are bounded by remaining bytes") {
        auto data = StaticModel();
        Str(data, "MDLA0001");
        U32(data, 0);
        U32(data, 0xFFFFFFFFu);
        WPMdl mdl;
        CHECK_FALSE(ParseModel(std::move(data), mdl));

        data = StaticModel();
        Str(data, "MDLA0001");
        U32(data, 0);
        U32(data, 1);
        U32(data, 1);
        U32(data, 0);
        Str(data, "test");
        Str(data, "loop");
        U32(data, 0x41F00000u); // 30.0f
        U32(data, 1);
        U32(data, 0);
        U32(data, 1);
        U32(data, 0);
        U32(data, 36); // One bone frame with no frame payload.
        CHECK_FALSE(ParseModel(std::move(data), mdl));
    }
    TEST_CASE("empty bone tracks and unknown playback modes fail before runtime") {
        WPMdl mdl;
        CHECK_FALSE(ParseModel(AnimatedModel("loop", 0), mdl));
        CHECK_FALSE(ParseModel(AnimatedModel("invalid", 36), mdl));
        REQUIRE(ParseModel(AnimatedModel("loop", 36), mdl));
        REQUIRE(mdl.puppet->anims.size() == 1);
        CHECK(mdl.puppet->anims[0].bframes_array[0].frames.size() == 1);
    }
    TEST_CASE("animation timing and track lengths are safe for interpolation") {
        WPMdl mdl;
        for (const int32_t length : { 0, -1, 2 }) {
            INFO("length: ", length);
            CHECK_FALSE(ParseModel(AnimatedModel("loop", 36, length), mdl));
        }
        for (const float fps : { 0.0f,
                                 -30.0f,
                                 std::numeric_limits<float>::quiet_NaN(),
                                 std::numeric_limits<float>::infinity() }) {
            INFO("fps: ", fps);
            CHECK_FALSE(ParseModel(AnimatedModel("loop", 36, 1, fps), mdl));
        }
        REQUIRE(ParseModel(AnimatedModel("loop", 72, 2), mdl));
        double      time          = 0.0;
        const auto& animation     = mdl.puppet->anims[0];
        const auto  interpolation = animation.getInterpolationInfo(&time);
        CHECK(interpolation.frame_a < (idx)animation.bframes_array[0].frames.size());
        CHECK(interpolation.frame_b < (idx)animation.bframes_array[0].frames.size());
    }

    TEST_CASE("flat eyes share their overlapping animated lids without moving decoration") {
        auto mdl = BlinkModel();
        mdl.puppet->prepared();
        const std::array layers { WPPuppetLayer::AnimationLayer { .id = 12 } };
        CHECK(MeshBoneIndices(mdl, layers) == std::vector<u32> { 1, 1, 3, 3, 5 });
        CHECK(mdl.vertexs[4].blend_indices[0] == 2);
        CHECK(mdl.vertexs[12].blend_indices[0] == 4);
        WPPuppetLayer layer(mdl.puppet);
        layer.prepared(layers);
        const auto open = layer.genFrame(0.0);
        CHECK(open[1].matrix().isApprox(open[2].matrix()));
        const auto closed = layer.genFrame(mdl.puppet->anims.front().frame_time);
        CHECK(closed[1].linear()(1, 1) == doctest::Approx(0.05f));
        CHECK(closed[2].linear()(1, 1) == doctest::Approx(1.0f));
    }

    TEST_CASE("inactive, padded and weakly blended collapse does not rebind geometry") {
        auto       mdl = BlinkModel();
        std::array layers { WPPuppetLayer::AnimationLayer { .id = 12 } };
        SUBCASE("unselected animation") { layers[0].id = 42; }
        SUBCASE("hidden layer") { layers[0].visible = false; }
        SUBCASE("zero blend") { layers[0].blend = 0.0; }
        SUBCASE("rest frame held at zero playback rate") { layers[0].rate = 0.0; }
        SUBCASE("the blend keeps the lid open") { layers[0].blend = 0.5; }
        SUBCASE("scale collapse exists only in the padding beyond the declared length") {
            mdl.puppet->anims.front().length = 1;
        }
        mdl.puppet->prepared();
        CHECK(MeshBoneIndices(mdl, layers) == std::vector<u32> { 1, 2, 3, 4, 5 });
    }

    TEST_CASE("a blended blink rebinds geometry only when its composed pose closes") {
        auto mdl   = BlinkModel();
        auto other = mdl.puppet->anims.front();
        other.id   = 13;
        for (auto& track : other.bframes_array) {
            for (auto& frame : track.frames) frame.scale = Eigen::Vector3f::Ones();
        }
        float            closing_scale = 0.05f;
        std::vector<u32> expected { 1, 1, 3, 3, 5 };
        SUBCASE("the second full-blend clip leaves the blink delta intact") {}
        SUBCASE("the second clip scales the parent and cancels the blink collapse") {
            other.bframes_array[0].frames[1].scale.y() = 20.0f;
            closing_scale                              = 1.0f;
            expected                                   = { 1, 2, 3, 4, 5 };
        }
        mdl.puppet->anims.push_back(std::move(other));
        mdl.puppet->prepared();
        const std::array layers { WPPuppetLayer::AnimationLayer { .id = 12 },
                                  WPPuppetLayer::AnimationLayer { .id = 13 } };
        WPPuppetLayer    layer(mdl.puppet);
        layer.prepared(layers);
        const auto closed = layer.genFrame(mdl.puppet->anims.front().frame_time);
        CHECK(closed[1].linear()(1, 1) == doctest::Approx(closing_scale));
        CHECK(MeshBoneIndices(mdl, layers) == expected);
    }

    TEST_CASE(
        "ambiguous matches, different rest transforms and unrelated motion keep the binding") {
        auto             mdl = BlinkModel();
        std::vector<u32> expected { 1, 2, 3, 4, 5 };
        auto&            tracks = mdl.puppet->anims.front().bframes_array;
        SUBCASE("two stationary groups overlap one closer") {
            AddQuad(mdl, 5, 0.3f, 0.3f, 0.5f);
            expected.push_back(5);
            for (usize i = 16; i < 20; ++i) mdl.vertexs[i].position[1] -= 3.0f;
        }
        SUBCASE("two equally good closing groups overlap one stationary group") {
            for (usize i = 4; i < 8; ++i) mdl.vertexs[i].position[0] += 0.5f;
            for (usize i = 8; i < 12; ++i) mdl.vertexs[i].position[0] -= 2.0f;
            for (usize i = 12; i < 16; ++i) mdl.vertexs[i].position[0] -= 1.5f;
        }
        SUBCASE("the stationary group has an independently translated rest pose") {
            for (auto& frame : tracks[2].frames) frame.position.x() = 1.0f;
        }
        SUBCASE("the overlapping group also animates a translation") {
            tracks[2].frames[1].position.x() = 0.5f;
        }
        SUBCASE("the target scales only depth and leaves the quad plane intact") {
            tracks[1].frames[1].scale = Eigen::Vector3f(1.0f, 1.0f, 0.05f);
            tracks[3].frames[1].scale = Eigen::Vector3f(1.0f, 1.0f, 0.05f);
        }
        mdl.puppet->prepared();
        const std::array layers { WPPuppetLayer::AnimationLayer { .id = 12 } };
        CHECK(MeshBoneIndices(mdl, layers) == expected);
    }
}
