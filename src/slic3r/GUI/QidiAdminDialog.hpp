#ifndef slic3r_GUI_QidiAdminDialog_hpp_
#define slic3r_GUI_QidiAdminDialog_hpp_

#include <memory>

#include <wx/weakref.h>
#include <wx/timer.h>

#include "GUI_Utils.hpp"
#include "QidiAdminGateway.hpp"

class wxButton;
class wxCheckBox;
class wxStaticText;
class wxStaticBitmap;
class wxTextCtrl;

namespace Slic3r::GUI {

// The first native surface for the existing Qidi Admin Server. It is kept
// deliberately separate from Orca's project/preset state: changing a server
// address can never alter a 3MF file or a printer profile.
class QidiAdminDialog : public DPIDialog {
public:
    explicit QidiAdminDialog(wxWindow* parent);
    ~QidiAdminDialog() override;

private:
    QidiAdminConnection connection() const;
    void                save_connection();
    void                check_connection();
    void                refresh_status(bool announce = false);
    void                send_command(const std::string& script, int priority, const wxString& action);
    void                refresh_camera();
    void                show_camera_frame(const QidiAdminResult& result);
    void                show_result(const QidiAdminResult& result);

    wxTextCtrl*         m_endpoint {nullptr};
    wxTextCtrl*         m_api_key {nullptr};
    wxTextCtrl*         m_command {nullptr};
    wxCheckBox*         m_verify_tls {nullptr};
    wxStaticText*       m_status {nullptr};
    wxStaticBitmap*     m_camera {nullptr};
    wxButton*           m_check {nullptr};
    wxButton*           m_pause {nullptr};
    wxButton*           m_resume {nullptr};
    wxButton*           m_stop {nullptr};
    Http::Ptr           m_pending_request;
    Http::Ptr           m_status_request;
    Http::Ptr           m_camera_request;
    wxTimer             m_camera_timer;
    int                 m_refresh_ticks {0};
};

} // namespace Slic3r::GUI

#endif
