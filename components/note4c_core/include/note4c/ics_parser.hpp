#pragma once
#include "note4c/calendar_config.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace note4c {

struct CivilDate {
    int year = 2026;
    int month = 1; // 1..12
    int day = 1;   // 1..31

    bool operator==(const CivilDate& o) const {
        return year == o.year && month == o.month && day == o.day;
    }
    bool operator<(const CivilDate& o) const {
        if (year != o.year) return year < o.year;
        if (month != o.month) return month < o.month;
        return day < o.day;
    }
};

struct LocalDateTime {
    int year = 2026;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

int64_t days_from_civil(CivilDate d);
CivilDate civil_from_days(int64_t days);
int weekday_sun0(CivilDate d); // 0=Sun..6=Sat
int days_in_month(int year, int month);
const char* weekday_short(int dow);
const char* month_short(int month);

struct DstTransition {
    bool valid = false;
    int month = 1;  // 1..12
    int week = 1;   // 1..5 (5 = last)
    int dow = 0;    // 0=Sun..6=Sat
    int time_sec = 7200; // default 02:00:00
};

struct TzRule {
    int std_offset_sec = 0; // east-positive seconds from UTC (e.g. MST = -25200)
    int dst_offset_sec = 0;
    bool has_dst = false;
    DstTransition dst_start{};
    DstTransition dst_end{};

    static TzRule parse_posix(std::string_view posix_tz);
    static bool lookup_iana(std::string_view tzid, TzRule& out);

    int offset_for_utc(int64_t utc_sec) const;
    int offset_for_local(LocalDateTime local) const;
    LocalDateTime utc_to_local(int64_t utc_sec) const;
    int64_t local_to_utc(LocalDateTime local) const;
};

struct CalendarEvent {
    std::string title;
    std::string calendar_name;
    CalendarColor color = CalendarColor::Black;
    bool all_day = false;
    int start_minute = 0; // 0..1439
    int end_minute = 0;   // 0..1440
    bool continues_from_prev = false;
    // Internal metadata used for RECURRENCE-ID override deduplication
    std::string uid;
    int64_t recurrence_key = 0; // UTC timestamp or day-number key of occurrence start
    bool is_override = false;
};

void sort_calendar_events(std::vector<CalendarEvent>& events);

class IcsParser {
public:
    static constexpr int kNumDays = 3;
    static constexpr size_t kMaxEventsPerDay = 64;

    IcsParser(CivilDate today, const TzRule& local_tz,
              CalendarColor color, std::string calendar_name);

    void feed(std::string_view chunk);
    void finish();

    const std::vector<CalendarEvent>& day_events(int day_index) const {
        return days_[static_cast<size_t>(day_index)];
    }
    const std::vector<CalendarEvent>& today_events() const { return days_[0]; }
    const std::vector<CalendarEvent>& tomorrow_events() const { return days_[1]; }
    const std::vector<CalendarEvent>& day3_events() const { return days_[2]; }
    size_t vevents_seen() const { return vevents_seen_; }

private:
    struct IcsDateTime {
        bool valid = false;
        bool is_date = false;
        bool is_utc = false;
        std::string tzid;
        LocalDateTime dt{};
    };

    struct RRule {
        bool valid = false;
        enum class Freq { None, Daily, Weekly, Monthly, Yearly } freq = Freq::None;
        int interval = 1;
        int count = 0; // 0 = unbounded
        bool has_until = false;
        int64_t until_utc = 0;
        CivilDate until_date{};
        bool until_is_date = false;
        struct ByDay {
            int nth = 0; // 0 = every, >0 = nth from start, <0 = nth from end
            int dow = 0; // 0=Sun..6=Sat
        };
        std::vector<ByDay> byday;
        std::vector<int> bymonthday;
        std::vector<int> bymonth;
    };

    struct PendingEvent {
        std::string uid;
        std::string summary;
        IcsDateTime dtstart;
        IcsDateTime dtend;
        int64_t duration_sec = -1;
        bool cancelled = false;
        bool has_recurrence_id = false;
        IcsDateTime recurrence_id;
        RRule rrule;
        std::vector<IcsDateTime> exdates;
    };

    struct VTimezoneBuilder {
        std::string tzid;
        int std_offset = 0;
        int dst_offset = 0;
        bool has_std = false;
        bool has_dst = false;
        DstTransition dst_start{};
        DstTransition dst_end{};
    };

    enum class Section { Root, VEvent, VAlarm, VTimezone, VTzStandard, VTzDaylight };

    void feed_raw_line(std::string_view line);
    void process_unfolded_line(std::string_view line);
    void finalize_event(const PendingEvent& ev);
    int64_t resolve_to_utc(const IcsDateTime& dt) const;
    void emit_occurrence(const PendingEvent& ev, int64_t start_utc, int64_t end_utc,
                         bool all_day, CivilDate start_date, CivilDate end_date,
                         int64_t rec_key, bool is_override);
    void expand_rrule(const PendingEvent& ev, int64_t duration_sec);

    static bool parse_ics_datetime(std::string_view params, std::string_view value, IcsDateTime& out);
    static int64_t parse_duration_sec(std::string_view value);
    static RRule parse_rrule(std::string_view value);
    static int parse_utc_offset_hhmm(std::string_view value, bool& ok);
    static std::string unescape_and_clean(std::string_view value);

    CivilDate today_;
    int64_t today_day_num_ = 0;
    std::array<int64_t, kNumDays + 1> day_bounds_utc_{};
    TzRule local_tz_;
    CalendarColor color_;
    std::string calendar_name_;

    std::string line_buf_;
    std::string unfolded_;
    bool has_unfolded_ = false;
    bool finished_ = false;

    Section section_ = Section::Root;
    Section alarm_parent_ = Section::Root;
    PendingEvent current_event_{};
    VTimezoneBuilder current_vtz_{};
    std::unordered_map<std::string, TzRule> vtimezones_;

    std::array<std::vector<CalendarEvent>, kNumDays> days_{};
    std::unordered_set<std::string> overridden_keys_;
    size_t vevents_seen_ = 0;
};

} // namespace note4c
