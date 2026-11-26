#include "note4c/ics_parser.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace note4c {
namespace {

bool is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int64_t utc_from_civil_hms(int y, int m, int d, int h, int mi, int s) {
    return days_from_civil({y, m, d}) * 86400LL + h * 3600LL + mi * 60LL + s;
}

LocalDateTime civil_hms_from_sec(int64_t sec) {
    int64_t days = sec / 86400LL;
    int64_t rem = sec % 86400LL;
    if (rem < 0) {
        rem += 86400LL;
        days -= 1;
    }
    const CivilDate d = civil_from_days(days);
    LocalDateTime out;
    out.year = d.year;
    out.month = d.month;
    out.day = d.day;
    out.hour = static_cast<int>(rem / 3600);
    out.minute = static_cast<int>((rem % 3600) / 60);
    out.second = static_cast<int>(rem % 60);
    return out;
}

int nth_dow_day_of_month(int year, int month, int week, int dow) {
    const int dim = days_in_month(year, month);
    const int first_dow = weekday_sun0({year, month, 1});
    int day = 1 + ((dow - first_dow + 7) % 7) + (week - 1) * 7;
    if (week >= 5) {
        while (day > dim) day -= 7;
        while (day + 7 <= dim) day += 7;
    }
    if (day > dim) day = dim;
    return day;
}

int64_t transition_utc(int year, const DstTransition& tr, int prior_offset_sec) {
    const int day = nth_dow_day_of_month(year, tr.month, tr.week, tr.dow);
    const int64_t local_sec = days_from_civil({year, tr.month, day}) * 86400LL + tr.time_sec;
    return local_sec - prior_offset_sec;
}

bool parse_int(std::string_view s, int& out) {
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') {
        neg = (s[i] == '-');
        ++i;
    }
    if (i >= s.size()) return false;
    int64_t val = 0;
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        val = val * 10 + (s[i] - '0');
        if (val > 1000000000LL) return false;
    }
    out = static_cast<int>(neg ? -val : val);
    return true;
}

bool parse_posix_offset(std::string_view s, size_t& pos, int& out_east_sec) {
    if (pos >= s.size()) return false;
    int sign = 1; // POSIX sign is west-positive, so east-positive is negated
    if (s[pos] == '+') { sign = 1; ++pos; }
    else if (s[pos] == '-') { sign = -1; ++pos; }
    size_t start = pos;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
    if (pos == start) return false;
    int hh = 0, mm = 0, ss = 0;
    if (!parse_int(s.substr(start, pos - start), hh)) return false;
    if (pos < s.size() && s[pos] == ':') {
        ++pos;
        size_t ms = pos;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
        if (!parse_int(s.substr(ms, pos - ms), mm)) return false;
        if (pos < s.size() && s[pos] == ':') {
            ++pos;
            size_t ss_pos = pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
            if (!parse_int(s.substr(ss_pos, pos - ss_pos), ss)) return false;
        }
    }
    const int west_sec = sign * (hh * 3600 + mm * 60 + ss);
    out_east_sec = -west_sec;
    return true;
}

void skip_tz_name(std::string_view s, size_t& pos) {
    if (pos < s.size() && s[pos] == '<') {
        ++pos;
        while (pos < s.size() && s[pos] != '>') ++pos;
        if (pos < s.size()) ++pos;
        return;
    }
    while (pos < s.size() && std::isalpha(static_cast<unsigned char>(s[pos]))) ++pos;
}

bool parse_m_rule(std::string_view s, DstTransition& tr) {
    if (s.size() < 6 || (s[0] != 'M' && s[0] != 'm')) return false;
    const size_t slash = s.find('/');
    const std::string_view date_part = (slash == std::string_view::npos) ? s.substr(1) : s.substr(1, slash - 1);
    const size_t d1 = date_part.find('.');
    if (d1 == std::string_view::npos) return false;
    const size_t d2 = date_part.find('.', d1 + 1);
    if (d2 == std::string_view::npos) return false;
    int m = 0, w = 0, d = 0;
    if (!parse_int(date_part.substr(0, d1), m) ||
        !parse_int(date_part.substr(d1 + 1, d2 - d1 - 1), w) ||
        !parse_int(date_part.substr(d2 + 1), d)) return false;
    if (m < 1 || m > 12 || w < 1 || w > 5 || d < 0 || d > 6) return false;
    tr.month = m;
    tr.week = w;
    tr.dow = d;
    tr.time_sec = 7200;
    if (slash != std::string_view::npos) {
        std::string_view tp = s.substr(slash + 1);
        int hh = 0, mm = 0;
        const size_t colon = tp.find(':');
        if (colon == std::string_view::npos) {
            if (parse_int(tp, hh)) tr.time_sec = hh * 3600;
        } else {
            if (parse_int(tp.substr(0, colon), hh) && parse_int(tp.substr(colon + 1), mm))
                tr.time_sec = hh * 3600 + mm * 60;
        }
    }
    tr.valid = true;
    return true;
}

int parse_dow_code(std::string_view s) {
    if (s == "SU") return 0;
    if (s == "MO") return 1;
    if (s == "TU") return 2;
    if (s == "WE") return 3;
    if (s == "TH") return 4;
    if (s == "FR") return 5;
    if (s == "SA") return 6;
    return -1;
}

std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string make_override_key(const std::string& uid, int64_t rec_key) {
    return uid + "#" + std::to_string(rec_key);
}

} // namespace

int64_t days_from_civil(CivilDate d) {
    int64_t y = d.year - (d.month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * static_cast<unsigned>(d.month + (d.month > 2 ? -3 : 9)) + 2) / 5 +
                         static_cast<unsigned>(d.day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

CivilDate civil_from_days(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2 ? 1 : 0);
    return {static_cast<int>(y), static_cast<int>(m), static_cast<int>(d)};
}

int weekday_sun0(CivilDate d) {
    const int64_t z = days_from_civil(d);
    const int r = static_cast<int>((z + 4) % 7);
    return r < 0 ? r + 7 : r;
}

int days_in_month(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2 && is_leap(year)) return 29;
    return kDays[month - 1];
}

const char* weekday_short(int dow) {
    static constexpr const char* kNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return kNames[(dow % 7 + 7) % 7];
}

const char* month_short(int month) {
    static constexpr const char* kNames[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (month < 1 || month > 12) return "???";
    return kNames[month - 1];
}

TzRule TzRule::parse_posix(std::string_view posix_tz) {
    TzRule rule{};
    size_t pos = 0;
    skip_tz_name(posix_tz, pos);
    if (!parse_posix_offset(posix_tz, pos, rule.std_offset_sec)) {
        rule.std_offset_sec = 0;
        rule.dst_offset_sec = 0;
        return rule;
    }
    rule.dst_offset_sec = rule.std_offset_sec;
    if (pos >= posix_tz.size()) return rule;
    const size_t dst_name_start = pos;
    skip_tz_name(posix_tz, pos);
    if (pos == dst_name_start) return rule;
    rule.has_dst = true;
    rule.dst_offset_sec = rule.std_offset_sec + 3600;
    if (pos < posix_tz.size() && posix_tz[pos] != ',') {
        int custom_dst = 0;
        if (parse_posix_offset(posix_tz, pos, custom_dst))
            rule.dst_offset_sec = custom_dst;
    }
    if (pos < posix_tz.size() && posix_tz[pos] == ',') {
        ++pos;
        const size_t c2 = posix_tz.find(',', pos);
        if (c2 != std::string_view::npos) {
            const bool ok1 = parse_m_rule(posix_tz.substr(pos, c2 - pos), rule.dst_start);
            const bool ok2 = parse_m_rule(posix_tz.substr(c2 + 1), rule.dst_end);
            if (!ok1 || !ok2) rule.has_dst = false;
        } else {
            rule.has_dst = false;
        }
    } else {
        // Default US rule if DST name provided without explicit transition dates
        rule.dst_start = {true, 3, 2, 0, 7200};
        rule.dst_end = {true, 11, 1, 0, 7200};
    }
    return rule;
}

bool TzRule::lookup_iana(std::string_view tzid, TzRule& out) {
    struct Entry { const char* iana; const char* posix; };
    static constexpr Entry kTable[] = {
        {"UTC", "UTC0"},
        {"Etc/UTC", "UTC0"},
        {"Etc/GMT", "UTC0"},
        {"GMT", "UTC0"},
        {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
        {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
        {"America/Detroit", "EST5EDT,M3.2.0,M11.1.0"},
        {"US/Eastern", "EST5EDT,M3.2.0,M11.1.0"},
        {"Eastern Standard Time", "EST5EDT,M3.2.0,M11.1.0"},
        {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
        {"America/Winnipeg", "CST6CDT,M3.2.0,M11.1.0"},
        {"US/Central", "CST6CDT,M3.2.0,M11.1.0"},
        {"Central Standard Time", "CST6CDT,M3.2.0,M11.1.0"},
        {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
        {"America/Edmonton", "MST7MDT,M3.2.0,M11.1.0"},
        {"America/Boise", "MST7MDT,M3.2.0,M11.1.0"},
        {"US/Mountain", "MST7MDT,M3.2.0,M11.1.0"},
        {"Mountain Standard Time", "MST7MDT,M3.2.0,M11.1.0"},
        {"America/Phoenix", "MST7"},
        {"America/Whitehorse", "MST7"},
        {"US/Arizona", "MST7"},
        {"US Mountain Standard Time", "MST7"},
        {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
        {"America/Vancouver", "PDT7"},
        {"US/Pacific", "PST8PDT,M3.2.0,M11.1.0"},
        {"Pacific Standard Time", "PST8PDT,M3.2.0,M11.1.0"},
        {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
        {"Pacific/Honolulu", "HST10"},
        {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
        {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
        {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Asia/Tokyo", "JST-9"},
        {"Asia/Seoul", "KST-9"},
        {"Asia/Shanghai", "CST-8"},
        {"Asia/Hong_Kong", "HKT-8"},
        {"Asia/Taipei", "CST-8"},
        {"Asia/Singapore", "SGT-8"},
        {"Asia/Kolkata", "IST-5:30"},
        {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    };
    for (const auto& e : kTable) {
        if (tzid == e.iana) {
            out = parse_posix(e.posix);
            return true;
        }
    }
    return false;
}

int TzRule::offset_for_utc(int64_t utc_sec) const {
    if (!has_dst || !dst_start.valid || !dst_end.valid) return std_offset_sec;
    const LocalDateTime approx = civil_hms_from_sec(utc_sec + std_offset_sec);
    const int64_t start_u = transition_utc(approx.year, dst_start, std_offset_sec);
    const int64_t end_u = transition_utc(approx.year, dst_end, dst_offset_sec);
    if (start_u < end_u) {
        return (utc_sec >= start_u && utc_sec < end_u) ? dst_offset_sec : std_offset_sec;
    }
    // Southern hemisphere: DST spans New Year
    return (utc_sec >= start_u || utc_sec < end_u) ? dst_offset_sec : std_offset_sec;
}

int TzRule::offset_for_local(LocalDateTime local) const {
    if (!has_dst) return std_offset_sec;
    const int64_t wall = utc_from_civil_hms(local.year, local.month, local.day,
                                            local.hour, local.minute, local.second);
    const int64_t cand_std = wall - std_offset_sec;
    if (offset_for_utc(cand_std) == std_offset_sec) return std_offset_sec;
    return dst_offset_sec;
}

LocalDateTime TzRule::utc_to_local(int64_t utc_sec) const {
    return civil_hms_from_sec(utc_sec + offset_for_utc(utc_sec));
}

int64_t TzRule::local_to_utc(LocalDateTime local) const {
    const int64_t wall = utc_from_civil_hms(local.year, local.month, local.day,
                                            local.hour, local.minute, local.second);
    return wall - offset_for_local(local);
}

void sort_calendar_events(std::vector<CalendarEvent>& events) {
    std::stable_sort(events.begin(), events.end(), [](const CalendarEvent& a, const CalendarEvent& b) {
        if (a.all_day != b.all_day) return a.all_day > b.all_day;
        if (!a.all_day) {
            if (a.start_minute != b.start_minute) return a.start_minute < b.start_minute;
            if (a.end_minute != b.end_minute) return a.end_minute < b.end_minute;
        }
        return a.title < b.title;
    });
}

IcsParser::IcsParser(CivilDate today, const TzRule& local_tz,
                     CalendarColor color, std::string calendar_name,
                     int num_days)
    : today_(today),
      today_day_num_(days_from_civil(today)),
      num_days_(std::clamp(num_days, 1, kMaxDays)),
      local_tz_(local_tz),
      color_(color),
      calendar_name_(std::move(calendar_name)) {
    for (int i = 0; i <= num_days_; ++i) {
        const CivilDate d = civil_from_days(today_day_num_ + i);
        day_bounds_utc_[static_cast<size_t>(i)] =
            local_tz_.local_to_utc({d.year, d.month, d.day, 0, 0, 0});
    }
}

void IcsParser::feed(std::string_view chunk) {
    if (finished_) return;
    for (char c : chunk) {
        if (c == '\r') continue;
        if (c == '\n') {
            feed_raw_line(line_buf_);
            line_buf_.clear();
        } else {
            if (line_buf_.size() < 4096) line_buf_.push_back(c);
        }
    }
}

void IcsParser::finish() {
    if (finished_) return;
    finished_ = true;
    if (!line_buf_.empty()) {
        feed_raw_line(line_buf_);
        line_buf_.clear();
    }
    if (has_unfolded_) {
        process_unfolded_line(unfolded_);
        unfolded_.clear();
        has_unfolded_ = false;
    }
    // Remove base recurring occurrences superseded by a RECURRENCE-ID override
    auto prune = [this](std::vector<CalendarEvent>& list) {
        list.erase(std::remove_if(list.begin(), list.end(), [this](const CalendarEvent& e) {
            if (e.is_override || e.uid.empty()) return false;
            return overridden_keys_.count(make_override_key(e.uid, e.recurrence_key)) > 0;
        }), list.end());
    };
    for (auto& day_list : days_) {
        prune(day_list);
        sort_calendar_events(day_list);
    }
}

void IcsParser::feed_raw_line(std::string_view line) {
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
        if (has_unfolded_ && unfolded_.size() < 4096) {
            unfolded_.append(line.substr(1));
        }
        return;
    }
    if (has_unfolded_) {
        process_unfolded_line(unfolded_);
        unfolded_.clear();
    }
    unfolded_.assign(line);
    has_unfolded_ = true;
}

void IcsParser::process_unfolded_line(std::string_view line) {
    if (line.empty()) return;
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) return;
    const std::string_view lhs = line.substr(0, colon);
    const std::string_view value = line.substr(colon + 1);
    const size_t semi = lhs.find(';');
    const std::string name = upper_ascii(semi == std::string_view::npos ? lhs : lhs.substr(0, semi));
    const std::string_view params = (semi == std::string_view::npos) ? std::string_view{} : lhs.substr(semi + 1);

    if (name == "BEGIN") {
        const std::string v = upper_ascii(value);
        if (v == "VEVENT" && section_ == Section::Root) {
            section_ = Section::VEvent;
            current_event_ = PendingEvent{};
        } else if (v == "VALARM") {
            alarm_parent_ = section_;
            section_ = Section::VAlarm;
        } else if (v == "VTIMEZONE" && section_ == Section::Root) {
            section_ = Section::VTimezone;
            current_vtz_ = VTimezoneBuilder{};
        } else if (v == "STANDARD" && section_ == Section::VTimezone) {
            section_ = Section::VTzStandard;
        } else if (v == "DAYLIGHT" && section_ == Section::VTimezone) {
            section_ = Section::VTzDaylight;
        }
        return;
    }
    if (name == "END") {
        const std::string v = upper_ascii(value);
        if (v == "VALARM" && section_ == Section::VAlarm) {
            section_ = alarm_parent_;
        } else if (v == "VEVENT" && section_ == Section::VEvent) {
            ++vevents_seen_;
            finalize_event(current_event_);
            section_ = Section::Root;
        } else if (v == "STANDARD" && section_ == Section::VTzStandard) {
            section_ = Section::VTimezone;
        } else if (v == "DAYLIGHT" && section_ == Section::VTzDaylight) {
            section_ = Section::VTimezone;
        } else if (v == "VTIMEZONE" && section_ == Section::VTimezone) {
            if (!current_vtz_.tzid.empty() && current_vtz_.has_std) {
                TzRule r{};
                r.std_offset_sec = current_vtz_.std_offset;
                r.dst_offset_sec = current_vtz_.has_dst ? current_vtz_.dst_offset : current_vtz_.std_offset;
                r.has_dst = current_vtz_.has_dst && current_vtz_.dst_start.valid && current_vtz_.dst_end.valid;
                r.dst_start = current_vtz_.dst_start;
                r.dst_end = current_vtz_.dst_end;
                vtimezones_[current_vtz_.tzid] = r;
            }
            section_ = Section::Root;
        }
        return;
    }

    if (section_ == Section::VAlarm) return;

    if (section_ == Section::VTimezone) {
        if (name == "TZID") current_vtz_.tzid = std::string(value);
        return;
    }
    if (section_ == Section::VTzStandard || section_ == Section::VTzDaylight) {
        if (name == "TZOFFSETTO") {
            bool ok = false;
            const int off = parse_utc_offset_hhmm(value, ok);
            if (ok) {
                if (section_ == Section::VTzStandard) {
                    current_vtz_.std_offset = off;
                    current_vtz_.has_std = true;
                } else {
                    current_vtz_.dst_offset = off;
                    current_vtz_.has_dst = true;
                }
            }
        } else if (name == "RRULE") {
            const RRule rr = parse_rrule(value);
            if (rr.valid && rr.freq == RRule::Freq::Yearly && !rr.bymonth.empty() && !rr.byday.empty()) {
                DstTransition tr{};
                tr.valid = true;
                tr.month = rr.bymonth[0];
                tr.dow = rr.byday[0].dow;
                const int nth = rr.byday[0].nth;
                tr.week = (nth < 0) ? 5 : std::clamp(nth == 0 ? 1 : nth, 1, 5);
                tr.time_sec = 7200;
                if (section_ == Section::VTzDaylight) current_vtz_.dst_start = tr;
                else current_vtz_.dst_end = tr;
            }
        }
        return;
    }

    if (section_ != Section::VEvent) return;

    if (name == "UID") {
        current_event_.uid = std::string(value);
    } else if (name == "SUMMARY") {
        current_event_.summary = unescape_and_clean(value);
    } else if (name == "LOCATION") {
        current_event_.location = unescape_and_clean(value);
        if (current_event_.location.size() > 96) current_event_.location.resize(96);
    } else if (name == "STATUS") {
        if (upper_ascii(value) == "CANCELLED") current_event_.cancelled = true;
    } else if (name == "DTSTART") {
        parse_ics_datetime(params, value, current_event_.dtstart);
    } else if (name == "DTEND") {
        parse_ics_datetime(params, value, current_event_.dtend);
    } else if (name == "DURATION") {
        current_event_.duration_sec = parse_duration_sec(value);
    } else if (name == "RECURRENCE-ID") {
        if (parse_ics_datetime(params, value, current_event_.recurrence_id))
            current_event_.has_recurrence_id = true;
    } else if (name == "RRULE") {
        current_event_.rrule = parse_rrule(value);
    } else if (name == "EXDATE") {
        size_t p = 0;
        while (p <= value.size()) {
            const size_t c = value.find(',', p);
            const std::string_view tok = (c == std::string_view::npos) ? value.substr(p) : value.substr(p, c - p);
            IcsDateTime ex{};
            if (parse_ics_datetime(params, tok, ex)) current_event_.exdates.push_back(std::move(ex));
            if (c == std::string_view::npos) break;
            p = c + 1;
        }
    }
}

int IcsParser::parse_utc_offset_hhmm(std::string_view value, bool& ok) {
    ok = false;
    if (value.size() < 5) return 0;
    int sign = 1;
    if (value[0] == '+') sign = 1;
    else if (value[0] == '-') sign = -1;
    else return 0;
    int hh = 0, mm = 0;
    if (!parse_int(value.substr(1, 2), hh) || !parse_int(value.substr(3, 2), mm)) return 0;
    ok = true;
    return sign * (hh * 3600 + mm * 60);
}

bool IcsParser::parse_ics_datetime(std::string_view params, std::string_view value, IcsDateTime& out) {
    out = IcsDateTime{};
    // Extract TZID and VALUE=DATE from params
    size_t pos = 0;
    while (pos < params.size()) {
        const size_t semi = params.find(';', pos);
        const std::string_view part = (semi == std::string_view::npos) ? params.substr(pos) : params.substr(pos, semi - pos);
        const size_t eq = part.find('=');
        if (eq != std::string_view::npos) {
            const std::string key = upper_ascii(part.substr(0, eq));
            std::string_view val = part.substr(eq + 1);
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
                val = val.substr(1, val.size() - 2);
            if (key == "TZID") out.tzid = std::string(val);
            else if (key == "VALUE" && upper_ascii(val) == "DATE") out.is_date = true;
        }
        if (semi == std::string_view::npos) break;
        pos = semi + 1;
    }

    if (value.size() < 8) return false;
    int y = 0, mo = 0, d = 0;
    if (!parse_int(value.substr(0, 4), y) ||
        !parse_int(value.substr(4, 2), mo) ||
        !parse_int(value.substr(6, 2), d)) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    out.dt = {y, mo, d, 0, 0, 0};

    if (value.size() == 8 || out.is_date) {
        out.is_date = true;
        out.valid = true;
        return true;
    }
    if (value.size() < 15 || (value[8] != 'T' && value[8] != 't')) return false;
    int hh = 0, mi = 0, ss = 0;
    if (!parse_int(value.substr(9, 2), hh) ||
        !parse_int(value.substr(11, 2), mi) ||
        !parse_int(value.substr(13, 2), ss)) return false;
    if (hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 60) return false;
    if (ss == 60) ss = 59;
    out.dt.hour = hh;
    out.dt.minute = mi;
    out.dt.second = ss;
    out.is_utc = (value.size() >= 16 && (value[15] == 'Z' || value[15] == 'z'));
    out.valid = true;
    return true;
}

int64_t IcsParser::parse_duration_sec(std::string_view value) {
    if (value.empty()) return -1;
    size_t i = 0;
    int sign = 1;
    if (value[i] == '-') { sign = -1; ++i; }
    else if (value[i] == '+') { ++i; }
    if (i >= value.size() || value[i] != 'P') return -1;
    ++i;
    bool in_time = false;
    int64_t total = 0;
    while (i < value.size()) {
        if (value[i] == 'T') { in_time = true; ++i; continue; }
        size_t start = i;
        while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) ++i;
        if (i == start || i >= value.size()) break;
        int n = 0;
        if (!parse_int(value.substr(start, i - start), n)) break;
        const char u = value[i++];
        if (!in_time && u == 'W') total += static_cast<int64_t>(n) * 7 * 86400;
        else if (!in_time && u == 'D') total += static_cast<int64_t>(n) * 86400;
        else if (in_time && u == 'H') total += static_cast<int64_t>(n) * 3600;
        else if (in_time && u == 'M') total += static_cast<int64_t>(n) * 60;
        else if (in_time && u == 'S') total += static_cast<int64_t>(n);
    }
    return sign * total;
}

IcsParser::RRule IcsParser::parse_rrule(std::string_view value) {
    RRule r{};
    size_t pos = 0;
    while (pos < value.size()) {
        const size_t semi = value.find(';', pos);
        const std::string_view part = (semi == std::string_view::npos) ? value.substr(pos) : value.substr(pos, semi - pos);
        const size_t eq = part.find('=');
        if (eq != std::string_view::npos) {
            const std::string key = upper_ascii(part.substr(0, eq));
            const std::string_view val = part.substr(eq + 1);
            if (key == "FREQ") {
                const std::string fv = upper_ascii(val);
                if (fv == "DAILY") r.freq = RRule::Freq::Daily;
                else if (fv == "WEEKLY") r.freq = RRule::Freq::Weekly;
                else if (fv == "MONTHLY") r.freq = RRule::Freq::Monthly;
                else if (fv == "YEARLY") r.freq = RRule::Freq::Yearly;
            } else if (key == "INTERVAL") {
                int iv = 1;
                if (parse_int(val, iv) && iv >= 1) r.interval = iv;
            } else if (key == "COUNT") {
                int cnt = 0;
                if (parse_int(val, cnt) && cnt >= 1) r.count = cnt;
            } else if (key == "UNTIL") {
                IcsDateTime u{};
                if (parse_ics_datetime({}, val, u)) {
                    r.has_until = true;
                    r.until_is_date = u.is_date;
                    r.until_date = {u.dt.year, u.dt.month, u.dt.day};
                    if (!u.is_date) {
                        r.until_utc = utc_from_civil_hms(u.dt.year, u.dt.month, u.dt.day,
                                                         u.dt.hour, u.dt.minute, u.dt.second);
                    }
                }
            } else if (key == "BYDAY") {
                size_t p = 0;
                while (p <= val.size()) {
                    const size_t c = val.find(',', p);
                    const std::string_view tok = (c == std::string_view::npos) ? val.substr(p) : val.substr(p, c - p);
                    if (tok.size() >= 2) {
                        const int dow = parse_dow_code(upper_ascii(tok.substr(tok.size() - 2)));
                        if (dow >= 0) {
                            int nth = 0;
                            if (tok.size() > 2) parse_int(tok.substr(0, tok.size() - 2), nth);
                            r.byday.push_back({nth, dow});
                        }
                    }
                    if (c == std::string_view::npos) break;
                    p = c + 1;
                }
            } else if (key == "BYMONTHDAY") {
                size_t p = 0;
                while (p <= val.size()) {
                    const size_t c = val.find(',', p);
                    const std::string_view tok = (c == std::string_view::npos) ? val.substr(p) : val.substr(p, c - p);
                    int md = 0;
                    if (parse_int(tok, md) && md != 0 && md >= -31 && md <= 31)
                        r.bymonthday.push_back(md);
                    if (c == std::string_view::npos) break;
                    p = c + 1;
                }
            } else if (key == "BYMONTH") {
                size_t p = 0;
                while (p <= val.size()) {
                    const size_t c = val.find(',', p);
                    const std::string_view tok = (c == std::string_view::npos) ? val.substr(p) : val.substr(p, c - p);
                    int mo = 0;
                    if (parse_int(tok, mo) && mo >= 1 && mo <= 12) r.bymonth.push_back(mo);
                    if (c == std::string_view::npos) break;
                    p = c + 1;
                }
            }
        }
        if (semi == std::string_view::npos) break;
        pos = semi + 1;
    }
    r.valid = (r.freq != RRule::Freq::None);
    return r;
}

std::string IcsParser::unescape_and_clean(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '\\' && i + 1 < value.size()) {
            const char n = value[++i];
            if (n == 'n' || n == 'N') c = ' ';
            else if (n == ',' || n == ';' || n == '\\') c = n;
            else c = n;
        }
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 32 && uc <= 126) {
            out.push_back(c);
        } else if (c == '\t') {
            out.push_back(' ');
        }
        // Drop non-ASCII bytes cleanly (English/basic Latin display)
    }
    // Collapse leading/trailing spaces
    size_t s = 0;
    while (s < out.size() && out[s] == ' ') ++s;
    size_t e = out.size();
    while (e > s && out[e - 1] == ' ') --e;
    out = out.substr(s, e - s);
    if (out.size() > 80) out.resize(80);
    return out;
}

int64_t IcsParser::resolve_to_utc(const IcsDateTime& dt) const {
    if (dt.is_utc) {
        return utc_from_civil_hms(dt.dt.year, dt.dt.month, dt.dt.day,
                                  dt.dt.hour, dt.dt.minute, dt.dt.second);
    }
    if (!dt.tzid.empty()) {
        // Prefer the file's own VTIMEZONE definition, then fall back to built-in IANA table
        const auto it = vtimezones_.find(dt.tzid);
        if (it != vtimezones_.end()) {
            return it->second.local_to_utc(dt.dt);
        }
        TzRule tz{};
        if (TzRule::lookup_iana(dt.tzid, tz)) {
            return tz.local_to_utc(dt.dt);
        }
    }
    return local_tz_.local_to_utc(dt.dt);
}

void IcsParser::emit_occurrence(const PendingEvent& ev, int64_t start_utc, int64_t end_utc,
                                bool all_day, CivilDate start_date, CivilDate end_date,
                                int64_t rec_key, bool is_override) {
    const std::string title = ev.summary.empty() ? "(No title)" : ev.summary;
    if (all_day) {
        const int64_t s_day = days_from_civil(start_date);
        int64_t e_day = days_from_civil(end_date);
        if (e_day <= s_day) e_day = s_day + 1;
        for (int offset = 0; offset < num_days_; ++offset) {
            const int64_t target_day = today_day_num_ + offset;
            if (s_day <= target_day && target_day < e_day) {
                auto& dst = days_[static_cast<size_t>(offset)];
                if (dst.size() < kMaxEventsPerDay) {
                    CalendarEvent ce;
                    ce.title = title;
                    ce.calendar_name = calendar_name_;
                    ce.color = color_;
                    ce.all_day = true;
                    ce.uid = ev.uid;
                    ce.recurrence_key = rec_key;
                    ce.is_override = is_override;
                    ce.location = ev.location;
                    dst.push_back(std::move(ce));
                }
            }
        }
        return;
    }

    if (end_utc < start_utc) end_utc = start_utc;
    // Handle zero-duration point events (e.g. reminders) as overlapping their start instant
    const int64_t eff_end = (end_utc == start_utc) ? start_utc + 1 : end_utc;

    for (int offset = 0; offset < num_days_; ++offset) {
        const int64_t d_start = day_bounds_utc_[static_cast<size_t>(offset)];
        const int64_t d_end = day_bounds_utc_[static_cast<size_t>(offset + 1)];
        if (start_utc < d_end && eff_end > d_start) {
            auto& dst = days_[static_cast<size_t>(offset)];
            if (dst.size() >= kMaxEventsPerDay) continue;
            CalendarEvent ce;
            ce.title = title;
            ce.calendar_name = calendar_name_;
            ce.color = color_;
            ce.all_day = false;
            ce.uid = ev.uid;
            ce.recurrence_key = rec_key;
            ce.is_override = is_override;
            ce.location = ev.location;
            if (start_utc < d_start) {
                ce.start_minute = 0;
                ce.continues_from_prev = true;
            } else {
                const LocalDateTime ls = local_tz_.utc_to_local(start_utc);
                ce.start_minute = ls.hour * 60 + ls.minute;
            }
            if (end_utc >= d_end) {
                ce.end_minute = 24 * 60;
            } else if (end_utc > start_utc) {
                const LocalDateTime le = local_tz_.utc_to_local(end_utc);
                ce.end_minute = le.hour * 60 + le.minute;
            } else {
                ce.end_minute = ce.start_minute;
            }
            dst.push_back(std::move(ce));
        }
    }
}

void IcsParser::expand_rrule(const PendingEvent& ev, int64_t duration_sec) {
    const auto& rr = ev.rrule;
    const bool all_day = ev.dtstart.is_date;
    const CivilDate start_date{ev.dtstart.dt.year, ev.dtstart.dt.month, ev.dtstart.dt.day};
    const int64_t start_day_num = days_from_civil(start_date);
    const int64_t day_span = all_day ? std::max<int64_t>(1, duration_sec / 86400) : 1;

    // Build EXDATE lookup set
    std::unordered_set<int64_t> ex_keys;
    for (const auto& ex : ev.exdates) {
        if (ex.is_date) ex_keys.insert(days_from_civil({ex.dt.year, ex.dt.month, ex.dt.day}));
        else ex_keys.insert(resolve_to_utc(ex));
    }

    // Window bounds in civil days (with margin for multi-day & timezone offsets)
    const int64_t win_lo_day = today_day_num_ - std::max<int64_t>(day_span, 2);
    const int64_t win_hi_day = today_day_num_ + num_days_;

    auto resolve_occ_utc = [&](CivilDate occ_d) -> int64_t {
        IcsDateTime inst = ev.dtstart;
        inst.dt.year = occ_d.year;
        inst.dt.month = occ_d.month;
        inst.dt.day = occ_d.day;
        return resolve_to_utc(inst);
    };

    auto past_until = [&](CivilDate occ_d, int64_t occ_utc) -> bool {
        if (!rr.has_until) return false;
        if (rr.until_is_date || all_day) return days_from_civil(occ_d) > days_from_civil(rr.until_date);
        return occ_utc > rr.until_utc;
    };

    int emitted_count = 0;
    auto consider_date = [&](CivilDate occ_d) -> bool {
        const int64_t dnum = days_from_civil(occ_d);
        if (dnum < start_day_num) return true; // before DTSTART, keep going
        const int64_t occ_utc = all_day ? 0 : resolve_occ_utc(occ_d);
        if (past_until(occ_d, occ_utc)) return false;
        if (rr.count > 0 && emitted_count >= rr.count) return false;
        ++emitted_count;

        const int64_t rec_key = all_day ? dnum : occ_utc;
        if (ex_keys.count(rec_key) > 0) return true;
        if (dnum >= win_lo_day && dnum <= win_hi_day) {
            if (all_day) {
                emit_occurrence(ev, 0, 0, true, occ_d, civil_from_days(dnum + day_span), rec_key, false);
            } else {
                emit_occurrence(ev, occ_utc, occ_utc + duration_sec, false, occ_d, occ_d, rec_key, false);
            }
        }
        return dnum <= win_hi_day;
    };

    const int interval = std::max(1, rr.interval);

    if (rr.freq == RRule::Freq::Daily) {
        int64_t cur = start_day_num;
        if (rr.count == 0 && cur < win_lo_day) {
            const int64_t skip = (win_lo_day - cur) / interval;
            cur += skip * interval;
        }
        for (int iter = 0; iter < 50000; ++iter, cur += interval) {
            if (!consider_date(civil_from_days(cur))) break;
        }
    } else if (rr.freq == RRule::Freq::Weekly) {
        std::vector<int> dows;
        if (rr.byday.empty()) {
            dows.push_back(weekday_sun0(start_date));
        } else {
            for (const auto& bd : rr.byday) dows.push_back(bd.dow);
            std::sort(dows.begin(), dows.end());
            dows.erase(std::unique(dows.begin(), dows.end()), dows.end());
        }
        // Anchor on Monday (or Sunday) of DTSTART's week
        const int start_dow = weekday_sun0(start_date);
        int64_t week_sun = start_day_num - start_dow;
        if (rr.count == 0 && week_sun + 6 < win_lo_day) {
            const int64_t step = static_cast<int64_t>(interval) * 7;
            const int64_t skip = (win_lo_day - (week_sun + 6)) / step;
            week_sun += skip * step;
        }
        for (int iter = 0; iter < 10000; ++iter, week_sun += static_cast<int64_t>(interval) * 7) {
            bool cont = true;
            for (int dow : dows) {
                if (!consider_date(civil_from_days(week_sun + dow))) {
                    cont = false;
                    break;
                }
            }
            if (!cont || week_sun > win_hi_day) break;
        }
    } else if (rr.freq == RRule::Freq::Monthly) {
        int64_t total_months = static_cast<int64_t>(start_date.year) * 12 + (start_date.month - 1);
        const int64_t target_lo_m = static_cast<int64_t>(today_.year) * 12 + (today_.month - 1) - 1;
        if (rr.count == 0 && total_months < target_lo_m) {
            const int64_t skip = (target_lo_m - total_months) / interval;
            total_months += skip * interval;
        }
        for (int iter = 0; iter < 5000; ++iter, total_months += interval) {
            const int y = static_cast<int>(total_months / 12);
            const int m = static_cast<int>(total_months % 12) + 1;
            const int dim = days_in_month(y, m);
            std::vector<int> mdays;
            if (!rr.byday.empty()) {
                for (const auto& bd : rr.byday) {
                    if (bd.nth == 0) {
                        for (int d = 1; d <= dim; ++d)
                            if (weekday_sun0({y, m, d}) == bd.dow) mdays.push_back(d);
                    } else if (bd.nth > 0) {
                        int count = 0;
                        for (int d = 1; d <= dim; ++d) {
                            if (weekday_sun0({y, m, d}) == bd.dow && ++count == bd.nth) {
                                mdays.push_back(d);
                                break;
                            }
                        }
                    } else {
                        int count = 0;
                        for (int d = dim; d >= 1; --d) {
                            if (weekday_sun0({y, m, d}) == bd.dow && --count == bd.nth) {
                                mdays.push_back(d);
                                break;
                            }
                        }
                    }
                }
            } else if (!rr.bymonthday.empty()) {
                for (int md : rr.bymonthday) {
                    const int d = (md > 0) ? md : (dim + 1 + md);
                    if (d >= 1 && d <= dim) mdays.push_back(d);
                }
            } else {
                if (start_date.day <= dim) mdays.push_back(start_date.day);
            }
            std::sort(mdays.begin(), mdays.end());
            mdays.erase(std::unique(mdays.begin(), mdays.end()), mdays.end());
            bool cont = true;
            for (int d : mdays) {
                if (!consider_date({y, m, d})) { cont = false; break; }
            }
            if (!cont || days_from_civil({y, m, 1}) > win_hi_day) break;
        }
    } else if (rr.freq == RRule::Freq::Yearly) {
        int y = start_date.year;
        if (rr.count == 0 && y < today_.year - 1) {
            const int skip = (today_.year - 1 - y) / interval;
            y += skip * interval;
        }
        const std::vector<int> months = rr.bymonth.empty() ? std::vector<int>{start_date.month} : rr.bymonth;
        for (int iter = 0; iter < 500; ++iter, y += interval) {
            bool cont = true;
            for (int m : months) {
                const int dim = days_in_month(y, m);
                if (!rr.byday.empty()) {
                    for (const auto& bd : rr.byday) {
                        int target_d = 0;
                        if (bd.nth > 0) {
                            int c = 0;
                            for (int d = 1; d <= dim; ++d)
                                if (weekday_sun0({y, m, d}) == bd.dow && ++c == bd.nth) { target_d = d; break; }
                        } else if (bd.nth < 0) {
                            int c = 0;
                            for (int d = dim; d >= 1; --d)
                                if (weekday_sun0({y, m, d}) == bd.dow && --c == bd.nth) { target_d = d; break; }
                        }
                        if (target_d >= 1 && !consider_date({y, m, target_d})) { cont = false; break; }
                    }
                } else {
                    const int d = rr.bymonthday.empty() ? start_date.day : rr.bymonthday[0];
                    if (d >= 1 && d <= dim && !consider_date({y, m, d})) { cont = false; break; }
                }
                if (!cont) break;
            }
            if (!cont || y > today_.year + 1) break;
        }
    }
}

void IcsParser::finalize_event(const PendingEvent& ev) {
    if (!ev.dtstart.valid) return;

    // Track RECURRENCE-ID overrides (even if CANCELLED, so base occurrence gets suppressed!)
    if (ev.has_recurrence_id && !ev.uid.empty()) {
        const int64_t rk = ev.recurrence_id.is_date
            ? days_from_civil({ev.recurrence_id.dt.year, ev.recurrence_id.dt.month, ev.recurrence_id.dt.day})
            : resolve_to_utc(ev.recurrence_id);
        overridden_keys_.insert(make_override_key(ev.uid, rk));
    }

    if (ev.cancelled) return;

    const bool all_day = ev.dtstart.is_date;
    int64_t duration_sec = 0;
    if (all_day) {
        const CivilDate s{ev.dtstart.dt.year, ev.dtstart.dt.month, ev.dtstart.dt.day};
        if (ev.dtend.valid) {
            const CivilDate e{ev.dtend.dt.year, ev.dtend.dt.month, ev.dtend.dt.day};
            const int64_t d = days_from_civil(e) - days_from_civil(s);
            duration_sec = std::max<int64_t>(1, d) * 86400LL;
        } else if (ev.duration_sec > 0) {
            duration_sec = ev.duration_sec;
        } else {
            duration_sec = 86400LL;
        }
    } else {
        const int64_t s_utc = resolve_to_utc(ev.dtstart);
        if (ev.dtend.valid) {
            const int64_t e_utc = resolve_to_utc(ev.dtend);
            duration_sec = std::max<int64_t>(0, e_utc - s_utc);
        } else if (ev.duration_sec >= 0) {
            duration_sec = ev.duration_sec;
        } else {
            duration_sec = 0;
        }
    }

    if (ev.rrule.valid && !ev.has_recurrence_id) {
        expand_rrule(ev, duration_sec);
        return;
    }

    if (all_day) {
        const CivilDate s{ev.dtstart.dt.year, ev.dtstart.dt.month, ev.dtstart.dt.day};
        const int64_t s_day = days_from_civil(s);
        const int64_t span = std::max<int64_t>(1, duration_sec / 86400LL);
        emit_occurrence(ev, 0, 0, true, s, civil_from_days(s_day + span), s_day, ev.has_recurrence_id);
    } else {
        const int64_t s_utc = resolve_to_utc(ev.dtstart);
        emit_occurrence(ev, s_utc, s_utc + duration_sec, false,
                        {ev.dtstart.dt.year, ev.dtstart.dt.month, ev.dtstart.dt.day},
                        {ev.dtstart.dt.year, ev.dtstart.dt.month, ev.dtstart.dt.day},
                        s_utc, ev.has_recurrence_id);
    }
}

} // namespace note4c
