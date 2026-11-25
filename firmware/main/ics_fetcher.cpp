#include "ics_fetcher.hpp"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_tls_errors.h"
#include <cstdio>
#include <ctime>
#include <memory>

namespace note4c::idf {
namespace {
constexpr const char* kTag = "ics_fetch";

struct StreamCtx {
    IcsParser* parser = nullptr;
    size_t bytes = 0;
    int status = 0;
    esp_err_t tls_last_err = ESP_OK;
    int tls_stack_err = 0;
    int tls_flags = 0;
};

esp_err_t http_event_cb(esp_http_client_event_t* evt) {
    auto* ctx = static_cast<StreamCtx*>(evt->user_data);
    if (!ctx) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ERROR && evt->data) {
        const auto* err_h = static_cast<const esp_tls_last_error_t*>(evt->data);
        ctx->tls_last_err = err_h->last_error;
        ctx->tls_stack_err = err_h->esp_tls_error_code;
        ctx->tls_flags = err_h->esp_tls_flags;
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        const int code = esp_http_client_get_status_code(evt->client);
        ctx->status = code;
        if (code == 200 && ctx->parser) {
            ctx->bytes += static_cast<size_t>(evt->data_len);
            ctx->parser->feed(std::string_view(static_cast<const char*>(evt->data),
                                               static_cast<size_t>(evt->data_len)));
        }
    }
    return ESP_OK;
}

int64_t rtc_to_utc(const RtcTime& t) {
    return days_from_civil({t.year, t.month, t.day}) * 86400LL +
           t.hour * 3600LL + t.minute * 60LL + t.second;
}

} // namespace

bool sync_time_sntp(Board& board, int64_t& out_utc_sec, uint32_t timeout_ms) {
    if (esp_sntp_enabled()) esp_sntp_stop();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.apple.com");
    esp_sntp_init();

    const auto start = now_ms();
    bool synced = false;
    while (now_ms() - start < timeout_ms) {
        time_t now = 0;
        std::time(&now);
        if (now >= 1735689600LL) { // >= 2025-01-01 UTC
            synced = true;
            out_utc_sec = static_cast<int64_t>(now);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_sntp_stop();

    if (synced) {
        const TzRule utc = TzRule::parse_posix("UTC0");
        const auto dt = utc.utc_to_local(out_utc_sec);
        board.write_rtc({dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second});
        ESP_LOGI(kTag, "SNTP synced UTC: %04d-%02d-%02d %02d:%02d:%02d",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
        return true;
    }

    // Fallback to onboard PCF8563 RTC
    RtcTime rtc{};
    if (board.read_rtc(rtc) && rtc.year >= 2025) {
        out_utc_sec = rtc_to_utc(rtc);
        ESP_LOGW(kTag, "SNTP timed out; using PCF8563 RTC %04d-%02d-%02d %02d:%02d:%02d",
                 rtc.year, rtc.month, rtc.day, rtc.hour, rtc.minute, rtc.second);
        return true;
    }
    ESP_LOGE(kTag, "SNTP failed and PCF8563 RTC has no valid time");
    return false;
}

SyncReport sync_all_calendars(const CalendarConfig& cfg, Board& board) {
    SyncReport report;
    report.time_ok = sync_time_sntp(board, report.utc_now, 10000);
    if (!report.time_ok) {
        report.error_count = 1;
        return report;
    }

    const TzRule local_tz = TzRule::parse_posix(cfg.timezone);
    const LocalDateTime loc = local_tz.utc_to_local(report.utc_now);
    report.today = {loc.year, loc.month, loc.day};
    report.local_hour = loc.hour;
    report.local_minute = loc.minute;
    report.local_second = loc.second;

    ESP_LOGI(kTag, "Local date %04d-%02d-%02d %02d:%02d (TZ=%s), fetching %u calendars",
             loc.year, loc.month, loc.day, loc.hour, loc.minute,
             cfg.timezone.c_str(), static_cast<unsigned>(cfg.calendars.size()));

    for (const auto& cal : cfg.calendars) {
        if (!cal.enabled) continue;
        CalendarFetchStatus st;
        st.name = cal.name;

        const std::string url = CalendarConfig::normalize_url(cal.url);
        if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
            st.error = "URL must start with https:// or webcal://";
            ++report.error_count;
            report.calendars.push_back(std::move(st));
            continue;
        }

        auto parser = std::make_unique<IcsParser>(report.today, local_tz, cal.color, cal.name);
        StreamCtx ctx{parser.get(), 0, 0, ESP_OK, 0, 0};

        ESP_LOGI(kTag, "Fetching '%s' (free internal=%u, psram=%u)",
                 cal.name.c_str(),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

        esp_http_client_config_t hcfg{};
        hcfg.url = url.c_str();
        hcfg.method = HTTP_METHOD_GET;
        hcfg.timeout_ms = 20000;
        hcfg.buffer_size = 4096;
        hcfg.buffer_size_tx = 1024;
        hcfg.max_redirection_count = 5;
        hcfg.event_handler = http_event_cb;
        hcfg.user_data = &ctx;
        hcfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&hcfg);
        if (!client) {
            st.error = "HTTP client init failed";
            ++report.error_count;
            report.calendars.push_back(std::move(st));
            continue;
        }
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        esp_http_client_set_header(client, "User-Agent", "NOTE4C-Calendar/1.0");

        const esp_err_t err = esp_http_client_perform(client);
        const int http_code = esp_http_client_get_status_code(client);
        const int sock_err = esp_http_client_get_errno(client);
        esp_http_client_cleanup(client);

        st.http_status = http_code;
        st.bytes = ctx.bytes;

        if (err != ESP_OK) {
            char ebuf[160];
            std::snprintf(ebuf, sizeof(ebuf), "%s (tls=%s code=-0x%x flags=0x%x errno=%d)",
                          esp_err_to_name(err), esp_err_to_name(ctx.tls_last_err),
                          ctx.tls_stack_err < 0 ? -ctx.tls_stack_err : ctx.tls_stack_err,
                          ctx.tls_flags, sock_err);
            st.error = ebuf;
            ++report.error_count;
            ESP_LOGW(kTag, "Calendar '%s': %s", cal.name.c_str(), st.error.c_str());
        } else if (http_code != 200) {
            st.error = "HTTP " + std::to_string(http_code);
            ++report.error_count;
            ESP_LOGW(kTag, "Calendar '%s': HTTP %d", cal.name.c_str(), http_code);
        } else {
            parser->finish();
            st.ok = true;
            st.vevents = parser->vevents_seen();
            for (int d = 0; d < IcsParser::kNumDays; ++d) {
                const auto& evs = parser->day_events(d);
                st.day_counts[static_cast<size_t>(d)] = evs.size();
                for (const auto& ev : evs) report.days[static_cast<size_t>(d)].push_back(ev);
            }
            ESP_LOGI(kTag, "Calendar '%s': %u bytes, %u VEVENTs -> %u/%u/%u (3 days)",
                     cal.name.c_str(), static_cast<unsigned>(st.bytes),
                     static_cast<unsigned>(st.vevents),
                     static_cast<unsigned>(st.day_counts[0]),
                     static_cast<unsigned>(st.day_counts[1]),
                     static_cast<unsigned>(st.day_counts[2]));
        }
        report.calendars.push_back(std::move(st));
    }

    for (auto& day_list : report.days) {
        sort_calendar_events(day_list);
    }
    return report;
}

} // namespace note4c::idf
