#include "QidiAdminPrintHost.hpp"

#include <sstream>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {

QidiAdminPrintHost::QidiAdminPrintHost(DynamicPrintConfig* config)
    : m_host(config->opt_string("print_host"))
    , m_api_key(config->opt_string("printhost_apikey"))
    , m_ca_file(config->opt_string("printhost_cafile"))
    , m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* QidiAdminPrintHost::get_name() const { return "Qidi Admin Server"; }

wxString QidiAdminPrintHost::get_test_ok_msg() const
{
    return _L("Connection to Qidi Admin Server is working correctly.");
}

wxString QidiAdminPrintHost::get_test_failed_msg(wxString& message) const
{
    return GUI::format_wxstr(_L("Could not connect to Qidi Admin Server: %s"), message);
}

std::string QidiAdminPrintHost::make_url(const std::string& path) const
{
    if (m_host.empty())
        return {};
    const bool has_scheme = m_host.rfind("http://", 0) == 0 || m_host.rfind("https://", 0) == 0;
    const std::string base = has_scheme ? m_host : "https://" + m_host;
    return base.back() == '/' ? base.substr(0, base.size() - 1) + path : base + path;
}

void QidiAdminPrintHost::set_auth(Http& http) const
{
    if (!m_api_key.empty())
        http.header("X-Api-Key", m_api_key);
    if (!m_ca_file.empty())
        http.ca_file(m_ca_file);
    // The gateway carries a control token. Do not silently downgrade HTTPS.
    if (m_host.rfind("https://", 0) == 0)
        http.tls_verify(true);
}

bool QidiAdminPrintHost::test(wxString& message) const
{
    if (m_host.empty()) {
        message = _L("Qidi Admin Server URL is empty.");
        return false;
    }
    bool ok = true;
    auto http = Http::get(make_url("/api/v1/status"));
    set_auth(http);
    http.timeout_connect(5).timeout_max(15)
        .on_error([&](std::string body, std::string error, unsigned status) {
            ok = false;
            message = format_error(body, error, status);
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    return ok;
}

bool QidiAdminPrintHost::start_print(wxString& error_message, const std::string& filename) const
{
    pt::ptree request;
    request.put("filename", filename);
    request.put("printer_id", "q2");
    std::ostringstream stream;
    pt::write_json(stream, request, false);

    bool ok = true;
    auto http = Http::post(make_url("/api/v1/printer/start"));
    set_auth(http);
    http.header("Content-Type", "application/json").set_post_body(stream.str())
        .timeout_connect(5).timeout_max(30)
        .on_error([&](std::string body, std::string error, unsigned status) {
            ok = false;
            error_message = format_error(body, error, status);
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    return ok;
}

bool QidiAdminPrintHost::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    wxString test_message;
    if (!test(test_message)) {
        error_fn(std::move(test_message));
        return false;
    }

    const auto filename = upload_data.upload_path.filename().string();
    bool ok = true;
    std::string stored_filename = filename;
    auto http = Http::post(make_url("/api/v1/files/upload"));
    set_auth(http);
    http.form_add("root", "gcodes").form_add("printer_id", "q2")
        .form_add_file("file", upload_data.source_path, filename)
        .on_complete([&](std::string body, unsigned) {
            try {
                std::stringstream stream(body);
                pt::ptree response;
                pt::read_json(stream, response);
                stored_filename = response.get<std::string>("filename", filename);
            } catch (const std::exception& error) {
                BOOST_LOG_TRIVIAL(warning) << "Qidi Admin upload response could not be parsed: " << error.what();
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            ok = false;
            error_fn(format_error(body, error, status));
        })
        .on_progress([&](Http::Progress progress, bool& cancel) { progress_fn(std::move(progress), cancel); })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    if (!ok)
        return false;
    info_fn(_L("Qidi Admin"), _L("G-code uploaded through Raspberry gateway."));
    if (upload_data.post_action != PrintHostPostUploadAction::StartPrint)
        return true;

    wxString start_error;
    if (!start_print(start_error, stored_filename)) {
        error_fn(std::move(start_error));
        return false;
    }
    info_fn(_L("Qidi Admin"), _L("Print started after server preflight."));
    return true;
}

} // namespace Slic3r
