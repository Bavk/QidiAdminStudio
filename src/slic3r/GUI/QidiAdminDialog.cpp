#include "QidiAdminDialog.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/listbox.h>
#include <wx/mstream.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wfstream.h>

#include <nlohmann/json.hpp>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "QidiAdminCredentials.hpp"
#include "libslic3r/AppConfig.hpp"

namespace Slic3r::GUI {
namespace {

wxString wx_from_utf8(const std::string& value)
{
    return wxString::FromUTF8(value.c_str());
}

std::string utf8_from_wx(const wxString& value)
{
    const wxCharBuffer utf8 = value.ToUTF8();
    return utf8.data() == nullptr ? std::string() : std::string(utf8.data());
}

wxString format_duration(double seconds)
{
    const long long total = std::max(0LL, static_cast<long long>(seconds));
    const long long hours = total / 3600;
    const long long minutes = (total % 3600) / 60;
    return hours > 0 ? wxString::Format("%lld h %02lld min", hours, minutes)
                     : wxString::Format("%lld min", minutes);
}

} // namespace

QidiAdminDialog::QidiAdminDialog(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
    , m_camera_timer(this)
{
    // The native tab contains a live video feed, queues and a terminal.
    // Its scrolled child keeps the controls reachable on laptop screens.
    auto* root_layout = new wxBoxSizer(wxVERTICAL);
    auto* content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL);
    content->SetScrollRate(0, FromDIP(12));
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(new wxStaticText(content, wxID_ANY, _L("Connect Orca prepare and preview to your Raspberry Qidi Admin Server.")), 0, wxALL, FromDIP(16));

    auto* form = new wxFlexGridSizer(2, FromDIP(10), FromDIP(10));
    form->AddGrowableCol(1, 1);
    form->Add(new wxStaticText(content, wxID_ANY, _L("Server URL")), 0, wxALIGN_CENTER_VERTICAL);
    m_endpoint = new wxTextCtrl(content, wxID_ANY, wx_from_utf8(wxGetApp().app_config->get("qidi_admin", "endpoint")));
    m_endpoint->SetHint("https://morrax3d.ru");
    form->Add(m_endpoint, 1, wxEXPAND);
    form->Add(new wxStaticText(content, wxID_ANY, _L("API key")), 0, wxALIGN_CENTER_VERTICAL);
    m_api_key = new wxTextCtrl(content, wxID_ANY, wx_from_utf8(QidiAdminCredentials::load_api_key()), wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    m_api_key->SetHint(_L("Optional — stored in Windows Credential Manager"));
    form->Add(m_api_key, 1, wxEXPAND);
    layout->Add(form, 1, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(16));

    m_verify_tls = new wxCheckBox(content, wxID_ANY, _L("Verify TLS certificate"));
    m_verify_tls->SetValue(wxGetApp().app_config->get("qidi_admin", "verify_tls") != "false");
    layout->Add(m_verify_tls, 0, wxALL, FromDIP(16));

    m_status = new wxStaticText(content, wxID_ANY, _L("Not checked yet."));
    layout->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_material = new wxStaticText(content, wxID_ANY, _L("Material: loading from Raspberry…"));
    layout->Add(m_material, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    layout->Add(new wxStaticText(content, wxID_ANY, _L("Spool catalog on Raspberry")), 0,
        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_spool_list = new wxListBox(content, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(480, 96)));
    m_spool_list->Append(_L("Loading spools…"));
    layout->Add(m_spool_list, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));
    auto* spool_actions = new wxBoxSizer(wxHORIZONTAL);
    m_add_spool = new wxButton(content, wxID_ANY, _L("Add spool…"));
    m_edit_spool = new wxButton(content, wxID_ANY, _L("Edit selected…"));
    m_add_spool->Disable();
    m_edit_spool->Disable();
    spool_actions->Add(m_add_spool, 0, wxRIGHT, FromDIP(8));
    spool_actions->Add(m_edit_spool, 0);
    layout->Add(spool_actions, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_maintenance = new wxStaticText(content, wxID_ANY, _L("Maintenance: loading from Raspberry…"));
    layout->Add(m_maintenance, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_history = new wxStaticText(content, wxID_ANY, _L("Print history: loading from Raspberry…"));
    layout->Add(m_history, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_diagnostics = new wxStaticText(content, wxID_ANY, _L("Raspberry health: loading…"));
    layout->Add(m_diagnostics, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_diagnostics_details = new wxButton(content, wxID_ANY, _L("Detailed Raspberry health…"));
    m_diagnostics_details->Disable();
    layout->Add(m_diagnostics_details, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    auto* backup_actions = new wxBoxSizer(wxHORIZONTAL);
    m_create_klipper_backup = new wxButton(content, wxID_ANY, _L("Backup printer.cfg"));
    m_compare_klipper_backups = new wxButton(content, wxID_ANY, _L("Compare Klipper backups…"));
    m_create_klipper_backup->Disable();
    m_compare_klipper_backups->Disable();
    backup_actions->Add(m_create_klipper_backup, 0, wxRIGHT, FromDIP(8));
    backup_actions->Add(m_compare_klipper_backups, 0);
    layout->Add(backup_actions, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_event = new wxStaticText(content, wxID_ANY, _L("Server events: loading…"));
    layout->Add(m_event, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_access_role = new wxStaticText(content, wxID_ANY, _L("Access role: checking…"));
    layout->Add(m_access_role, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    layout->Add(new wxStaticText(content, wxID_ANY, _L("Raspberry command queue")), 0,
        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_queue = new wxListBox(content, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(480, 84)));
    m_queue->Append(_L("Loading queue…"));
    layout->Add(m_queue, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));
    m_cancel_queued_command = new wxButton(content, wxID_ANY, _L("Cancel selected queued command"));
    m_retry_queued_command = new wxButton(content, wxID_ANY, _L("Retry selected failed command"));
    m_cancel_queued_command->Disable();
    m_retry_queued_command->Disable();
    auto* queue_actions = new wxBoxSizer(wxHORIZONTAL);
    queue_actions->Add(m_retry_queued_command, 0, wxRIGHT, FromDIP(8));
    queue_actions->Add(m_cancel_queued_command, 0);
    layout->Add(queue_actions, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_RIGHT, FromDIP(16));

    layout->Add(new wxStaticText(content, wxID_ANY, _L("Server command log")), 0,
        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_command_log = new wxListBox(content, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(480, 76)));
    m_command_log->Append(_L("Loading command log…"));
    layout->Add(m_command_log, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));
    m_command_response = new wxTextCtrl(content, wxID_ANY, wxEmptyString,
        wxDefaultPosition, FromDIP(wxSize(480, 66)), wxTE_MULTILINE | wxTE_READONLY);
    m_command_response->SetHint(_L("Select a command to view the Raspberry/Moonraker response."));
    layout->Add(m_command_response, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));
    auto* export_json = new wxButton(content, wxID_ANY, _L("Export JSON"));
    auto* export_csv = new wxButton(content, wxID_ANY, _L("Export CSV"));
    auto* export_actions = new wxBoxSizer(wxHORIZONTAL);
    export_actions->Add(export_json, 0, wxRIGHT, FromDIP(8));
    export_actions->Add(export_csv, 0);
    layout->Add(export_actions, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_RIGHT, FromDIP(16));

    auto* macro_row = new wxBoxSizer(wxHORIZONTAL);
    m_macro_choice = new wxChoice(content, wxID_ANY);
    m_macro_choice->SetMinSize(FromDIP(wxSize(330, -1)));
    m_macro_choice->Append(_L("Loading server macros…"));
    m_macro_choice->SetSelection(0);
    m_run_macro = new wxButton(content, wxID_ANY, _L("Run macro"));
    m_run_macro->Disable();
    m_add_macro = new wxButton(content, wxID_ANY, _L("Add…"));
    m_edit_macro = new wxButton(content, wxID_ANY, _L("Edit…"));
    m_add_macro->Disable();
    m_edit_macro->Disable();
    macro_row->Add(m_macro_choice, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
    macro_row->Add(m_run_macro, 0, wxRIGHT, FromDIP(8));
    macro_row->Add(m_add_macro, 0, wxRIGHT, FromDIP(8));
    macro_row->Add(m_edit_macro, 0);
    layout->Add(macro_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));

    m_camera = new wxStaticBitmap(content, wxID_ANY, wxNullBitmap, wxDefaultPosition, FromDIP(wxSize(480, 270)));
    m_camera->SetMinSize(FromDIP(wxSize(480, 270)));
    layout->Add(m_camera, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER_HORIZONTAL, FromDIP(16));
    m_camera_status = new wxStaticText(content, wxID_ANY, _L("Live camera: waiting for first frame…"));
    layout->Add(m_camera_status, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER_HORIZONTAL, FromDIP(16));
    m_save_camera_snapshot = new wxButton(content, wxID_ANY, _L("Save full-resolution snapshot…"));
    layout->Add(m_save_camera_snapshot, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER_HORIZONTAL, FromDIP(16));

    auto* quick_actions = new wxBoxSizer(wxHORIZONTAL);
    m_pause = new wxButton(content, wxID_ANY, _L("Pause"));
    m_resume = new wxButton(content, wxID_ANY, _L("Resume"));
    m_stop = new wxButton(content, wxID_ANY, _L("Emergency stop"));
    // Enable controlling actions only after the server has confirmed the
    // caller's role. This avoids a misleading enabled state for camera-only
    // credentials during the first asynchronous role lookup.
    m_pause->Disable();
    m_resume->Disable();
    m_stop->Disable();
    quick_actions->Add(m_pause, 0, wxRIGHT, FromDIP(8));
    quick_actions->Add(m_resume, 0, wxRIGHT, FromDIP(8));
    quick_actions->Add(m_stop, 0);
    layout->Add(quick_actions, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    layout->Add(new wxStaticText(content, wxID_ANY, _L("G-code terminal (queued through Raspberry)")), 0,
        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_command = new wxTextCtrl(content, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(480, 72)), wxTE_MULTILINE);
    m_command->SetHint("SET_PIN PIN=caselight VALUE=1");
    layout->Add(m_command, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));
    m_send_command = new wxButton(content, wxID_ANY, _L("Queue G-code"));
    m_simulate_command = new wxButton(content, wxID_ANY, _L("Review G-code"));
    m_send_command->Disable();
    m_simulate_command->Disable();
    auto* command_actions = new wxBoxSizer(wxHORIZONTAL);
    command_actions->Add(new wxStaticText(content, wxID_ANY, _L("Priority")), 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
    m_command_priority = new wxChoice(content, wxID_ANY);
    m_command_priority->Append(_L("Normal"));
    m_command_priority->Append(_L("High"));
    m_command_priority->Append(_L("Emergency"));
    m_command_priority->SetSelection(0);
    command_actions->Add(m_command_priority, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
    m_command_group = new wxChoice(content, wxID_ANY);
    m_command_group->Append(_L("Manual"));
    m_command_group->Append(_L("Camera and lighting"));
    m_command_group->Append(_L("Calibration"));
    m_command_group->SetSelection(0);
    command_actions->Add(m_command_group, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
    command_actions->AddStretchSpacer();
    command_actions->Add(m_simulate_command, 0, wxRIGHT, FromDIP(8));
    command_actions->Add(m_send_command, 0);
    layout->Add(command_actions, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    m_check = new wxButton(content, wxID_ANY, _L("Check server"));
    auto* save_button = new wxButton(content, wxID_ANY, _L("Save connection"));
    buttons->Add(m_check, 0, wxRIGHT, FromDIP(8));
    buttons->Add(save_button, 0);
    layout->Add(buttons, 0, wxALL | wxALIGN_RIGHT, FromDIP(12));
    content->SetSizer(layout);
    // Explicitly calculate the virtual size. This is required for reliable
    // vertical scrolling with some wxWidgets Windows builds.
    layout->FitInside(content);
    content->SetMinSize(FromDIP(wxSize(520, -1)));
    root_layout->Add(content, 1, wxEXPAND);
    SetSizer(root_layout);
    SetMinSize(FromDIP(wxSize(520, 480)));

    m_check->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { check_connection(); });
    m_endpoint->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { invalidate_access_role(); });
    m_api_key->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { invalidate_access_role(); });
    m_verify_tls->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { invalidate_access_role(); });
    m_pause->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_command("PAUSE", 80, _L("Pause")); });
    m_resume->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_command("RESUME", 80, _L("Resume")); });
    m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (wxMessageBox(_L("Immediately stop the printer?"), _L("Emergency stop"), wxYES_NO | wxICON_WARNING, this) == wxYES)
        {
            m_status->SetLabel(_L("Sending emergency stop directly to Raspberry…"));
            wxWeakRef<QidiAdminDialog> weak_this(this);
            m_pending_request = QidiAdminGateway::emergency_stop(connection(), [weak_this](QidiAdminResult result) {
                wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
                    if (!weak_this)
                        return;
                    weak_this->m_pending_request.reset();
                    if (result.ok)
                        weak_this->m_status->SetLabel(_L("Emergency stop sent to Moonraker."));
                    else
                        weak_this->show_result(result);
                });
            });
        }
    });
    m_send_command->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string script = utf8_from_wx(m_command->GetValue());
        if (script.empty()) {
            m_status->SetLabel(_L("Enter G-code before queueing it."));
            return;
        }
        static constexpr int priorities[] = {50, 80, 100};
        const int selection = m_command_priority ? m_command_priority->GetSelection() : 0;
        const size_t priority_index = selection >= 0 && selection < 3 ? static_cast<size_t>(selection) : 0;
        if (priority_index == 2 &&
            wxMessageBox(_L("Queue this terminal command as emergency priority?"), _L("Confirm priority"), wxYES_NO | wxICON_WARNING, this) != wxYES)
            return;
        static constexpr const char* groups[] = {"Manual", "Camera and lighting", "Calibration"};
        const int group_selection = m_command_group ? m_command_group->GetSelection() : 0;
        const size_t group_index = group_selection >= 0 && group_selection < 3 ? static_cast<size_t>(group_selection) : 0;
        send_command(script, priorities[priority_index], _L("G-code"), groups[group_index]);
        m_command->Clear();
    });
    m_run_macro->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { run_selected_macro(); });
    m_add_macro->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { edit_macro(true); });
    m_edit_macro->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { edit_macro(false); });
    m_diagnostics_details->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_diagnostics_details(); });
    m_create_klipper_backup->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { create_klipper_backup(); });
    m_compare_klipper_backups->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { compare_klipper_backups(); });
    m_add_spool->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { edit_spool(true); });
    m_edit_spool->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { edit_spool(false); });
    m_simulate_command->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { simulate_command(); });
    m_save_camera_snapshot->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_camera_snapshot(); });
    m_command_log->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
        const int selection = m_command_log->GetSelection();
        if (m_command_response == nullptr)
            return;
        if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= m_command_responses.size()) {
            m_command_response->Clear();
            return;
        }
        m_command_response->SetValue(m_command_responses[selection]);
    });
    export_json->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { export_command_history(true); });
    export_csv->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { export_command_history(false); });
    m_cancel_queued_command->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_selected_command(); });
    m_retry_queued_command->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { retry_selected_command(); });
    save_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        save_connection();
        m_status->SetLabel(_L("Server connection saved."));
        check_connection();
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        // Do not continuously create failing requests while the connection
        // form is still empty. This tab also serves as first-run setup.
        const QidiAdminConnection value = connection();
        if (!QidiAdminGateway::is_valid_endpoint(value.endpoint))
            return;
        refresh_camera();
        if (++m_refresh_ticks % 8 == 0)
            refresh_status();
        if (m_refresh_ticks % 16 == 0)
            refresh_command_queue();
        if (m_refresh_ticks % 20 == 0)
            refresh_command_history();
        if (m_refresh_ticks % 24 == 0)
            refresh_materials();
        if (m_refresh_ticks % 40 == 0)
            refresh_macros();
        if (m_refresh_ticks % 120 == 0)
            refresh_maintenance();
        if (m_refresh_ticks % 60 == 0)
            refresh_print_history();
        if (m_refresh_ticks % 60 == 0)
            refresh_diagnostics();
        if (m_refresh_ticks % 40 == 0)
            refresh_events();
        if (m_refresh_ticks % 120 == 0)
            refresh_access_role();
    }, m_camera_timer.GetId());
    wxGetApp().UpdateDarkUIWin(this);
}

void QidiAdminDialog::set_active(bool active)
{
    if (!active) {
        m_camera_timer.Stop();
        ++m_camera_generation;
        if (m_camera_request)
            m_camera_request->cancel();
        m_camera_request.reset();
        m_camera_last_frame = {};
        m_camera_fps = 0.0;
        return;
    }
    if (m_camera_timer.IsRunning())
        return;
    m_refresh_ticks = 0;
    m_camera_timer.Start(250);
    if (QidiAdminGateway::is_valid_endpoint(connection().endpoint)) {
        refresh_status();
        refresh_materials();
        refresh_macros();
        refresh_maintenance();
        refresh_print_history();
        refresh_diagnostics();
        refresh_events();
        refresh_access_role();
        refresh_command_queue();
        refresh_command_history();
    }
}

QidiAdminDialog::~QidiAdminDialog()
{
    m_camera_timer.Stop();
    if (m_pending_request)
        m_pending_request->cancel();
    if (m_status_request)
        m_status_request->cancel();
    if (m_material_request)
        m_material_request->cancel();
    if (m_spool_write_request)
        m_spool_write_request->cancel();
    if (m_macro_request)
        m_macro_request->cancel();
    if (m_macro_write_request)
        m_macro_write_request->cancel();
    if (m_maintenance_request)
        m_maintenance_request->cancel();
    if (m_history_request)
        m_history_request->cancel();
    if (m_diagnostics_request)
        m_diagnostics_request->cancel();
    if (m_klipper_backup_request)
        m_klipper_backup_request->cancel();
    if (m_events_request)
        m_events_request->cancel();
    if (m_access_role_request)
        m_access_role_request->cancel();
    if (m_queue_request)
        m_queue_request->cancel();
    if (m_command_history_request)
        m_command_history_request->cancel();
    if (m_queue_cancel_request)
        m_queue_cancel_request->cancel();
    if (m_queue_retry_request)
        m_queue_retry_request->cancel();
    if (m_camera_request)
        m_camera_request->cancel();
    if (m_snapshot_save_request)
        m_snapshot_save_request->cancel();
    if (m_simulation_request)
        m_simulation_request->cancel();
}

void QidiAdminDialog::refresh_command_queue()
{
    if (m_queue_request)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_queue_request = QidiAdminGateway::fetch_command_queue(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_queue_request.reset();
            if (!result.ok)
                return;
            try {
                const auto entries = nlohmann::json::parse(result.body);
                if (!entries.is_array())
                    return;
                const int previous_selection = weak_this->m_queue->GetSelection();
                const int selected_command_id = previous_selection != wxNOT_FOUND &&
                    static_cast<size_t>(previous_selection) < weak_this->m_queue_command_ids.size()
                    ? weak_this->m_queue_command_ids[previous_selection] : 0;
                weak_this->m_queue->Clear();
                weak_this->m_queue_command_ids.clear();
                if (entries.empty()) {
                    weak_this->m_queue->Append(_L("No queued or recent commands."));
                    return;
                }
                for (const auto& entry : entries) {
                    const wxString status = wx_from_utf8(entry.value("status", "unknown"));
                    const wxString raw_script = wx_from_utf8(entry.value("script", ""));
                    wxString script = raw_script;
                    script.Replace("\n", " ");
                    if (script.length() > 70)
                        script = script.Left(67) + "…";
                    const int priority = entry.value("priority", 0);
                    weak_this->m_queue->Append(wxString::Format(_L("[%s · P%d] %s"), status, priority, script));
                    const int command_id = entry.value("id", 0);
                    weak_this->m_queue_command_ids.emplace_back(command_id);
                    if (command_id > 0 && command_id == selected_command_id)
                        weak_this->m_queue->SetSelection(static_cast<int>(weak_this->m_queue_command_ids.size() - 1));
                }
            } catch (const std::exception&) {
                weak_this->m_queue->Clear();
                weak_this->m_queue_command_ids.clear();
                weak_this->m_queue->Append(_L("Command queue response could not be parsed."));
            }
        });
    });
}

void QidiAdminDialog::refresh_command_history()
{
    if (m_command_history_request || !m_command_log)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_command_history_request = QidiAdminGateway::fetch_command_history(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_command_history_request.reset();
            if (!result.ok)
                return;
            try {
                const auto entries = nlohmann::json::parse(result.body);
                if (!entries.is_array())
                    return;
                weak_this->m_command_log->Clear();
                weak_this->m_command_responses.clear();
                weak_this->m_command_history_entries.clear();
                if (weak_this->m_command_response)
                    weak_this->m_command_response->Clear();
                if (entries.empty()) {
                    weak_this->m_command_log->Append(_L("No server command history yet."));
                    return;
                }
                for (const auto& entry : entries) {
                    const wxString raw_script = wx_from_utf8(entry.value("script", ""));
                    wxString script = raw_script;
                    script.Replace("\n", " ");
                    if (script.length() > 62)
                        script = script.Left(59) + "…";
                    const bool success = entry.value("success", 0) != 0;
                    const int latency = entry.value("latency_ms", -1);
                    wxString response = wx_from_utf8(entry.value("response", ""));
                    if (response.empty())
                        response = _L("The server did not return a response body.");
                    weak_this->m_command_log->Append(wxString::Format(
                        _L("[%s · %d ms] %s"), success ? _L("OK") : _L("ERROR"), latency, script));
                    weak_this->m_command_responses.emplace_back(std::move(response));
                    weak_this->m_command_history_entries.push_back({raw_script, weak_this->m_command_responses.back(), success, latency});
                }
            } catch (const std::exception&) {
                weak_this->m_command_log->Clear();
                weak_this->m_command_responses.clear();
                weak_this->m_command_history_entries.clear();
                if (weak_this->m_command_response)
                    weak_this->m_command_response->Clear();
                weak_this->m_command_log->Append(_L("Command history response could not be parsed."));
            }
        });
    });
}

void QidiAdminDialog::export_command_history(bool json_format)
{
    if (m_command_history_entries.empty()) {
        m_status->SetLabel(_L("No command history is available to export."));
        return;
    }
    const wxString extension = json_format ? "json" : "csv";
    const wxString wildcard = json_format ? _L("JSON files (*.json)|*.json") : _L("CSV files (*.csv)|*.csv");
    wxFileDialog dialog(this, _L("Export command history"), wxEmptyString,
        "qidi-admin-command-history." + extension, wildcard,
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK)
        return;

    try {
        if (json_format) {
            nlohmann::json output = nlohmann::json::array();
            for (const auto& entry : m_command_history_entries) {
                output.push_back({
                    {"script", utf8_from_wx(entry.script)},
                    {"success", entry.success},
                    {"latencyMs", entry.latency_ms},
                    {"response", utf8_from_wx(entry.response)},
                });
            }
            std::ofstream file(dialog.GetPath().ToStdWstring(), std::ios::binary);
            file << output.dump(2);
            if (!file)
                throw std::runtime_error("write failed");
        } else {
            std::ofstream file(dialog.GetPath().ToStdWstring(), std::ios::binary);
            file << "script,success,latency_ms,response\n";
            const auto quote_csv = [](const wxString& value) {
                wxString escaped = value;
                escaped.Replace("\"", "\"\"");
                return "\"" + escaped + "\"";
            };
            for (const auto& entry : m_command_history_entries)
                file << utf8_from_wx(quote_csv(entry.script)) << ',' << (entry.success ? "true" : "false")
                     << ',' << entry.latency_ms << ',' << utf8_from_wx(quote_csv(entry.response)) << '\n';
            if (!file)
                throw std::runtime_error("write failed");
        }
        m_status->SetLabel(wxString::Format(_L("Command history exported: %s"), dialog.GetPath()));
    } catch (const std::exception&) {
        m_status->SetLabel(_L("Command history export failed."));
    }
}

void QidiAdminDialog::cancel_selected_command()
{
    const int selection = m_queue ? m_queue->GetSelection() : wxNOT_FOUND;
    if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= m_queue_command_ids.size()) {
        m_status->SetLabel(_L("Select a queued command first."));
        return;
    }
    const int command_id = m_queue_command_ids[selection];
    if (command_id <= 0)
        return;
    m_status->SetLabel(_L("Cancelling queued command…"));
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_queue_cancel_request = QidiAdminGateway::cancel_queued_command(connection(), command_id, [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_queue_cancel_request.reset();
            if (result.ok) {
                weak_this->m_status->SetLabel(_L("Queued command cancelled."));
                weak_this->refresh_command_queue();
            } else {
                weak_this->show_result(result);
            }
        });
    });
}

void QidiAdminDialog::retry_selected_command()
{
    const int selection = m_queue ? m_queue->GetSelection() : wxNOT_FOUND;
    if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= m_queue_command_ids.size()) {
        m_status->SetLabel(_L("Select a failed, blocked or cancelled command first."));
        return;
    }
    const int command_id = m_queue_command_ids[selection];
    if (command_id <= 0)
        return;
    m_status->SetLabel(_L("Retrying command through Raspberry…"));
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_queue_retry_request = QidiAdminGateway::retry_queued_command(connection(), command_id, [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_queue_retry_request.reset();
            if (result.ok) {
                weak_this->m_status->SetLabel(_L("Command returned to the Raspberry queue."));
                weak_this->refresh_command_queue();
            } else {
                weak_this->show_result(result);
            }
        });
    });
}

void QidiAdminDialog::simulate_command()
{
    if (m_simulation_request)
        return;
    const std::string script = utf8_from_wx(m_command->GetValue());
    if (script.empty()) {
        m_status->SetLabel(_L("Enter G-code before reviewing it."));
        return;
    }
    m_status->SetLabel(_L("Reviewing G-code on Raspberry…"));
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_simulation_request = QidiAdminGateway::simulate_command(connection(), script, [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_simulation_request.reset();
            if (!result.ok) {
                weak_this->show_result(result);
                return;
            }
            try {
                const auto report = nlohmann::json::parse(result.body);
                const bool safe = report.value("ok", false);
                wxString text = safe ? _L("No blocking safety rules were found.") : _L("Safety review found blocking rules.");
                if (const auto warnings = report.find("warnings"); warnings != report.end() && warnings->is_array()) {
                    for (const auto& warning : *warnings) {
                        text += "\n" + wxString::Format(_L("Line %d: %s"), warning.value("line", 0),
                            wx_from_utf8(warning.value("message", "")));
                    }
                }
                if (weak_this->m_command_response)
                    weak_this->m_command_response->SetValue(text);
                weak_this->m_status->SetLabel(safe ? _L("G-code review passed.") : _L("G-code review requires attention."));
            } catch (const std::exception&) {
                weak_this->m_status->SetLabel(_L("G-code review response could not be parsed."));
            }
        });
    });
}

void QidiAdminDialog::refresh_maintenance()
{
    if (m_maintenance_request)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_maintenance_request = QidiAdminGateway::fetch_maintenance_tasks(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_maintenance_request.reset();
            if (!result.ok)
                return;
            try {
                const auto tasks = nlohmann::json::parse(result.body);
                if (!tasks.is_array() || tasks.empty()) {
                    weak_this->m_maintenance->SetLabel(_L("Maintenance: no tasks scheduled."));
                    return;
                }
                const auto& next = tasks.front();
                const wxString title = wx_from_utf8(next.value("title", "Scheduled service"));
                const int minutes = next.value("estimated_minutes", 0);
                weak_this->m_maintenance->SetLabel(wxString::Format(_L("Maintenance: %zu tasks · latest: %s (%d min)"), tasks.size(), title, minutes));
            } catch (const std::exception&) {
                weak_this->m_maintenance->SetLabel(_L("Maintenance: response could not be parsed."));
            }
        });
    });
}

void QidiAdminDialog::refresh_print_history()
{
    if (m_history_request)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_history_request = QidiAdminGateway::fetch_print_history(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_history_request.reset();
            if (!result.ok)
                return;
            try {
                const auto payload = nlohmann::json::parse(result.body);
                const auto result_it = payload.find("result");
                const auto jobs_it = result_it == payload.end() ? payload.end() : result_it->find("jobs");
                if (jobs_it == payload.end() || !jobs_it->is_array() || jobs_it->empty()) {
                    weak_this->m_history->SetLabel(_L("Print history: no completed jobs on Raspberry."));
                    return;
                }
                const auto& job = jobs_it->front();
                const wxString filename = wx_from_utf8(job.value("filename", "Unknown file"));
                const wxString status = wx_from_utf8(job.value("status", "unknown"));
                const double duration = job.value("print_duration", job.value("total_duration", 0.0));
                weak_this->m_history->SetLabel(wxString::Format(
                    _L("Last print: %s · %s · %.0f min"), filename, status, duration / 60.0));
            } catch (const std::exception&) {
                weak_this->m_history->SetLabel(_L("Print history: response could not be parsed."));
            }
        });
    });
}

void QidiAdminDialog::refresh_diagnostics()
{
    if (m_diagnostics_request)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    const auto generation = m_access_generation;
    m_diagnostics_request = QidiAdminGateway::fetch_diagnostics(connection(), [weak_this, generation](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, generation, result = std::move(result)]() {
            if (!weak_this || generation != weak_this->m_access_generation)
                return;
            weak_this->m_diagnostics_request.reset();
            if (!result.ok) {
                weak_this->m_diagnostics_json.clear();
                weak_this->m_diagnostics_details->Disable();
                weak_this->m_diagnostics->SetLabel(_L("Raspberry health: diagnostics unavailable."));
                return;
            }
            try {
                const auto report = nlohmann::json::parse(result.body);
                const bool ready = report.value("ok", false);
                const int latency = report.value("latencyMs", -1);
                const auto camera_it = report.find("camera");
                const bool camera_ok = camera_it != report.end() && camera_it->value("snapshot", nlohmann::json::object()).value("ok", false);
                const auto moonraker_it = report.find("moonraker");
                const bool moonraker_ok = moonraker_it != report.end() && moonraker_it->value("reachable", false);
                weak_this->m_diagnostics->SetLabel(wxString::Format(
                    _L("Raspberry health: %s · Moonraker %s · camera %s · %d ms"),
                    ready ? _L("ready") : _L("attention"),
                    moonraker_ok ? _L("online") : _L("offline"),
                    camera_ok ? _L("online") : _L("unavailable"), latency));
                weak_this->m_diagnostics_json = result.body;
                weak_this->m_diagnostics_details->Enable();
            } catch (const std::exception&) {
                weak_this->m_diagnostics_json.clear();
                weak_this->m_diagnostics_details->Disable();
                weak_this->m_diagnostics->SetLabel(_L("Raspberry health: response could not be parsed."));
            }
        });
    });
}

void QidiAdminDialog::show_diagnostics_details()
{
    if (m_diagnostics_json.empty())
        return;
    try {
        const auto report = nlohmann::json::parse(m_diagnostics_json);
        const auto object_at = [&report](const char* key) {
            const auto it = report.find(key);
            return it != report.end() && it->is_object() ? *it : nlohmann::json::object();
        };
        const auto moonraker = object_at("moonraker");
        const auto camera = object_at("camera");
        const auto database = object_at("database");
        const auto host = object_at("host");
        const auto network = object_at("network");
        const auto maintenance = object_at("maintenance");
        const auto storage_it = host.find("storage");
        const auto storage = storage_it != host.end() && storage_it->is_object()
            ? *storage_it : nlohmann::json::object();
        const auto gateway_it = network.find("publicGateway");
        const auto gateway = gateway_it != network.end() && gateway_it->is_object()
            ? *gateway_it : nlohmann::json::object();
        const auto snapshot_it = camera.find("snapshot");
        const auto snapshot = snapshot_it != camera.end() && snapshot_it->is_object()
            ? *snapshot_it : nlohmann::json::object();
        const auto stream_it = camera.find("stream");
        const auto stream = stream_it != camera.end() && stream_it->is_object()
            ? *stream_it : nlohmann::json::object();
        const wxString online = _L("online");
        const wxString offline = _L("unavailable");
        wxString details;
        details << wxString::Format(_L("Overall: %s\n"), report.value("ok", false) ? _L("ready") : _L("attention"));
        details << wxString::Format(_L("Moonraker: %s\n"), moonraker.value("reachable", false) ? online : offline);
        details << wxString::Format(_L("Camera snapshot: %s\n"), snapshot.value("ok", false) ? online : offline);
        details << wxString::Format(_L("Camera stream: %s\n"), stream.value("ok", false) ? online : offline);
        details << wxString::Format(_L("Database: %s · %.1f MB\n"),
            database.value("healthy", false) ? _L("healthy") : _L("check database"),
            database.value("sizeBytes", 0.0) / 1048576.0);
        details << wxString::Format(_L("Storage: %.1f%% used · %.1f GB free\n"),
            storage.value("usedPercent", 0.0), storage.value("freeBytes", 0.0) / 1073741824.0);
        const auto temp_it = host.find("temperatureC");
        if (temp_it != host.end() && temp_it->is_number())
            details << wxString::Format(_L("Raspberry CPU temperature: %.1f°C\n"), temp_it->get<double>());
        if (gateway.value("configured", false))
            details << wxString::Format(_L("Public VPS/Rathole path: %s · %d ms\n"),
                gateway.value("reachable", false) ? online : offline, gateway.value("latencyMs", -1));
        else
            details << _L("Public VPS/Rathole path: not configured\n");
        details << wxString::Format(_L("Maintenance mode: %s\n"),
            maintenance.value("enabled", false) ? _L("enabled") : _L("off"));
        details << wxString::Format(_L("Server response: %d ms\n"), report.value("latencyMs", -1));
        const auto tasks = object_at("tasks");
        if (!tasks.empty()) {
            details << _L("\nBackground services:\n");
            for (auto it = tasks.begin(); it != tasks.end(); ++it)
                if (it.value().is_string())
                    details << wx_from_utf8(it.key()) << ": " << wx_from_utf8(it.value().get<std::string>()) << "\n";
        }
        wxDialog dialog(this, wxID_ANY, _L("Raspberry server health"),
            wxDefaultPosition, FromDIP(wxSize(520, 460)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        auto* body = new wxTextCtrl(&dialog, wxID_ANY, details, wxDefaultPosition,
            FromDIP(wxSize(480, 380)), wxTE_MULTILINE | wxTE_READONLY);
        layout->Add(body, 1, wxALL | wxEXPAND, FromDIP(12));
        layout->Add(dialog.CreateButtonSizer(wxOK), 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_RIGHT, FromDIP(12));
        dialog.SetSizer(layout);
        wxGetApp().UpdateDarkUIWin(&dialog);
        dialog.ShowModal();
    } catch (const std::exception&) {
        m_diagnostics->SetLabel(_L("Raspberry health: response could not be parsed."));
    }
}

void QidiAdminDialog::create_klipper_backup()
{
    if (!m_may_manage_spools || m_klipper_backup_request)
        return;
    m_create_klipper_backup->Disable();
    const auto generation = m_access_generation;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_klipper_backup_request = QidiAdminGateway::create_klipper_backup(connection(), [weak_this, generation](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, generation, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_klipper_backup_request.reset();
            if (generation != weak_this->m_access_generation)
                return;
            weak_this->m_create_klipper_backup->Enable(weak_this->m_may_manage_spools);
            if (!result.ok) {
                weak_this->m_status->SetLabel(wxString::Format(_L("printer.cfg backup failed: HTTP %u"), result.status));
                return;
            }
            try {
                const auto payload = nlohmann::json::parse(result.body);
                weak_this->m_status->SetLabel(wxString::Format(_L("printer.cfg backup saved on Raspberry: %s"),
                    wx_from_utf8(payload.value("backup", "unknown"))));
            } catch (const std::exception&) {
                weak_this->m_status->SetLabel(_L("printer.cfg backup created on Raspberry."));
            }
        });
    });
}

void QidiAdminDialog::compare_klipper_backups()
{
    if (!m_may_manage_spools || m_klipper_backup_request)
        return;
    m_compare_klipper_backups->Disable();
    const auto generation = m_access_generation;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_klipper_backup_request = QidiAdminGateway::fetch_klipper_backups(connection(), [weak_this, generation](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, generation, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_klipper_backup_request.reset();
            if (generation != weak_this->m_access_generation)
                return;
            weak_this->m_compare_klipper_backups->Enable(weak_this->m_may_manage_spools);
            if (!result.ok) {
                weak_this->m_status->SetLabel(wxString::Format(_L("Klipper backups unavailable: HTTP %u"), result.status));
                return;
            }
            try {
                const auto backups = nlohmann::json::parse(result.body);
                if (!backups.is_array())
                    throw std::runtime_error("Backup list is not an array");
                wxArrayString choices;
                std::vector<std::string> names;
                for (const auto& item : backups) {
                    if (!item.is_object())
                        continue;
                    const std::string name = item.value("name", "");
                    if (name.empty())
                        continue;
                    names.push_back(name);
                    choices.Add(wx_from_utf8(name));
                }
                if (names.size() < 2) {
                    weak_this->m_status->SetLabel(_L("Create at least two Klipper backups before comparing."));
                    return;
                }
                wxMultiChoiceDialog dialog(weak_this, _L("Select exactly two backups to compare"),
                    _L("Klipper backup versions"), choices);
                wxGetApp().UpdateDarkUIWin(&dialog);
                if (dialog.ShowModal() != wxID_OK)
                    return;
                const wxArrayInt selected = dialog.GetSelections();
                if (selected.GetCount() != 2) {
                    wxMessageBox(_L("Select exactly two backups."), _L("Compare backups"),
                        wxOK | wxICON_INFORMATION, weak_this);
                    return;
                }
                const std::string before = names[static_cast<size_t>(selected[1])];
                const std::string after = names[static_cast<size_t>(selected[0])];
                weak_this->m_compare_klipper_backups->Disable();
                weak_this->m_klipper_backup_request = QidiAdminGateway::diff_klipper_backups(
                    weak_this->connection(), before, after, [weak_this, generation](QidiAdminResult comparison) {
                        wxTheApp->CallAfter([weak_this, generation, comparison = std::move(comparison)]() {
                            if (!weak_this)
                                return;
                            weak_this->m_klipper_backup_request.reset();
                            if (generation != weak_this->m_access_generation)
                                return;
                            weak_this->m_compare_klipper_backups->Enable(weak_this->m_may_manage_spools);
                            if (!comparison.ok) {
                                weak_this->m_status->SetLabel(wxString::Format(_L("Backup comparison failed: HTTP %u"), comparison.status));
                                return;
                            }
                            try {
                                const auto payload = nlohmann::json::parse(comparison.body);
                                wxString diff;
                                const auto lines = payload.find("diff");
                                if (lines != payload.end() && lines->is_array())
                                    for (const auto& line : *lines)
                                        if (line.is_string())
                                            diff << wx_from_utf8(line.get<std::string>()) << "\n";
                                if (diff.empty())
                                    diff = _L("No differences between these backups.");
                                wxDialog diff_dialog(weak_this, wxID_ANY, _L("Klipper configuration differences"),
                                    wxDefaultPosition, weak_this->FromDIP(wxSize(720, 520)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
                                auto* layout = new wxBoxSizer(wxVERTICAL);
                                auto* text = new wxTextCtrl(&diff_dialog, wxID_ANY, diff,
                                    wxDefaultPosition, weak_this->FromDIP(wxSize(680, 450)), wxTE_MULTILINE | wxTE_READONLY);
                                layout->Add(text, 1, wxALL | wxEXPAND, weak_this->FromDIP(12));
                                layout->Add(diff_dialog.CreateButtonSizer(wxOK), 0,
                                    wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_RIGHT, weak_this->FromDIP(12));
                                diff_dialog.SetSizer(layout);
                                wxGetApp().UpdateDarkUIWin(&diff_dialog);
                                diff_dialog.ShowModal();
                            } catch (const std::exception&) {
                                weak_this->m_status->SetLabel(_L("Backup comparison response could not be parsed."));
                            }
                        });
                    });
            } catch (const std::exception&) {
                weak_this->m_status->SetLabel(_L("Klipper backup list could not be parsed."));
            }
        });
    });
}

void QidiAdminDialog::refresh_events()
{
    if (m_events_request)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_events_request = QidiAdminGateway::fetch_events(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_events_request.reset();
            if (!result.ok)
                return;
            try {
                const auto entries = nlohmann::json::parse(result.body);
                if (!entries.is_array() || entries.empty()) {
                    weak_this->m_event->SetLabel(_L("Server events: no recent events."));
                    return;
                }
                const auto& event = entries.front();
                const wxString severity = wx_from_utf8(event.value("severity", "info"));
                wxString message = wx_from_utf8(event.value("message", ""));
                if (message.length() > 110)
                    message = message.Left(107) + "…";
                weak_this->m_event->SetLabel(wxString::Format(_L("Latest server event [%s]: %s"), severity, message));
            } catch (const std::exception&) {
                weak_this->m_event->SetLabel(_L("Server events: response could not be parsed."));
            }
        });
    });
}

void QidiAdminDialog::invalidate_access_role()
{
    ++m_access_generation;
    m_may_control = false;
    m_may_manage_spools = false;
    if (m_material_request)
        m_material_request->cancel();
    m_material_request.reset();
    if (m_macro_request)
        m_macro_request->cancel();
    m_macro_request.reset();
    m_macros.clear();
    m_macro_choice->Clear();
    m_macro_choice->Append(_L("Connect to view server macros."));
    m_macro_choice->SetSelection(0);
    m_spool_rows.clear();
    m_spool_list->Clear();
    m_spool_list->Append(_L("Connect to view Raspberry spools."));
    m_material->SetLabel(_L("Material: waiting for server access…"));
    m_diagnostics_json.clear();
    m_diagnostics_details->Disable();
    m_create_klipper_backup->Disable();
    m_compare_klipper_backups->Disable();
    if (m_access_role_request)
        m_access_role_request->cancel();
    m_access_role_request.reset();
    m_access_role->SetLabel(_L("Access role: checking…"));
    m_pause->Disable();
    m_resume->Disable();
    m_stop->Disable();
    m_run_macro->Disable();
    m_add_macro->Disable();
    m_edit_macro->Disable();
    m_add_spool->Disable();
    m_edit_spool->Disable();
    m_send_command->Disable();
    m_simulate_command->Disable();
    m_cancel_queued_command->Disable();
    m_retry_queued_command->Disable();
}

void QidiAdminDialog::refresh_access_role()
{
    if (m_access_role_request)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    const auto generation = m_access_generation;
    m_access_role_request = QidiAdminGateway::fetch_access_role(connection(), [weak_this, generation](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, generation, result = std::move(result)]() {
            if (!weak_this || generation != weak_this->m_access_generation)
                return;
            weak_this->m_access_role_request.reset();
            if (!result.ok) {
                weak_this->invalidate_access_role();
                weak_this->m_access_role->SetLabel(_L("Access role: unavailable."));
                return;
            }
            try {
                const auto payload = nlohmann::json::parse(result.body);
                const wxString role = wx_from_utf8(payload.value("role", "unknown"));
                weak_this->m_access_role->SetLabel(wxString::Format(_L("Access role: %s"), role));
                const bool may_control = role == _L("admin") || role == _L("operator");
                weak_this->m_may_control = may_control;
                weak_this->m_may_manage_spools = role == _L("admin");
                if (weak_this->m_pause) weak_this->m_pause->Enable(may_control);
                if (weak_this->m_resume) weak_this->m_resume->Enable(may_control);
                if (weak_this->m_stop) weak_this->m_stop->Enable(may_control);
                if (weak_this->m_run_macro) weak_this->m_run_macro->Enable(may_control);
                if (weak_this->m_add_macro) weak_this->m_add_macro->Enable(weak_this->m_may_manage_spools);
                if (weak_this->m_edit_macro) weak_this->m_edit_macro->Enable(weak_this->m_may_manage_spools && !weak_this->m_macros.empty());
                if (weak_this->m_add_spool) weak_this->m_add_spool->Enable(weak_this->m_may_manage_spools);
                if (weak_this->m_edit_spool) weak_this->m_edit_spool->Enable(weak_this->m_may_manage_spools && !weak_this->m_spool_rows.empty());
                if (weak_this->m_create_klipper_backup) weak_this->m_create_klipper_backup->Enable(weak_this->m_may_manage_spools);
                if (weak_this->m_compare_klipper_backups) weak_this->m_compare_klipper_backups->Enable(weak_this->m_may_manage_spools);
                if (weak_this->m_send_command) weak_this->m_send_command->Enable(may_control);
                if (weak_this->m_simulate_command) weak_this->m_simulate_command->Enable(may_control);
                if (weak_this->m_cancel_queued_command) weak_this->m_cancel_queued_command->Enable(may_control);
                if (weak_this->m_retry_queued_command) weak_this->m_retry_queued_command->Enable(may_control);
            } catch (const std::exception&) {
                weak_this->invalidate_access_role();
                weak_this->m_access_role->SetLabel(_L("Access role: response could not be parsed."));
            }
        });
    });
}

void QidiAdminDialog::refresh_macros()
{
    if (m_macro_request)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    const auto generation = m_access_generation;
    m_macro_request = QidiAdminGateway::fetch_macros(connection(), [weak_this, generation](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, generation, result = std::move(result)]() {
            if (!weak_this || generation != weak_this->m_access_generation)
                return;
            weak_this->m_macro_request.reset();
            if (!result.ok)
                return;
            try {
                const auto payload = nlohmann::json::parse(result.body);
                if (!payload.is_array())
                    return;
                weak_this->m_macros.clear();
                weak_this->m_macro_choice->Clear();
                for (const auto& item : payload) {
                    ServerMacro macro;
                    macro.id = item.value("id", 0);
                    macro.name = wx_from_utf8(item.value("name", "Unnamed macro"));
                    macro.description = wx_from_utf8(item.value("description", ""));
                    macro.script = item.value("script", "");
                    macro.requires_confirmation = item.value("requires_confirmation", 1) != 0;
                    macro.tags_json = item.value("tags_json", "[]");
                    if (!macro.script.empty())
                        weak_this->m_macros.emplace_back(std::move(macro));
                }
                weak_this->m_edit_macro->Enable(weak_this->m_may_manage_spools && !weak_this->m_macros.empty());
                if (weak_this->m_macros.empty()) {
                    weak_this->m_macro_choice->Append(_L("No server macros"));
                    weak_this->m_macro_choice->SetSelection(0);
                    return;
                }
                for (const auto& macro : weak_this->m_macros)
                    weak_this->m_macro_choice->Append(macro.description.empty() ? macro.name : macro.name + " — " + macro.description);
                weak_this->m_macro_choice->SetSelection(0);
            } catch (const std::exception&) {
                weak_this->m_macro_choice->Clear();
                weak_this->m_macro_choice->Append(_L("Server macros unavailable"));
                weak_this->m_macro_choice->SetSelection(0);
            }
        });
    });
}

void QidiAdminDialog::edit_macro(bool create_new)
{
    if (m_macro_write_request || !m_may_manage_spools)
        return;
    ServerMacro original;
    if (!create_new) {
        const int selection = m_macro_choice->GetSelection();
        if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= m_macros.size()) {
            m_status->SetLabel(_L("Choose a macro to edit."));
            return;
        }
        original = m_macros[selection];
    }
    wxDialog dialog(this, wxID_ANY, create_new ? _L("Add server macro") : _L("Edit server macro"),
        wxDefaultPosition, FromDIP(wxSize(540, 440)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(new wxStaticText(&dialog, wxID_ANY, _L("Name")), 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    auto* name = new wxTextCtrl(&dialog, wxID_ANY, original.name);
    outer->Add(name, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(12));
    outer->Add(new wxStaticText(&dialog, wxID_ANY, _L("Description")), 0, wxLEFT | wxRIGHT, FromDIP(12));
    auto* description = new wxTextCtrl(&dialog, wxID_ANY, original.description);
    outer->Add(description, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(12));
    outer->Add(new wxStaticText(&dialog, wxID_ANY, _L("G-code script")), 0, wxLEFT | wxRIGHT, FromDIP(12));
    auto* script = new wxTextCtrl(&dialog, wxID_ANY, wx_from_utf8(original.script),
        wxDefaultPosition, FromDIP(wxSize(480, 170)), wxTE_MULTILINE);
    outer->Add(script, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(12));
    auto* confirmation = new wxCheckBox(&dialog, wxID_ANY, _L("Confirm before running"));
    confirmation->SetValue(original.requires_confirmation);
    outer->Add(confirmation, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    outer->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxALIGN_RIGHT, FromDIP(12));
    dialog.SetSizerAndFit(outer);
    wxGetApp().UpdateDarkUIWin(&dialog);
    if (dialog.ShowModal() != wxID_OK)
        return;
    const std::string name_value = utf8_from_wx(name->GetValue());
    const std::string description_value = utf8_from_wx(description->GetValue());
    const std::string script_value = utf8_from_wx(script->GetValue());
    if (name_value.empty() || name_value.size() > 120 || description_value.size() > 500 ||
        script_value.empty() || script_value.size() > 16384) {
        wxMessageBox(_L("Macro name and G-code are required; check their lengths."),
            _L("Invalid macro"), wxOK | wxICON_WARNING, this);
        return;
    }
    nlohmann::json tags = nlohmann::json::array();
    if (!original.tags_json.empty()) {
        tags = nlohmann::json::parse(original.tags_json, nullptr, false);
        if (!tags.is_array())
            tags = nlohmann::json::array();
    }
    const nlohmann::json payload = {
        {"name", name_value}, {"description", description_value}, {"script", script_value},
        {"printer_id", "q2"}, {"requires_confirmation", confirmation->GetValue()}, {"tags", tags},
    };
    m_add_macro->Disable();
    m_edit_macro->Disable();
    const auto generation = m_access_generation;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_macro_write_request = QidiAdminGateway::save_macro(connection(), create_new ? 0 : original.id,
        payload.dump(), [weak_this, generation](QidiAdminResult result) {
            wxTheApp->CallAfter([weak_this, generation, result = std::move(result)]() {
                if (!weak_this)
                    return;
                weak_this->m_macro_write_request.reset();
                if (generation != weak_this->m_access_generation)
                    return;
                weak_this->m_add_macro->Enable(weak_this->m_may_manage_spools);
                weak_this->m_edit_macro->Enable(weak_this->m_may_manage_spools && !weak_this->m_macros.empty());
                if (!result.ok) {
                    weak_this->m_status->SetLabel(wxString::Format(_L("Could not save macro: HTTP %u"), result.status));
                    return;
                }
                weak_this->m_status->SetLabel(_L("Macro saved on Raspberry."));
                weak_this->refresh_macros();
            });
        });
}

void QidiAdminDialog::run_selected_macro()
{
    const int selection = m_macro_choice ? m_macro_choice->GetSelection() : wxNOT_FOUND;
    if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= m_macros.size()) {
        m_status->SetLabel(_L("Choose a server macro first."));
        return;
    }
    const ServerMacro& macro = m_macros[selection];
    if (macro.requires_confirmation &&
        wxMessageBox(wxString::Format(_L("Run macro '%s'?"), macro.name), _L("Confirm macro"), wxYES_NO | wxICON_WARNING, this) != wxYES)
        return;
    send_command(macro.script, 60, macro.name);
}

void QidiAdminDialog::refresh_materials()
{
    if (m_material_request)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    const auto generation = m_access_generation;
    m_material_request = QidiAdminGateway::fetch_materials(connection(), [weak_this, generation](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, generation, result = std::move(result)]() {
            if (!weak_this || generation != weak_this->m_access_generation)
                return;
            weak_this->m_material_request.reset();
            if (!result.ok) {
                weak_this->m_material->SetLabel(_L("Material: Raspberry catalog unavailable."));
                return;
            }
            try {
                const auto spools = nlohmann::json::parse(result.body);
                if (!spools.is_array())
                    throw std::runtime_error("Spool catalog is not an array");
                const int previous_selection = weak_this->m_spool_list->GetSelection();
                weak_this->m_spool_rows.clear();
                weak_this->m_spool_list->Clear();
                for (const auto& spool : spools) {
                    if (!spool.is_object())
                        continue;
                    const bool active = spool.value("active", 0) != 0;
                    const wxString name = wx_from_utf8(spool.value("name", "Unknown spool"));
                    const wxString type = wx_from_utf8(spool.value("material_type", ""));
                    const double weight = spool.value("remaining_weight_g", 0.0);
                    weak_this->m_spool_list->Append(wxString::Format(
                        _L("%s%s · %s · %.0f g"), active ? _L("● ") : wxString(), name, type, weight));
                    weak_this->m_spool_rows.emplace_back(spool.dump());
                }
                if (previous_selection != wxNOT_FOUND &&
                    static_cast<size_t>(previous_selection) < weak_this->m_spool_rows.size())
                    weak_this->m_spool_list->SetSelection(previous_selection);
                weak_this->m_edit_spool->Enable(weak_this->m_may_manage_spools && !weak_this->m_spool_rows.empty());
                if (spools.empty()) {
                    weak_this->m_material->SetLabel(_L("Material: no active spool configured."));
                    return;
                }
                const auto active = std::find_if(spools.begin(), spools.end(), [](const auto& spool) {
                    return spool.is_object() && spool.value("active", 0) != 0;
                });
                const auto& spool = active == spools.end() ? spools.front() : *active;
                const wxString name = wx_from_utf8(spool.value("name", "Unknown spool"));
                const wxString type = wx_from_utf8(spool.value("material_type", ""));
                const double weight = spool.value("remaining_weight_g", 0.0);
                weak_this->m_material->SetLabel(wxString::Format(_L("Material: %s · %s · %.0f g remaining"), name, type, weight));
            } catch (const std::exception&) {
                weak_this->m_material->SetLabel(_L("Material: response could not be parsed."));
            }
        });
    });
}

void QidiAdminDialog::edit_spool(bool create_new)
{
    if (m_spool_write_request || !m_may_manage_spools)
        return;
    nlohmann::json original = nlohmann::json::object();
    if (!create_new) {
        const int selection = m_spool_list->GetSelection();
        if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= m_spool_rows.size()) {
            m_material->SetLabel(_L("Select a spool to edit."));
            return;
        }
        try {
            original = nlohmann::json::parse(m_spool_rows[selection]);
        } catch (const std::exception&) {
            m_material->SetLabel(_L("Selected spool could not be parsed."));
            return;
        }
    }
    wxDialog dialog(this, wxID_ANY, create_new ? _L("Add spool") : _L("Edit spool"),
        wxDefaultPosition, FromDIP(wxSize(440, 390)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* form = new wxFlexGridSizer(2, FromDIP(8), FromDIP(12));
    form->AddGrowableCol(1, 1);
    auto add_field = [&](const wxString& label, const wxString& value) {
        form->Add(new wxStaticText(&dialog, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        auto* field = new wxTextCtrl(&dialog, wxID_ANY, value);
        form->Add(field, 1, wxEXPAND);
        return field;
    };
    auto* name = add_field(_L("Name"), wx_from_utf8(original.value("name", "")));
    auto* material_type = add_field(_L("Material (PLA/PETG/…)"), wx_from_utf8(original.value("material_type", "")));
    auto* slot = add_field(_L("Slot"), wx_from_utf8(original.value("slot", "")));
    auto* initial = add_field(_L("Initial weight, g"), wxString::Format("%.2f", original.value("initial_weight_g", 1000.0)));
    auto* remaining = add_field(_L("Remaining weight, g"), wxString::Format("%.2f", original.value("remaining_weight_g", 1000.0)));
    auto* price = add_field(_L("Spool price"), wxString::Format("%.2f", original.value("price", 0.0)));
    form->Add(new wxStaticText(&dialog, wxID_ANY, _L("Active for Q2")), 0, wxALIGN_CENTER_VERTICAL);
    auto* active = new wxCheckBox(&dialog, wxID_ANY, wxEmptyString);
    active->SetValue(original.value("active", 0) != 0);
    form->Add(active, 0);
    outer->Add(form, 1, wxALL | wxEXPAND, FromDIP(16));
    outer->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxALIGN_RIGHT, FromDIP(12));
    dialog.SetSizerAndFit(outer);
    wxGetApp().UpdateDarkUIWin(&dialog);
    if (dialog.ShowModal() != wxID_OK)
        return;

    double initial_g = 0.0, remaining_g = 0.0, spool_price = 0.0;
    if (name->GetValue().Trim().empty() || material_type->GetValue().Trim().empty() ||
        !initial->GetValue().ToDouble(&initial_g) || !remaining->GetValue().ToDouble(&remaining_g) ||
        !price->GetValue().ToDouble(&spool_price) || initial_g <= 0.0 || initial_g > 100000.0 ||
        remaining_g < 0.0 || remaining_g > initial_g || spool_price < 0.0 || spool_price > 1000000.0) {
        wxMessageBox(_L("Check the name, material and weights. Remaining weight must not exceed initial weight."),
            _L("Invalid spool"), wxOK | wxICON_WARNING, this);
        return;
    }
    const std::string id = create_new
        ? "spool-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())
        : original.value("id", "");
    nlohmann::json metadata = nlohmann::json::object();
    if (!create_new && original.contains("metadata_json") && original["metadata_json"].is_string()) {
        metadata = nlohmann::json::parse(original["metadata_json"].get<std::string>(), nullptr, false);
        if (!metadata.is_object())
            metadata = nlohmann::json::object();
    }
    const nlohmann::json payload = {
        {"id", id}, {"printer_id", "q2"}, {"slot", utf8_from_wx(slot->GetValue())},
        {"name", utf8_from_wx(name->GetValue())}, {"material_type", utf8_from_wx(material_type->GetValue())},
        {"color_value", original.value("color_value", 0)}, {"initial_weight_g", initial_g},
        {"remaining_weight_g", remaining_g}, {"price", spool_price}, {"active", active->GetValue()},
        {"metadata", metadata},
    };
    m_add_spool->Disable();
    m_edit_spool->Disable();
    wxWeakRef<QidiAdminDialog> weak_this(this);
    const auto generation = m_access_generation;
    m_spool_write_request = QidiAdminGateway::upsert_spool(connection(), id, payload.dump(), [weak_this, generation](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, generation, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_spool_write_request.reset();
            if (generation != weak_this->m_access_generation)
                return;
            weak_this->m_add_spool->Enable(weak_this->m_may_manage_spools);
            weak_this->m_edit_spool->Enable(weak_this->m_may_manage_spools && !weak_this->m_spool_rows.empty());
            if (!result.ok) {
                weak_this->m_material->SetLabel(wxString::Format(_L("Could not save spool: HTTP %u"), result.status));
                return;
            }
            weak_this->m_material->SetLabel(_L("Spool saved on Raspberry."));
            weak_this->refresh_materials();
        });
    });
}

QidiAdminConnection QidiAdminDialog::connection() const
{
    return {utf8_from_wx(m_endpoint->GetValue()), utf8_from_wx(m_api_key->GetValue()), m_verify_tls->GetValue()};
}

void QidiAdminDialog::save_connection()
{
    const QidiAdminConnection value = connection();
    wxGetApp().app_config->set("qidi_admin", "endpoint", QidiAdminGateway::normalized_endpoint(value.endpoint));
    wxGetApp().app_config->set("qidi_admin", "verify_tls", value.verify_tls ? "true" : "false");
    QidiAdminCredentials::save_api_key(value.api_key);
}

void QidiAdminDialog::check_connection()
{
    invalidate_access_role();
    refresh_access_role();
    refresh_status(true);
}

void QidiAdminDialog::refresh_status(bool announce)
{
    if (m_status_request)
        return;
    const QidiAdminConnection value = connection();
    if (announce) {
        m_check->Disable();
        m_status->SetLabel(_L("Checking Qidi Admin Server…"));
    }
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_status_request = QidiAdminGateway::fetch_status(value, [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (weak_this) {
                weak_this->m_status_request.reset();
                weak_this->show_result(result);
            }
        });
    });
    if (!m_status_request)
        show_result({false, 0, {}, _L("Connection settings are incomplete.").ToUTF8().data()});
}

void QidiAdminDialog::send_command(const std::string& script, int priority, const wxString& action, const std::string& queue_group)
{
    m_status->SetLabel(wxString::Format(_L("Sending: %s…"), action));
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_pending_request = QidiAdminGateway::enqueue_command(connection(), script, priority, queue_group, [weak_this, action](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, action, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_pending_request.reset();
            if (result.ok)
                weak_this->m_status->SetLabel(wxString::Format(_L("%s queued on Raspberry."), action));
            else
                weak_this->show_result(result);
        });
    });
}

void QidiAdminDialog::refresh_camera()
{
    if (m_camera_request || !m_camera)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    const auto generation = m_camera_generation;
    m_camera_request = QidiAdminGateway::fetch_camera_stream_frame(connection(), [weak_this, generation](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, generation, result = std::move(result)]() {
            if (!weak_this || generation != weak_this->m_camera_generation)
                return;
            weak_this->m_camera_request.reset();
            if (!weak_this->m_camera_timer.IsRunning())
                return;
            // A snapshot remains a useful degraded mode for camera proxies
            // that do not expose multipart MJPEG to native clients.
            if (!result.ok) {
                weak_this->refresh_camera_snapshot();
                return;
            }
            weak_this->show_camera_frame(result);
        });
    });
}

void QidiAdminDialog::refresh_camera_snapshot()
{
    if (m_camera_request || !m_camera)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    const auto generation = m_camera_generation;
    m_camera_request = QidiAdminGateway::fetch_camera_snapshot(connection(), [weak_this, generation](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, generation, result = std::move(result)]() {
            if (!weak_this || generation != weak_this->m_camera_generation)
                return;
            weak_this->m_camera_request.reset();
            if (!weak_this->m_camera_timer.IsRunning())
                return;
            weak_this->show_camera_frame(result);
        });
    });
}

void QidiAdminDialog::save_camera_snapshot()
{
    if (m_snapshot_save_request)
        return;
    if (!QidiAdminGateway::is_valid_endpoint(connection().endpoint)) {
        m_camera_status->SetLabel(_L("Configure the Raspberry server before saving a snapshot."));
        return;
    }
    m_save_camera_snapshot->Disable();
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_snapshot_save_request = QidiAdminGateway::fetch_camera_snapshot(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_snapshot_save_request.reset();
            weak_this->m_save_camera_snapshot->Enable();
            if (!result.ok || result.body.empty()) {
                weak_this->m_camera_status->SetLabel(_L("Snapshot could not be received from Raspberry."));
                return;
            }
            wxMemoryInputStream stream(result.body.data(), result.body.size());
            wxImage image(stream, wxBITMAP_TYPE_JPEG);
            if (!image.IsOk()) {
                weak_this->m_camera_status->SetLabel(_L("The camera returned an invalid JPEG snapshot."));
                return;
            }
            wxFileDialog dialog(weak_this, _L("Save camera snapshot"), wxEmptyString,
                "qidi-camera-snapshot.jpg", _L("JPEG image (*.jpg)|*.jpg"),
                wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (dialog.ShowModal() != wxID_OK)
                return;
            wxFileOutputStream file(dialog.GetPath());
            if (!file.IsOk() || file.Write(result.body.data(), result.body.size()).LastWrite() != result.body.size()) {
                weak_this->m_camera_status->SetLabel(_L("Could not save the camera snapshot."));
                return;
            }
            weak_this->m_camera_status->SetLabel(wxString::Format(
                _L("Saved original camera snapshot: %d x %d"), image.GetWidth(), image.GetHeight()));
        });
    });
}

void QidiAdminDialog::show_camera_frame(const QidiAdminResult& result)
{
    if (!result.ok || result.body.empty())
        return;
    wxMemoryInputStream stream(result.body.data(), result.body.size());
    wxImage image(stream, wxBITMAP_TYPE_JPEG);
    if (!image.IsOk())
        return;
    const int source_width = image.GetWidth();
    const int source_height = image.GetHeight();
    const auto now = std::chrono::steady_clock::now();
    if (m_camera_last_frame.time_since_epoch().count() != 0) {
        const double seconds = std::chrono::duration<double>(now - m_camera_last_frame).count();
        if (seconds > 0.001)
            m_camera_fps = m_camera_fps <= 0.0 ? 1.0 / seconds : (m_camera_fps * 0.7 + (1.0 / seconds) * 0.3);
    }
    m_camera_last_frame = now;
    const wxSize target = m_camera->GetSize();
    // Never stretch a camera source. Besides looking wrong, stretching makes
    // it harder to judge first-layer and nozzle details in the live view.
    if (target.x > 0 && target.y > 0 && source_width > 0 && source_height > 0) {
        const double scale = std::min(
            static_cast<double>(target.x) / static_cast<double>(source_width),
            static_cast<double>(target.y) / static_cast<double>(source_height));
        const int scaled_width = std::max(1, static_cast<int>(source_width * scale));
        const int scaled_height = std::max(1, static_cast<int>(source_height * scale));
        image.Rescale(scaled_width, scaled_height, wxIMAGE_QUALITY_HIGH);
    }
    m_camera->SetBitmap(wxBitmap(image));
    if (m_camera_status)
        m_camera_status->SetLabel(wxString::Format(_L("Live camera: %.1f FPS · %d x %d"), m_camera_fps, source_width, source_height));
    Layout();
}

void QidiAdminDialog::show_result(const QidiAdminResult& result)
{
    if (m_check)
        m_check->Enable();
    if (result.ok) {
        try {
            const auto payload = nlohmann::json::parse(result.body);
            const auto& printer = payload.at("printer").at("result").at("status");
            const auto& print_stats = printer.at("print_stats");
            const auto& display = printer.at("display_status");
            const auto& extruder = printer.at("extruder");
            const auto& bed = printer.at("heater_bed");
            const wxString state = wx_from_utf8(print_stats.value("state", "unknown"));
            const double progress = display.value("progress", 0.0) * 100.0;
            const double elapsed = print_stats.value("print_duration", 0.0);
            const double remaining = progress > 0.5 && elapsed > 0.0
                ? std::max(0.0, elapsed * (100.0 - progress) / progress)
                : -1.0;
            const double nozzle = extruder.value("temperature", 0.0);
            const double nozzle_target = extruder.value("target", 0.0);
            const double bed_temp = bed.value("temperature", 0.0);
            const double bed_target = bed.value("target", 0.0);
            const wxString duration = elapsed > 0.0
                ? (remaining >= 0.0
                    ? wxString::Format(_L(" · %s elapsed · ~%s left"), format_duration(elapsed), format_duration(remaining))
                    : wxString::Format(_L(" · %s elapsed"), format_duration(elapsed)))
                // clang-cl sees wxEmptyString as convertible in both
                // directions in this conditional expression. Use a concrete
                // wxString so MSVC and clang-cl agree on the value type.
                : wxString();
            m_status->SetLabel(wxString::Format(_L("%s · %.0f%%%s · Nozzle %.0f/%.0f°C · Bed %.0f/%.0f°C"),
                state, progress, duration, nozzle, nozzle_target, bed_temp, bed_target));
        } catch (const std::exception&) {
            m_status->SetLabel(wxString::Format(_L("Connected — HTTP %u."), result.status));
        }
        return;
    }
    const wxString detail = wx_from_utf8(result.error.empty() ? result.body : result.error);
    m_status->SetLabel(wxString::Format(_L("Connection failed%s"), detail.empty() ? "." : ": " + detail));
}

} // namespace Slic3r::GUI
