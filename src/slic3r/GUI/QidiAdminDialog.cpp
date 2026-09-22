#include "QidiAdminDialog.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
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

QidiAdminDialog::QidiAdminDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Qidi Admin Server"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_camera_timer(this)
{
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(new wxStaticText(this, wxID_ANY, _L("Connect Orca prepare and preview to your Raspberry Qidi Admin Server.")), 0, wxALL, FromDIP(16));

    auto* form = new wxFlexGridSizer(2, FromDIP(10), FromDIP(10));
    form->AddGrowableCol(1, 1);
    form->Add(new wxStaticText(this, wxID_ANY, _L("Server URL")), 0, wxALIGN_CENTER_VERTICAL);
    m_endpoint = new wxTextCtrl(this, wxID_ANY, from_u8(wxGetApp().app_config->get("qidi_admin", "endpoint")));
    m_endpoint->SetHint("https://morrax3d.ru");
    form->Add(m_endpoint, 1, wxEXPAND);
    form->Add(new wxStaticText(this, wxID_ANY, _L("API key")), 0, wxALIGN_CENTER_VERTICAL);
    m_api_key = new wxTextCtrl(this, wxID_ANY, from_u8(QidiAdminCredentials::load_api_key()), wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    m_api_key->SetHint(_L("Stored in Windows Credential Manager"));
    form->Add(m_api_key, 1, wxEXPAND);
    layout->Add(form, 1, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(16));

    m_verify_tls = new wxCheckBox(this, wxID_ANY, _L("Verify TLS certificate"));
    m_verify_tls->SetValue(wxGetApp().app_config->get("qidi_admin", "verify_tls") != "false");
    layout->Add(m_verify_tls, 0, wxALL, FromDIP(16));

    m_status = new wxStaticText(this, wxID_ANY, _L("Not checked yet."));
    layout->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

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
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_connection(); EndModal(wxID_OK); }, wxID_OK);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        refresh_camera();
        if (++m_refresh_ticks % 4 == 0)
            refresh_status();
    }, m_camera_timer.GetId());
    m_camera_timer.Start(500);
    wxGetApp().UpdateDlgDarkUI(this);
}

QidiAdminDialog::~QidiAdminDialog()
{
    m_camera_timer.Stop();
    if (m_pending_request)
        m_pending_request->cancel();
    if (m_status_request)
        m_status_request->cancel();
    if (m_camera_request)
        m_camera_request->cancel();
}

QidiAdminConnection QidiAdminDialog::connection() const
{
    return {into_u8(m_endpoint->GetValue()), into_u8(m_api_key->GetValue()), m_verify_tls->GetValue()};
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
            const wxString state = from_u8(print_stats.value("state", "unknown"));
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
    const wxString detail = from_u8(result.error.empty() ? result.body : result.error);
    m_status->SetLabel(wxString::Format(_L("Connection failed%s"), detail.empty() ? "." : ": " + detail));
}

} // namespace Slic3r::GUI
