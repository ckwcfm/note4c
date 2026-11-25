#include "note4c/calendar_renderer.hpp"
#include "note4c/font.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace note4c {
namespace {

std::string format_hhmm(int total_min) {
    if (total_min < 0) total_min = 0;
    if (total_min >= 24 * 60) total_min = 24 * 60 - 1;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", total_min / 60, total_min % 60);
    return buf;
}

std::string format_date_long(CivilDate d) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s, %s %d",
                  weekday_short(weekday_sun0(d)), month_short(d.month), d.day);
    return buf;
}

std::string format_weekday_upper(CivilDate d) {
    std::string s = weekday_short(weekday_sun0(d));
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// 13x10 crisp pixel-art Wi-Fi icon on dark header.
void draw_wifi_icon(Frame& f, int x, int y, bool connected) {
    static constexpr const char* kOn[10] = {
        "...XXXXXXX...",
        ".XX.......XX.",
        "X...........X",
        "...XXXXXXX...",
        "..X.......X..",
        "....XXXXX....",
        "...X.....X...",
        ".....XXX.....",
        "......X......",
        "......X......",
    };
    const Color fg = connected ? Color::White : Color::Red;
    for (int r = 0; r < 10; ++r) {
        for (int c = 0; c < 13; ++c) {
            if (kOn[r][c] == 'X') f.pixel(x + c, y + r, fg);
        }
    }
    if (!connected) {
        // Crisp red diagonal slash indicating offline
        for (int i = 0; i < 10; ++i) {
            f.pixel(x + 1 + i, y + 9 - i, Color::Red);
            f.pixel(x + 2 + i, y + 9 - i, Color::Red);
        }
    }
}

void draw_battery_icon(Frame& f, int x, int y, int pct, bool charging) {
    // 18x10 battery outline on dark header
    const Color border = (pct >= 0 && pct <= 20 && !charging) ? Color::Red : Color::White;
    f.rect(x, y, 16, 1, border);
    f.rect(x, y + 9, 16, 1, border);
    f.rect(x, y, 1, 10, border);
    f.rect(x + 15, y, 1, 10, border);
    f.rect(x + 16, y + 3, 2, 4, border);
    if (pct >= 0) {
        const int fill = std::clamp((pct * 12 + 50) / 100, 1, 12);
        const Color col = charging ? Color::Yellow : (pct <= 20 ? Color::Red : Color::White);
        f.rect(x + 2, y + 2, fill, 6, col);
    } else if (charging) {
        f.rect(x + 2, y + 2, 12, 6, Color::Yellow);
    }
}

void draw_event_row(Frame& f, int y, const CalendarEvent& ev) {
    const Color bar_color = to_frame_color(ev.color);
    // Left color indicator bar (4x14)
    f.rect(10, y + 1, 4, 14, bar_color);

    // Time badge (x=18..72, w=54, h=14)
    std::string time_str;
    if (ev.all_day) {
        time_str = "ALL DAY";
    } else if (ev.continues_from_prev) {
        time_str = "CONT.";
    } else {
        time_str = format_hhmm(ev.start_minute);
    }

    const int badge_x = 18;
    const int badge_w = 54;
    const int badge_h = 14;
    if (ev.color == CalendarColor::Yellow) {
        f.rect(badge_x, y + 1, badge_w, badge_h, Color::Yellow);
        const int tw = font::measure_text(time_str, 1, true);
        font::draw_text(f, badge_x + (badge_w - tw) / 2, y + 4, time_str, Color::Black, 1, true);
    } else if (ev.color == CalendarColor::Red) {
        f.rect(badge_x, y + 1, badge_w, badge_h, Color::Red);
        const int tw = font::measure_text(time_str, 1, true);
        font::draw_text(f, badge_x + (badge_w - tw) / 2, y + 4, time_str, Color::White, 1, true);
    } else {
        if (ev.all_day) {
            f.rect(badge_x, y + 1, badge_w, badge_h, Color::Black);
            const int tw = font::measure_text(time_str, 1, true);
            font::draw_text(f, badge_x + (badge_w - tw) / 2, y + 4, time_str, Color::White, 1, true);
        } else {
            f.rect(badge_x, y + 1, badge_w, 1, Color::Black);
            f.rect(badge_x, y + badge_h, badge_w, 1, Color::Black);
            f.rect(badge_x, y + 1, 1, badge_h, Color::Black);
            f.rect(badge_x + badge_w - 1, y + 1, 1, badge_h, Color::Black);
            const int tw = font::measure_text(time_str, 1, true);
            font::draw_text(f, badge_x + (badge_w - tw) / 2, y + 4, time_str, Color::Black, 1, true);
        }
    }

    // Event title (x=80..390)
    const int title_x = 80;
    const int max_w = Frame::width - title_x - 10;
    const std::string clipped = font::truncate_text(ev.title, max_w, 1, false);
    font::draw_text(f, title_x, y + 4, clipped, Color::Black, 1, false);
}

void draw_section_header(Frame& f, int y, std::string_view label,
                         std::string_view date_str, Color accent) {
    const int pill_w = font::measure_text(label, 1, true) + 10;
    f.rect(10, y, pill_w, 14, accent);
    const Color fg = (accent == Color::Yellow) ? Color::Black : Color::White;
    font::draw_text(f, 15, y + 3, label, fg, 1, true);
    font::draw_text(f, 10 + pill_w + 7, y + 3, date_str, Color::Black, 1, true);
    f.rect(10, y + 16, Frame::width - 20, 1, Color::Black);
}

} // namespace

void CalendarRenderer::draw_agenda(const CalendarViewStatus& status,
                                   const std::array<std::vector<CalendarEvent>, IcsParser::kNumDays>& days,
                                   Frame& f) const {
    f.clear(Color::White);

    // Top Header Bar (y=0..33)
    f.rect(0, 0, Frame::width, 32, Color::Black);
    f.rect(0, 32, Frame::width, 2, Color::Red);

    const std::string header_date = format_date_long(status.today);
    font::draw_text(f, 10, 7, header_date, Color::White, 2, true);

    // Right side of header (right-to-left): Battery icon + %, Wi-Fi icon, Sync time / error
    int right_x = Frame::width - 8;
    if (status.battery_percent >= 0 || status.charging) {
        right_x -= 18;
        draw_battery_icon(f, right_x, 11, status.battery_percent, status.charging);
        if (status.battery_percent >= 0) {
            char pbuf[12];
            std::snprintf(pbuf, sizeof(pbuf), "%d%%", std::clamp(status.battery_percent, 0, 100));
            const int pw = font::measure_text(pbuf, 1, true);
            right_x -= pw + 4;
            const Color pc = status.charging ? Color::Yellow
                           : (status.battery_percent <= 20 ? Color::Red : Color::White);
            font::draw_text(f, right_x, 12, pbuf, pc, 1, true);
        }
        right_x -= 8;
    }

    // Wi-Fi status icon
    right_x -= 13;
    draw_wifi_icon(f, right_x, 11, status.wifi_connected);
    right_x -= 8;

    // Sync time or error badge
    if (!status.error_text.empty()) {
        const int ew = font::measure_text(status.error_text, 1, true) + 6;
        right_x -= ew;
        f.rect(right_x, 9, ew, 14, Color::Red);
        font::draw_text(f, right_x + 3, 12, status.error_text, Color::White, 1, true);
    } else if (status.sync_hour >= 0) {
        char sbuf[16];
        std::snprintf(sbuf, sizeof(sbuf), "%02d:%02d", status.sync_hour, status.sync_minute);
        const int sw = font::measure_text(sbuf, 1, false);
        right_x -= sw;
        font::draw_text(f, right_x, 12, sbuf, Color::Yellow, 1, false);
    }

    // Dynamic 3-day slot allocation in y=38..276 (238 px)
    // 3 headers x 19px = 57px, 2 section gaps x 5px = 10px -> 171px for rows (17px/row = 10 slots)
    constexpr int kRowH = 17;
    constexpr int kHeaderH = 19;
    constexpr int kGapH = 5;
    constexpr int kTotalSlots = 10;

    int need[3];
    int slots[3] = {1, 1, 1};
    for (int i = 0; i < 3; ++i) {
        need[i] = std::max(1, static_cast<int>(days[static_cast<size_t>(i)].size()));
    }
    int rem = kTotalSlots - 3;
    while (rem > 0) {
        bool progressed = false;
        for (int i = 0; i < 3 && rem > 0; ++i) {
            if (slots[i] < need[i]) {
                ++slots[i];
                --rem;
                progressed = true;
            }
        }
        if (!progressed) break;
    }

    const int64_t base_day = days_from_civil(status.today);
    const CivilDate dates[3] = {
        status.today,
        civil_from_days(base_day + 1),
        civil_from_days(base_day + 2),
    };
    const std::string day2_label = format_weekday_upper(dates[1]);
    const std::string day3_label = format_weekday_upper(dates[2]);
    const std::string_view labels[3] = {"TODAY", day2_label, day3_label};
    const Color accents[3] = {Color::Red, Color::Yellow, Color::Black};

    int y = 39;
    for (int i = 0; i < 3; ++i) {
        std::string day_date;
        if (i == 0) {
            day_date = format_date_long(dates[i]);
        } else {
            char dbuf[16];
            std::snprintf(dbuf, sizeof(dbuf), "%s %d", month_short(dates[i].month), dates[i].day);
            day_date = dbuf;
        }
        draw_section_header(f, y, labels[i], day_date, accents[i]);
        y += kHeaderH;
        const auto& evs = days[static_cast<size_t>(i)];
        if (evs.empty()) {
            font::draw_text(f, 18, y + 4, "No events", Color::Black, 1, false);
            y += kRowH;
        } else {
            const bool overflow = static_cast<int>(evs.size()) > slots[i];
            const int show_count = overflow ? std::max(1, slots[i] - 1) : static_cast<int>(evs.size());
            for (int r = 0; r < show_count; ++r) {
                draw_event_row(f, y, evs[static_cast<size_t>(r)]);
                y += kRowH;
            }
            if (overflow) {
                const int more = static_cast<int>(evs.size()) - show_count;
                char mbuf[32];
                std::snprintf(mbuf, sizeof(mbuf), "+%d more event%s", more, more == 1 ? "" : "s");
                font::draw_text(f, 18, y + 4, mbuf, Color::Red, 1, true);
                y += kRowH;
            }
        }
        y += kGapH;
    }

    // Footer bar (y=278..299)
    f.rect(0, 278, Frame::width, 1, Color::Black);
    const std::string_view hint = "[OK] Sync  [DN] Manage";
    const int hint_w = font::measure_text(hint, 1, false);
    const int hint_x = Frame::width - hint_w - 10;
    font::draw_text(f, hint_x, 285, hint, Color::Black, 1, false);

    // Calendar color legend on left of footer
    int lx = 10;
    for (const auto& cal : status.legend) {
        if (!cal.enabled) continue;
        const std::string short_name = font::truncate_text(cal.name, 70, 1, false);
        const int item_w = 10 + font::measure_text(short_name, 1, false) + 10;
        if (lx + item_w > hint_x - 8) break;
        const Color c = to_frame_color(cal.color);
        f.rect(lx, 286, 7, 7, c);
        if (cal.color == CalendarColor::Yellow) {
            f.rect(lx, 286, 7, 1, Color::Black);
            f.rect(lx, 292, 7, 1, Color::Black);
            f.rect(lx, 286, 1, 7, Color::Black);
            f.rect(lx + 6, 286, 1, 7, Color::Black);
        }
        font::draw_text(f, lx + 10, 285, short_name, Color::Black, 1, false);
        lx += item_w;
    }
}

void CalendarRenderer::draw_manage(bool is_ap_mode,
                                   std::string_view ssid,
                                   std::string_view url,
                                   std::string_view mac,
                                   std::string_view version,
                                   std::string_view detail,
                                   Frame& f) const {
    f.clear(Color::White);
    const Color banner = is_ap_mode ? Color::Red : Color::Black;
    f.rect(0, 0, Frame::width, 42, banner);
    f.rect(0, 42, Frame::width, 4, Color::Yellow);

    const std::string_view title = is_ap_mode ? "WIFI & CALENDAR SETUP" : "MANAGE CALENDARS";
    font::draw_text(f, 16, 12, title, Color::White, 2, true);

    if (is_ap_mode) {
        font::draw_text(f, 16, 56, "1. Connect your phone or computer to Wi-Fi:", Color::Black, 1, true);
        f.rect(16, 72, Frame::width - 32, 28, Color::Yellow);
        font::draw_text(f, 26, 78, ssid, Color::Black, 2, true);

        font::draw_text(f, 16, 110, "2. Open this address in your browser:", Color::Black, 1, true);
        f.rect(16, 126, Frame::width - 32, 28, Color::Black);
        font::draw_text(f, 26, 132, url, Color::White, 2, true);

        font::draw_text(f, 16, 164, "Add your Wi-Fi, .ics calendar URLs, and colors.", Color::Black, 1, false);
    } else {
        font::draw_text(f, 16, 56, "Connected to Wi-Fi:", Color::Black, 1, true);
        font::draw_text(f, 140, 56, ssid, Color::Red, 1, true);

        font::draw_text(f, 16, 78, "Open this page in your browser to manage calendars:", Color::Black, 1, true);
        f.rect(16, 94, Frame::width - 32, 34, Color::Yellow);
        font::draw_text(f, 26, 102, url, Color::Black, 2, true);

        font::draw_text(f, 16, 140, "Add/remove .ics subscriptions, pick Black/Red/Yellow", Color::Black, 1, false);
        font::draw_text(f, 16, 156, "colors, change Wi-Fi, or install firmware updates.", Color::Black, 1, false);
    }

    if (!mac.empty()) {
        font::draw_text(f, 16, 186, "Wi-Fi MAC:", Color::Black, 1, true);
        font::draw_text(f, 92, 186, mac, Color::Black, 1, true);
    }
    if (!version.empty()) {
        font::draw_text(f, 16, 204, "Firmware:", Color::Black, 1, true);
        font::draw_text(f, 92, 204, version, Color::Black, 1, true);
    }

    if (!detail.empty()) {
        font::draw_text(f, 16, 230, font::truncate_text(detail, Frame::width - 32, 1, true), Color::Red, 1, true);
    }

    f.rect(0, 264, Frame::width, 1, Color::Black);
    font::draw_text(f, 16, 274, "[OK] Exit   [UP] Update   [Hold DN] Hotspot", Color::Black, 1, true);
}

void CalendarRenderer::draw_message(std::string_view title,
                                    std::string_view line1,
                                    std::string_view line2,
                                    Color accent,
                                    Frame& f) const {
    f.clear(Color::White);
    f.rect(0, 0, Frame::width, 46, accent);
    const Color fg = (accent == Color::Yellow) ? Color::Black : Color::White;
    font::draw_text(f, 16, 14, title, fg, 2, true);
    font::draw_text(f, 16, 84, font::truncate_text(line1, Frame::width - 32, 1, true), Color::Black, 1, true);
    if (!line2.empty()) {
        font::draw_text(f, 16, 112, font::truncate_text(line2, Frame::width - 32, 1, false), Color::Black, 1, false);
    }
    f.rect(0, 264, Frame::width, 1, Color::Black);
    font::draw_text(f, 16, 274, "[OK] Sync Now   [DN] Manage Web UI", Color::Black, 1, true);
}

} // namespace note4c
