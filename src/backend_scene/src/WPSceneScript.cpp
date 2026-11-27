#include "WPSceneScript.hpp"

#include "Utils/Sha.hpp"
#include "WPUserProperties.hpp"

#include <algorithm>
#include <array>
#include <cmath>

using namespace wallpaper;

namespace
{
// Exact source fingerprints for the supported SceneScript programs in
// Workshop 2927820378. The scripts themselves are never evaluated.
constexpr std::string_view clock_program       = "49ddf6ffcdf70cfb7bb28ac9457d3b84458876d8";
constexpr std::string_view date_program        = "c543571778f9855cd460023516d3c78f96258328";
constexpr std::string_view spectrum16_program  = "32f46faef454524a8468be86f243a9dad2d8c574";
constexpr std::string_view sound_click_program = "f32263758432f01c89cd7738ebceab2bf39c82e0";

std::string ProgramHash(const nlohmann::json& property) {
    const auto script = property.find("script");
    return script != property.end() && script->is_string()
               ? utils::genSha1(script->get_ref<const std::string&>())
               : std::string {};
}

class ScriptProperties {
public:
    ScriptProperties(const nlohmann::json& property, const WPUserProperties& properties)
        : m_values(property.find("scriptproperties")),
          m_end(property.end()),
          m_user_properties(properties) {}

    template<typename T>
    T Get(std::string_view name, T fallback) const {
        try {
            if (m_values != m_end) {
                const auto it = m_values->find(name);
                if (it != m_values->end()) return m_user_properties.ResolveValue(*it).get<T>();
            }
        } catch (const nlohmann::json::exception&) {
            // Use this program's default for an invalid property.
        }
        return fallback;
    }

private:
    nlohmann::json::const_iterator m_values;
    nlohmann::json::const_iterator m_end;
    const WPUserProperties&        m_user_properties;
};

std::string TwoDigits(int number) { return (number < 10 ? "0" : "") + std::to_string(number); }

std::string FormatDate(const WPDateFormat& format, const std::tm& time) {
    constexpr std::array<std::string_view, 12> months { "January", "February", "March",
                                                        "April",   "May",      "June",
                                                        "July",    "August",   "September",
                                                        "October", "November", "December" };
    constexpr std::array<std::string_view, 7>  days { "Sunday",   "Monday", "Tuesday", "Wednesday",
                                                      "Thursday", "Friday", "Saturday" };
    std::string                                result;
    if (format.day != WPDateFormat::Day::Hidden) {
        // The authored Saturday label starts on a separate line.
        if (time.tm_wday == 6) result += '\n';
        const auto name = days.at(time.tm_wday);
        result += format.day == WPDateFormat::Day::Short ? name.substr(0, 3) : name;
        if (format.vertical) result += '\n';
        result += ' ';
    }
    result += std::to_string(time.tm_mday) + format.delimiter;
    if (format.month == WPDateFormat::Month::Number) {
        result += std::to_string(time.tm_mon + 1);
    } else {
        const auto month = months.at(time.tm_mon);
        result += format.month == WPDateFormat::Month::Short ? month.substr(0, 3) : month;
    }
    return result + format.delimiter + std::to_string(time.tm_year + 1900);
}
} // namespace

std::optional<WPTextScript> wallpaper::ParseTextScript(const nlohmann::json&   property,
                                                       const WPUserProperties& user_properties) {
    const auto             program = ProgramHash(property);
    const ScriptProperties values(property, user_properties);
    if (program == clock_program) {
        return WPTextScript { WPClockFormat {
            values.Get("use24hFormat", true),
            values.Get("showSeconds", false),
            values.Get("delimiter", std::string(":")),
        } };
    }
    if (program == date_program) {
        const auto   month = values.Get("monthFormat", std::string("1"));
        const auto   day   = values.Get("dayFormat", std::string("1"));
        WPDateFormat format;
        if (month == "2") format.month = WPDateFormat::Month::Short;
        if (month == "3") format.month = WPDateFormat::Month::Full;
        if (values.Get("showDay", false)) {
            format.day = day == "2" ? WPDateFormat::Day::Full : WPDateFormat::Day::Short;
        }
        format.vertical = values.Get("alignVertical", false);
        if (values.Get("useDelimiter", false)) {
            format.delimiter = values.Get("addDelimiter", std::string("/"));
        }
        return WPTextScript { std::move(format) };
    }
    return std::nullopt;
}

std::optional<WPSpectrum16Script>
wallpaper::ParseSpectrum16Script(const nlohmann::json&   property,
                                 const WPUserProperties& user_properties) {
    if (ProgramHash(property) != spectrum16_program) return std::nullopt;
    const ScriptProperties values(property, user_properties);
    const auto             band      = values.Get("frequency", 0);
    const auto             smoothing = values.Get("smoothing", 15.0f);
    const auto             minimum   = values.Get("minvalue", 0.8f);
    const auto             maximum   = values.Get("maxvalue", 1.2f);
    if (band < 0 || band >= 16 || ! std::isfinite(smoothing) || smoothing < 0 ||
        ! std::isfinite(minimum) || ! std::isfinite(maximum))
        return std::nullopt;
    return WPSpectrum16Script { static_cast<std::size_t>(band), smoothing, minimum, maximum };
}

std::string WPTextScript::Format(const std::tm& local_time) const {
    const auto* clock = std::get_if<WPClockFormat>(&format);
    if (clock == nullptr) return FormatDate(std::get<WPDateFormat>(format), local_time);
    int hour = local_time.tm_hour;
    if (! clock->use_24_hour) hour = (hour + 11) % 12 + 1;
    std::string text = TwoDigits(hour) + clock->delimiter + TwoDigits(local_time.tm_min);
    if (clock->show_seconds) text += clock->delimiter + TwoDigits(local_time.tm_sec);
    return text;
}

std::optional<std::string_view> wallpaper::ParseSoundClickScript(const nlohmann::json& property) {
    if (ProgramHash(property) != sound_click_program) return std::nullopt;
    return "KaineSalvation.mp3";
}

std::string_view WPTextScript::ExtraGlyphs() const {
    return std::visit(
        [](const auto& options) -> std::string_view {
            return options.delimiter;
        },
        format);
}

float WPSpectrum16Script::Update(std::span<const float, 16> average, float delta_seconds) {
    const float response = std::clamp(delta_seconds * smoothing, 0.0f, 1.0f);
    smoothed             = std::min(1.0f, std::lerp(smoothed, average[band], response));
    return std::lerp(minimum, maximum, smoothed);
}
