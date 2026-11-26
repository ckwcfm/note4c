#include "github_ota.hpp"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace note4c::idf {
namespace {
constexpr const char* kTag = "gh_ota";

bool valid_repo_slug(std::string_view repo) {
    if (repo.size() < 3 || repo.size() > 100) return false;
    size_t slashes = 0;
    for (char c : repo) {
        if (c == '/') {
            ++slashes;
        } else if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
            return false;
        }
    }
    return slashes == 1 && repo.front() != '/' && repo.back() != '/';
}

// Extract a JSON string value for key starting at search_pos (minimal, zero-dependency).
bool find_json_string(std::string_view json, std::string_view key, size_t start_pos,
                      std::string& out, size_t* next_pos = nullptr) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle, start_pos);
    while (pos != std::string_view::npos) {
        size_t p = pos + needle.size();
        while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
        if (p < json.size() && json[p] == ':') {
            ++p;
            while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
            if (p < json.size() && json[p] == '"') {
                ++p;
                std::string val;
                while (p < json.size()) {
                    const char c = json[p++];
                    if (c == '"') {
                        out = std::move(val);
                        if (next_pos) *next_pos = p;
                        return true;
                    }
                    if (c == '\\' && p < json.size()) {
                        const char e = json[p++];
                        if (e == '"' || e == '\\' || e == '/') val.push_back(e);
                        else if (e == 'n') val.push_back('\n');
                    } else {
                        val.push_back(c);
                    }
                }
                return false;
            }
        }
        pos = json.find(needle, pos + needle.size());
    }
    return false;
}

bool find_json_uint(std::string_view json, std::string_view key, size_t start_pos, size_t& out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t pos = json.find(needle, start_pos);
    if (pos == std::string_view::npos) return false;
    size_t p = pos + needle.size();
    while (p < json.size() && (std::isspace(static_cast<unsigned char>(json[p])) || json[p] == ':')) ++p;
    if (p >= json.size() || !std::isdigit(static_cast<unsigned char>(json[p]))) return false;
    size_t v = 0;
    while (p < json.size() && std::isdigit(static_cast<unsigned char>(json[p]))) {
        v = v * 10 + static_cast<size_t>(json[p++] - '0');
    }
    out = v;
    return true;
}

struct JsonBodyCtx {
    std::string body;
};

esp_err_t json_http_cb(esp_http_client_event_t* evt) {
    auto* ctx = static_cast<JsonBodyCtx*>(evt->user_data);
    if (!ctx) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        if (esp_http_client_get_status_code(evt->client) == 200 && ctx->body.size() < 49152) {
            const size_t space = 49152 - ctx->body.size();
            const size_t take = std::min<size_t>(space, static_cast<size_t>(evt->data_len));
            ctx->body.append(static_cast<const char*>(evt->data), take);
        }
    }
    return ESP_OK;
}

struct HeaderCaptureCtx {
    std::string location;
    size_t content_length = 0;
};

esp_err_t header_capture_cb(esp_http_client_event_t* evt) {
    auto* ctx = static_cast<HeaderCaptureCtx*>(evt->user_data);
    if (!ctx) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "Location") == 0) {
            ctx->location = evt->header_value;
        } else if (strcasecmp(evt->header_key, "Content-Length") == 0) {
            const long long cl = std::atoll(evt->header_value);
            if (cl > 0) ctx->content_length = static_cast<size_t>(cl);
        }
    }
    return ESP_OK;
}

bool is_redirect_status(int code) {
    return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

} // namespace

int compare_semver(std::string_view a, std::string_view b) {
    auto parse = [](std::string_view s) -> std::array<int, 3> {
        std::array<int, 3> v{0, 0, 0};
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.remove_prefix(1);
        size_t pos = 0;
        for (int idx = 0; idx < 3 && pos < s.size(); ++idx) {
            int num = 0;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
                num = num * 10 + (s[pos++] - '0');
            }
            v[static_cast<size_t>(idx)] = num;
            if (pos < s.size() && s[pos] == '.') ++pos;
            else break;
        }
        return v;
    };
    const auto pa = parse(a);
    const auto pb = parse(b);
    for (size_t i = 0; i < 3; ++i) {
        if (pa[i] != pb[i]) return pa[i] > pb[i] ? 1 : -1;
    }
    return 0;
}

GitHubReleaseInfo check_github_release(std::string_view repo) {
    GitHubReleaseInfo info;
    if (!valid_repo_slug(repo)) {
        info.error = "Invalid GitHub repo format (expected owner/repo)";
        return info;
    }
    const std::string api_url = "https://api.github.com/repos/" + std::string(repo) + "/releases/latest";

    esp_err_t err = ESP_FAIL;
    int code = 0;
    JsonBodyCtx ctx;

    for (int attempt = 0; attempt < 2; ++attempt) {
        ctx.body.clear();
        esp_http_client_config_t hcfg{};
        hcfg.url = api_url.c_str();
        hcfg.method = HTTP_METHOD_GET;
        hcfg.timeout_ms = 20000;
        hcfg.buffer_size = 8192;
        hcfg.buffer_size_tx = 2048;
        hcfg.max_redirection_count = 3;
        hcfg.event_handler = json_http_cb;
        hcfg.user_data = &ctx;
        hcfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&hcfg);
        if (!client) {
            info.error = "HTTP client init failed";
            return info;
        }
        esp_http_client_set_header(client, "User-Agent", "NOTE4C-Calendar/1.0");
        esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");

        err = esp_http_client_perform(client);
        code = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK) break;
        ESP_LOGW(kTag, "GitHub release check attempt %d failed: %s", attempt + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(600));
    }

    if (err != ESP_OK) {
        info.error = std::string("GitHub request failed: ") + esp_err_to_name(err);
        return info;
    }
    if (code == 404) {
        info.error = "No published GitHub Release found yet (push a v* tag first)";
        return info;
    }
    if (code != 200) {
        info.error = "GitHub API HTTP " + std::to_string(code);
        return info;
    }

    if (!find_json_string(ctx.body, "tag_name", 0, info.tag)) {
        info.error = "Could not parse tag_name from GitHub release";
        return info;
    }

    // Locate the note4c_framework.bin asset entry
    const size_t assets_pos = ctx.body.find("\"assets\"");
    const size_t bin_pos = ctx.body.find("\"note4c_framework.bin\"",
                                         assets_pos == std::string::npos ? 0 : assets_pos);
    if (bin_pos == std::string::npos) {
        info.error = "Release " + info.tag + " has no note4c_framework.bin asset yet";
        return info;
    }
    if (!find_json_string(ctx.body, "browser_download_url", bin_pos, info.asset_url)) {
        info.error = "Could not find download URL for note4c_framework.bin";
        return info;
    }
    find_json_uint(ctx.body, "size", bin_pos, info.asset_size);
    info.ok = true;
    ESP_LOGI(kTag, "Found GitHub release %s (%u bytes)",
             info.tag.c_str(), static_cast<unsigned>(info.asset_size));
    return info;
}

bool install_github_ota(std::string_view asset_url,
                        OtaProgress& progress,
                        std::string& out_slot,
                        std::string& out_error) {
    if (asset_url.rfind("https://", 0) != 0) {
        out_error = "OTA URL must start with https://";
        return false;
    }
    const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
    if (!update) {
        out_error = "No inactive OTA partition available";
        return false;
    }

    progress.written.store(0, std::memory_order_release);
    progress.total.store(0, std::memory_order_release);
    progress.active.store(true, std::memory_order_release);

    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    bool header_checked = false;
    std::vector<uint8_t> header_buf;
    header_buf.reserve(512);
    std::vector<uint8_t> io_buf(4096);

    auto abort_and_fail = [&](const std::string& msg) {
        if (ota_started) {
            esp_ota_abort(ota_handle);
            ota_started = false;
        }
        progress.active.store(false, std::memory_order_release);
        out_error = msg;
        ESP_LOGE(kTag, "GitHub OTA failed: %s", msg.c_str());
        return false;
    };

    std::string active_url(asset_url);
    size_t expected_total = 0;
    size_t written = 0;
    std::string last_err = "Download did not complete";
    constexpr size_t kMinHeader = 32 + sizeof(esp_app_desc_t);
    constexpr int kMaxAttempts = 5;

    ESP_LOGI(kTag, "Downloading GitHub OTA into '%s' @ 0x%08lx...",
             update->label, static_cast<unsigned long>(update->address));

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            ESP_LOGW(kTag, "OTA resume attempt %d/%d from byte %u / %u...",
                     attempt + 1, kMaxAttempts,
                     static_cast<unsigned>(written), static_cast<unsigned>(expected_total));
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        // Resolve redirects (github.com -> release-assets.githubusercontent.com)
        esp_http_client_handle_t client = nullptr;
        HeaderCaptureCtx hdr{};
        int code = 0;
        int64_t body_len = 0;
        bool opened_ok = false;

        for (int redir = 0; redir < 6; ++redir) {
            hdr = HeaderCaptureCtx{};
            esp_http_client_config_t hcfg{};
            hcfg.url = active_url.c_str();
            hcfg.method = HTTP_METHOD_GET;
            hcfg.timeout_ms = 30000;
            hcfg.buffer_size = 8192;
            hcfg.buffer_size_tx = 4096;
            hcfg.disable_auto_redirect = true;
            hcfg.keep_alive_enable = true;
            hcfg.event_handler = header_capture_cb;
            hcfg.user_data = &hdr;
            hcfg.crt_bundle_attach = esp_crt_bundle_attach;

            client = esp_http_client_init(&hcfg);
            if (!client) {
                last_err = "HTTP client init failed";
                break;
            }
            esp_http_client_set_header(client, "User-Agent", "NOTE4C-Calendar/1.0");
            esp_http_client_set_header(client, "Accept", "application/octet-stream");
            esp_http_client_set_header(client, "Accept-Encoding", "identity");
            if (written > 0) {
                char rbuf[48];
                std::snprintf(rbuf, sizeof(rbuf), "bytes=%u-", static_cast<unsigned>(written));
                esp_http_client_set_header(client, "Range", rbuf);
            }

            esp_err_t oerr = esp_http_client_open(client, 0);
            if (oerr != ESP_OK) {
                last_err = std::string("Connect failed: ") + esp_err_to_name(oerr);
                esp_http_client_cleanup(client);
                client = nullptr;
                break;
            }

            body_len = esp_http_client_fetch_headers(client);
            code = esp_http_client_get_status_code(client);
            if (body_len < 0) {
                last_err = "Failed to read HTTP response headers";
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                client = nullptr;
                break;
            }

            if (is_redirect_status(code)) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                client = nullptr;
                if (hdr.location.empty() || hdr.location.rfind("https://", 0) != 0) {
                    return abort_and_fail("Invalid redirect URL from GitHub");
                }
                active_url = hdr.location;
                continue;
            }

            opened_ok = true;
            break;
        }

        if (!opened_ok || !client) {
            continue; // Retry outer loop
        }

        if (written > 0 && code == 200) {
            // Server ignored Range and restarted from byte 0 -> cleanly restart OTA slot
            ESP_LOGW(kTag, "Server returned 200 instead of 206; restarting OTA from byte 0");
            if (ota_started) {
                esp_ota_abort(ota_handle);
                ota_started = false;
            }
            header_checked = false;
            header_buf.clear();
            written = 0;
            progress.written.store(0, std::memory_order_release);
        } else if (code != 200 && !(written > 0 && code == 206)) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (code == 403 || code == 404 || code == 410) {
                // Presigned CDN URL may have expired -> fall back to original asset_url on next attempt
                if (active_url != asset_url) {
                    active_url = std::string(asset_url);
                    continue;
                }
                return abort_and_fail("Download HTTP " + std::to_string(code));
            }
            last_err = "Download HTTP " + std::to_string(code);
            continue;
        }

        if (code == 200) {
            const size_t cl = (body_len > 0) ? static_cast<size_t>(body_len) : hdr.content_length;
            if (cl > 0) {
                expected_total = cl;
                progress.total.store(expected_total, std::memory_order_release);
            }
        } else if (code == 206 && expected_total == 0 && body_len > 0) {
            expected_total = written + static_cast<size_t>(body_len);
            progress.total.store(expected_total, std::memory_order_release);
        }

        if (expected_total > update->size) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return abort_and_fail("Firmware exceeds OTA partition size");
        }

        // Stream chunks directly into the inactive OTA slot
        bool fatal = false;
        std::string fatal_msg;
        int eagain_streak = 0;

        for (;;) {
            const int r = esp_http_client_read(client, reinterpret_cast<char*>(io_buf.data()),
                                               static_cast<int>(io_buf.size()));
            if (r > 0) {
                eagain_streak = 0;
                const uint8_t* chunk = io_buf.data();
                const size_t len = static_cast<size_t>(r);

                if (!header_checked) {
                    header_buf.insert(header_buf.end(), chunk, chunk + len);
                    if (header_buf.size() < kMinHeader) continue;

                    const uint8_t magic = header_buf[0];
                    const uint16_t chip_id = static_cast<uint16_t>(header_buf[12]) |
                                             static_cast<uint16_t>(static_cast<uint16_t>(header_buf[13]) << 8);
                    esp_app_desc_t desc{};
                    std::memcpy(&desc, header_buf.data() + 32, sizeof(desc));
                    if (magic != 0xE9 || chip_id != 9 || desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
                        fatal = true;
                        fatal_msg = "Downloaded file is not a valid ESP32-S3 firmware image";
                        break;
                    }
                    if (std::strncmp(desc.project_name, "note4c_framework", sizeof(desc.project_name)) != 0) {
                        fatal = true;
                        fatal_msg = "Rejected: firmware is not note4c_framework";
                        break;
                    }
                    const esp_err_t berr = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
                    if (berr != ESP_OK) {
                        fatal = true;
                        fatal_msg = std::string("esp_ota_begin failed: ") + esp_err_to_name(berr);
                        break;
                    }
                    ota_started = true;
                    header_checked = true;

                    const esp_err_t werr = esp_ota_write(ota_handle, header_buf.data(), header_buf.size());
                    if (werr != ESP_OK) {
                        fatal = true;
                        fatal_msg = std::string("esp_ota_write failed: ") + esp_err_to_name(werr);
                        break;
                    }
                    written += header_buf.size();
                    progress.written.store(written, std::memory_order_release);
                    header_buf.clear();
                    continue;
                }

                if (written + len > update->size) {
                    fatal = true;
                    fatal_msg = "Firmware exceeds OTA partition size";
                    break;
                }
                const esp_err_t werr = esp_ota_write(ota_handle, chunk, len);
                if (werr != ESP_OK) {
                    fatal = true;
                    fatal_msg = std::string("esp_ota_write failed: ") + esp_err_to_name(werr);
                    break;
                }
                written += len;
                progress.written.store(written, std::memory_order_release);
                if (expected_total > 0 && written >= expected_total) {
                    break;
                }
            } else if (r == 0) {
                if (esp_http_client_is_complete_data_received(client)) {
                    break;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (++eagain_streak <= 5) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        continue;
                    }
                }
                last_err = "Connection closed early (" + std::to_string(written / 1024) + " / " +
                           std::to_string(expected_total / 1024) + " KB)";
                break;
            } else {
                last_err = "Read error at " + std::to_string(written / 1024) + " KB (errno=" +
                           std::to_string(errno) + ")";
                break;
            }
        }

        const bool stream_complete = esp_http_client_is_complete_data_received(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = nullptr;

        if (fatal) {
            return abort_and_fail(fatal_msg);
        }

        const bool done = (expected_total > 0) ? (written == expected_total) : (ota_started && stream_complete);
        if (done) {
            break;
        }
        // Otherwise loop and resume via HTTP Range from `written`!
    }

    if (!ota_started || (expected_total > 0 && written != expected_total)) {
        return abort_and_fail(last_err);
    }

    ota_started = false;
    const esp_err_t eerr = esp_ota_end(ota_handle);
    if (eerr != ESP_OK) {
        return abort_and_fail(std::string("Image verification failed: ") + esp_err_to_name(eerr));
    }
    const esp_err_t serr = esp_ota_set_boot_partition(update);
    if (serr != ESP_OK) {
        return abort_and_fail(std::string("Failed to set boot slot: ") + esp_err_to_name(serr));
    }

    out_slot = update->label;
    progress.active.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "GitHub OTA verified (%u bytes) -> boot slot '%s'",
             static_cast<unsigned>(written), out_slot.c_str());
    return true;
}

} // namespace note4c::idf
