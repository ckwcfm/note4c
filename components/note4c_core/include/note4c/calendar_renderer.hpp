#pragma once
#include "note4c/calendar_config.hpp"
#include "note4c/frame.hpp"
#include "note4c/ics_parser.hpp"
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace note4c {

struct CalendarViewStatus {
    CivilDate today{};
    int display_days = 3; // 1..7 days to show
    int sync_hour = -1;
    int sync_minute = -1;
    bool wifi_connected = false;
    int battery_percent = -1; // -1 if unknown
    bool charging = false;
    std::string error_text;   // non-empty if any calendar failed
    std::vector<CalendarSub> legend;
};

class CalendarRenderer {
public:
    void draw_agenda(const CalendarViewStatus& status,
                     const std::array<std::vector<CalendarEvent>, IcsParser::kMaxDays>& days,
                     Frame& frame) const;

    void draw_manage(bool is_ap_mode,
                     std::string_view ssid,
                     std::string_view url,
                     std::string_view mac,
                     std::string_view version,
                     std::string_view detail,
                     Frame& frame) const;

    void draw_message(std::string_view title,
                      std::string_view line1,
                      std::string_view line2,
                      Color accent,
                      Frame& frame) const;

    void draw_ota_progress(std::string_view title,
                           std::string_view subtitle,
                           std::string_view status_line,
                           int percent,
                           Frame& frame) const;
};

} // namespace note4c
