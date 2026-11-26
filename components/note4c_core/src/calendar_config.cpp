#include "note4c/calendar_config.hpp"
#include <algorithm>
#include <cctype>

namespace note4c {
namespace {

constexpr uint32_t kMagic = 0x4E344343u; // "N4CC"

void put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void put_str(std::vector<uint8_t>& out, const std::string& s, size_t max_len) {
    const size_t n = std::min(s.size(), max_len);
    put_u16(out, static_cast<uint16_t>(n));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

bool get_u16(const uint8_t* data, size_t len, size_t& pos, uint16_t& out) {
    if (pos + 2 > len) return false;
    out = static_cast<uint16_t>(data[pos]) |
          static_cast<uint16_t>(static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    return true;
}

bool get_u32(const uint8_t* data, size_t len, size_t& pos, uint32_t& out) {
    if (pos + 4 > len) return false;
    out = 0;
    for (int i = 0; i < 4; ++i)
        out |= static_cast<uint32_t>(data[pos + i]) << (8 * i);
    pos += 4;
    return true;
}

bool get_str(const uint8_t* data, size_t len, size_t& pos, size_t max_len, std::string& out) {
    uint16_t n = 0;
    if (!get_u16(data, len, pos, n)) return false;
    if (n > max_len || pos + n > len) return false;
    out.assign(reinterpret_cast<const char*>(data + pos), n);
    pos += n;
    return true;
}

std::string trim(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start]))) ++start;
    size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) --end;
    return std::string(sv.substr(start, end - start));
}

} // namespace

std::string CalendarConfig::normalize_url(std::string_view url) {
    std::string s = trim(url);
    auto starts_with_ci = [](const std::string& str, const char* prefix) {
        size_t i = 0;
        while (prefix[i]) {
            if (i >= str.size()) return false;
            if (std::tolower(static_cast<unsigned char>(str[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
            ++i;
        }
        return true;
    };
    if (starts_with_ci(s, "webcal://")) {
        s = "https://" + s.substr(9);
    } else if (starts_with_ci(s, "webcals://")) {
        s = "https://" + s.substr(10);
    }
    if (s.size() > kMaxUrlLen) s.resize(kMaxUrlLen);
    return s;
}

void CalendarConfig::sanitize(CalendarConfig& cfg) {
    cfg.wifi_ssid = trim(cfg.wifi_ssid);
    if (cfg.wifi_ssid.size() > kMaxSsidLen) cfg.wifi_ssid.resize(kMaxSsidLen);
    if (cfg.wifi_password.size() > kMaxPassLen) cfg.wifi_password.resize(kMaxPassLen);
    cfg.timezone = trim(cfg.timezone);
    if (cfg.timezone.empty()) cfg.timezone = "UTC0";
    if (cfg.timezone.size() > kMaxTzLen) cfg.timezone.resize(kMaxTzLen);
    if (cfg.sync_hour > 23) cfg.sync_hour = 5;
    if (cfg.sync_minute > 59) cfg.sync_minute = 0;
    if (cfg.sync_interval_minutes < kMinIntervalMin) cfg.sync_interval_minutes = kMinIntervalMin;
    if (cfg.sync_interval_minutes > kMaxIntervalMin) cfg.sync_interval_minutes = kMaxIntervalMin;
    if (cfg.display_days < kMinDisplayDays) cfg.display_days = kMinDisplayDays;
    if (cfg.display_days > kMaxDisplayDays) cfg.display_days = kMaxDisplayDays;
    if (static_cast<uint8_t>(cfg.title_mode) > 2) cfg.title_mode = TitleMode::EventTitle;
    if (static_cast<uint8_t>(cfg.location_mode) > 2) cfg.location_mode = LocationMode::Full;
    if (cfg.calendars.size() > kMaxCalendars) cfg.calendars.resize(kMaxCalendars);
    for (auto& cal : cfg.calendars) {
        cal.name = trim(cal.name);
        if (cal.name.size() > kMaxNameLen) cal.name.resize(kMaxNameLen);
        cal.url = normalize_url(cal.url);
        if (cal.name.empty()) cal.name = "Calendar";
        if (static_cast<uint8_t>(cal.color) > 5) cal.color = CalendarColor::Black;
    }
}

std::vector<uint8_t> CalendarConfig::encode(const CalendarConfig& in) {
    CalendarConfig cfg = in;
    sanitize(cfg);
    std::vector<uint8_t> out;
    out.reserve(512);
    put_u32(out, kMagic);
    put_u32(out, kVersion);
    put_str(out, cfg.wifi_ssid, kMaxSsidLen);
    put_str(out, cfg.wifi_password, kMaxPassLen);
    put_str(out, cfg.timezone, kMaxTzLen);
    out.push_back(cfg.sync_hour);
    out.push_back(cfg.sync_minute);
    put_u16(out, cfg.sync_interval_minutes);
    out.push_back(cfg.display_days);
    const uint8_t flags =
        (cfg.use_12h ? 0x01u : 0u) |
        (cfg.show_end_time ? 0x02u : 0u) |
        (cfg.hide_empty_days ? 0x04u : 0u) |
        (cfg.hide_all_day ? 0x08u : 0u) |
        (cfg.show_footer ? 0x10u : 0u);
    const uint8_t modes =
        static_cast<uint8_t>((static_cast<uint8_t>(cfg.title_mode) & 0x0Fu) |
                             ((static_cast<uint8_t>(cfg.location_mode) & 0x0Fu) << 4));
    out.push_back(flags);
    out.push_back(modes);
    out.push_back(static_cast<uint8_t>(cfg.calendars.size()));
    for (const auto& cal : cfg.calendars) {
        put_str(out, cal.name, kMaxNameLen);
        put_str(out, cal.url, kMaxUrlLen);
        out.push_back(static_cast<uint8_t>(cal.color));
        out.push_back(cal.enabled ? 1 : 0);
    }
    return out;
}

bool CalendarConfig::decode(const uint8_t* data, size_t len, CalendarConfig& out) {
    if (!data || len < 11) return false;
    size_t pos = 0;
    uint32_t magic = 0, ver = 0;
    if (!get_u32(data, len, pos, magic) || magic != kMagic) return false;
    if (!get_u32(data, len, pos, ver) || ver < 1 || ver > kVersion) return false;
    CalendarConfig parsed;
    if (!get_str(data, len, pos, kMaxSsidLen, parsed.wifi_ssid)) return false;
    if (!get_str(data, len, pos, kMaxPassLen, parsed.wifi_password)) return false;
    if (!get_str(data, len, pos, kMaxTzLen, parsed.timezone)) return false;
    if (pos + 2 > len) return false;
    parsed.sync_hour = data[pos++];
    parsed.sync_minute = data[pos++];
    if (ver >= 2) {
        if (!get_u16(data, len, pos, parsed.sync_interval_minutes)) return false;
    } else {
        parsed.sync_interval_minutes = 1440;
    }
    if (ver >= 3) {
        if (pos + 1 > len) return false;
        parsed.display_days = data[pos++];
    } else {
        parsed.display_days = 3;
    }
    if (ver >= 4) {
        if (pos + 2 > len) return false;
        const uint8_t flags = data[pos++];
        const uint8_t modes = data[pos++];
        parsed.use_12h = (flags & 0x01u) != 0;
        parsed.show_end_time = (flags & 0x02u) != 0;
        parsed.hide_empty_days = (flags & 0x04u) != 0;
        parsed.hide_all_day = (flags & 0x08u) != 0;
        parsed.show_footer = (flags & 0x10u) != 0;
        const uint8_t tm = modes & 0x0Fu;
        const uint8_t lm = (modes >> 4) & 0x0Fu;
        if (tm > 2 || lm > 2) return false;
        parsed.title_mode = static_cast<TitleMode>(tm);
        parsed.location_mode = static_cast<LocationMode>(lm);
    } else {
        parsed.title_mode = TitleMode::EventTitle;
        parsed.location_mode = LocationMode::Full;
        parsed.use_12h = false;
        parsed.show_end_time = true;
        parsed.hide_empty_days = false;
        parsed.hide_all_day = false;
        parsed.show_footer = true;
    }
    if (pos + 1 > len) return false;
    const uint8_t count = data[pos++];
    if (parsed.sync_hour > 23 || parsed.sync_minute > 59 || count > kMaxCalendars) return false;
    if (parsed.sync_interval_minutes < kMinIntervalMin || parsed.sync_interval_minutes > kMaxIntervalMin)
        return false;
    if (parsed.display_days < kMinDisplayDays || parsed.display_days > kMaxDisplayDays)
        return false;
    parsed.calendars.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        CalendarSub sub;
        if (!get_str(data, len, pos, kMaxNameLen, sub.name)) return false;
        if (!get_str(data, len, pos, kMaxUrlLen, sub.url)) return false;
        if (pos + 2 > len) return false;
        const uint8_t c = data[pos++];
        const uint8_t en = data[pos++];
        if (c > 5 || en > 1) return false;
        sub.color = static_cast<CalendarColor>(c);
        sub.enabled = (en != 0);
        parsed.calendars.push_back(std::move(sub));
    }
    if (pos != len) return false;
    out = std::move(parsed);
    return true;
}

uint32_t CalendarConfig::seconds_until_next_sync(int cur_hour, int cur_min, int cur_sec,
                                                 uint8_t target_hour, uint8_t target_min,
                                                 uint16_t interval_minutes) {
    if (cur_hour < 0 || cur_hour > 23) cur_hour = 0;
    if (cur_min < 0 || cur_min > 59) cur_min = 0;
    if (cur_sec < 0 || cur_sec > 59) cur_sec = 0;
    const int interval_s = static_cast<int>(
        std::clamp<uint16_t>(interval_minutes, kMinIntervalMin, kMaxIntervalMin)) * 60;
    const int now_s = cur_hour * 3600 + cur_min * 60 + cur_sec;
    const int anchor_s = static_cast<int>(target_hour % 24) * 3600 +
                         static_cast<int>(target_min % 60) * 60;

    // Find smallest k (integer) such that (anchor_s + k * interval_s) >= now_s + 60
    // Start at anchor_s modulo interval_s, then find first slot >= now_s + 60.
    const int min_target = now_s + 60;
    int diff = (anchor_s - min_target) % interval_s;
    if (diff < 0) diff += interval_s;
    int wait_s = 60 + diff;
    if (wait_s < 60) wait_s = 60;
    if (wait_s > 86460) wait_s = 86460;
    return static_cast<uint32_t>(wait_s);
}

} // namespace note4c
