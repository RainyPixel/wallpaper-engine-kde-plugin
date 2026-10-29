#include <doctest.h>

#include "Audio/SoundManager.h"
#include "Fs/MemBinaryStream.h"
#include "Fs/VFS.h"
#include "Scene/Scene.h"
#include "SceneLoader.hpp"
#include "WPPkgFs.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace wallpaper;

namespace
{
struct SceneFiles {
    std::filesystem::path path;
    SceneFiles() {
        char  pattern[] = "/tmp/wekde-scene-load-XXXXXX";
        auto* dir       = mkdtemp(pattern);
        if (! dir) throw std::runtime_error("cannot create scene fixture directory");
        path = dir;
    }
    ~SceneFiles() { std::filesystem::remove_all(path); }
    void Write(const std::string& name, const std::string& content) {
        std::ofstream out(path / name, std::ios::binary);
        out << content;
    }
    auto Load(audio::SoundManager& sound) {
        return LoadScene((path / "scene.json").string(), path.string(), "", sound, "");
    }
};

const std::string validScene = R"({
    "camera":{"center":"32 32 0","eye":"32 32 1","up":"0 1 0"},
    "general":{"orthogonalprojection":{"width":64,"height":64}},"objects":[]})";

class TrackedStream : public audio::SoundStream {
public:
    explicit TrackedStream(int& destroyed): m_destroyed(destroyed) {}
    ~TrackedStream() override { ++m_destroyed; }
    uint64_t NextPcmData(void*, uint32_t) override { return 0; }
    void     PassDesc(const Desc&) override {}

private:
    int& m_destroyed;
};
} // namespace

TEST_CASE("loader preserves scene files and audio on failures then accepts a valid scene") {
    SceneFiles          files;
    int                 destroyed = 0;
    audio::SoundManager sound;
    sound.SetMuted(true);
    sound.SetVolume(0.3f);
    files.Write("scene.json", validScene);
    auto current = files.Load(sound);
    REQUIRE(current);
    sound.MountStream(std::make_unique<TrackedStream>(destroyed));
    for (const auto& invalid : { std::string("malformed json"),
                                 std::string("{}"),
                                 std::string("{\"camera\":{},\"general\":{}}"),
                                 std::string("{\"camera\":{},\"general\":{},\"objects\":42}") }) {
        files.Write("scene.json", invalid);
        CHECK_FALSE(files.Load(sound));
        CHECK(destroyed == 0);
        CHECK(sound.Muted());
        CHECK(sound.Volume() == doctest::Approx(0.3f));
        REQUIRE(current->vfs);
        CHECK(current->vfs->Contains("/assets/scene.json"));
    }
    files.Write("scene.json", validScene);
    auto replacement = files.Load(sound);
    REQUIRE(replacement);
    CHECK(destroyed == 1);
    CHECK(sound.Muted());
    CHECK(sound.Volume() == doctest::Approx(0.3f));
    CHECK(replacement->vfs->Contains("/assets/scene.json"));
    sound.UnMountAll();
}

TEST_CASE("package loader rejects negative and truncated lengths and invalid entries") {
    SceneFiles files;
    const auto path = (files.path / "scene.pkg").string();
    for (const auto& bytes :
         { std::string("\xff\xff\xff\xff", 4),
           std::string("\x10\0\0\0x", 5),
           std::string("\0\0\0\0\xff\xff\xff\xff", 8),
           std::string("\0\0\0\0\x01\0\0\0\0\0\0\0\xff\xff\xff\xff\x01\0\0\0", 20),
           std::string("\0\0\0\0\x01\0\0\0\0\0\0\0\x01\0\0\0\x01\0\0\0", 20) }) {
        files.Write("scene.pkg", bytes);
        CHECK_FALSE(fs::WPPkgFs::CreatePkgFs(path));
    }
    files.Write("scene.json", validScene);
    audio::SoundManager sound;
    auto                scene = files.Load(sound);
    REQUIRE(scene);
    CHECK(scene->vfs->Contains("/assets/scene.json"));
}

TEST_CASE("audio decoding rejects missing and corrupt source streams") {
    const audio::SoundStream::Desc desc { .channels = 2, .sampleRate = 48000 };
    CHECK_FALSE(audio::CreateSoundStream(nullptr, desc));
    auto invalid = std::make_shared<fs::MemBinaryStream>(std::vector<uint8_t> { 1, 2, 3 });
    CHECK_FALSE(audio::CreateSoundStream(invalid, desc));
}
