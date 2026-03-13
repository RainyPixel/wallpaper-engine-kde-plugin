#include <doctest.h>

#include "SpecTexs.hpp"

using namespace wallpaper;

TEST_SUITE("SpecTexs") {

TEST_CASE("IsSpecTex") {
    CHECK(IsSpecTex("_rt_default") == true);
    CHECK(IsSpecTex("_rt_link_42") == true);
    CHECK(IsSpecTex("_rt_offscreen_5") == true);
    CHECK(IsSpecTex("_rt_") == true);
    CHECK(IsSpecTex("mytex") == false);
    CHECK(IsSpecTex("_r") == false);
    CHECK(IsSpecTex("") == false);
}

TEST_CASE("IsSpecLinkTex") {
    CHECK(IsSpecLinkTex("_rt_link_42") == true);
    CHECK(IsSpecLinkTex("_rt_link_0") == true);
    CHECK(IsSpecLinkTex("_rt_link_") == true);
    CHECK(IsSpecLinkTex("_rt_default") == false);
    CHECK(IsSpecLinkTex("mytex") == false);
    CHECK(IsSpecLinkTex("") == false);
}

TEST_CASE("ParseLinkTex") {
    CHECK(ParseLinkTex("_rt_link_42") == 42u);
    CHECK(ParseLinkTex("_rt_link_0") == 0u);
    CHECK(ParseLinkTex("_rt_link_100") == 100u);
}

TEST_CASE("GenLinkTex") {
    CHECK(GenLinkTex(0) == "_rt_link_0");
    CHECK(GenLinkTex(42) == "_rt_link_42");
    CHECK(GenLinkTex(100) == "_rt_link_100");
}

TEST_CASE("GenOffscreenRT") {
    CHECK(GenOffscreenRT(0) == "_rt_offscreen_0");
    CHECK(GenOffscreenRT(5) == "_rt_offscreen_5");
    CHECK(GenOffscreenRT(99) == "_rt_offscreen_99");
}

TEST_CASE("GenLinkTex / ParseLinkTex round-trip") {
    for (uint32_t i = 0; i < 20; i++) {
        auto tex = GenLinkTex(static_cast<idx>(i));
        CHECK(IsSpecLinkTex(tex));
        CHECK(ParseLinkTex(tex) == i);
    }
}

} // TEST_SUITE
