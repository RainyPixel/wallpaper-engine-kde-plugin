#include <doctest.h>

#include "WPSceneScript.hpp"
#include "WPUserProperties.hpp"

#include <array>

using namespace wallpaper;

TEST_CASE("native clock formats the configured hours, seconds and delimiter") {
    WPTextScript clock { WPClockFormat {} };
    std::tm      time { .tm_sec = 5, .tm_min = 4, .tm_hour = 0 };
    CHECK(clock.Format(time) == "00:04");
    clock.format = WPClockFormat { false, true, "·" };
    CHECK(clock.Format(time) == "12·04·05");
    CHECK(clock.ExtraGlyphs() == "·");
    time.tm_hour = 15;
    CHECK(clock.Format(time) == "03·04·05");
}

TEST_CASE("native date formats numeric and named components with the authored alignment") {
    using Month = WPDateFormat::Month;
    using Day   = WPDateFormat::Day;
    const std::tm      time { .tm_mday = 27, .tm_mon = 10, .tm_year = 126, .tm_wday = 5 };
    const WPTextScript plain { WPDateFormat { Month::Number, Day::Hidden, false, "/" } };
    const WPTextScript long_day { WPDateFormat { Month::Full, Day::Full, false, "/" } };
    const WPTextScript short_day { WPDateFormat { Month::Short, Day::Short, true, " " } };
    CHECK(plain.Format(time) == "27/11/2026");
    CHECK(long_day.Format(time) == "Friday 27/November/2026");
    CHECK(short_day.Format(time) == "Fri\n 27 Nov 2026");
}

TEST_CASE("Spectrum16 scale and rate share the selected band and time-based smoothing") {
    WPSpectrum16Script    scale { 15, 10.0f, 0.8f, 1.2f };
    WPSpectrum16Script    rate { 0, 5.0f, 0.68f, 1.58f };
    std::array<float, 16> energy {};
    CHECK(scale.Update(energy, 0.1f) == doctest::Approx(0.8));
    CHECK(rate.Update(energy, 0.1f) == doctest::Approx(0.68));
    energy[15] = 1.0f;
    CHECK(scale.Update(energy, 0.05f) == doctest::Approx(1.0));
    CHECK(rate.Update(energy, 0.1f) == doctest::Approx(0.68));
    CHECK(scale.Update(energy, 0.2f) == doctest::Approx(1.2));
    energy[15] = 0.0f;
    energy[0]  = 1.0f;
    CHECK(scale.Update(energy, 0.1f) == doctest::Approx(0.8));
    CHECK(rate.Update(energy, 0.1f) == doctest::Approx(1.13));
}

TEST_CASE("arbitrary scripts with familiar property names keep the authored fallback") {
    const auto             property = nlohmann::json::parse(R"({
      "script": "export function update() { return new Date(); }",
      "scriptproperties": { "use24hFormat": true, "frequency": 15 },
      "value": "fallback"
    })");
    const WPUserProperties properties;
    CHECK_FALSE(ParseTextScript(property, properties).has_value());
    CHECK_FALSE(ParseSpectrum16Script(property, properties).has_value());
    CHECK_FALSE(ParseSoundClickScript(property).has_value());
}
