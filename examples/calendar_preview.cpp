#include "note4c/calendar_renderer.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <string>

using namespace note4c;

static void write_ppm(const Frame& f, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << Frame::width << " " << Frame::height << "\n255\n";
    for (int y = 0; y < Frame::height; ++y) {
        for (int x = 0; x < Frame::width; ++x) {
            unsigned char rgb[3];
            switch (f.pixel(x, y)) {
            case Color::Black:  rgb[0] = 20;  rgb[1] = 20;  rgb[2] = 20;  break;
            case Color::White:  rgb[0] = 248; rgb[1] = 248; rgb[2] = 245; break;
            case Color::Yellow: rgb[0] = 240; rgb[1] = 200; rgb[2] = 30;  break;
            case Color::Red:    rgb[0] = 195; rgb[1] = 35;  rgb[2] = 35;  break;
            }
            out.write(reinterpret_cast<const char*>(rgb), 3);
        }
    }
}

int main(int argc, char** argv) {
    const std::string out_dir = (argc > 1) ? argv[1] : ".";
    CalendarViewStatus st;
    st.today = {2026, 11, 25};
    st.sync_hour = 14;
    st.sync_minute = 5;
    st.wifi_connected = true;
    st.battery_percent = 84;
    st.legend = {
        {"Work", "https://example.com/a.ics", CalendarColor::Red, true},
        {"Sports", "https://example.com/b.ics", CalendarColor::RedOutline, true},
        {"Personal", "https://example.com/c.ics", CalendarColor::Yellow, true},
        {"School", "https://example.com/d.ics", CalendarColor::YellowOutline, true},
        {"Family", "https://example.com/e.ics", CalendarColor::Black, true},
        {"Other", "https://example.com/f.ics", CalendarColor::BlackOutline, true},
    };
    std::array<std::vector<CalendarEvent>, IcsParser::kMaxDays> days{};
    days[0] = {
        {"Engineering Standup", "Work", CalendarColor::Red, false, 9 * 60 + 30, 10 * 60, false, "", 0, false, "Conference Room B"},
        {"Lunch with Alex", "Family", CalendarColor::Black, false, 12 * 60, 13 * 60, false, "", 0, false, "Bistro on Main St"},
        {"Soccer Practice", "Sports", CalendarColor::RedOutline, false, 17 * 60 + 30, 19 * 60, false, "", 0, false, "Burnaby Lake Field 3"},
    };
    days[1] = {
        {"School Assembly", "School", CalendarColor::YellowOutline, false, 10 * 60, 11 * 60 + 30, false, "", 0, false, "Gymnasium"},
        {"Dentist Checkup", "Other", CalendarColor::BlackOutline, false, 14 * 60, 15 * 60, false, "", 0, false, ""},
    };
    days[2] = {
        {"Weekend Retreat", "Personal", CalendarColor::Yellow, true, 0, 0, false, "", 0, false, "Whistler Village"},
        {"Morning Hike", "Personal", CalendarColor::Yellow, false, 9 * 60, 11 * 60, false, "", 0, false, ""},
    };
    days[4] = {
        {"Sunday Brunch", "Family", CalendarColor::Black, false, 11 * 60, 12 * 60 + 30, false, "", 0, false, "Granville Island"},
    };
    days[6] = {
        {"Sprint Kickoff", "Work", CalendarColor::Red, false, 10 * 60, 11 * 60, false, "", 0, false, "HQ Room 201"},
        {"Team Sync", "Sports", CalendarColor::RedOutline, false, 15 * 60, 16 * 60, false, "", 0, false, ""},
    };

    CalendarRenderer renderer;
    Frame f_agenda, f_agenda_7d, f_manage, f_ota;
    st.display_days = 3;
    renderer.draw_agenda(st, days, f_agenda);
    st.display_days = 7;
    renderer.draw_agenda(st, days, f_agenda_7d);
    renderer.draw_manage(false, "HomeWiFi", "http://192.168.1.42", "AA:BB:CC:DD:EE:FF", "v0.2.4 (ota_0)", "Update v0.2.5 available! Hold UP to install", f_manage);
    renderer.draw_ota_progress("UPDATING FIRMWARE", "v0.2.2 -> v0.2.3 (ckwcfm/note4c)", "Downloading... 616 / 1120 KB", 55, f_ota);

    write_ppm(f_agenda, out_dir + "/calendar_agenda.ppm");
    write_ppm(f_agenda_7d, out_dir + "/calendar_agenda_7d.ppm");
    write_ppm(f_manage, out_dir + "/calendar_manage.ppm");
    write_ppm(f_ota, out_dir + "/calendar_ota.ppm");
    std::cout << "Wrote calendar previews to " << out_dir << "\n";
    return 0;
}
