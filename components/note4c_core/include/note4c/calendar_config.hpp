#pragma once
#include "note4c/frame.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace note4c {

enum class CalendarColor : uint8_t {
    Black = 0,
    Red = 1,
    Yellow = 2,
    BlackOutline = 3,
    RedOutline = 4,
    YellowOutline = 5,
};

inline bool is_outline_color(CalendarColor c) {
    return c == CalendarColor::BlackOutline ||
           c == CalendarColor::RedOutline ||
           c == CalendarColor::YellowOutline;
}

inline Color to_frame_color(CalendarColor c) {
    switch (c) {
    case CalendarColor::Red:
    case CalendarColor::RedOutline: return Color::Red;
    case CalendarColor::Yellow:
    case CalendarColor::YellowOutline: return Color::Yellow;
    case CalendarColor::Black:
    case CalendarColor::BlackOutline:
    default: return Color::Black;
    }
}

inline const char* calendar_color_name(CalendarColor c) {
    switch (c) {
    case CalendarColor::Red: return "red";
    case CalendarColor::Yellow: return "yellow";
    case CalendarColor::BlackOutline: return "black_outline";
    case CalendarColor::RedOutline: return "red_outline";
    case CalendarColor::YellowOutline: return "yellow_outline";
    case CalendarColor::Black:
    default: return "black";
    }
}

inline CalendarColor parse_calendar_color(std::string_view s) {
    if (s == "red" || s == "RED" || s == "1") return CalendarColor::Red;
    if (s == "yellow" || s == "YELLOW" || s == "2") return CalendarColor::Yellow;
    if (s == "black_outline" || s == "BLACK_OUTLINE" || s == "3") return CalendarColor::BlackOutline;
    if (s == "red_outline" || s == "RED_OUTLINE" || s == "4") return CalendarColor::RedOutline;
    if (s == "yellow_outline" || s == "YELLOW_OUTLINE" || s == "5") return CalendarColor::YellowOutline;
    return CalendarColor::Black;
}

enum class TitleMode : uint8_t {
    EventTitle = 0,
    CalendarName = 1,
    Hidden = 2,
};

inline const char* title_mode_name(TitleMode m) {
    switch (m) {
    case TitleMode::CalendarName: return "calendar";
    case TitleMode::Hidden: return "none";
    case TitleMode::EventTitle:
    default: return "title";
    }
}

inline TitleMode parse_title_mode(std::string_view s) {
    if (s == "calendar" || s == "1") return TitleMode::CalendarName;
    if (s == "none" || s == "hidden" || s == "2") return TitleMode::Hidden;
    return TitleMode::EventTitle;
}

enum class LocationMode : uint8_t {
    Off = 0,
    ShortVenue = 1,
    Full = 2,
};

inline const char* location_mode_name(LocationMode m) {
    switch (m) {
    case LocationMode::Off: return "off";
    case LocationMode::ShortVenue: return "short";
    case LocationMode::Full:
    default: return "full";
    }
}

inline LocationMode parse_location_mode(std::string_view s) {
    if (s == "off" || s == "none" || s == "0") return LocationMode::Off;
    if (s == "short" || s == "venue" || s == "1") return LocationMode::ShortVenue;
    return LocationMode::Full;
}

struct CalendarSub {
    std::string name;
    std::string url;
    CalendarColor color = CalendarColor::Black;
    bool enabled = true;

    bool operator==(const CalendarSub& o) const {
        return name == o.name && url == o.url && color == o.color && enabled == o.enabled;
    }
};

struct CalendarConfig {
    static constexpr uint32_t kVersion = 4;
    static constexpr size_t kMaxCalendars = 8;
    static constexpr size_t kMaxSsidLen = 32;
    static constexpr size_t kMaxPassLen = 63;
    static constexpr size_t kMaxTzLen = 64;
    static constexpr size_t kMaxNameLen = 24;
    static constexpr size_t kMaxUrlLen = 320;
    static constexpr uint16_t kMinIntervalMin = 15;
    static constexpr uint16_t kMaxIntervalMin = 1440;
    static constexpr uint8_t kMinDisplayDays = 1;
    static constexpr uint8_t kMaxDisplayDays = 7;

    std::string wifi_ssid;
    std::string wifi_password;
    std::string timezone = "PDT7";
    uint8_t sync_hour = 5;
    uint8_t sync_minute = 0;
    uint16_t sync_interval_minutes = 1440; // 1440 = once a day
    uint8_t display_days = 3;              // 1..7 days to sync & display
    TitleMode title_mode = TitleMode::EventTitle;
    LocationMode location_mode = LocationMode::Full;
    bool use_12h = false;
    bool show_end_time = true;
    bool hide_empty_days = false;
    bool hide_all_day = false;
    bool show_footer = true;
    std::vector<CalendarSub> calendars;

    bool has_wifi() const { return !wifi_ssid.empty(); }

    bool operator==(const CalendarConfig& o) const {
        return wifi_ssid == o.wifi_ssid &&
               wifi_password == o.wifi_password &&
               timezone == o.timezone &&
               sync_hour == o.sync_hour &&
               sync_minute == o.sync_minute &&
               sync_interval_minutes == o.sync_interval_minutes &&
               display_days == o.display_days &&
               title_mode == o.title_mode &&
               location_mode == o.location_mode &&
               use_12h == o.use_12h &&
               show_end_time == o.show_end_time &&
               hide_empty_days == o.hide_empty_days &&
               hide_all_day == o.hide_all_day &&
               show_footer == o.show_footer &&
               calendars == o.calendars;
    }

    static std::string normalize_url(std::string_view url);
    static void sanitize(CalendarConfig& cfg);
    static std::vector<uint8_t> encode(const CalendarConfig& cfg);
    // Decodes v1, v2, v3, and v4 blobs cleanly with backward-compatible defaults.
    static bool decode(const uint8_t* data, size_t len, CalendarConfig& out);

    // Compute seconds until next scheduled sync anchored at target_hour:target_min
    // repeating every interval_minutes (clamped to [60, 86400]).
    static uint32_t seconds_until_next_sync(int cur_hour, int cur_min, int cur_sec,
                                            uint8_t target_hour, uint8_t target_min,
                                            uint16_t interval_minutes = 1440);
};

} // namespace note4c
