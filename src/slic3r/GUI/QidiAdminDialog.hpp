#ifndef slic3r_GUI_QidiAdminDialog_hpp_
#define slic3r_GUI_QidiAdminDialog_hpp_

#include <memory>

#include <wx/weakref.h>

#include "GUI_Utils.hpp"
#include "QidiAdminGateway.hpp"

class wxButton;
class wxCheckBox;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r::GUI {

// The first native surface for the existing Qidi Admin Server. It is kept
// deliberately separate from Orca's project/preset state: changing a server
// address can never alter a 3MF file or a printer profile.
class QidiAdminDialog : public DPIDialog {
public:
    explicit QidiAdminDialog(wxWindow* parent);

private:
    QidiAdminConnection connection() const;
    void                save_connection();
    void                check_connection();
    void                show_result(const QidiAdminResult& result);

    wxTextCtrl*         m_endpoint {nullptr};
    wxTextCtrl*         m_api_key {nullptr};
    wxCheckBox*         m_verify_tls {nullptr};
    wxStaticText*       m_status {nullptr};
    wxButton*           m_check {nullptr};
    Http::Ptr           m_pending_request;
};

} // namespace Slic3r::GUI

#endif
