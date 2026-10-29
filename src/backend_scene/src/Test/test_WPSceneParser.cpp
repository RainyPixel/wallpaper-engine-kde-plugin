#include <doctest.h>

#include "Audio/SoundManager.h"
#include "ParserTestFs.hpp"
#include "Particle/ParticleSystem.h"
#include "WPSceneParser.hpp"
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
