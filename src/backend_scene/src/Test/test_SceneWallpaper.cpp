#include <doctest.h>

// Exercise the real handlers without creating a Vulkan device or a desktop shell.
#include "../SceneWallpaper.cpp"

#include <future>
#include <limits>

namespace
{
struct ReleaseGate {
    std::promise<void>& release;
    bool                released { false };
    void                Release() {
        if (! released) {
            released = true;
            release.set_value();
        }
    }
    ~ReleaseGate() { Release(); }
};
struct StopLoops {
    std::shared_ptr<MainHandler> main;
    ~StopLoops() { main->stopRender(); }
};

class CallbackHandler : public looper::Handler {
public:
    explicit CallbackHandler(std::function<void()> callback): m_callback(std::move(callback)) {}
    void onMessageReceived(const std::shared_ptr<looper::Message>&) override { m_callback(); }

private:
    std::function<void()> m_callback;
};

std::shared_ptr<CallbackHandler> PostCallback(const std::shared_ptr<looper::Looper>& loop,
                                              std::function<void()>                  callback) {
    auto handler = std::make_shared<CallbackHandler>(std::move(callback));
    loop->registerHandler(handler);
    looper::Message::create(0, handler)->post();
    return handler;
}
} // namespace

TEST_CASE("user properties survive source changes until explicitly replaced or cleared") {
    auto main = std::make_shared<MainHandler>();
    REQUIRE(main->init());
    StopLoops         stop { main };
    const std::string first  = R"({"backgroundcolor":"0 0 0"})";
    const std::string second = R"({"backgroundcolor":"1 0 0"})";
    struct Change {
        std::string_view property;
        std::string      value;
        std::string      expected;
    };
    for (const auto& change : { Change { PROPERTY_USER_PROPS, first, first },
                                Change { PROPERTY_SOURCE, "first.pkg", first },
                                Change { PROPERTY_SOURCE, "", first },
                                Change { PROPERTY_SOURCE, "second.pkg", first },
                                Change { PROPERTY_USER_PROPS, second, second },
                                Change { PROPERTY_USER_PROPS, "", "" } }) {
        auto msg = CreateMsgWithCmd(main, MainHandler::CMD::CMD_SET_PROPERTY);
        msg->setString("property", std::string(change.property));
        msg->setString("value", change.value);
        msg->post();
        std::promise<std::string> observed;
        auto                      result  = observed.get_future();
        auto                      barrier = PostCallback(main->getLooper().lock(), [&] {
            observed.set_value(main->userProps());
        });
        REQUIRE(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(result.get() == change.expected);
    }
}

TEST_CASE("FPS changes wait for the render loop and clamp before narrowing") {
    auto main = std::make_shared<MainHandler>();
    REQUIRE(main->init());
    StopLoops stop { main };
    auto      render = main->renderHandler();

    for (const auto& [requested, expected] : { std::pair { 256, 256 },
                                               { std::numeric_limits<int32_t>::max(), 1000 },
                                               { 60, 60 },
                                               { 0, 60 } }) {
        std::promise<void> entered;
        std::promise<void> release;
        auto               releaseFuture = release.get_future().share();
        auto               blocked       = PostCallback(render->getLooper().lock(), [&] {
            entered.set_value();
            releaseFuture.wait();
        });
        std::future<void>  mainDone;
        int                before;
        {
            ReleaseGate releaseGate { release };
            REQUIRE(entered.get_future().wait_for(std::chrono::seconds(5)) ==
                    std::future_status::ready);
            before   = render->frame_timer.RequiredFps();
            auto msg = CreateMsgWithCmd(main, MainHandler::CMD::CMD_SET_PROPERTY);
            msg->setString("property", std::string(PROPERTY_FPS));
            msg->setInt32("value", requested);
            msg->post();
            std::promise<void> processed;
            mainDone         = processed.get_future();
            auto mainBarrier = PostCallback(main->getLooper().lock(), [&] {
                processed.set_value();
            });
            REQUIRE(mainDone.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
            CHECK(render->frame_timer.RequiredFps() == before);
        }
        std::promise<int> observed;
        auto              result        = observed.get_future();
        auto              renderBarrier = PostCallback(render->getLooper().lock(), [&] {
            observed.set_value(render->frame_timer.RequiredFps());
        });
        REQUIRE(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(result.get() == expected);
    }
}

TEST_CASE("frame timer bounds prevent zero division and zero intervals") {
    FrameTimer timer;
    timer.SetRequiredFps(0);
    CHECK(timer.RequiredFps() == 1);
    CHECK(timer.IdeaTime() > 0.0);
    timer.SetRequiredFps(std::numeric_limits<u16>::max());
    CHECK(timer.RequiredFps() == 1000);
    CHECK(timer.IdeaTime() > 0.0);
}

TEST_CASE("screenshots are completed once when uninitialised or stopped") {
    SceneWallpaper wallpaper;
    int            called  = 0;
    auto           request = std::make_shared<ScreenshotRequest>();
    request->path          = "/tmp/unused-screenshot.ppm";
    request->done          = [&](bool ok, const std::string&) {
        CHECK_FALSE(ok);
        ++called;
    };
    wallpaper.setPropertyObject(PROPERTY_SCREENSHOT, request);
    CHECK(called == 1);
    wallpaper.stopRender();
    CHECK(called == 1);
    wallpaper.setPropertyObject(PROPERTY_SCREENSHOT, request);
    CHECK(called == 2);
}

TEST_CASE("queued screenshots cancel on stop and cannot complete twice") {
    for (const bool blockMain : { true, false }) {
        INFO("blocked main loop: " << blockMain);
        auto main = std::make_shared<MainHandler>();
        REQUIRE(main->init());
        StopLoops          stop { main };
        std::promise<void> entered;
        std::promise<void> release;
        auto               releaseFuture = release.get_future().share();
        auto               blocked       = PostCallback(
            blockMain ? main->getLooper().lock() : main->renderHandler()->getLooper().lock(), [&] {
                entered.set_value();
                releaseFuture.wait();
            });
        ReleaseGate releaseGate { release };
        REQUIRE(entered.get_future().wait_for(std::chrono::seconds(5)) ==
                std::future_status::ready);
        std::atomic<int> called { 0 };
        auto             request = std::make_shared<ScreenshotRequest>();
        request->path            = "/tmp/unused-screenshot.ppm";
        request->done            = [&](bool ok, const std::string&) {
            CHECK_FALSE(ok);
            ++called;
        };
        main->submitScreenshot(request);
        auto stopping = std::async(std::launch::async, [&] {
            main->stopRender();
        });
        releaseGate.Release();
        REQUIRE(stopping.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(called == 1);
        main->stopRender();
        CHECK(called == 1);
        main->submitScreenshot(request);
        CHECK(called == 2);
    }
}

TEST_CASE("screenshot completion releases captures while tracking entry remains alive") {
    auto               completion = std::make_shared<ScreenshotCompletion>();
    auto               owner      = std::make_shared<int>(1);
    std::weak_ptr<int> weak       = owner;
    completion->done              = [owner](bool, const std::string&) {
    };
    owner.reset();
    completion->Complete(true, "done");
    CHECK(weak.expired());
    completion->Complete(false, "cancelled");
    CHECK(weak.expired());
}
