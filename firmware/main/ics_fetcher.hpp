#pragma once
#include "board.hpp"
#include "note4c/calendar_config.hpp"
#include "note4c/ics_parser.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace note4c::idf {

struct CalendarFetchStatus {
    std::string name;
    bool ok = false;
    int http_status = 0;
    size_t bytes = 0;
    size_t vevents = 0;
    std::array<size_t, IcsParser::kNumDays> day_counts{};
    std::string error;
};

struct SyncReport {
    bool wifi_ok = true;
    bool time_ok = false;
    int64_t utc_now = 0;
    CivilDate today{};
    int local_hour = 0;
    int local_minute = 0;
    int local_second = 0;
    std::vector<CalendarFetchStatus> calendars;
    std::array<std::vector<CalendarEvent>, IcsParser::kNumDays> days{};
    int error_count = 0;
};

// Synchronizes system clock via SNTP and writes UTC time to PCF8563 RTC.
// Falls back to PCF8563 RTC if SNTP fails.
bool sync_time_sntp(Board& board, int64_t& out_utc_sec, uint32_t timeout_ms = 10000);

// Fetches all enabled calendars in cfg and aggregates sorted 3-day events.
SyncReport sync_all_calendars(const CalendarConfig& cfg, Board& board);

} // namespace note4c::idf
