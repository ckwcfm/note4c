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

void draw_event_row(Frame& f, int y, const CalendarEvent& ev, bool compact, int more_badge = 0) {
    const Color bar_color = to_frame_color(ev.color);
    const int top = compact ? y : (y + 1);
    const int badge_h = compact ? 13 : 14;
    const int text_y = compact ? (y + 2) : (y + 4);
    // Left color indicator bar
    f.rect(10, top, 4, badge_h, bar_color);

    // Time badge (x=18..72, w=54)
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
    if (ev.color == CalendarColor::Yellow) {
        f.rect(badge_x, top, badge_w, badge_h, Color::Yellow);
        const int tw = font::measure_text(time_str, 1, true);
        font::draw_text(f, badge_x + (badge_w - tw) / 2, text_y, time_str, Color::Black, 1, true);
    } else if (ev.color == CalendarColor::Red) {
        f.rect(badge_x, top, badge_w, badge_h, Color::Red);
        const int tw = font::measure_text(time_str, 1, true);
        font::draw_text(f, badge_x + (badge_w - tw) / 2, text_y, time_str, Color::White, 1, true);
    } else {
        if (ev.all_day) {
            f.rect(badge_x, top, badge_w, badge_h, Color::Black);
            const int tw = font::measure_text(time_str, 1, true);
            font::draw_text(f, badge_x + (badge_w - tw) / 2, text_y, time_str, Color::White, 1, true);
        } else {
            f.rect(badge_x, top, badge_w, 1, Color::Black);
            f.rect(badge_x, top + badge_h - 1, badge_w, 1, Color::Black);
            f.rect(badge_x, top, 1, badge_h, Color::Black);
            f.rect(badge_x + badge_w - 1, top, 1, badge_h, Color::Black);
            const int tw = font::measure_text(time_str, 1, true);
            font::draw_text(f, badge_x + (badge_w - tw) / 2, text_y, time_str, Color::Black, 1, true);
        }
    }

    // Event title (x=80..390), reserving space on right for (+N) if single-row overflow
    const int title_x = 80;
    int max_w = Frame::width - title_x - 10;
    if (more_badge > 0) {
        char mbuf[16];
        std::snprintf(mbuf, sizeof(mbuf), "+%d", more_badge);
        const int mw = font::measure_text(mbuf, 1, true);
        font::draw_text(f, Frame::width - 10 - mw, text_y, mbuf, Color::Red, 1, true);
        max_w -= mw + 6;
    }
    const std::string clipped = font::truncate_text(ev.title, max_w, 1, false);
    font::draw_text(f, title_x, text_y, clipped, Color::Black, 1, false);
}

void draw_section_header(Frame& f, int y, std::string_view label,
                         std::string_view date_str, Color accent,
                         bool compact, bool empty_inline) {
    const int pill_w = font::measure_text(label, 1, true) + 10;
    const int pill_h = compact ? 13 : 14;
    const int text_y = compact ? (y + 2) : (y + 3);
    const int rule_y = compact ? (y + 14) : (y + 16);
    f.rect(10, y, pill_w, pill_h, accent);
    const Color fg = (accent == Color::Yellow) ? Color::Black : Color::White;
    font::draw_text(f, 15, text_y, label, fg, 1, true);
    font::draw_text(f, 10 + pill_w + 7, text_y, date_str, Color::Black, 1, true);
    if (empty_inline) {
        const std::string_view empty_lbl = "No events";
        const int ew = font::measure_text(empty_lbl, 1, false);
        font::draw_text(f, Frame::width - 10 - ew, text_y, empty_lbl, Color::Black, 1, false);
    }
    f.rect(10, rule_y, Frame::width - 20, 1, Color::Black);
}

} // namespace

void CalendarRenderer::draw_agenda(const CalendarViewStatus& status,
                                   const std::array<std::vector<CalendarEvent>, IcsParser::kMaxDays>& days,
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

    // Dynamic N-day (1..7) slot allocation in y=39..277 (238 px)
    const int num_days = std::clamp(status.display_days, 1, IcsParser::kMaxDays);
    const bool compact = (num_days >= 6);
    const int header_h = compact ? 16 : 19;
    const int row_h = compact ? 15 : 17;
    const int gap_h = compact ? 2 : 5;
    constexpr int kContentH = 238;

    const int headers_and_gaps = num_days * header_h + (num_days - 1) * gap_h;
    const int total_slots = std::max(num_days, (kContentH - headers_and_gaps) / row_h);

    int need[IcsParser::kMaxDays]{};
    int slots[IcsParser::kMaxDays]{};
    int allocated = 0;
    for (int i = 0; i < num_days; ++i) {
        const int ev_cnt = static_cast<int>(days[static_cast<size_t>(i)].size());
        if (compact && ev_cnt == 0) {
            // In 6-7 day view, empty days render "No events" inline in the header so busy days get more rows
            need[i] = 0;
            slots[i] = 0;
        } else {
            need[i] = std::max(1, ev_cnt);
            slots[i] = 1;
            ++allocated;
        }
    }
    int rem = total_slots - allocated;
    while (rem > 0) {
        bool progressed = false;
        for (int i = 0; i < num_days && rem > 0; ++i) {
            if (slots[i] < need[i]) {
                ++slots[i];
                --rem;
                progressed = true;
            }
        }
        if (!progressed) break;
    }

    const int64_t base_day = days_from_civil(status.today);
    int y = 39;
    for (int i = 0; i < num_days; ++i) {
        const CivilDate d = civil_from_days(base_day + i);
        const std::string label = (i == 0) ? "TODAY" : format_weekday_upper(d);
        const Color accent = (i == 0) ? Color::Red : (i == 1 ? Color::Yellow : Color::Black);
        std::string day_date;
        if (i == 0) {
            day_date = format_date_long(d);
        } else {
            char dbuf[16];
            std::snprintf(dbuf, sizeof(dbuf), "%s %d", month_short(d.month), d.day);
            day_date = dbuf;
        }
        const auto& evs = days[static_cast<size_t>(i)];
        const bool empty_inline = compact && evs.empty();
        draw_section_header(f, y, label, day_date, accent, compact, empty_inline);
        y += header_h;

        if (evs.empty()) {
            if (!empty_inline) {
                font::draw_text(f, 18, y + 4, "No events", Color::Black, 1, false);
                y += row_h;
            }
        } else {
            const bool overflow = static_cast<int>(evs.size()) > slots[i];
            if (overflow && (compact || slots[i] <= 1)) {
                const int show_count = std::max(1, slots[i]);
                for (int r = 0; r < show_count; ++r) {
                    const int badge = (r == show_count - 1) ? (static_cast<int>(evs.size()) - show_count) : 0;
                    draw_event_row(f, y, evs[static_cast<size_t>(r)], compact, badge);
                    y += row_h;
                }
            } else {
                const int show_count = overflow ? (slots[i] - 1) : static_cast<int>(evs.size());
                for (int r = 0; r < show_count; ++r) {
                    draw_event_row(f, y, evs[static_cast<size_t>(r)], compact);
                    y += row_h;
                }
                if (overflow) {
                    const int more = static_cast<int>(evs.size()) - show_count;
                    char mbuf[32];
                    std::snprintf(mbuf, sizeof(mbuf), "+%d more event%s", more, more == 1 ? "" : "s");
                    font::draw_text(f, 18, y + 4, mbuf, Color::Red, 1, true);
                    y += row_h;
                }
            }
        }
        y += gap_h;
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
