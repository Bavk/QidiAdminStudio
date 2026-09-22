#include "QidiAdminDialog.hpp"

#include <algorithm>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/listbox.h>
#include <wx/mstream.h>
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

} // namespace

QidiAdminDialog::QidiAdminDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Qidi Admin Server"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_camera_timer(this)
{
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(new wxStaticText(this, wxID_ANY, _L("Connect Orca prepare and preview to your Raspberry Qidi Admin Server.")), 0, wxALL, FromDIP(16));

    auto* form = new wxFlexGridSizer(2, FromDIP(10), FromDIP(10));
    form->AddGrowableCol(1, 1);
    form->Add(new wxStaticText(this, wxID_ANY, _L("Server URL")), 0, wxALIGN_CENTER_VERTICAL);
    m_endpoint = new wxTextCtrl(this, wxID_ANY, wx_from_utf8(wxGetApp().app_config->get("qidi_admin", "endpoint")));
    m_endpoint->SetHint("https://morrax3d.ru");
    form->Add(m_endpoint, 1, wxEXPAND);
    form->Add(new wxStaticText(this, wxID_ANY, _L("API key")), 0, wxALIGN_CENTER_VERTICAL);
    m_api_key = new wxTextCtrl(this, wxID_ANY, wx_from_utf8(QidiAdminCredentials::load_api_key()), wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    m_api_key->SetHint(_L("Stored in Windows Credential Manager"));
    form->Add(m_api_key, 1, wxEXPAND);
    layout->Add(form, 1, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(16));

    m_verify_tls = new wxCheckBox(this, wxID_ANY, _L("Verify TLS certificate"));
    m_verify_tls->SetValue(wxGetApp().app_config->get("qidi_admin", "verify_tls") != "false");
    layout->Add(m_verify_tls, 0, wxALL, FromDIP(16));

    m_status = new wxStaticText(this, wxID_ANY, _L("Not checked yet."));
    layout->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_material = new wxStaticText(this, wxID_ANY, _L("Material: loading from Raspberry…"));
    layout->Add(m_material, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_maintenance = new wxStaticText(this, wxID_ANY, _L("Maintenance: loading from Raspberry…"));
    layout->Add(m_maintenance, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    layout->Add(new wxStaticText(this, wxID_ANY, _L("Raspberry command queue")), 0,
        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_queue = new wxListBox(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(480, 84)));
    m_queue->Append(_L("Loading queue…"));
    layout->Add(m_queue, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));
    auto* cancel_command_button = new wxButton(this, wxID_ANY, _L("Cancel selected queued command"));
    layout->Add(cancel_command_button, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_RIGHT, FromDIP(16));

    auto* macro_row = new wxBoxSizer(wxHORIZONTAL);
    m_macro_choice = new wxChoice(this, wxID_ANY);
    m_macro_choice->SetMinSize(FromDIP(wxSize(330, -1)));
    m_macro_choice->Append(_L("Loading server macros…"));
    m_macro_choice->SetSelection(0);
    auto* run_macro_button = new wxButton(this, wxID_ANY, _L("Run macro"));
    macro_row->Add(m_macro_choice, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
    macro_row->Add(run_macro_button, 0);
    layout->Add(macro_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));

    m_camera = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, FromDIP(wxSize(480, 270)));
    m_camera->SetMinSize(FromDIP(wxSize(480, 270)));
    layout->Add(m_camera, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER_HORIZONTAL, FromDIP(16));

    auto* quick_actions = new wxBoxSizer(wxHORIZONTAL);
    m_pause = new wxButton(this, wxID_ANY, _L("Pause"));
    m_resume = new wxButton(this, wxID_ANY, _L("Resume"));
    m_stop = new wxButton(this, wxID_ANY, _L("Emergency stop"));
    quick_actions->Add(m_pause, 0, wxRIGHT, FromDIP(8));
    quick_actions->Add(m_resume, 0, wxRIGHT, FromDIP(8));
    quick_actions->Add(m_stop, 0);
    layout->Add(quick_actions, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    layout->Add(new wxStaticText(this, wxID_ANY, _L("G-code terminal (queued through Raspberry)")), 0,
        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_command = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(480, 72)), wxTE_MULTILINE);
    m_command->SetHint("SET_PIN PIN=caselight VALUE=1");
    layout->Add(m_command, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));
    auto* send_command_button = new wxButton(this, wxID_ANY, _L("Queue G-code"));
    layout->Add(send_command_button, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_RIGHT, FromDIP(16));

    auto* buttons = new wxStdDialogButtonSizer();
    m_check = new wxButton(this, wxID_ANY, _L("Check server"));
    buttons->AddButton(m_check);
    buttons->AddButton(new wxButton(this, wxID_OK, _L("Save")));
    buttons->AddButton(new wxButton(this, wxID_CANCEL));
    buttons->Realize();
    layout->Add(buttons, 0, wxALL | wxALIGN_RIGHT, FromDIP(12));
    SetSizerAndFit(layout);
    SetMinSize(FromDIP(wxSize(500, 0)));
    CentreOnParent();

    m_check->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { check_connection(); });
    m_pause->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_command("PAUSE", 80, _L("Pause")); });
    m_resume->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_command("RESUME", 80, _L("Resume")); });
    m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (wxMessageBox(_L("Immediately stop the printer?"), _L("Emergency stop"), wxYES_NO | wxICON_WARNING, this) == wxYES)
            send_command("M112", 100, _L("Emergency stop"));
    });
    send_command_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string script = utf8_from_wx(m_command->GetValue());
        if (script.empty()) {
            m_status->SetLabel(_L("Enter G-code before queueing it."));
            return;
        }
        send_command(script, 50, _L("G-code"));
        m_command->Clear();
    });
    run_macro_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { run_selected_macro(); });
    cancel_command_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_selected_command(); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_connection(); EndModal(wxID_OK); }, wxID_OK);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        // Do not continuously create failing requests while the connection
        // form is still empty. This dialog is also used as the first-run
        // setup surface.
        const QidiAdminConnection value = connection();
        if (!QidiAdminGateway::is_valid_endpoint(value.endpoint) || value.api_key.empty())
            return;
        refresh_camera();
        if (++m_refresh_ticks % 4 == 0)
            refresh_status();
        if (m_refresh_ticks % 8 == 0)
            refresh_command_queue();
        if (m_refresh_ticks % 12 == 0)
            refresh_materials();
        if (m_refresh_ticks % 20 == 0)
            refresh_macros();
        if (m_refresh_ticks % 60 == 0)
            refresh_maintenance();
    }, m_camera_timer.GetId());
    m_camera_timer.Start(500);
    const QidiAdminConnection saved_connection = connection();
    if (QidiAdminGateway::is_valid_endpoint(saved_connection.endpoint) && !saved_connection.api_key.empty()) {
        refresh_status();
        refresh_materials();
        refresh_macros();
        refresh_maintenance();
        refresh_command_queue();
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
    if (m_queue_request)
        m_queue_request->cancel();
    if (m_queue_cancel_request)
        m_queue_cancel_request->cancel();
    if (m_camera_request)
        m_camera_request->cancel();
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
                    wxString script = wx_from_utf8(entry.value("script", ""));
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

void QidiAdminDialog::send_command(const std::string& script, int priority, const wxString& action)
{
    m_status->SetLabel(wxString::Format(_L("Sending: %s…"), action));
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_pending_request = QidiAdminGateway::enqueue_command(connection(), script, priority, [weak_this, action](QidiAdminResult result) {
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
    const wxSize target = m_camera->GetSize();
    if (target.x > 0 && target.y > 0)
        image.Rescale(target.x, target.y, wxIMAGE_QUALITY_HIGH);
    m_camera->SetBitmap(wxBitmap(image));
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
            const double nozzle = extruder.value("temperature", 0.0);
            const double nozzle_target = extruder.value("target", 0.0);
            const double bed_temp = bed.value("temperature", 0.0);
            const double bed_target = bed.value("target", 0.0);
            m_status->SetLabel(wxString::Format(_L("%s · %.0f%% · Nozzle %.0f/%.0f°C · Bed %.0f/%.0f°C"),
                state, progress, nozzle, nozzle_target, bed_temp, bed_target));
        } catch (const std::exception&) {
            m_status->SetLabel(wxString::Format(_L("Connected — HTTP %u."), result.status));
        }
        return;
    }
    const wxString detail = wx_from_utf8(result.error.empty() ? result.body : result.error);
    m_status->SetLabel(wxString::Format(_L("Connection failed%s"), detail.empty() ? "." : ": " + detail));
}

} // namespace Slic3r::GUI
