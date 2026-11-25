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
        {"Personal", "https://example.com/b.ics", CalendarColor::Yellow, true},
        {"Family", "https://example.com/c.ics", CalendarColor::Black, true},
    };
    std::array<std::vector<CalendarEvent>, 3> days{};
    days[0] = {
        {"Engineering Standup", "Work", CalendarColor::Red, false, 9 * 60 + 30, 10 * 60, false, "", 0, false},
        {"Lunch with Alex", "Family", CalendarColor::Black, false, 12 * 60, 13 * 60, false, "", 0, false},
        {"Evening Run", "Personal", CalendarColor::Yellow, false, 17 * 60 + 30, 18 * 60 + 30, false, "", 0, false},
    };
    days[1] = {
        {"School Holiday", "Family", CalendarColor::Black, true, 0, 0, false, "", 0, false},
        {"Design Review", "Work", CalendarColor::Red, false, 14 * 60, 15 * 60, false, "", 0, false},
    };
    days[2] = {
        {"Weekend Retreat", "Personal", CalendarColor::Yellow, true, 0, 0, false, "", 0, false},
        {"Morning Hike", "Personal", CalendarColor::Yellow, false, 9 * 60, 11 * 60, false, "", 0, false},
        {"Family Dinner", "Work", CalendarColor::Red, false, 18 * 60, 20 * 60, false, "", 0, false},
    };

    CalendarRenderer renderer;
    Frame f_agenda, f_manage;
    renderer.draw_agenda(st, days, f_agenda);
    renderer.draw_manage(true, "NOTE4C-Setup", "http://192.168.4.1", "AA:BB:CC:DD:EE:FF", "First-time setup", f_manage);

    write_ppm(f_agenda, out_dir + "/calendar_agenda.ppm");
    write_ppm(f_manage, out_dir + "/calendar_manage.ppm");
    std::cout << "Wrote 3-day previews to " << out_dir << "\n";
    return 0;
}
