#ifndef slic3r_GUI_QidiAdminDialog_hpp_
#define slic3r_GUI_QidiAdminDialog_hpp_

#include <chrono>
#include <memory>
#include <vector>

#include <wx/weakref.h>
#include <wx/timer.h>

#include "GUI_Utils.hpp"
#include "QidiAdminGateway.hpp"

class wxButton;
class wxCheckBox;
class wxChoice;
class wxListBox;
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
    struct ServerMacro {
        wxString name;
        wxString description;
        std::string script;
        bool requires_confirmation {true};
    };
    struct CommandHistoryEntry {
        wxString script;
        wxString response;
        bool success {false};
        int latency_ms {-1};
    };

    QidiAdminConnection connection() const;
    void                on_dpi_changed(const wxRect&) override { Fit(); Layout(); }
    void                save_connection();
    void                check_connection();
    void                refresh_status(bool announce = false);
    void                refresh_materials();
    void                refresh_macros();
    void                refresh_maintenance();
    void                refresh_print_history();
    void                refresh_diagnostics();
    void                refresh_events();
    void                refresh_access_role();
    void                refresh_command_queue();
    void                refresh_command_history();
    void                export_command_history(bool json_format);
    void                cancel_selected_command();
    void                retry_selected_command();
    void                simulate_command();
    void                run_selected_macro();
    void                send_command(const std::string& script, int priority, const wxString& action,
                                     const std::string& queue_group = "Qidi Admin Studio");
    void                refresh_camera();
    void                refresh_camera_snapshot();
    void                show_camera_frame(const QidiAdminResult& result);
    void                show_result(const QidiAdminResult& result);

    wxTextCtrl*         m_endpoint {nullptr};
    wxTextCtrl*         m_api_key {nullptr};
    wxTextCtrl*         m_command {nullptr};
    wxChoice*           m_command_priority {nullptr};
    wxChoice*           m_command_group {nullptr};
    wxCheckBox*         m_verify_tls {nullptr};
    wxStaticText*       m_status {nullptr};
    wxStaticText*       m_material {nullptr};
    wxStaticText*       m_maintenance {nullptr};
    wxStaticText*       m_history {nullptr};
    wxStaticText*       m_diagnostics {nullptr};
    wxStaticText*       m_event {nullptr};
    wxStaticText*       m_access_role {nullptr};
    wxListBox*          m_queue {nullptr};
    wxListBox*          m_command_log {nullptr};
    wxTextCtrl*         m_command_response {nullptr};
    wxChoice*           m_macro_choice {nullptr};
    wxStaticBitmap*     m_camera {nullptr};
    wxStaticText*       m_camera_status {nullptr};
    wxButton*           m_check {nullptr};
    wxButton*           m_pause {nullptr};
    wxButton*           m_resume {nullptr};
    wxButton*           m_stop {nullptr};
    wxButton*           m_run_macro {nullptr};
    wxButton*           m_send_command {nullptr};
    wxButton*           m_simulate_command {nullptr};
    wxButton*           m_cancel_queued_command {nullptr};
    wxButton*           m_retry_queued_command {nullptr};
    Http::Ptr           m_pending_request;
    Http::Ptr           m_status_request;
    Http::Ptr           m_material_request;
    Http::Ptr           m_macro_request;
    Http::Ptr           m_maintenance_request;
    Http::Ptr           m_history_request;
    Http::Ptr           m_diagnostics_request;
    Http::Ptr           m_events_request;
    Http::Ptr           m_access_role_request;
    Http::Ptr           m_queue_request;
    Http::Ptr           m_command_history_request;
    Http::Ptr           m_queue_cancel_request;
    Http::Ptr           m_queue_retry_request;
    Http::Ptr           m_camera_request;
    Http::Ptr           m_simulation_request;
    wxTimer             m_camera_timer;
    int                 m_refresh_ticks {0};
    std::chrono::steady_clock::time_point m_camera_last_frame;
    double              m_camera_fps {0.0};
    std::vector<ServerMacro> m_macros;
    std::vector<int>    m_queue_command_ids;
    std::vector<wxString> m_command_responses;
    std::vector<CommandHistoryEntry> m_command_history_entries;
};

} // namespace Slic3r::GUI

#endif
