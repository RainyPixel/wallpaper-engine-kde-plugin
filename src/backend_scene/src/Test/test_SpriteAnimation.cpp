#include <doctest.h>

#include "SpriteAnimation.hpp"

using namespace wallpaper;

TEST_SUITE("SpriteAnimation") {
    TEST_CASE("Empty animation has zero frames") {
        SpriteAnimation anim;
        CHECK(anim.numFrames() == 0);
    }

    TEST_CASE("Single frame") {
        SpriteAnimation anim;
        SpriteFrame     f;
        f.imageId   = 0;
        f.frametime = 1.0f;
        f.x         = 0.0f;
        f.y         = 0.0f;
        f.width     = 0.5f;
        f.height    = 0.5f;
        anim.AppendFrame(f);

        CHECK(anim.numFrames() == 1);

        auto& cur = anim.GetCurFrame();
        CHECK(cur.imageId == 0);
        CHECK(cur.frametime == doctest::Approx(1.0f));
        CHECK(cur.width == doctest::Approx(0.5f));
    }

    TEST_CASE("GetAnimateFrame cycles through frames") {
        SpriteAnimation anim;

        SpriteFrame f0;
        f0.imageId   = 0;
        f0.frametime = 0.5f;
        anim.AppendFrame(f0);

        SpriteFrame f1;
        f1.imageId   = 1;
        f1.frametime = 0.5f;
        anim.AppendFrame(f1);

        CHECK(anim.numFrames() == 2);

        // First call: m_remainTime starts at 0, 0 - 0.0 = 0 which is NOT < 0,
        // so no switch happens on a zero-time call
        {
            auto& frame = anim.GetAnimateFrame(0.0);
            CHECK(frame.imageId == 0);
        }

        // Advance by 0.6 — exceeds frametime of 0.5, should switch to frame 1
        {
            auto& frame = anim.GetAnimateFrame(0.6);
            CHECK(frame.imageId == 1);
        }

        // Advance by 0.6 again — should wrap back to frame 0
        {
            auto& frame = anim.GetAnimateFrame(0.6);
            CHECK(frame.imageId == 0);
        }
    }

    TEST_CASE("GetAnimateFrame with small time steps stays on frame") {
        SpriteAnimation anim;

        SpriteFrame f0;
        f0.imageId   = 0;
        f0.frametime = 1.0f;
        anim.AppendFrame(f0);

        SpriteFrame f1;
        f1.imageId   = 1;
        f1.frametime = 1.0f;
        anim.AppendFrame(f1);

        // m_remainTime starts at 0, so the first positive-time call triggers a
        // switch (0 - dt < 0). After the switch, m_remainTime is set to the
        // new frame's frametime. Use a tiny dt to trigger the initial switch.
        {
            auto& frame = anim.GetAnimateFrame(0.001);
            // Switched from frame 0 to frame 1, m_remainTime = 1.0
            CHECK(frame.imageId == 1);
        }

        // Small steps (total 0.5s) should stay on frame 1 (frametime = 1.0)
        for (int i = 0; i < 5; i++) {
            auto& frame = anim.GetAnimateFrame(0.1);
            CHECK(frame.imageId == 1);
        }

        // Advance past the remaining ~0.5s to trigger switch back to frame 0
        auto& frame = anim.GetAnimateFrame(0.6);
        CHECK(frame.imageId == 0);
    }

    TEST_CASE("AppendFrame preserves insertion order") {
        SpriteAnimation anim;

        for (int i = 0; i < 5; i++) {
            SpriteFrame f;
            f.imageId   = i;
            f.frametime = 0.1f;
            anim.AppendFrame(f);
        }

        CHECK(anim.numFrames() == 5);
        CHECK(anim.GetCurFrame().imageId == 0);
    }

} // TEST_SUITE
