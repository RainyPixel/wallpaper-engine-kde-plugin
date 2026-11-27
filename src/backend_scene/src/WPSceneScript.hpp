#pragma once

#include <nlohmann/json_fwd.hpp>

#include <ctime>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace wallpaper
{
class WPUserProperties;

struct WPClockFormat {
    bool        use_24_hour { true };
    bool        show_seconds { false };
    std::string delimiter { ":" };
};

struct WPDateFormat {
    enum class Month
    {
        Number,
        Short,
        Full
    };
    enum class Day
    {
        Hidden,
        Short,
        Full
    };

    Month       month { Month::Number };
    Day         day { Day::Hidden };
    bool        vertical { false };
    std::string delimiter { " " };
};

using WPTextFormat = std::variant<WPClockFormat, WPDateFormat>;

struct WPTextScript {
    WPTextFormat format;

    std::string      Format(const std::tm& local_time) const;
    std::string_view ExtraGlyphs() const;
};

struct WPSpectrum16Script {
    std::size_t band { 0 };
    float       smoothing { 15.0f };
    float       minimum { 0.8f };
    float       maximum { 1.2f };
    float       smoothed { 0.0f };

    float Update(std::span<const float, 16> average, float delta_seconds);
};

std::optional<WPTextScript>       ParseTextScript(const nlohmann::json&   property,
                                                  const WPUserProperties& user_properties);
std::optional<WPSpectrum16Script> ParseSpectrum16Script(const nlohmann::json&   property,
                                                        const WPUserProperties& user_properties);
std::optional<std::string_view>   ParseSoundClickScript(const nlohmann::json& property);
} // namespace wallpaper
