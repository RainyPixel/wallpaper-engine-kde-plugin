#include <doctest.h>

#include "OrthoCamera.hpp"

using namespace wallpaper;

TEST_SUITE("OrthoCamera") {
    TEST_CASE("fractional zoom rounds the ortho extent and keeps the unscaled center") {
        const auto identity = OrthoCameraForZoom(1920, 1080, 1.0f);
        CHECK(identity.width == 1920);
        CHECK(identity.height == 1080);
        CHECK(identity.center_x == 960.0f);
        CHECK(identity.center_y == 540.0f);

        // 1920/1.08 = 1777.777... -> 1778; 1080/1.08 = 1000.
        const auto zoomed = OrthoCameraForZoom(1920, 1080, 1.08f);
        CHECK(zoomed.width == 1778);
        CHECK(zoomed.height == 1000);
        CHECK(zoomed.width < identity.width);
        CHECK(zoomed.height < identity.height);
        CHECK(zoomed.center_x == identity.center_x);
        CHECK(zoomed.center_y == identity.center_y);
    }

    TEST_CASE("a non-positive zoom matches zoom 1") {
        const auto base = OrthoCameraForZoom(1920, 1080, 1.0f);
        for (float zoom : { 0.0f, -0.25f, -1.0f }) {
            const auto got = OrthoCameraForZoom(1920, 1080, zoom);
            CHECK(got.width == base.width);
            CHECK(got.height == base.height);
            CHECK(got.center_x == base.center_x);
            CHECK(got.center_y == base.center_y);
        }
    }

    TEST_CASE("a zoom that would round below 1 clamps the extent and leaves the center") {
        const auto tiny = OrthoCameraForZoom(1920, 1080, 100000.0f);
        CHECK(tiny.width == 1);
        CHECK(tiny.height == 1);
        CHECK(tiny.center_x == 960.0f);
        CHECK(tiny.center_y == 540.0f);
    }

    TEST_CASE("fill mode applies the same zoom to the ortho camera only") {
        const auto parsed  = OrthoCameraForZoom(1920, 1080, 1.08f);
        const auto stretch = CameraExtentsForFill(1920, 1080, 1920, 1080, FillMode::STRETCH, 1.08f);
        CHECK(stretch.ortho_width == parsed.width);
        CHECK(stretch.ortho_height == parsed.height);
        CHECK(stretch.perspective_height == 1080);
        CHECK(stretch.perspective_aspect == doctest::Approx(1920.0 / 1080.0));

        const auto stretch_one =
            CameraExtentsForFill(1920, 1080, 2560, 1440, FillMode::STRETCH, 1.0f);
        CHECK(stretch_one.ortho_width == 1920);
        CHECK(stretch_one.ortho_height == 1080);
        CHECK(stretch_one.perspective_height == 1080);

        // Wider view, aspect-crop: unzoomed size is 1920 x 810. Zoom must not
        // change the height fed to the perspective FOV.
        const auto crop = CameraExtentsForFill(1920, 1080, 2560, 1080, FillMode::ASPECTCROP, 1.0f);
        const auto crop_zoom =
            CameraExtentsForFill(1920, 1080, 2560, 1080, FillMode::ASPECTCROP, 1.08f);
        CHECK(crop.ortho_width == 1920);
        CHECK(crop.ortho_height == doctest::Approx(810));
        CHECK(crop.perspective_height == crop.ortho_height);
        CHECK(crop_zoom.ortho_width == 1778);
        CHECK(crop_zoom.ortho_height == doctest::Approx(750));
        CHECK(crop_zoom.perspective_height == crop.perspective_height);
        CHECK(crop_zoom.ortho_height < crop_zoom.perspective_height);

        // Taller view, aspect-fit grows the camera height. Zoom 1 keeps that
        // height for both the ortho camera and the perspective FOV input.
        const auto fit = CameraExtentsForFill(1920, 1080, 1280, 1080, FillMode::ASPECTFIT, 1.0f);
        CHECK(fit.ortho_width == 1920);
        CHECK(fit.ortho_height == doctest::Approx(1620));
        CHECK(fit.perspective_height == fit.ortho_height);

        const auto fit_zoom =
            CameraExtentsForFill(1920, 1080, 1280, 1080, FillMode::ASPECTFIT, 1.08f);
        CHECK(fit_zoom.perspective_height == fit.perspective_height);
        CHECK(fit_zoom.ortho_width == 1778);
        CHECK(fit_zoom.ortho_height < fit.ortho_height);

        // zoom 1 keeps a fractional axis instead of rounding it.
        const auto fractional =
            CameraExtentsForFill(100.5, 50.25, 200, 100, FillMode::STRETCH, 1.0f);
        CHECK(fractional.ortho_width == 100.5);
        CHECK(fractional.ortho_height == 50.25);
    }
}
