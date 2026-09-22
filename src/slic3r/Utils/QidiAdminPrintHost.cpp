#include "QidiAdminPrintHost.hpp"

#include <sstream>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/QidiAdminCredentials.hpp"
#include "slic3r/GUI/format.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {

QidiAdminPrintHost::QidiAdminPrintHost(DynamicPrintConfig* config)
    : m_host(config->opt_string("print_host"))
    , m_api_key(config->opt_string("printhost_apikey"))
    , m_ca_file(config->opt_string("printhost_cafile"))
    , m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{
    // The physical-printer profile may deliberately omit credentials because
    // presets are shareable.  The Admin dialog owns the global server address
    // and keeps the API key in Windows Credential Manager, so use it only as
    // a fallback. Per-printer values always take precedence.
    const bool using_admin_connection = m_host.empty();
    if (using_admin_connection) {
        m_host = GUI::wxGetApp().app_config->get("qidi_admin", "endpoint");
        m_verify_tls = GUI::wxGetApp().app_config->get("qidi_admin", "verify_tls") != "false";
    }
    if (m_api_key.empty())
        m_api_key = GUI::QidiAdminCredentials::load_api_key();
}

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
        http.tls_verify(m_verify_tls);
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

bool QidiAdminPrintHost::preflight(wxString& error_message, const std::string& filename) const
{
    bool ok = true;
    std::string response;
    auto http = Http::get(make_url("/api/v1/printer/preflight?filename=" + Http::url_encode(filename) + "&printer_id=q2"));
    set_auth(http);
    http.timeout_connect(5).timeout_max(20)
        .on_complete([&](std::string body, unsigned) { response = std::move(body); })
        .on_error([&](std::string body, std::string error, unsigned status) {
            ok = false;
            error_message = format_error(body, error, status);
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    if (!ok)
        return false;

    try {
        std::stringstream stream(response);
        pt::ptree report;
        pt::read_json(stream, report);
        if (report.get<bool>("readyToStart", false))
            return true;
        std::string issues;
        if (const auto issue_list = report.get_child_optional("issues")) {
            for (const auto& issue : *issue_list) {
                if (!issues.empty())
                    issues += "\n";
                issues += issue.second.get_value<std::string>();
            }
        }
        error_message = wxString::FromUTF8(
            ("Raspberry blocked this print before upload." + (issues.empty() ? std::string() : "\n" + issues)).c_str());
        return false;
    } catch (const std::exception&) {
        error_message = _L("Qidi Admin Server returned an invalid preflight response.");
        return false;
    }
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
    wxString preflight_error;
    if (!preflight(preflight_error, filename)) {
        error_fn(std::move(preflight_error));
        return false;
    }
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
