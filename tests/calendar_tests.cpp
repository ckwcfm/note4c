#include "note4c/calendar_config.hpp"
#include "note4c/calendar_renderer.hpp"
#include "note4c/font.hpp"
#include "note4c/ics_parser.hpp"
#include <cstdlib>
#include <iostream>

using namespace note4c;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; \
        std::exit(1); \
    } \
} while (0)

void test_config_roundtrip() {
    CalendarConfig cfg;
    cfg.wifi_ssid = "HomeWiFi";
    cfg.wifi_password = "supersecretpassword";
    cfg.timezone = "MST7";
    cfg.sync_hour = 6;
    cfg.sync_minute = 30;
    cfg.sync_interval_minutes = 240;
    cfg.calendars.push_back({"Work", "webcal://calendar.google.com/calendar/ical/abc/basic.ics", CalendarColor::Red, true});
    cfg.calendars.push_back({"Personal", "https://p01-caldav.icloud.com/published/2/xyz", CalendarColor::Yellow, true});
    cfg.calendars.push_back({"Holidays", "https://example.com/hol.ics", CalendarColor::Black, false});

    const auto blob = CalendarConfig::encode(cfg);
    CalendarConfig decoded;
    CHECK(CalendarConfig::decode(blob.data(), blob.size(), decoded));
    CHECK(decoded.wifi_ssid == "HomeWiFi");
    CHECK(decoded.wifi_password == "supersecretpassword");
    CHECK(decoded.timezone == "MST7");
    CHECK(decoded.sync_hour == 6 && decoded.sync_minute == 30);
    CHECK(decoded.sync_interval_minutes == 240);
    CHECK(decoded.calendars.size() == 3);
    CHECK(decoded.calendars[0].url == "https://calendar.google.com/calendar/ical/abc/basic.ics");
    CHECK(decoded.calendars[0].color == CalendarColor::Red);
    CHECK(decoded.calendars[1].color == CalendarColor::Yellow);
    CHECK(decoded.calendars[2].enabled == false);

    // Corrupted/truncated blobs must fail without crashing
    CHECK(!CalendarConfig::decode(blob.data(), blob.size() - 1, decoded));
    auto bad = blob;
    bad[0] ^= 0xFF;
    CHECK(!CalendarConfig::decode(bad.data(), bad.size(), decoded));
}

void test_next_sync_seconds() {
    // Once a day (1440 min) anchored at 05:00
    CHECK(CalendarConfig::seconds_until_next_sync(4, 0, 0, 5, 0, 1440) == 3600);
    CHECK(CalendarConfig::seconds_until_next_sync(5, 0, 0, 5, 0, 1440) == 86400);
    CHECK(CalendarConfig::seconds_until_next_sync(4, 59, 30, 5, 0, 1440) == 86430);
    CHECK(CalendarConfig::seconds_until_next_sync(23, 0, 0, 5, 0, 1440) == 6 * 3600);

    // Every 4 hours (240 min) anchored at 05:00 -> 01:00, 05:00, 09:00, 13:00, 17:00, 21:00
    CHECK(CalendarConfig::seconds_until_next_sync(14, 0, 0, 5, 0, 240) == 3 * 3600); // -> 17:00
    CHECK(CalendarConfig::seconds_until_next_sync(17, 0, 0, 5, 0, 240) == 4 * 3600); // -> 21:00
    CHECK(CalendarConfig::seconds_until_next_sync(22, 0, 0, 5, 0, 240) == 3 * 3600); // -> 01:00

    // Every 60 minutes anchored at 05:15 -> 14:15
    CHECK(CalendarConfig::seconds_until_next_sync(14, 0, 0, 5, 15, 60) == 15 * 60);
    CHECK(CalendarConfig::seconds_until_next_sync(14, 15, 0, 5, 15, 60) == 60 * 60);
}

void test_timezone_and_dates() {
    CHECK(weekday_sun0({2026, 11, 25}) == 3); // Wed
    const TzRule mt = TzRule::parse_posix("MST7MDT,M3.2.0,M11.1.0");
    const int64_t nov25_noon_utc = days_from_civil({2026, 11, 25}) * 86400LL + 19 * 3600;
    const auto loc_nov = mt.utc_to_local(nov25_noon_utc);
    CHECK(loc_nov.year == 2026 && loc_nov.month == 11 && loc_nov.day == 25 && loc_nov.hour == 12);
    const int64_t jul15_noon_utc = days_from_civil({2026, 7, 15}) * 86400LL + 18 * 3600;
    const auto loc_jul = mt.utc_to_local(jul15_noon_utc);
    CHECK(loc_jul.year == 2026 && loc_jul.month == 7 && loc_jul.day == 15 && loc_jul.hour == 12);
}

void test_ics_3day_streaming_rrule_and_overrides() {
    const TzRule mt = TzRule::parse_posix("MST7MDT,M3.2.0,M11.1.0");
    const std::string ics =
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        // 1. All-day multi-day event spanning Nov 25, Nov 26, and Nov 27
        "BEGIN:VEVENT\r\n"
        "UID:allday-1\r\n"
        "DTSTART;VALUE=DATE:20261125\r\n"
        "DTEND;VALUE=DATE:20261128\r\n"
        "SUMMARY:Thanksgiving Break\\, Family\r\n"
        "BEGIN:VALARM\r\n"
        "SUMMARY:ALARM SHOULD BE IGNORED\r\n"
        "DURATION:PT15M\r\n"
        "END:VALARM\r\n"
        "END:VEVENT\r\n"
        // 2. UTC event on Nov 25 09:30 MST (= 16:30 UTC) with folded summary
        "BEGIN:VEVENT\r\n"
        "UID:utc-1\r\n"
        "DTSTART:20261125T163000Z\r\n"
        "DTEND:20261125T173000Z\r\n"
        "SUMMARY:Design Review\r\n"
        "  with Hardware Team\r\n"
        "END:VEVENT\r\n"
        // 3. Weekly recurring event on Mon, Wed, Thu, Fri in America/New_York (10:00 EST = 08:00 MST)
        //    with EXDATE excluding Nov 25 -> appears on Thu Nov 26 and Fri Nov 27!
        "BEGIN:VEVENT\r\n"
        "UID:weekly-1\r\n"
        "DTSTART;TZID=America/New_York:20261102T100000\r\n"
        "DURATION:PT30M\r\n"
        "RRULE:FREQ=WEEKLY;BYDAY=MO,WE,TH,FR\r\n"
        "EXDATE;TZID=America/New_York:20261125T100000\r\n"
        "SUMMARY:Daily Standup\r\n"
        "END:VEVENT\r\n"
        // 4. Daily recurring event with RECURRENCE-ID override moving Nov 25 from 14:00 to 15:30
        "BEGIN:VEVENT\r\n"
        "UID:daily-ov\r\n"
        "DTSTART;TZID=America/Denver:20261120T140000\r\n"
        "DTEND;TZID=America/Denver:20261120T143000\r\n"
        "RRULE:FREQ=DAILY\r\n"
        "SUMMARY:Afternoon Sync\r\n"
        "END:VEVENT\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:daily-ov\r\n"
        "RECURRENCE-ID;TZID=America/Denver:20261125T140000\r\n"
        "DTSTART;TZID=America/Denver:20261125T153000\r\n"
        "DTEND;TZID=America/Denver:20261125T160000\r\n"
        "SUMMARY:Afternoon Sync (Moved)\r\n"
        "END:VEVENT\r\n"
        // 5. Cancelled event must not appear
        "BEGIN:VEVENT\r\n"
        "UID:cancel-1\r\n"
        "DTSTART:20261125T180000Z\r\n"
        "STATUS:CANCELLED\r\n"
        "SUMMARY:Cancelled Meeting\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n";

    IcsParser parser({2026, 11, 25}, mt, CalendarColor::Red, "Work");
    for (size_t i = 0; i < ics.size(); i += 7) {
        parser.feed(std::string_view(ics).substr(i, std::min<size_t>(7, ics.size() - i)));
    }
    parser.finish();

    const auto& today = parser.today_events();
    const auto& tomorrow = parser.tomorrow_events();
    const auto& day3 = parser.day3_events();

    CHECK(today.size() == 3);
    CHECK(today[0].all_day && today[0].title == "Thanksgiving Break, Family");
    CHECK(!today[1].all_day && today[1].start_minute == 9 * 60 + 30);
    CHECK(today[1].title == "Design Review with Hardware Team");
    CHECK(!today[2].all_day && today[2].start_minute == 15 * 60 + 30);
    CHECK(today[2].title == "Afternoon Sync (Moved)");

    CHECK(tomorrow.size() == 3);
    CHECK(tomorrow[0].all_day && tomorrow[0].title == "Thanksgiving Break, Family");
    CHECK(!tomorrow[1].all_day && tomorrow[1].start_minute == 8 * 60 && tomorrow[1].title == "Daily Standup");
    CHECK(!tomorrow[2].all_day && tomorrow[2].start_minute == 14 * 60 && tomorrow[2].title == "Afternoon Sync");

    CHECK(day3.size() == 3);
    CHECK(day3[0].all_day && day3[0].title == "Thanksgiving Break, Family");
    CHECK(!day3[1].all_day && day3[1].start_minute == 8 * 60 && day3[1].title == "Daily Standup");
    CHECK(!day3[2].all_day && day3[2].start_minute == 14 * 60 && day3[2].title == "Afternoon Sync");
}

void test_renderer_3day_smoke() {
    CalendarViewStatus st;
    st.today = {2026, 11, 25};
    st.sync_hour = 14;
    st.sync_minute = 5;
    st.wifi_connected = true;
    st.battery_percent = 84;
    st.legend = {
        {"Work", "https://example.com/a.ics", CalendarColor::Red, true},
        {"Personal", "https://example.com/b.ics", CalendarColor::Yellow, true},
        {"Family", "https://example.com/c.ics", CalendarColor::Black, true},
    };
    std::array<std::vector<CalendarEvent>, 3> days{};
    days[0] = {
        {"Morning Standup", "Work", CalendarColor::Red, false, 9 * 60 + 30, 10 * 60, false, "", 0, false},
        {"Dentist", "Family", CalendarColor::Black, false, 14 * 60, 15 * 60, false, "", 0, false},
    };
    days[1] = {
        {"Project Review", "Work", CalendarColor::Red, false, 15 * 60, 16 * 60, false, "", 0, false},
    };
    days[2] = {
        {"Weekend Trip", "Personal", CalendarColor::Yellow, true, 0, 0, false, "", 0, false},
        {"Dinner with Friends", "Personal", CalendarColor::Yellow, false, 18 * 60, 20 * 60, false, "", 0, false},
    };

    Frame f;
    CalendarRenderer renderer;
    renderer.draw_agenda(st, days, f);

    int counts[4]{};
    for (int y = 0; y < Frame::height; ++y)
        for (int x = 0; x < Frame::width; ++x)
            ++counts[static_cast<int>(f.pixel(x, y))];
    CHECK(counts[static_cast<int>(Color::White)] > 10000);
    CHECK(counts[static_cast<int>(Color::Black)] > 2000);
    CHECK(counts[static_cast<int>(Color::Red)] > 500);
    CHECK(counts[static_cast<int>(Color::Yellow)] > 500);

    Frame mf;
    renderer.draw_manage(true, "NOTE4C-Setup", "http://192.168.4.1", "AA:BB:CC:DD:EE:FF", "", mf);
    CHECK(mf != f);
}

int main() {
    test_config_roundtrip();
    test_next_sync_seconds();
    test_timezone_and_dates();
    test_ics_3day_streaming_rrule_and_overrides();
    test_renderer_3day_smoke();
    std::cout << "All note4c_calendar tests passed!\n";
    return 0;
}
