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

void draw_pin_icon(Frame& f, int x, int y, Color c) {
    static constexpr const char* kPin[7] = {
        ".XXX.",
        "X...X",
        "X.X.X",
        "X...X",
        ".XXX.",
        "..X..",
        "..X..",
    };
    for (int r = 0; r < 7; ++r) {
        for (int col = 0; col < 5; ++col) {
            if (kPin[r][col] == 'X') f.pixel(x + col, y + r, c);
        }
    }
}

std::string format_time_single(int total_min, bool use_12h) {
    if (total_min < 0) total_min = 0;
    if (total_min >= 24 * 60) total_min = 24 * 60 - 1;
    const int h24 = total_min / 60;
    const int m = total_min % 60;
    char buf[16];
    if (!use_12h) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", h24, m);
    } else {
        const int h12 = (h24 % 12 == 0) ? 12 : (h24 % 12);
        std::snprintf(buf, sizeof(buf), "%d:%02d%c", h12, m, h24 < 12 ? 'a' : 'p');
    }
    return buf;
}

std::string format_event_time(const CalendarEvent& ev, bool use_12h, bool show_end_time) {
    if (ev.all_day) return "ALL DAY";
    if (ev.continues_from_prev) {
        if (show_end_time && ev.end_minute > 0 && ev.end_minute < 24 * 60) {
            return "->" + format_time_single(ev.end_minute, use_12h);
        }
        return "CONT.";
    }
    if (!show_end_time || ev.end_minute <= ev.start_minute) {
        return format_time_single(ev.start_minute, use_12h);
    }
    if (!use_12h) {
        const std::string st = format_hhmm(ev.start_minute);
        if (ev.end_minute >= 24 * 60) return st + "-24:00";
        return st + "-" + format_hhmm(ev.end_minute);
    }
    // Compact 12-hour range: omit duplicate a/p when start & end share meridiem (e.g. "2:00-3:30p")
    const int sh24 = std::clamp(ev.start_minute, 0, 1439) / 60;
    const int sm = std::clamp(ev.start_minute, 0, 1439) % 60;
    const int sh12 = (sh24 % 12 == 0) ? 12 : (sh24 % 12);
    char buf[24];
    if (ev.end_minute >= 24 * 60) {
        std::snprintf(buf, sizeof(buf), "%d:%02d%c-12a", sh12, sm, sh24 < 12 ? 'a' : 'p');
        return buf;
    }
    const int eh24 = std::clamp(ev.end_minute, 0, 1439) / 60;
    const int em = std::clamp(ev.end_minute, 0, 1439) % 60;
    const int eh12 = (eh24 % 12 == 0) ? 12 : (eh24 % 12);
    if ((sh24 < 12) == (eh24 < 12)) {
        std::snprintf(buf, sizeof(buf), "%d:%02d-%d:%02d%c", sh12, sm, eh12, em, eh24 < 12 ? 'a' : 'p');
    } else {
        std::snprintf(buf, sizeof(buf), "%d:%02d%c-%d:%02d%c",
                      sh12, sm, sh24 < 12 ? 'a' : 'p', eh12, em, eh24 < 12 ? 'a' : 'p');
    }
    return buf;
}

std::string format_event_location(const CalendarEvent& ev, LocationMode mode) {
    if (mode == LocationMode::Off || ev.location.empty()) return {};
    if (mode == LocationMode::ShortVenue) {
        const size_t comma = ev.location.find(',');
        if (comma != std::string::npos && comma > 0) {
            size_t end = comma;
            while (end > 0 && ev.location[end - 1] == ' ') --end;
            if (end > 0) return ev.location.substr(0, end);
        }
    }
    return ev.location;
}

std::string format_event_title(const CalendarEvent& ev, TitleMode mode) {
    switch (mode) {
    case TitleMode::CalendarName:
        return ev.calendar_name.empty() ? ev.title : ev.calendar_name;
    case TitleMode::Hidden:
        return {};
    case TitleMode::EventTitle:
    default:
        return ev.title;
    }
}

void draw_event_row(Frame& f, int y, const CalendarEvent& ev, const CalendarViewStatus& status,
                    bool compact, bool expand_loc, int more_badge = 0) {
    const Color accent = to_frame_color(ev.color);
    const bool outlined = is_outline_color(ev.color);
    const int top = compact ? y : (y + 1);
    const int badge_h = compact ? 13 : 14;
    const int text_y = compact ? (y + 2) : (y + 4);

    // Time pill supporting 24h/12h, optional end time, and 6 solid/outlined styles
    const std::string time_str = format_event_time(ev, status.use_12h, status.show_end_time);
    const int badge_x = 15;
    const int badge_w = status.show_end_time ? (status.use_12h ? 86 : 80) : 54;
    const bool bold_time = (time_str.size() <= 7) ||
                           (outlined && accent == Color::Yellow && time_str.size() <= 11);
    const int tw = font::measure_text(time_str, 1, bold_time);
    const int tx = badge_x + std::max(2, (badge_w - tw) / 2);

    if (!outlined) {
        f.rect(badge_x, top, badge_w, badge_h, accent);
        const Color fg = (accent == Color::Yellow) ? Color::Black : Color::White;
        font::draw_text(f, tx, text_y, time_str, fg, 1, bold_time);
    } else if (accent == Color::Yellow) {
        // Yellow outline with matching bold Yellow text
        f.rect(badge_x, top, badge_w, 2, Color::Yellow);
        f.rect(badge_x, top + badge_h - 2, badge_w, 2, Color::Yellow);
        f.rect(badge_x, top, 2, badge_h, Color::Yellow);
        f.rect(badge_x + badge_w - 2, top, 2, badge_h, Color::Yellow);
        font::draw_text(f, tx, text_y, time_str, Color::Yellow, 1, bold_time);
    } else {
        // BlackOutline or RedOutline: crisp 1px colored border + matching colored text
        f.rect(badge_x, top, badge_w, 1, accent);
        f.rect(badge_x, top + badge_h - 1, badge_w, 1, accent);
        f.rect(badge_x, top, 1, badge_h, accent);
        f.rect(badge_x + badge_w - 1, top, 1, badge_h, accent);
        font::draw_text(f, tx, text_y, time_str, accent, 1, bold_time);
    }

    // Event title & location area
    const int title_x = badge_x + badge_w + 6;
    int max_w = Frame::width - title_x - 10;
    if (more_badge > 0) {
        char mbuf[16];
        std::snprintf(mbuf, sizeof(mbuf), "+%d", more_badge);
        const int mw = font::measure_text(mbuf, 1, true);
        font::draw_text(f, Frame::width - 10 - mw, text_y, mbuf, Color::Red, 1, true);
        max_w -= mw + 6;
    }

    const std::string title = format_event_title(ev, status.title_mode);
    const std::string loc = format_event_location(ev, status.location_mode);
    const Color pin_c = (accent == Color::Red) ? Color::Red : Color::Black;

    if (title.empty()) {
        // Title hidden -> render location directly on line 1 with pin icon if available
        if (!loc.empty()) {
            draw_pin_icon(f, title_x, text_y + 1, pin_c);
            const std::string loc_clipped = font::truncate_text(loc, std::max(0, max_w - 9), 1, false);
            font::draw_text(f, title_x + 9, text_y, loc_clipped, Color::Black, 1, false);
        }
        return;
    }

    if (!expand_loc && !loc.empty()) {
        // Fallback inline location when vertical space is full
        const std::string combined = title + " @ " + loc;
        const std::string clipped = font::truncate_text(combined, max_w, 1, false);
        font::draw_text(f, title_x, text_y, clipped, Color::Black, 1, false);
    } else {
        const std::string clipped = font::truncate_text(title, max_w, 1, false);
        font::draw_text(f, title_x, text_y, clipped, Color::Black, 1, false);
    }

    // Adaptive 2nd detail line for location indented cleanly under the title
    if (expand_loc && !loc.empty()) {
        const int loc_y = y + (compact ? 14 : 16);
        draw_pin_icon(f, title_x, loc_y + 1, pin_c);
        const int loc_max_w = Frame::width - (title_x + 9) - 10;
        const std::string loc_clipped = font::truncate_text(loc, loc_max_w, 1, false);
        font::draw_text(f, title_x + 9, loc_y, loc_clipped, Color::Black, 1, false);
    }
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
        if (!status.use_12h) {
            std::snprintf(sbuf, sizeof(sbuf), "%02d:%02d", status.sync_hour, status.sync_minute);
        } else {
            const int h12 = (status.sync_hour % 12 == 0) ? 12 : (status.sync_hour % 12);
            std::snprintf(sbuf, sizeof(sbuf), "%d:%02d%s", h12, status.sync_minute,
                          status.sync_hour < 12 ? "AM" : "PM");
        }
        const int sw = font::measure_text(sbuf, 1, false);
        right_x -= sw;
        font::draw_text(f, right_x, 12, sbuf, Color::Yellow, 1, false);
    }

    // Build visible day list (applying hide_all_day and hide_empty_days filters)
    const int num_days = std::clamp(status.display_days, 1, IcsParser::kMaxDays);
    struct VisibleDay {
        int day_offset = 0;
        std::vector<CalendarEvent> events;
    };
    std::vector<VisibleDay> vis_days;
    vis_days.reserve(static_cast<size_t>(num_days));
    for (int i = 0; i < num_days; ++i) {
        VisibleDay vd;
        vd.day_offset = i;
        for (const auto& ev : days[static_cast<size_t>(i)]) {
            if (status.hide_all_day && ev.all_day) continue;
            vd.events.push_back(ev);
        }
        if (status.hide_empty_days && vd.events.empty()) continue;
        vis_days.push_back(std::move(vd));
    }
    const bool all_hidden_empty = vis_days.empty();
    if (all_hidden_empty) {
        vis_days.push_back(VisibleDay{0, {}});
    }

    const int vis_count = static_cast<int>(vis_days.size());
    const bool compact = (vis_count >= 6);
    const int header_h = compact ? 16 : 19;
    const int row_h = compact ? 15 : 17;
    const int loc_h = compact ? 11 : 12;
    const int gap_h = compact ? 2 : 5;
    const int content_h = status.show_footer ? 238 : 256;

    const int headers_and_gaps = vis_count * header_h + (vis_count - 1) * gap_h;
    const int total_slots = std::max(vis_count, (content_h - headers_and_gaps) / row_h);

    int need[IcsParser::kMaxDays]{};
    int slots[IcsParser::kMaxDays]{};
    int allocated = 0;
    for (int i = 0; i < vis_count; ++i) {
        const int ev_cnt = static_cast<int>(vis_days[static_cast<size_t>(i)].events.size());
        if (compact && ev_cnt == 0) {
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
        for (int i = 0; i < vis_count && rem > 0; ++i) {
            if (slots[i] < need[i]) {
                ++slots[i];
                --rem;
                progressed = true;
            }
        }
        if (!progressed) break;
    }

    // Pre-compute visible event counts & spare vertical pixels for Adaptive 2-Line Locations
    int show_cnt[IcsParser::kMaxDays]{};
    bool has_more_row[IcsParser::kMaxDays]{};
    bool inline_badge[IcsParser::kMaxDays]{};
    int used_px = headers_and_gaps;

    for (int i = 0; i < vis_count; ++i) {
        const auto& evs = vis_days[static_cast<size_t>(i)].events;
        if (evs.empty()) {
            if (!compact) used_px += row_h;
        } else {
            const bool overflow = static_cast<int>(evs.size()) > slots[i];
            if (overflow && (compact || slots[i] <= 1)) {
                show_cnt[i] = std::max(1, slots[i]);
                inline_badge[i] = true;
                used_px += show_cnt[i] * row_h;
            } else {
                show_cnt[i] = overflow ? (slots[i] - 1) : static_cast<int>(evs.size());
                has_more_row[i] = overflow;
                used_px += (show_cnt[i] + (overflow ? 1 : 0)) * row_h;
            }
        }
    }

    int spare_px = std::max(0, content_h - used_px);
    std::array<std::vector<bool>, IcsParser::kMaxDays> expand_loc{};
    for (int i = 0; i < vis_count; ++i) {
        expand_loc[static_cast<size_t>(i)].assign(static_cast<size_t>(show_cnt[i]), false);
        if (status.title_mode == TitleMode::Hidden || status.location_mode == LocationMode::Off) continue;
        const auto& evs = vis_days[static_cast<size_t>(i)].events;
        for (int r = 0; r < show_cnt[i]; ++r) {
            if (!format_event_location(evs[static_cast<size_t>(r)], status.location_mode).empty() &&
                spare_px >= loc_h) {
                expand_loc[static_cast<size_t>(i)][static_cast<size_t>(r)] = true;
                spare_px -= loc_h;
            }
        }
    }

    const int64_t base_day = days_from_civil(status.today);
    int y = 39;
    for (int i = 0; i < vis_count; ++i) {
        const int off = vis_days[static_cast<size_t>(i)].day_offset;
        const CivilDate d = civil_from_days(base_day + off);
        const std::string label = (off == 0) ? "TODAY" : format_weekday_upper(d);
        const Color accent = (off == 0) ? Color::Red : (off == 1 ? Color::Yellow : Color::Black);
        std::string day_date;
        if (off == 0) {
            day_date = format_date_long(d);
        } else {
            char dbuf[16];
            std::snprintf(dbuf, sizeof(dbuf), "%s %d", month_short(d.month), d.day);
            day_date = dbuf;
        }
        const auto& evs = vis_days[static_cast<size_t>(i)].events;
        const bool empty_inline = compact && evs.empty();
        const int rule_y = compact ? (y + 14) : (y + 16);
        draw_section_header(f, y, label, day_date, accent, compact, empty_inline);
        y += header_h;

        int group_bot = rule_y;
        if (evs.empty()) {
            if (!empty_inline) {
                font::draw_text(f, 16, y + 4,
                                all_hidden_empty ? "No upcoming events" : "No events",
                                Color::Black, 1, false);
                y += row_h;
                group_bot = y - 2;
            }
        } else {
            for (int r = 0; r < show_cnt[i]; ++r) {
                const int badge = (inline_badge[i] && r == show_cnt[i] - 1)
                                ? (static_cast<int>(evs.size()) - show_cnt[i]) : 0;
                const bool exp = expand_loc[static_cast<size_t>(i)][static_cast<size_t>(r)];
                draw_event_row(f, y, evs[static_cast<size_t>(r)], status, compact, exp, badge);
                y += row_h + (exp ? loc_h : 0);
            }
            if (has_more_row[i]) {
                const int more = static_cast<int>(evs.size()) - show_cnt[i];
                char mbuf[32];
                std::snprintf(mbuf, sizeof(mbuf), "+%d more event%s", more, more == 1 ? "" : "s");
                font::draw_text(f, 16, y + 4, mbuf, Color::Red, 1, true);
                y += row_h;
            }
            group_bot = y - (compact ? 2 : 3);
        }
        // Black vertical rail dropping cleanly from the day's horizontal divider to wrap its events
        if (group_bot > rule_y) {
            f.rect(10, rule_y, 1, group_bot - rule_y + 1, Color::Black);
        }
        y += gap_h;
    }

    // Optional Footer bar (y=278..299)
    if (status.show_footer) {
        f.rect(0, 278, Frame::width, 1, Color::Black);
        const std::string_view hint = "[OK] Sync  [DN] Manage";
        const int hint_w = font::measure_text(hint, 1, false);
        const int hint_x = Frame::width - hint_w - 10;
        font::draw_text(f, hint_x, 285, hint, Color::Black, 1, false);

        // Calendar color legend on left of footer (solid vs outlined matching 6 styles)
        int lx = 10;
        for (const auto& cal : status.legend) {
            if (!cal.enabled) continue;
            const std::string short_name = font::truncate_text(cal.name, 62, 1, false);
            const int item_w = 10 + font::measure_text(short_name, 1, false) + 8;
            if (lx + item_w > hint_x - 8) break;
            const Color c = to_frame_color(cal.color);
            if (is_outline_color(cal.color)) {
                const int bw = (c == Color::Yellow) ? 2 : 1;
                f.rect(lx, 286, 7, bw, c);
                f.rect(lx, 293 - bw, 7, bw, c);
                f.rect(lx, 286, bw, 7, c);
                f.rect(lx + 7 - bw, 286, bw, 7, c);
            } else {
                f.rect(lx, 286, 7, 7, c);
                if (cal.color == CalendarColor::Yellow) {
                    f.rect(lx, 286, 7, 1, Color::Black);
                    f.rect(lx, 292, 7, 1, Color::Black);
                    f.rect(lx, 286, 1, 7, Color::Black);
                    f.rect(lx + 6, 286, 1, 7, Color::Black);
                }
            }
            font::draw_text(f, lx + 10, 285, short_name, Color::Black, 1, false);
            lx += item_w;
        }
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
    font::draw_text(f, 12, 274, "[OK] Exit  [Hold UP] Update  [Hold DN] Hotspot", Color::Black, 1, true);
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

void CalendarRenderer::draw_ota_progress(std::string_view title,
                                         std::string_view subtitle,
                                         std::string_view status_line,
                                         int percent,
                                         Frame& f) const {
    f.clear(Color::White);
    const int pct = std::clamp(percent, 0, 100);

    // Header banner (Red + Yellow accent bar)
    f.rect(0, 0, Frame::width, 42, Color::Red);
    f.rect(0, 42, Frame::width, 4, Color::Yellow);
    font::draw_text(f, 16, 12, title.empty() ? "UPDATING FIRMWARE" : title, Color::White, 2, true);

    // Subtitle (e.g. "v0.2.2 -> v0.2.3 (ckwcfm/note4c)")
    if (!subtitle.empty()) {
        const std::string sub = font::truncate_text(subtitle, Frame::width - 32, 1, true);
        const int sw = font::measure_text(sub, 1, true);
        font::draw_text(f, (Frame::width - sw) / 2, 66, sub, Color::Black, 1, true);
    }

    // Large centered percentage readout
    char pbuf[16];
    std::snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
    const int pw = font::measure_text(pbuf, 3, true);
    font::draw_text(f, (Frame::width - pw) / 2, 94, pbuf, pct >= 100 ? Color::Black : Color::Red, 3, true);

    // Crisp 4-color progress bar frame (x=24..376, y=144..174)
    constexpr int bx = 24;
    constexpr int by = 144;
    constexpr int bw = Frame::width - 48; // 352 px
    constexpr int bh = 30;
    f.rect(bx, by, bw, 2, Color::Black);
    f.rect(bx, by + bh - 2, bw, 2, Color::Black);
    f.rect(bx, by, 2, bh, Color::Black);
    f.rect(bx + bw - 2, by, 2, bh, Color::Black);

    const int inner_w = bw - 8;
    const int inner_h = bh - 8;
    const int fill_w = (inner_w * pct + 50) / 100;
    if (fill_w > 0) {
        f.rect(bx + 4, by + 4, fill_w, inner_h, Color::Red);
        if (pct < 100 && fill_w >= 8) {
            f.rect(bx + 4 + fill_w - 4, by + 4, 4, inner_h, Color::Yellow);
        }
    }

    // Status / byte progress line
    if (!status_line.empty()) {
        const std::string st = font::truncate_text(status_line, Frame::width - 32, 1, true);
        const int stw = font::measure_text(st, 1, true);
        font::draw_text(f, (Frame::width - stw) / 2, 192, st, Color::Black, 1, true);
    }

    // Warning footer
    f.rect(0, 264, Frame::width, 1, Color::Black);
    const std::string_view warn = (pct >= 100) ? "Update verified! Rebooting NOTE4C..."
                                               : "Do not unplug power or reset during update.";
    const int ww = font::measure_text(warn, 1, true);
    font::draw_text(f, (Frame::width - ww) / 2, 274, warn, Color::Red, 1, true);
}

} // namespace note4c
