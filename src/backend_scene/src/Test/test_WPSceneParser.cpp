#include <doctest.h>

#include "Audio/SoundManager.h"
#include "ParserTestFs.hpp"
#include "Particle/ParticleSystem.h"
#include "RenderGraph/RenderGraph.hpp"
#include "SpecTexs.hpp"
#include "VulkanRender/SceneToRenderGraph.hpp"
#include "WPSceneParser.hpp"
#include "WPShaderValueUpdater.hpp"
#include <nlohmann/json.hpp>

using namespace wallpaper;

namespace
{
std::unique_ptr<ParserTestFs> TestFiles() {
    auto                                      fs      = std::make_unique<ParserTestFs>();
    const std::pair<std::string, std::string> files[] = {
        { "/models/image.json", R"({"material":"materials/image.json","width":64,"height":64})" },
        { "/materials/image.json", R"({"passes":[{"shader":"test"}]})" },
        { "/particles/test.json",
          R"({"material":"materials/image.json","maxcount":2,"starttime":0,"emitter":[{"name":"boxrandom","id":0}]})" },
        { "/shaders/test.vert",
          "attribute vec3 a_Position; void main() { gl_Position = vec4(a_Position, 1.0); }" },
        { "/shaders/test.frag", "uniform vec4 g_Color4; void main() { gl_FragColor = g_Color4; }" },
    };
    for (const auto& [path, content] : files) {
        fs->files[path] = std::vector<uint8_t>(content.begin(), content.end());
    }
    return fs;
}

nlohmann::json TestScene() {
    return nlohmann::json::parse(R"({
        "camera":{"center":"32 32 0","eye":"32 32 1","up":"0 1 0"},
        "general":{"clearcolor":"0 0 0","cameraparallax":false,
                   "cameraparallaxamount":0,"cameraparallaxdelay":1,
                   "cameraparallaxmouseinfluence":0,
                   "orthogonalprojection":{"width":64,"height":64}},
        "objects":[
            {"id":1,"image":"models/image.json","origin":"32 32 0",
             "angles":"0 0 0","scale":"1 1 1",
             "color":{"user":"backgroundcolor","value":"0.7 0.6 0.5"},
             "visible":{"user":"imagevisible","value":true}},
            {"id":2,"particle":"particles/test.json","origin":"32 32 0",
             "angles":"0 0 0","scale":"1 1 1",
             "visible":{"user":"particlesvisible","value":true}}
        ]})");
}

std::shared_ptr<Scene> ParseScene(fs::VFS& vfs, const std::string& overrides) {
    audio::SoundManager sound;
    WPSceneParser       parser;
    return parser.Parse("test", TestScene().dump(), vfs, sound, overrides);
}

SceneNode* FindObject(Scene& scene, i32 id) {
    for (const auto& node : scene.sceneGraph->GetChildren()) {
        if (node->ID() == id) return node.get();
    }
    return nullptr;
}

Eigen::Matrix4f ModelUniform(Scene& scene, SceneNode& node) {
    Eigen::Matrix4f matrix  = Eigen::Matrix4f::Zero();
    auto&           updater = *scene.shaderValueUpdater;
    sprite_map_t    sprites;
    updater.InitUniforms(&node, [](std::string_view name) {
        return name == G_M;
    });
    updater.UpdateUniforms(&node, sprites, [&](std::string_view name, const ShaderValue& value) {
        if (name == G_M && value.size() == 16)
            matrix = Eigen::Map<const Eigen::Matrix4f>(value.data());
    });
    return matrix;
}
} // namespace

TEST_CASE("scene user overrides change image uniforms and keep invisible images offscreen") {
    fs::VFS vfs;
    REQUIRE(vfs.Mount("/assets", TestFiles()));
    auto scene = ParseScene(vfs, R"({"backgroundcolor":"0 0 0","imagevisible":false})");
    REQUIRE(scene);
    auto image = scene->sceneGraph->GetChildren().end();
    for (auto it = scene->sceneGraph->GetChildren().begin();
         it != scene->sceneGraph->GetChildren().end();
         ++it) {
        if ((*it)->HasMaterial() && ! (*it)->Mesh()->Dynamic()) image = it;
    }
    REQUIRE(image != scene->sceneGraph->GetChildren().end());
    CHECK((*image)->IsOffscreen());
    const auto& uniforms = (*image)->Mesh()->Material()->customShader.constValues;
    REQUIRE(uniforms.count("g_Color4") == 1);
    CHECK(uniforms.at("g_Color4")[0] == 0.0f);
    CHECK(uniforms.at("g_Color4")[1] == 0.0f);
    CHECK(uniforms.at("g_Color4")[2] == 0.0f);
    CHECK(scene->paritileSys->subsystems.size() == 1);
}

TEST_CASE("scene user visibility removes particle systems") {
    fs::VFS vfs;
    REQUIRE(vfs.Mount("/assets", TestFiles()));
    auto visible = ParseScene(vfs, "{}");
    REQUIRE(visible);
    REQUIRE(visible->paritileSys->subsystems.size() == 1);
    auto hidden = ParseScene(vfs, R"({"particlesvisible":false})");
    REQUIRE(hidden);
    CHECK(hidden->paritileSys->subsystems.empty());
}

TEST_CASE("flat image shaders receive layer color and alpha") {
    fs::VFS           vfs;
    auto              files            = TestFiles();
    const std::string fragment         = "uniform float g_Alpha; uniform vec3 g_Color; "
                                         "void main() { gl_FragColor = vec4(g_Color, g_Alpha); }";
    files->files["/shaders/test.frag"] = { fragment.begin(), fragment.end() };
    REQUIRE(vfs.Mount("/assets", std::move(files)));
    audio::SoundManager sound;
    WPSceneParser       parser;
    for (const float alpha : { 1.0f, 0.35f, 0.0f }) {
        auto json                   = TestScene();
        json["objects"][0]["alpha"] = alpha;
        const std::string overrides = alpha == 1.0f ? "{}" : R"({"backgroundcolor":"0 0 0"})";
        auto              scene     = parser.Parse("flat", json.dump(), vfs, sound, overrides);
        REQUIRE(scene);
        bool found = false;
        for (const auto& node : scene->sceneGraph->GetChildren()) {
            if (! node->HasMaterial() || node->Mesh()->Dynamic()) continue;
            const auto& uniforms = node->Mesh()->Material()->customShader.constValues;
            REQUIRE(uniforms.count("g_Color") == 1);
            REQUIRE(uniforms.count("g_Alpha") == 1);
            const auto& color = uniforms.at("g_Color");
            CHECK(color[0] == doctest::Approx(alpha == 1.0f ? 0.7f : 0.0f));
            CHECK(color[1] == doctest::Approx(alpha == 1.0f ? 0.6f : 0.0f));
            CHECK(color[2] == doctest::Approx(alpha == 1.0f ? 0.5f : 0.0f));
            CHECK(uniforms.at("g_Alpha")[0] == doctest::Approx(alpha));
            found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("parsed scenes retain their own user property identity") {
    fs::VFS vfs;
    REQUIRE(vfs.Mount("/assets", TestFiles()));
    const std::string firstProps = R"({"backgroundcolor":"0 0 0"})";
    const std::string nextProps  = R"({"backgroundcolor":"1 0 0"})";
    auto              first      = ParseScene(vfs, firstProps);
    auto              next       = ParseScene(vfs, nextProps);
    auto              defaults   = ParseScene(vfs, "");
    REQUIRE(first);
    REQUIRE(next);
    REQUIRE(defaults);
    CHECK(first->user_props_json == firstProps);
    CHECK(next->user_props_json == nextProps);
    CHECK(defaults->user_props_json.empty());
    for (const auto& [scene, red] : { std::pair { first, 0.0f }, std::pair { next, 1.0f } }) {
        bool found = false;
        for (const auto& node : scene->sceneGraph->GetChildren()) {
            if (! node->HasMaterial() || node->Mesh()->Dynamic()) continue;
            const auto& values = node->Mesh()->Material()->customShader.constValues;
            CHECK(values.at("g_Color4")[0] == red);
            found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("loose project metadata supplies defaults for packed scenes and overrides win") {
    fs::VFS vfs;
    REQUIRE(vfs.Mount("/assets", TestFiles()));
    auto              project       = std::make_unique<ParserTestFs>();
    const std::string metadata      = R"({"general":{"properties":{
        "particlesvisible":{"value":false},"backgroundcolor":{"value":"0.2 0.4 0.6"}
    }}})";
    project->files["/project.json"] = { metadata.begin(), metadata.end() };
    REQUIRE(vfs.Mount("/project", std::move(project)));
    auto defaults = ParseScene(vfs, "");
    REQUIRE(defaults);
    CHECK(defaults->paritileSys->subsystems.empty());
    bool found = false;
    for (const auto& node : defaults->sceneGraph->GetChildren()) {
        if (node->HasMaterial() && ! node->Mesh()->Dynamic()) {
            const auto& color = node->Mesh()->Material()->customShader.constValues.at("g_Color4");
            CHECK(color[0] == doctest::Approx(0.2f));
            CHECK(color[1] == doctest::Approx(0.4f));
            CHECK(color[2] == doctest::Approx(0.6f));
            found = true;
        }
    }
    CHECK(found);
    auto override = ParseScene(vfs, R"({"particlesvisible":true})");
    REQUIRE(override);
    CHECK(override->paritileSys->subsystems.size() == 1);
}

TEST_CASE("invalid scene structure and asset JSON fail without poisoning later parses") {
    fs::VFS vfs;
    auto    files    = TestFiles();
    auto*   filesPtr = files.get();
    REQUIRE(vfs.Mount("/assets", std::move(files)));
    audio::SoundManager sound;
    WPSceneParser       parser;
    for (const auto& invalid : { std::string("malformed json"),
                                 std::string("{}"),
                                 std::string("{\"camera\":{},\"general\":{}}"),
                                 std::string("{\"camera\":{},\"general\":{},\"objects\":42}") }) {
        CHECK_FALSE(parser.Parse("bad", invalid, vfs, sound));
    }
    // Malformed material metadata is rejected along with malformed scene metadata.
    const auto        material               = filesPtr->files.at("/materials/image.json");
    const std::string invalidMaterial        = R"({"passes":42})";
    filesPtr->files["/materials/image.json"] = { invalidMaterial.begin(), invalidMaterial.end() };
    CHECK_FALSE(parser.Parse("bad-asset", TestScene().dump(), vfs, sound));
    filesPtr->files["/materials/image.json"] = material;
    REQUIRE(parser.Parse("valid", TestScene().dump(), vfs, sound));
}

TEST_CASE("flat scene parents resolve late objects, missing IDs, and duplicate IDs in cycles") {
    fs::VFS vfs;
    REQUIRE(vfs.Mount("/assets", TestFiles()));
    auto json       = TestScene();
    json["objects"] = nlohmann::json::parse(R"([
        {"id":1,"parent":7,"image":"models/image.json","origin":"1 0 0"},
        {"id":5,"sound":[],"volume":1,"playbackmode":"loop","origin":"15 0 0"},
        {"id":2,"parent":999,"image":"models/image.json","origin":"2 0 0"},
        {"id":3,"parent":5,"image":"models/image.json","origin":"3 0 0"},
        {"id":7,"parent":1,"origin":"100 0 0"},
        {"id":7,"parent":9,"origin":"5 0 0"},
        {"id":9,"parent":7,"origin":"10 0 0"}
    ])");
    audio::SoundManager sound;
    WPSceneParser       parser;
    auto                scene = parser.Parse("flat-parents", json.dump(), vfs, sound);
    REQUIRE(scene);
    for (const auto& [id, x] :
         std::array { std::pair { 1, 16.0 }, std::pair { 2, 2.0 }, std::pair { 3, 18.0 } }) {
        auto* node = FindObject(*scene, id);
        REQUIRE(node);
        node->UpdateTrans();
        CHECK(node->ModelTrans()(0, 3) == doctest::Approx(x));
    }
}

TEST_CASE("flat scene grouping preserves effect transforms, parallax, and particle draw order") {
    auto files   = TestFiles();
    auto addFile = [&](const std::string& path, std::string_view contents) {
        files->files[path] = { contents.begin(), contents.end() };
    };
    for (const auto* name : { "outer", "inner", "final", "particle", "spawned", "flat" }) {
        const std::string shader = name;
        addFile("/materials/" + shader + ".json", R"({"passes":[{"shader":")" + shader + R"("}]})");
        files->files["/shaders/" + shader + ".vert"] = files->files.at("/shaders/test.vert");
        files->files["/shaders/" + shader + ".frag"] = files->files.at("/shaders/test.frag");
    }
    addFile("/models/flat.json", R"({"material":"materials/flat.json","width":64,"height":64})");
    addFile("/effects/outer.json",
            R"({"name":"outer","passes":[{"material":"materials/outer.json"}]})");
    addFile("/effects/inner.json", R"({"name":"inner","passes":[
        {"material":"materials/inner.json"},{"material":"materials/final.json"}]})");
    addFile("/particles/parent.json", R"({"material":"materials/particle.json","maxcount":2,
        "emitter":[{"name":"boxrandom","id":0}],"children":[
            {"name":"particles/spawned.json","type":"static","maxcount":1,"origin":"1 2 0"}]})");
    addFile("/particles/spawned.json", R"({"material":"materials/spawned.json","maxcount":2,
        "emitter":[{"name":"boxrandom","id":0}]})");
    fs::VFS vfs;
    REQUIRE(vfs.Mount("/assets", std::move(files)));

    auto json                               = TestScene();
    json["general"]["cameraparallax"]       = true;
    json["general"]["cameraparallaxamount"] = 0.5;
    json["objects"]                         = nlohmann::json::parse(R"([
        {"id":1,"parent":7,"image":"models/image.json","origin":"13 17 2","angles":"0 0 0.7",
         "effects":[{"file":"effects/outer.json"}]},
        {"id":2,"parent":1,"particle":"particles/parent.json","origin":"5 6 0"},
        {"id":3,"image":"models/flat.json","origin":"30 10 0","parallaxDepth":"0.5 0.5"},
        {"id":4,"parent":1,"image":"models/image.json","origin":"3 7 0","angles":"0 0 0.4",
         "parallaxDepth":"0.5 0.5","effects":[{"file":"effects/inner.json"}]},
        {"id":7,"origin":"43 52 4","scale":"-2 3 0"}
    ])");
    audio::SoundManager sound;
    WPSceneParser       parser;
    auto                scene = parser.Parse("grouped-effects", json.dump(), vfs, sound);
    REQUIRE(scene);
    auto graph = sceneToRenderGraph(*scene);
    REQUIRE(graph);

    std::vector<std::string_view> composition;
    for (const auto pass : graph->topologicalOrder()) {
        const auto name = graph->getPassNode(pass)->name();
        if (name != "test" && name != "inner") composition.push_back(name);
    }
    const std::vector<std::string_view> expectedOrder {
        "outer", "particle", "spawned", "flat", "final"
    };
    CHECK(composition == expectedOrder);

    using namespace Eigen;
    const Matrix4d group = (Translation3d(43, 52, 4) * Scaling(-2.0, 3.0, 0.0)).matrix();
    const Matrix4d parent =
        (Translation3d(13, 17, 2) * AngleAxisd(0.7f, Vector3d::UnitZ())).matrix();
    const Matrix4d child = (Translation3d(3, 7, 0) * AngleAxisd(0.4f, Vector3d::UnitZ())).matrix();
    const Matrix4d world = group * parent * child;
    auto*          image = FindObject(*scene, 4);
    REQUIRE(image);
    CHECK(ModelUniform(*scene, *image).isIdentity());
    auto& layer = scene->cameras.at(image->Camera())->GetImgEffect();
    REQUIRE(layer);
    REQUIRE(layer->EffectCount() == 1);
    auto& passes = layer->GetEffect(0)->nodes;
    REQUIRE(passes.size() == 2);
    CHECK(ModelUniform(*scene, *passes.front().sceneNode).isIdentity());
    CHECK(passes.back().output == SpecTex_Default);
    Matrix4d parallax = world;
    parallax.block<2, 1>(0, 3) += (world.block<2, 1>(0, 3) - Vector2d(32, 32)) * 0.25;
    CHECK(ModelUniform(*scene, *passes.back().sceneNode).isApprox(parallax.cast<float>()));

    auto* flat = FindObject(*scene, 3);
    REQUIRE(flat);
    CHECK(ModelUniform(*scene, *flat)(0, 3) == doctest::Approx(29.5));
    CHECK(ModelUniform(*scene, *flat)(1, 3) == doctest::Approx(4.5));
    auto* particle = FindObject(*scene, 2);
    REQUIRE(particle);
    REQUIRE(particle->GetChildren().size() == 1);
    auto& spawned = *particle->GetChildren().front();
    spawned.UpdateTrans();
    const Matrix4d spawnWorld = group * parent * Affine3d(Translation3d(6, 8, 0)).matrix();
    CHECK(spawned.ModelTrans().isApprox(spawnWorld));

    json["objects"][3]["visible"] = false;
    auto hidden                   = parser.Parse("offscreen-effects", json.dump(), vfs, sound);
    REQUIRE(hidden);
    auto* offscreen = FindObject(*hidden, 4);
    REQUIRE(offscreen);
    auto& offscreenLayer = hidden->cameras.at(offscreen->Camera())->GetImgEffect();
    REQUIRE(offscreenLayer);
    REQUIRE(offscreenLayer->EffectCount() == 1);
    auto& offscreenPasses = offscreenLayer->GetEffect(0)->nodes;
    REQUIRE(offscreenPasses.size() == 2);
    for (int build = 0; build < 2; ++build) {
        REQUIRE(sceneToRenderGraph(*hidden));
        CHECK(offscreenPasses.front().sceneNode->Camera() == "effect");
        CHECK(offscreenPasses.back().output == GenOffscreenRT(4));
        CHECK(offscreenPasses.back().sceneNode->Camera().empty());
        CHECK(ModelUniform(*hidden, *offscreenPasses.back().sceneNode)
                  .isApprox(parallax.cast<float>()));
    }
}

TEST_CASE("flat scene attachment passes animation on to existing particle children") {
    using namespace Eigen;
    SceneNode image;
    image.SetAnimatedScale(1.0f);
    SceneNode particle(Vector3f(2, 0, 0), Vector3f::Ones(), Vector3f::Zero());
    auto      spawned =
        std::make_shared<SceneNode>(Vector3f(1, 0, 0), Vector3f::Ones(), Vector3f::Zero());
    particle.AppendChild(spawned);
    particle.SetTransformParent(&image);

    spawned->UpdateTrans();
    CHECK(spawned->ModelTrans()(0, 3) == doctest::Approx(3.0));
    image.SetAnimatedScale(2.0f);
    spawned->UpdateTrans();
    CHECK(spawned->ModelTrans()(0, 3) == doctest::Approx(6.0));
}
