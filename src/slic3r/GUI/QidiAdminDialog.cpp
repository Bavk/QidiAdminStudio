#include "QidiAdminDialog.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/listbox.h>
#include <wx/mstream.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

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
    : DPIDialog(parent, wxID_ANY, _L("Qidi Admin Server"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_camera_timer(this)
{
    // The administrator contains a live video feed, queues and a terminal.
    // Keeping it in a scrolled child is essential on laptop screens: wx's
    // default "fit" behaviour otherwise makes the bottom actions unreachable.
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
    m_maintenance = new wxStaticText(content, wxID_ANY, _L("Maintenance: loading from Raspberry…"));
    layout->Add(m_maintenance, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_history = new wxStaticText(content, wxID_ANY, _L("Print history: loading from Raspberry…"));
    layout->Add(m_history, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_diagnostics = new wxStaticText(content, wxID_ANY, _L("Raspberry health: loading…"));
    layout->Add(m_diagnostics, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_event = new wxStaticText(content, wxID_ANY, _L("Server events: loading…"));
    layout->Add(m_event, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_access_role = new wxStaticText(content, wxID_ANY, _L("Access role: checking…"));
    layout->Add(m_access_role, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    layout->Add(new wxStaticText(content, wxID_ANY, _L("Raspberry command queue")), 0,
        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_queue = new wxListBox(content, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(480, 84)));
    m_queue->Append(_L("Loading queue…"));
    layout->Add(m_queue, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));
    auto* cancel_command_button = new wxButton(content, wxID_ANY, _L("Cancel selected queued command"));
    auto* retry_command_button = new wxButton(content, wxID_ANY, _L("Retry selected failed command"));
    auto* queue_actions = new wxBoxSizer(wxHORIZONTAL);
    queue_actions->Add(retry_command_button, 0, wxRIGHT, FromDIP(8));
    queue_actions->Add(cancel_command_button, 0);
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
    macro_row->Add(m_macro_choice, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
    macro_row->Add(m_run_macro, 0);
    layout->Add(macro_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));

    m_camera = new wxStaticBitmap(content, wxID_ANY, wxNullBitmap, wxDefaultPosition, FromDIP(wxSize(480, 270)));
    m_camera->SetMinSize(FromDIP(wxSize(480, 270)));
    layout->Add(m_camera, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER_HORIZONTAL, FromDIP(16));
    m_camera_status = new wxStaticText(content, wxID_ANY, _L("Live camera: waiting for first frame…"));
    layout->Add(m_camera_status, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER_HORIZONTAL, FromDIP(16));

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

    auto* buttons = new wxStdDialogButtonSizer();
    m_check = new wxButton(content, wxID_ANY, _L("Check server"));
    buttons->AddButton(m_check);
    buttons->AddButton(new wxButton(content, wxID_OK, _L("Save")));
    buttons->AddButton(new wxButton(content, wxID_CANCEL));
    buttons->Realize();
    layout->Add(buttons, 0, wxALL | wxALIGN_RIGHT, FromDIP(12));
    content->SetSizer(layout);
    // Explicitly calculate the virtual size. This is required for reliable
    // vertical scrolling with some wxWidgets Windows builds.
    layout->FitInside(content);
    content->SetMinSize(FromDIP(wxSize(520, 580)));
    root_layout->Add(content, 1, wxEXPAND);
    SetSizer(root_layout);
    SetSize(FromDIP(wxSize(620, 760)));
    SetMinSize(FromDIP(wxSize(520, 480)));
    CentreOnParent();

    m_check->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { check_connection(); });
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
    m_simulate_command->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { simulate_command(); });
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
    cancel_command_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_selected_command(); });
    retry_command_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { retry_selected_command(); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_connection(); EndModal(wxID_OK); }, wxID_OK);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        // Do not continuously create failing requests while the connection
        // form is still empty. This dialog is also used as the first-run
        // setup surface.
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
    m_camera_timer.Start(250);
    const QidiAdminConnection saved_connection = connection();
    if (QidiAdminGateway::is_valid_endpoint(saved_connection.endpoint)) {
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
    wxGetApp().UpdateDlgDarkUI(this);
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
    if (m_macro_request)
        m_macro_request->cancel();
    if (m_maintenance_request)
        m_maintenance_request->cancel();
    if (m_history_request)
        m_history_request->cancel();
    if (m_diagnostics_request)
        m_diagnostics_request->cancel();
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
                    weak_this->m_queue_command_ids.emplace_back(entry.value("id", 0));
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
    m_diagnostics_request = QidiAdminGateway::fetch_diagnostics(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_diagnostics_request.reset();
            if (!result.ok)
                return;
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
            } catch (const std::exception&) {
                weak_this->m_diagnostics->SetLabel(_L("Raspberry health: response could not be parsed."));
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

void QidiAdminDialog::refresh_access_role()
{
    if (m_access_role_request)
        return;
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_access_role_request = QidiAdminGateway::fetch_access_role(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_access_role_request.reset();
            if (!result.ok)
                return;
            try {
                const auto payload = nlohmann::json::parse(result.body);
                const wxString role = wx_from_utf8(payload.value("role", "unknown"));
                weak_this->m_access_role->SetLabel(wxString::Format(_L("Access role: %s"), role));
                const bool may_control = role == _L("admin") || role == _L("operator");
                if (weak_this->m_pause) weak_this->m_pause->Enable(may_control);
                if (weak_this->m_resume) weak_this->m_resume->Enable(may_control);
                if (weak_this->m_stop) weak_this->m_stop->Enable(may_control);
                if (weak_this->m_run_macro) weak_this->m_run_macro->Enable(may_control);
                if (weak_this->m_send_command) weak_this->m_send_command->Enable(may_control);
                if (weak_this->m_simulate_command) weak_this->m_simulate_command->Enable(may_control);
            } catch (const std::exception&) {
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
    m_macro_request = QidiAdminGateway::fetch_macros(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
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
                    macro.name = wx_from_utf8(item.value("name", "Unnamed macro"));
                    macro.description = wx_from_utf8(item.value("description", ""));
                    macro.script = item.value("script", "");
                    macro.requires_confirmation = item.value("requires_confirmation", true);
                    if (!macro.script.empty())
                        weak_this->m_macros.emplace_back(std::move(macro));
                }
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
    m_material_request = QidiAdminGateway::fetch_materials(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_material_request.reset();
            if (!result.ok)
                return;
            try {
                const auto spools = nlohmann::json::parse(result.body);
                if (!spools.is_array() || spools.empty()) {
                    weak_this->m_material->SetLabel(_L("Material: no active spool configured."));
                    return;
                }
                const auto active = std::find_if(spools.begin(), spools.end(), [](const auto& spool) {
                    return spool.value("active", false);
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
    m_camera_request = QidiAdminGateway::fetch_camera_stream_frame(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_camera_request.reset();
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
    m_camera_request = QidiAdminGateway::fetch_camera_snapshot(connection(), [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (!weak_this)
                return;
            weak_this->m_camera_request.reset();
            weak_this->show_camera_frame(result);
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
                : wxEmptyString;
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
