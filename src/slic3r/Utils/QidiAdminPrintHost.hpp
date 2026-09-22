#ifndef slic3r_QidiAdminPrintHost_hpp_
#define slic3r_QidiAdminPrintHost_hpp_

#include <string>

#include "PrintHost.hpp"

namespace Slic3r {

class DynamicPrintConfig;

// Print-host adapter used by Orca's native Send G-code workflow.  It talks to
// the Raspberry gateway, never to a public Moonraker endpoint directly.
class QidiAdminPrintHost final : public PrintHost {
public:
    explicit QidiAdminPrintHost(DynamicPrintConfig* config);

    const char* get_name() const override;
    bool test(wxString& message) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString& message) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }

private:
    std::string make_url(const std::string& path) const;
    void set_auth(Http& http) const;
    bool start_print(wxString& error_message, const std::string& filename) const;

    std::string m_host;
    std::string m_api_key;
    std::string m_ca_file;
    bool        m_ssl_revoke_best_effort;
};

} // namespace Slic3r

#endif
