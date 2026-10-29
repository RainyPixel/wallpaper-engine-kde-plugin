#include <iostream>
#include <set>
#include <fstream>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include "arg.hpp"
#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"

#include "Utils/Platform.hpp"

using namespace std;

atomic<bool> renderCall(false);

struct UserData {
    wallpaper::SceneWallpaper* psw { nullptr };

    uint16_t width;
    uint16_t height;
};

extern "C" {
void framebuffer_size_callback(GLFWwindow*, int width, int height) {}

void mouse_button_callback(GLFWwindow* win, int button, int action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
        // data->psw->setPropertyString(wallpaper::PROPERTY_SOURCE,
    }
}

void cursor_position_callback(GLFWwindow* win, double xpos, double ypos) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    data->psw->mouseInput(xpos / data->width, ypos / data->height);
}
}

void updateCallback() {
    renderCall = true;
    glfwPostEmptyEvent();
}

int main(int argc, char** argv) {
    argparse::ArgumentParser program("scene-viewer");
    setAndParseArg(program, argc, argv);
    auto [w_width, w_height] = program.get<Resolution>(OPT_RESOLUTION);

    // WP_VIEWER_X11=1 forces XWayland so X11 automation (xdotool) can drive the window
    if (getenv("WP_VIEWER_X11") != nullptr) glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(w_width, w_height, "WP", nullptr, nullptr);

    UserData data;
    data.width  = w_width;
    data.height = w_height;

    const std::string shot_path    = program.get<std::string>(OPT_SCREENSHOT);
    const double      shot_delay   = program.get<double>(OPT_SHOT_DELAY);
    const double      shot_timeout = program.get<double>(OPT_SHOT_TIMEOUT);

    wallpaper::RenderInitInfo info;
    info.enable_valid_layer = program.get<bool>(OPT_VALID_LAYER);
    // Only a screenshot run asks for readable swapchain images.
    info.enable_readback = ! shot_path.empty();
    info.width           = w_width;
    info.height          = w_height;

    auto& sf_info = info.surface_info;
    {
        uint32_t glfwExtCount = 0;
        auto     exts         = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        for (int i = 0; i < glfwExtCount; i++) {
            sf_info.instanceExts.emplace_back(exts[i]);
        }

        sf_info.createSurfaceOp = [window](VkInstance inst, VkSurfaceKHR* surface) {
            return glfwCreateWindowSurface(inst, window, NULL, surface);
        };
    }

    if (window == nullptr) {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    auto* psw = new wallpaper::SceneWallpaper();
    data.psw  = psw;

    psw->init();
    psw->initVulkan(info);
    psw->setPropertyString(wallpaper::PROPERTY_ASSETS, program.get<std::string>(ARG_ASSETS));
    psw->setPropertyString(wallpaper::PROPERTY_SOURCE, program.get<std::string>(ARG_SCENE));
    psw->setPropertyBool(wallpaper::PROPERTY_GRAPHIVZ, program.get<bool>(OPT_GRAPHVIZ));
    psw->setPropertyInt32(wallpaper::PROPERTY_FPS, program.get<int32_t>(OPT_FPS));

    std::string cache_path = program.get<std::string>(OPT_CACHE_PATH);
    if (cache_path.empty()) cache_path = wallpaper::platform::GetCachePath("wescene-renderer");
    psw->setPropertyString(wallpaper::PROPERTY_CACHE_PATH, cache_path);

    glfwSetWindowUserPointer(window, &data);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);

    // pinned cursor: re-send every poll so it also wins over real pointer motion
    double pin_x = -1, pin_y = -1;
    {
        auto  spec = program.get<std::string>(OPT_CURSOR);
        char* end  = nullptr;
        if (! spec.empty()) {
            pin_x = std::strtod(spec.c_str(), &end);
            if (end && *end == ',') pin_y = std::strtod(end + 1, nullptr);
        }
    }
    const bool pinned = pin_x >= 0 && pin_y >= 0;
    if (pinned) glfwSetCursorPosCallback(window, nullptr);

    // Screenshot run: request one frame after the delay and exit with its result.
    // The renderer reports completion (or the reason it failed) through the
    // request callback; the output file itself is never polled.
    enum ShotState : int
    {
        SHOT_PENDING,
        SHOT_OK,
        SHOT_FAILED
    };
    struct ShotResult {
        std::atomic<int> state { SHOT_PENDING };
        std::mutex       mtx;
        std::string      detail;
    };
    auto       shot_result = std::make_shared<ShotResult>();
    const auto t0          = std::chrono::steady_clock::now();
    bool       shot_sent   = false;
    bool       timed_out   = false;

    while (! glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (pinned) psw->mouseInput(pin_x, pin_y);
        if (shot_path.empty()) continue;

        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (! shot_sent && elapsed >= shot_delay) {
            auto req  = std::make_shared<wallpaper::ScreenshotRequest>();
            req->path = shot_path;
            req->done = [shot_result](bool ok, const std::string& detail) {
                {
                    std::lock_guard<std::mutex> lk(shot_result->mtx);
                    shot_result->detail = detail;
                }
                shot_result->state = ok ? SHOT_OK : SHOT_FAILED;
                glfwPostEmptyEvent();
            };
            psw->setPropertyObject(wallpaper::PROPERTY_SCREENSHOT, req);
            shot_sent = true;
        }
        if (shot_result->state != SHOT_PENDING) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        } else if (elapsed > shot_delay + shot_timeout) {
            timed_out = true;
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    }
    delete psw;
    // wgl.Clear();
    glfwDestroyWindow(window);
    glfwTerminate();

    if (shot_path.empty()) return 0;
    if (timed_out) {
        std::cerr << "screenshot failed: no frame was presented before the timeout" << std::endl;
        return 2;
    }
    // Destroying the wallpaper above answers any request still in flight, so the
    // state is final here.
    std::string detail;
    {
        std::lock_guard<std::mutex> lk(shot_result->mtx);
        detail = shot_result->detail;
    }
    switch (shot_result->state.load()) {
    case SHOT_OK: std::cout << "screenshot written: " << detail << std::endl; return 0;
    case SHOT_FAILED: std::cerr << "screenshot failed: " << detail << std::endl; return 1;
    default:
        std::cerr << "screenshot failed: window closed before the screenshot was taken"
                  << std::endl;
        return 1;
    }
}
