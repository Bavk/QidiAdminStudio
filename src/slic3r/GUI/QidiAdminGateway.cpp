#include "QidiAdminGateway.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

namespace Slic3r::GUI {
namespace {

bool has_control_character(const std::string& value)
{
    return std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

std::string trim_copy(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last  = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    return first < last ? std::string(first, last) : std::string();
}

} // namespace

bool QidiAdminGateway::is_valid_endpoint(const std::string& endpoint)
{
    const std::string value = trim_copy(endpoint);
    if (value.empty() || has_control_character(value))
        return false;

    const size_t scheme_end = value.find("://");
    if (scheme_end == std::string::npos)
        return false;
    const std::string scheme = value.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https")
        return false;

    const size_t authority_begin = scheme_end + 3;
    const size_t authority_end = value.find_first_of("/?#", authority_begin);
    const std::string authority = value.substr(authority_begin, authority_end - authority_begin);
    // Credentials in a server URL are too easy to leak through diagnostics.
    return !authority.empty() && authority.find('@') == std::string::npos;
}

std::string QidiAdminGateway::normalized_endpoint(const std::string& endpoint)
{
    std::string normalized = trim_copy(endpoint);
    while (!normalized.empty() && normalized.back() == '/')
        normalized.pop_back();
    return normalized;
}

Http::Ptr QidiAdminGateway::fetch_status(const QidiAdminConnection& connection, ResultCallback callback)
{
    return request(connection, "/api/v1/status", nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::fetch_camera_snapshot(const QidiAdminConnection& connection, ResultCallback callback)
{
    if (!is_valid_endpoint(connection.endpoint)) {
        callback({false, 0, {}, "Qidi Admin Server URL must begin with http:// or https:// and contain a host."});
        return nullptr;
    }
    auto callback_holder = std::make_shared<ResultCallback>(std::move(callback));
    Http request = Http::get(normalized_endpoint(connection.endpoint) + "/api/v1/camera/snapshot");
    request.timeout_connect(5).timeout_max(10).tls_verify(connection.verify_tls);
    if (!connection.api_key.empty())
        request.header("X-Api-Key", connection.api_key);
    request.header("Accept", "image/jpeg")
        .on_complete([callback_holder](std::string body, unsigned status) {
            (*callback_holder)({status >= 200 && status < 300, status, std::move(body), {}});
        })
        .on_error([callback_holder](std::string body, std::string error, unsigned status) {
            (*callback_holder)({false, status, std::move(body), std::move(error)});
        });
    return request.perform();
}

Http::Ptr QidiAdminGateway::fetch_camera_stream_frame(const QidiAdminConnection& connection, ResultCallback callback)
{
    if (!is_valid_endpoint(connection.endpoint)) {
        callback({false, 0, {}, "Qidi Admin Server URL must begin with http:// or https:// and contain a host."});
        return nullptr;
    }
    // Http owns the curl worker. A shared state guarantees that cancelling the
    // multipart request after a completed frame cannot later report a second
    // (spurious) cancellation error to the UI.
    struct FrameState {
        bool delivered {false};
    };
    auto state = std::make_shared<FrameState>();
    auto callback_holder = std::make_shared<ResultCallback>(std::move(callback));
    Http request = Http::get(normalized_endpoint(connection.endpoint) + "/api/v1/camera/stream");
    request.timeout_connect(5).timeout_max(12).size_limit(8 * 1024 * 1024).tls_verify(connection.verify_tls);
    if (!connection.api_key.empty())
        request.header("X-Api-Key", connection.api_key);
    request.header("Accept", "multipart/x-mixed-replace, image/jpeg")
        .on_progress([state, callback_holder](Http::Progress progress, bool& cancel) {
            // libcurl may emit another progress callback while it is applying
            // cancellation. Delivering the same JPEG twice makes the UI FPS
            // counter jump and can schedule redundant rescaling work.
            if (state->delivered) {
                cancel = true;
                return;
            }
            const std::string& buffer = progress.buffer;
            const size_t jpeg_start = buffer.rfind("\xFF\xD8");
            if (jpeg_start == std::string::npos)
                return;
            const size_t jpeg_end = buffer.find("\xFF\xD9", jpeg_start + 2);
            if (jpeg_end == std::string::npos)
                return;
            state->delivered = true;
            (*callback_holder)({true, 200, buffer.substr(jpeg_start, jpeg_end - jpeg_start + 2), {}});
            // Stop exactly at one frame. The next timer tick reconnects at the
            // live edge, avoiding an ever-growing MJPEG buffer.
            cancel = true;
        })
        .on_complete([state, callback_holder](std::string, unsigned status) {
            if (!state->delivered)
                (*callback_holder)({false, status, {}, "Camera stream ended before a JPEG frame arrived."});
        })
        .on_error([state, callback_holder](std::string body, std::string error, unsigned status) {
            if (!state->delivered)
                (*callback_holder)({false, status, std::move(body), std::move(error)});
        });
    return request.perform();
}

Http::Ptr QidiAdminGateway::fetch_materials(const QidiAdminConnection& connection, ResultCallback callback)
{
    return request(connection, "/api/v1/materials/spools?printer_id=q2", nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::upsert_spool(const QidiAdminConnection& connection,
                                         const std::string& spool_id,
                                         const std::string& json_body,
                                         ResultCallback callback)
{
    if (!is_valid_endpoint(connection.endpoint) || spool_id.empty() || json_body.empty()) {
        callback({false, 0, {}, "Spool or Raspberry connection settings are incomplete."});
        return nullptr;
    }
    auto callback_holder = std::make_shared<ResultCallback>(std::move(callback));
    Http request = Http::put2(normalized_endpoint(connection.endpoint) +
        "/api/v1/materials/spools/" + Http::url_encode(spool_id));
    request.timeout_connect(5).timeout_max(20).tls_verify(connection.verify_tls);
    if (!connection.api_key.empty())
        request.header("X-Api-Key", connection.api_key);
    request.header("Accept", "application/json")
        .header("Content-Type", "application/json")
        .set_post_body(json_body)
        .on_complete([callback_holder](std::string body, unsigned status) {
            (*callback_holder)({status >= 200 && status < 300, status, std::move(body), {}});
        })
        .on_error([callback_holder](std::string body, std::string error, unsigned status) {
            (*callback_holder)({false, status, std::move(body), std::move(error)});
        });
    return request.perform();
}

Http::Ptr QidiAdminGateway::fetch_macros(const QidiAdminConnection& connection, ResultCallback callback)
{
    return request(connection, "/api/v1/macros?printer_id=q2", nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::save_macro(const QidiAdminConnection& connection,
                                       int macro_id,
                                       const std::string& json_body,
                                       ResultCallback callback)
{
    if (!is_valid_endpoint(connection.endpoint) || macro_id < 0 || json_body.empty()) {
        callback({false, 0, {}, "Macro or Raspberry connection settings are incomplete."});
        return nullptr;
    }
    const std::string url = normalized_endpoint(connection.endpoint) + "/api/v1/macros" +
        (macro_id > 0 ? "/" + std::to_string(macro_id) : "");
    auto callback_holder = std::make_shared<ResultCallback>(std::move(callback));
    Http request = macro_id > 0 ? Http::put2(url) : Http::post(url);
    request.timeout_connect(5).timeout_max(20).tls_verify(connection.verify_tls);
    if (!connection.api_key.empty())
        request.header("X-Api-Key", connection.api_key);
    request.header("Accept", "application/json")
        .header("Content-Type", "application/json")
        .set_post_body(json_body)
        .on_complete([callback_holder](std::string body, unsigned status) {
            (*callback_holder)({status >= 200 && status < 300, status, std::move(body), {}});
        })
        .on_error([callback_holder](std::string body, std::string error, unsigned status) {
            (*callback_holder)({false, status, std::move(body), std::move(error)});
        });
    return request.perform();
}

Http::Ptr QidiAdminGateway::fetch_maintenance_tasks(const QidiAdminConnection& connection, ResultCallback callback)
{
    return request(connection, "/api/v1/maintenance/tasks?printer_id=q2", nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::fetch_print_history(const QidiAdminConnection& connection, ResultCallback callback)
{
    return request(connection, "/api/v1/history", nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::fetch_diagnostics(const QidiAdminConnection& connection, ResultCallback callback)
{
    return request(connection, "/api/v1/diagnostics", nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::create_klipper_backup(const QidiAdminConnection& connection, ResultCallback callback)
{
    static const std::string empty_body = "{}";
    return request(connection, "/api/v1/backup/klipper/printer.cfg", &empty_body, std::move(callback));
}

Http::Ptr QidiAdminGateway::fetch_klipper_backups(const QidiAdminConnection& connection, ResultCallback callback)
{
    return request(connection, "/api/v1/backup/klipper", nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::diff_klipper_backups(const QidiAdminConnection& connection,
                                                  const std::string& before,
                                                  const std::string& after,
                                                  ResultCallback callback)
{
    if (before.empty() || after.empty()) {
        callback({false, 0, {}, "Select two Klipper backups to compare."});
        return nullptr;
    }
    const std::string path = "/api/v1/backup/klipper/diff?before=" + Http::url_encode(before) +
        "&after=" + Http::url_encode(after);
    return request(connection, path, nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::fetch_events(const QidiAdminConnection& connection, ResultCallback callback)
{
    return request(connection, "/api/v1/events?limit=1", nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::fetch_access_role(const QidiAdminConnection& connection, ResultCallback callback)
{
    return request(connection, "/api/v1/access/role", nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::fetch_command_queue(const QidiAdminConnection& connection, ResultCallback callback)
{
    return request(connection, "/api/v1/command-queue?limit=12", nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::fetch_command_history(const QidiAdminConnection& connection, ResultCallback callback)
{
    return request(connection, "/api/v1/commands?limit=12", nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::cancel_queued_command(const QidiAdminConnection& connection, int command_id, ResultCallback callback)
{
    if (command_id <= 0) {
        callback({false, 0, {}, "Select a queued command first."});
        return nullptr;
    }
    if (!is_valid_endpoint(connection.endpoint)) {
        callback({false, 0, {}, "Qidi Admin Server connection settings are incomplete."});
        return nullptr;
    }
    auto callback_holder = std::make_shared<ResultCallback>(std::move(callback));
    Http request = Http::del(normalized_endpoint(connection.endpoint) + "/api/v1/command-queue/" + std::to_string(command_id));
    request.timeout_connect(5).timeout_max(15).tls_verify(connection.verify_tls);
    if (!connection.api_key.empty())
        request.header("X-Api-Key", connection.api_key);
    request.header("Accept", "application/json")
        .on_complete([callback_holder](std::string body, unsigned status) {
            (*callback_holder)({status >= 200 && status < 300, status, std::move(body), {}});
        })
        .on_error([callback_holder](std::string body, std::string error, unsigned status) {
            (*callback_holder)({false, status, std::move(body), std::move(error)});
        });
    return request.perform();
}

Http::Ptr QidiAdminGateway::retry_queued_command(const QidiAdminConnection& connection, int command_id, ResultCallback callback)
{
    if (command_id <= 0) {
        callback({false, 0, {}, "Select a command first."});
        return nullptr;
    }
    if (!is_valid_endpoint(connection.endpoint)) {
        callback({false, 0, {}, "Qidi Admin Server connection settings are incomplete."});
        return nullptr;
    }
    auto callback_holder = std::make_shared<ResultCallback>(std::move(callback));
    Http request = Http::post(normalized_endpoint(connection.endpoint) + "/api/v1/command-queue/" + std::to_string(command_id) + "/retry");
    request.timeout_connect(5).timeout_max(15).tls_verify(connection.verify_tls);
    if (!connection.api_key.empty())
        request.header("X-Api-Key", connection.api_key);
    request.header("Accept", "application/json")
        .header("Content-Type", "application/json")
        .set_post_body(std::string("{}"))
        .on_complete([callback_holder](std::string body, unsigned status) {
            (*callback_holder)({status >= 200 && status < 300, status, std::move(body), {}});
        })
        .on_error([callback_holder](std::string body, std::string error, unsigned status) {
            (*callback_holder)({false, status, std::move(body), std::move(error)});
        });
    return request.perform();
}

Http::Ptr QidiAdminGateway::preflight(const QidiAdminConnection& connection,
                                      const std::string& filename,
                                      const std::string& printer_id,
                                      ResultCallback callback)
{
    if (filename.empty()) {
        callback({false, 0, {}, "A file must be uploaded before Qidi Admin preflight."});
        return nullptr;
    }
    const std::string path = "/api/v1/printer/preflight?filename=" + Http::url_encode(filename) +
                             "&printer_id=" + Http::url_encode(printer_id.empty() ? "q2" : printer_id);
    return request(connection, path, nullptr, std::move(callback));
}

Http::Ptr QidiAdminGateway::enqueue_command(const QidiAdminConnection& connection,
                                            const std::string& script,
                                            int priority,
                                            const std::string& queue_group,
                                            ResultCallback callback)
{
    if (script.empty()) {
        callback({false, 0, {}, "G-code command is empty."});
        return nullptr;
    }
    const nlohmann::json payload = {
        {"script", script},
        {"printer_id", "q2"},
        {"queue_group", queue_group.empty() ? "Qidi Admin Studio" : queue_group},
        {"priority", std::clamp(priority, 0, 100)},
    };
    const std::string body = payload.dump();
    return request(connection, "/api/v1/command-queue", &body, std::move(callback));
}

Http::Ptr QidiAdminGateway::emergency_stop(const QidiAdminConnection& connection,
                                           ResultCallback callback)
{
    static const std::string empty_body = "{}";
    return request(connection, "/api/v1/printer/emergency-stop", &empty_body, std::move(callback));
}

Http::Ptr QidiAdminGateway::simulate_command(const QidiAdminConnection& connection,
                                             const std::string& script,
                                             ResultCallback callback)
{
    if (script.empty()) {
        callback({false, 0, {}, "G-code command is empty."});
        return nullptr;
    }
    const nlohmann::json payload = {{"script", script}, {"printer_id", "q2"}};
    const std::string body = payload.dump();
    return request(connection, "/api/v1/printer/gcode/simulate", &body, std::move(callback));
}

Http::Ptr QidiAdminGateway::request(const QidiAdminConnection& connection,
                                    const std::string& path,
                                    const std::string* json_body,
                                    ResultCallback callback)
{
    if (!is_valid_endpoint(connection.endpoint)) {
        callback({false, 0, {}, "Qidi Admin Server URL must begin with http:// or https:// and contain a host."});
        return nullptr;
    }
    const std::string url = normalized_endpoint(connection.endpoint) + path;
    auto callback_holder = std::make_shared<ResultCallback>(std::move(callback));
    Http request = json_body ? Http::post(url) : Http::get(url);
    request.timeout_connect(5)
           .timeout_max(20)
           .tls_verify(connection.verify_tls);
    if (!connection.api_key.empty())
        request.header("X-Api-Key", connection.api_key);
    request.header("Accept", "application/json");
    if (json_body != nullptr)
        request.header("Content-Type", "application/json").set_post_body(*json_body);

    return request.on_complete([callback_holder](std::string body, unsigned status) {
                      (*callback_holder)({status >= 200 && status < 300, status, std::move(body), {}});
                  })
                  .on_error([callback_holder](std::string body, std::string error, unsigned status) {
                      (*callback_holder)({false, status, std::move(body), std::move(error)});
                  })
                  .perform();
}

} // namespace Slic3r::GUI
