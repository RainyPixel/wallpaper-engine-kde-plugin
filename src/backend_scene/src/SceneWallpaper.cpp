#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"

#include "Utils/Logging.h"
#include "Looper/Looper.hpp"

#include "Timer/FrameTimer.hpp"
#include "Utils/FpsCounter.h"
#include "WPSceneParser.hpp"
#include "Scene/Scene.h"
#include "Particle/ParticleSystem.h"
#include "Interface/IShaderValueUpdater.h"
#include "WPShaderValueUpdater.hpp"

#include "Fs/VFS.h"
#include "Fs/PhysicalFs.h"
#include "WPPkgFs.hpp"

#include "Audio/SoundManager.h"

#include "RenderGraph/RenderGraph.hpp"

#include "VulkanRender/SceneToRenderGraph.hpp"
#include "VulkanRender/VulkanRender.hpp"
#include <atomic>
#include <sstream>

using namespace wallpaper;

#define CASE_CMD(cmd) \
    case CMD::CMD_##cmd: handle_##cmd(msg); break;
#define MHANDLER_CMD(cmd) void handle_##cmd(const std::shared_ptr<looper::Message>& msg)
#define MHANDLER_CMD_IMPL(cl, cmd) \
    void impl_##cl::handle_##cmd(const std::shared_ptr<looper::Message>& msg)
#define CALL_MHANDLER_CMD(cmd, msg) handle_##cmd(msg)

namespace
{
// The WP_MIRROR env var overrides the per-wallpaper setting (0 forces off).
bool MirrorEnabled(bool requested) {
    if (const char* e = std::getenv("WP_MIRROR")) return e[0] != '0' && e[0] != '\0';
    return requested;
}

std::string UuidHex(std::span<const std::uint8_t> uuid) {
    static const char* k = "0123456789abcdef";
    std::string        s;
    s.reserve(uuid.size() * 2);
    for (auto b : uuid) {
        s.push_back(k[b >> 4]);
        s.push_back(k[b & 0xf]);
    }
    return s;
}

template<typename T>
void AddMsgCmd(looper::Message& msg, T cmd) {
    msg.setInt32("cmd", (int32_t)cmd);
}
template<typename T>
std::shared_ptr<looper::Message> CreateMsgWithCmd(const std::shared_ptr<looper::Handler>& handler,
                                                  T                                       cmd) {
    auto msg = looper::Message::create(0, handler);
    AddMsgCmd(*msg, cmd);
    return msg;
}
} // namespace

namespace wallpaper
{
class RenderHandler;

class MainHandler : public looper::Handler {
public:
    enum class CMD
    {
        CMD_LOAD_SCENE,
        CMD_SET_PROPERTY,
        CMD_STOP,
        CMD_FIRST_FRAME,
        CMD_NO
    };

public:
    MainHandler();
    virtual ~MainHandler();

    // Quiesce the render thread: stop the frame timer, stop emitting redraws, and
    // join both loops so no DRAW runs while the scene/device are torn down. Safe to
    // call more than once. Called from the texture node teardown because at
    // plasmashell exit the SceneObject (and thus this handler) is never destroyed.
    void stopRender();

    bool init();
    auto renderHandler() const { return m_render_handler; }
    bool inited() const { return m_inited; }

public:
    void onMessageReceived(const std::shared_ptr<looper::Message>& msg) override {
        int32_t cmd_int = (int32_t)CMD::CMD_NO;
        if (msg->findInt32("cmd", &cmd_int)) {
            CMD cmd = static_cast<CMD>(cmd_int);
            switch (cmd) {
                CASE_CMD(SET_PROPERTY);
                CASE_CMD(LOAD_SCENE);
                CASE_CMD(STOP);
                CASE_CMD(FIRST_FRAME);
            default: break;
            }
        }
    }

    void               sendCmdLoadScene();
    void               sendFirstFrameOk();
    bool               isGenGraphviz() const { return m_gen_graphviz; }
    bool               cachePasses() const { return m_cache_passes; }
    const std::string& userProps() const { return m_user_props_json; }

private:
    void loadScene();

    MHANDLER_CMD(LOAD_SCENE);
    MHANDLER_CMD(SET_PROPERTY);
    MHANDLER_CMD(STOP);
    MHANDLER_CMD(FIRST_FRAME);

private:
    bool m_inited { false };

    std::string m_assets;
    std::string m_source;
    std::string m_cache_path;
    bool        m_gen_graphviz { false };
    bool        m_cache_passes { true };

    WPSceneParser                        m_scene_parser;
    std::unique_ptr<audio::SoundManager> m_sound_manager;
    FirstFrameCallback                   m_first_frame_callback;
    std::string                          m_user_props_json;

private:
    std::shared_ptr<looper::Looper> m_main_loop;
    std::shared_ptr<looper::Looper> m_render_loop;
    std::shared_ptr<RenderHandler>  m_render_handler;
};
// for macro
using impl_MainHandler = MainHandler;

class RenderHandler : public looper::Handler {
public:
    enum class CMD
    {
        CMD_INIT_VULKAN,
        CMD_SET_SCENE,
        CMD_SET_FILLMODE,
        CMD_SET_SPEED,
        CMD_STOP,
        CMD_DRAW,
        CMD_NO
    };
    MainHandler& main_handler;
    RenderHandler(MainHandler& m)
        : main_handler(m), m_render(std::make_unique<vulkan::VulkanRender>()) {}
    virtual ~RenderHandler() {
        frame_timer.Stop();
        m_render->destroy();
        LOG_INFO("render handler deleted");
    }

    void onMessageReceived(const std::shared_ptr<looper::Message>& msg) override {
        int32_t cmd_int = (int32_t)CMD::CMD_NO;
        if (msg->findInt32("cmd", &cmd_int)) {
            CMD cmd = static_cast<CMD>(cmd_int);
            switch (cmd) {
                CASE_CMD(DRAW);
                CASE_CMD(STOP);
                CASE_CMD(SET_FILLMODE);
                CASE_CMD(SET_SCENE);
                CASE_CMD(SET_SPEED);
                CASE_CMD(INIT_VULKAN);
            default: break;
            }
        }
    }

    ExSwapchain*                 exSwapchain() const { return m_render->exSwapchain(); }
    std::shared_ptr<ExSwapchain> currentSwapchain() const { return m_render->currentSwapchain(); }

    void clearRedrawCallback() { m_render->clearRedrawCallback(); }

    void stopRendering() { frame_timer.Stop(); }

    void releaseMirrorGroup() { m_render->releaseMirror(); }

    bool renderInited() const { return m_render->inited(); }

    void setMousePos(double x, double y) { m_mouse_pos.store(std::array { (float)x, (float)y }); }

private:
    std::string buildMirrorKey() const {
        if (! m_scene) return {};
        std::ostringstream os;
        os << m_uuid_hex << '|' << m_scene->scene_id << '|' << m_width << 'x' << m_height << '|'
           << (int)m_fillmode << '|' << m_speed << '|' << main_handler.userProps();
        return os.str();
    }

    // After the graph is compiled, decide whether this screen renders or mirrors,
    // and remember the result for the draw loop.
    void decideFrameSource() {
        auto* wpUpdater = static_cast<WPShaderValueUpdater*>(m_scene->shaderValueUpdater.get());
        bool  mouse_dep = wpUpdater->MouseDependent();
        std::string key =
            (MirrorEnabled(m_mirror_setting) && ! mouse_dep) ? buildMirrorKey() : std::string {};
        m_mirror = ! m_render->beginFrameSource(key);
        LOG_INFO("scene '%s' mouse_dependent=%d mirror=%d",
                 m_scene->scene_id.c_str(),
                 (int)mouse_dep,
                 (int)m_mirror);
    }

    void rebuildAndDecide() {
        if (! m_scene) return;
        m_rg = sceneToRenderGraph(*m_scene);
        m_render->compileRenderGraph(*m_scene, *m_rg);
        m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
        decideFrameSource();
    }

    // A live change to a mirror-key input (fillmode/speed) must re-run the election:
    // a mirroring secondary now differs from its primary and should render its own,
    // and a primary's group must re-form under the new key.
    void onMirrorKeyChanged() {
        if (! (m_scene && renderInited() && MirrorEnabled(m_mirror_setting))) return;
        m_render->releaseMirror();
        rebuildAndDecide();
    }

    MHANDLER_CMD(STOP) {
        bool stop { false };
        if (msg->findBool("value", &stop)) {
            if (stop)
                frame_timer.Stop();
            else
                frame_timer.Run();
        }
    }
    MHANDLER_CMD(DRAW) {
        frame_timer.FrameBegin();
        if (m_mirror) {
            if (m_render->mirrorLost()) {
                // Primary went away: rebuild our graph and re-run the election;
                // the first surviving screen for this group becomes the new primary.
                LOG_INFO("mirror: primary gone, screen re-electing");
                m_render->releaseMirror();
                rebuildAndDecide();
            } else {
                m_render->drawFrame(*m_scene);
            }
            frame_timer.FrameEnd();
            return;
        }
        if (m_rg) {
            // LOG_INFO("frame info, fps: %.1f, frametime: %.1f", 1.0f, 1000.0f*m_scene->frameTime);
            m_scene->shaderValueUpdater->FrameBegin();
            {
                auto pos = m_mouse_pos.load();
                m_scene->shaderValueUpdater->MouseInput(pos[0], pos[1]);

                // Update particle control points that follow the mouse
                auto* wpUpdater =
                    static_cast<WPShaderValueUpdater*>(m_scene->shaderValueUpdater.get());
                auto mousePos = wpUpdater->GetMousePosition();
                m_scene->paritileSys->UpdateMouseControlPoints(
                    mousePos, { m_scene->ortho[0], m_scene->ortho[1] });
            }
            m_scene->paritileSys->Emitt();

            m_render->drawFrame(*m_scene);

            m_scene->PassFrameTime(frame_timer.IdeaTime() * m_speed);

            m_scene->shaderValueUpdater->FrameEnd();
            // fps_counter.RegisterFrame();

            if (! m_scene->first_frame_ok) {
                m_scene->first_frame_ok = true;
                main_handler.sendFirstFrameOk();
            }
        }
        frame_timer.FrameEnd();
    }
    MHANDLER_CMD(SET_FILLMODE) {
        int32_t value;
        if (msg->findInt32("value", &value) && (FillMode)value != m_fillmode) {
            m_fillmode = (FillMode)value;
            if (m_scene && renderInited()) {
                if (MirrorEnabled(m_mirror_setting))
                    onMirrorKeyChanged();
                else
                    m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
            }
        }
    }
    MHANDLER_CMD(SET_SCENE) {
        if (msg->findObject("scene", &m_scene)) {
            m_scene->cache_passes = main_handler.cachePasses();
            m_render->releaseMirror();
            if (m_rg) m_render->clearLastRenderGraph();
            m_rg = sceneToRenderGraph(*m_scene);

            if (main_handler.isGenGraphviz()) m_rg->ToGraphviz("graph.dot");
            m_render->compileRenderGraph(*m_scene, *m_rg);
            m_render->UpdateCameraFillMode(*m_scene, m_fillmode);

            decideFrameSource();
        }
    }
    MHANDLER_CMD(SET_SPEED) {
        float v { 1.0f };
        if (msg->findFloat("value", &v) && v != m_speed) {
            m_speed = v;
            onMirrorKeyChanged();
        }
    }
    MHANDLER_CMD(INIT_VULKAN) {
        std::shared_ptr<RenderInitInfo> info;
        if (msg->findObject("info", &info)) {
            m_width          = info->width;
            m_height         = info->height;
            m_uuid_hex       = UuidHex(info->uuid);
            m_mirror_setting = info->mirror_scene;
            m_render->init(*info);

            // inited, callback to laod scene
            main_handler.sendCmdLoadScene();
        }
    }

public:
    FrameTimer frame_timer;
    FpsCounter fps_counter;

private:
    std::shared_ptr<Scene> m_scene { nullptr };
    float                  m_speed { 1.0f };

    std::unique_ptr<vulkan::VulkanRender> m_render;
    std::unique_ptr<rg::RenderGraph>      m_rg { nullptr };

    FillMode m_fillmode { FillMode::ASPECTCROP };

    bool        m_mirror { false };
    bool        m_mirror_setting { false };
    std::string m_uuid_hex;
    uint16_t    m_width { 0 };
    uint16_t    m_height { 0 };

    std::atomic<std::array<float, 2>> m_mouse_pos { std::array { 0.5f, 0.5f } };
};

void MainHandler::stopRender() {
    if (m_render_handler) {
        m_render_handler->stopRendering();
        m_render_handler->clearRedrawCallback();
    }
    if (m_render_loop) m_render_loop->stop();
    if (m_main_loop) m_main_loop->stop();
    // After the loops are joined (no render thread left), drop any mirror-group
    // membership so a primary leaving lets the remaining screens re-elect.
    if (m_render_handler) m_render_handler->releaseMirrorGroup();
}

MainHandler::~MainHandler() { stopRender(); }
} // namespace wallpaper

SceneWallpaper::SceneWallpaper(): m_main_handler(std::make_shared<MainHandler>()) {}

SceneWallpaper::~SceneWallpaper() {
    /*
    if(m_offscreen) {
        // no wait
        auto msg = looper::Message::create(0, m_main_handler);
        msg->setObject("self_clean", m_main_handler);
        msg->setCleanAfterDeliver(true);
        m_main_handler = nullptr;
        msg->post();
    }
    */
}

bool SceneWallpaper::inited() const { return m_main_handler->inited(); }

bool SceneWallpaper::init() { return m_main_handler->init(); }

void SceneWallpaper::initVulkan(const RenderInitInfo& info) {
    m_offscreen                             = info.offscreen;
    std::shared_ptr<RenderInitInfo> sp_info = std::make_shared<RenderInitInfo>(info);
    auto                            msg =
        CreateMsgWithCmd(m_main_handler->renderHandler(), RenderHandler::CMD::CMD_INIT_VULKAN);
    msg->setObject("info", sp_info);
    msg->post();
}

void SceneWallpaper::play() {
    auto msg = CreateMsgWithCmd(m_main_handler, MainHandler::CMD::CMD_STOP);
    msg->setBool("value", false);
    msg->post();
}
void SceneWallpaper::pause() {
    auto msg = CreateMsgWithCmd(m_main_handler, MainHandler::CMD::CMD_STOP);
    msg->setBool("value", true);
    msg->post();
}

void SceneWallpaper::mouseInput(double x, double y) {
    m_main_handler->renderHandler()->setMousePos(x, y);
}

#define BASIC_TYPE(NAME, TYPENAME)                                                       \
    void SceneWallpaper::setProperty##NAME(std::string_view name, TYPENAME value) {      \
        auto msg = CreateMsgWithCmd(m_main_handler, MainHandler::CMD::CMD_SET_PROPERTY); \
        msg->setString("property", std::string(name));                                   \
        msg->set##NAME("value", value);                                                  \
        msg->post();                                                                     \
    }

BASIC_TYPE(Bool, bool);
BASIC_TYPE(Int32, int32_t);
BASIC_TYPE(Float, float);
BASIC_TYPE(String, std::string);
BASIC_TYPE(Object, std::shared_ptr<void>);

ExSwapchain* SceneWallpaper::exSwapchain() const {
    return m_main_handler->renderHandler()->exSwapchain();
}

std::shared_ptr<ExSwapchain> SceneWallpaper::currentSwapchain() const {
    return m_main_handler->renderHandler()->currentSwapchain();
}

void SceneWallpaper::clearRedrawCallback() {
    m_main_handler->renderHandler()->clearRedrawCallback();
}

void SceneWallpaper::stopRender() { m_main_handler->stopRender(); }

MHANDLER_CMD_IMPL(MainHandler, LOAD_SCENE) {
    if (m_render_handler->renderInited()) {
        loadScene();
    }
}

MHANDLER_CMD_IMPL(MainHandler, SET_PROPERTY) {
    std::string property;
    if (msg->findString("property", &property)) {
        if (property == PROPERTY_SOURCE) {
            msg->findString("value", &m_source);
            LOG_INFO("source: %s", m_source.c_str());
            // Reset user properties when source changes (new wallpaper)
            m_user_props_json.clear();
            CALL_MHANDLER_CMD(LOAD_SCENE, msg);
        } else if (property == PROPERTY_ASSETS) {
            msg->findString("value", &m_assets);
            CALL_MHANDLER_CMD(LOAD_SCENE, msg);
        } else if (property == PROPERTY_FPS) {
            int32_t fps { 15 };
            msg->findInt32("value", &fps);
            if (fps >= 5) {
                m_render_handler->frame_timer.SetRequiredFps((uint8_t)fps);
            }
        } else if (property == PROPERTY_FILLMODE) {
            int32_t value;
            if (msg->findInt32("value", &value)) {
                auto nmsg =
                    CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_SET_FILLMODE);
                nmsg->setInt32("value", value);
                nmsg->post();
            }
        } else if (property == PROPERTY_GRAPHIVZ) {
            msg->findBool("value", &m_gen_graphviz);
        } else if (property == PROPERTY_MUTED) {
            bool muted { false };
            msg->findBool("value", &muted);
            m_sound_manager->SetMuted(muted);
        } else if (property == PROPERTY_VOLUME) {
            float volume { 1.0f };
            msg->findFloat("value", &volume);
            m_sound_manager->SetVolume(volume);
        } else if (property == PROPERTY_CACHE_PATH) {
            std::string path;
            msg->findString("value", &path);
            m_cache_path = path;
        } else if (property == PROPERTY_FIRST_FRAME_CALLBACK) {
            std::shared_ptr<FirstFrameCallback> cb;
            msg->findObject("value", &cb);
            m_first_frame_callback = *cb;
        } else if (property == PROPERTY_SPEED) {
            float speed { 1.0f };
            if (msg->findFloat("value", &speed)) {
                auto nmsg = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_SET_SPEED);
                nmsg->setFloat("value", speed);
                nmsg->post();
            }
        } else if (property == PROPERTY_USER_PROPS) {
            std::string json;
            msg->findString("value", &json);
            if (m_user_props_json != json) {
                m_user_props_json = json;
                // Reload scene to apply new user properties (only if we have actual properties)
                // Skip reload if json is empty - this means wallpaper is changing
                if (! json.empty() && ! m_source.empty() && ! m_assets.empty()) {
                    LOG_INFO("Reloading scene to apply user properties: %s", json.c_str());
                    CALL_MHANDLER_CMD(LOAD_SCENE, msg);
                }
            }
        } else if (property == PROPERTY_CACHE_PASSES) {
            bool value { true };
            msg->findBool("value", &value);
            if (m_cache_passes != value) {
                m_cache_passes = value;
                // rebuild the render graph so the new setting takes effect live
                if (! m_source.empty() && ! m_assets.empty()) {
                    CALL_MHANDLER_CMD(LOAD_SCENE, msg);
                }
            }
        }
    }
}

MHANDLER_CMD_IMPL(MainHandler, STOP) {
    bool stop { false };
    if (msg->findBool("value", &stop)) {
        if (stop) {
            m_sound_manager->Pause();
        } else {
            m_sound_manager->Play();
        }

        auto msg_r = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_STOP);
        msg_r->setBool("value", stop);
        msg_r->post();
    }
}

MHANDLER_CMD_IMPL(MainHandler, FIRST_FRAME) {
    if (m_first_frame_callback) m_first_frame_callback();
}

void MainHandler::loadScene() {
    if (m_source.empty() || m_assets.empty()) return;

    LOG_INFO("loading scene: %s", m_source.c_str());

    if (! m_sound_manager->IsInited()) {
        m_sound_manager->Init();
        m_sound_manager->Play();
    } else {
        m_sound_manager->UnMountAll();
    }

    std::shared_ptr<Scene> scene { nullptr };

    // mount assets dir
    std::unique_ptr<fs::VFS> pVfs = std::make_unique<fs::VFS>();
    auto&                    vfs  = *pVfs;
    if (! vfs.IsMounted("assets")) {
        bool sus = vfs.Mount("/assets", fs::CreatePhysicalFs(m_assets), "assets");
        if (! sus) {
            LOG_ERROR("Mount assets dir failed");
            return;
        }
    }
    std::filesystem::path pkgPath_fs { m_source };
    pkgPath_fs.replace_extension("pkg");
    std::string pkgPath  = pkgPath_fs.native();
    std::string pkgEntry = pkgPath_fs.filename().replace_extension("json").native();
    std::string pkgDir   = pkgPath_fs.parent_path().native();
    std::string scene_id = pkgPath_fs.parent_path().filename().native();

    // load pkgfile
    if (! vfs.Mount("/assets", fs::WPPkgFs::CreatePkgFs(pkgPath))) {
        LOG_INFO("load pkg file %s failed, fallback to use dir", pkgPath.c_str());
        // load pkg dir
        if (! vfs.Mount("/assets", fs::CreatePhysicalFs(pkgDir))) {
            LOG_ERROR("can't load pkg directory: %s", pkgDir.c_str());
            return;
        }
    }
    if (! m_cache_path.empty()) {
        if (! vfs.Mount("/cache", fs::CreatePhysicalFs(m_cache_path, true), "cache")) {
            LOG_ERROR("can't load cache folder: %s", m_cache_path.c_str());
        } else {
            LOG_INFO("cache folder: %s", m_cache_path.c_str());
        }
    }

    {
        std::string       scene_src;
        const std::string base { "/assets/" };
        {
            std::string scenePath = base + pkgEntry;
            if (vfs.Contains(scenePath)) {
                auto f = vfs.Open(scenePath);
                if (f) scene_src = f->ReadAllStr();
            }
        }
        if (scene_src.empty()) {
            LOG_ERROR("Not supported scene type");
            return;
        }
        scene = m_scene_parser.Parse(scene_id, scene_src, vfs, *m_sound_manager, m_user_props_json);
        scene->vfs.swap(pVfs);
    }

    {
        auto msg = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_SET_SCENE);
        msg->setObject("scene", scene);
        msg->post();
    }

    // draw first frame
    {
        auto msg = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_DRAW);
        msg->post();
    }
}
void MainHandler::sendCmdLoadScene() {
    auto msg = CreateMsgWithCmd(shared_from_this(), MainHandler::CMD::CMD_LOAD_SCENE);
    msg->post();
}
void MainHandler::sendFirstFrameOk() {
    auto msg = CreateMsgWithCmd(shared_from_this(), MainHandler::CMD::CMD_FIRST_FRAME);
    msg->post();
}

bool MainHandler::init() {
    if (m_inited) return true;
    m_main_loop->setName("main");
    m_render_loop->setName("render");

    m_main_loop->start();
    m_render_loop->start();

    m_main_loop->registerHandler(shared_from_this());
    m_render_loop->registerHandler(m_render_handler);

    {
        auto  msg        = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_DRAW);
        auto& frameTimer = m_render_handler->frame_timer;
        frameTimer.SetCallback([msg]() {
            msg->post();
        });
        frameTimer.SetRequiredFps(15);
        frameTimer.Run();
    }

    m_inited = true;
    return true;
}
MainHandler::MainHandler()
    : m_sound_manager(std::make_unique<audio::SoundManager>()),
      m_main_loop(std::make_shared<looper::Looper>()),
      m_render_loop(std::make_shared<looper::Looper>()),
      m_render_handler(std::make_shared<RenderHandler>(*this)) {}
