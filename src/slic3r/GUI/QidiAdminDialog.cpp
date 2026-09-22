#include "QidiAdminDialog.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"

namespace Slic3r::GUI {

QidiAdminDialog::QidiAdminDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Qidi Admin Server"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
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
    m_api_key = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    m_api_key->SetHint(_L("Entered for this session only"));
    form->Add(m_api_key, 1, wxEXPAND);
    layout->Add(form, 1, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(16));

    m_verify_tls = new wxCheckBox(this, wxID_ANY, _L("Verify TLS certificate"));
    m_verify_tls->SetValue(wxGetApp().app_config->get("qidi_admin", "verify_tls") != "false");
    layout->Add(m_verify_tls, 0, wxALL, FromDIP(16));

    m_status = new wxStaticText(this, wxID_ANY, _L("Not checked yet."));
    layout->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

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
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_connection(); EndModal(wxID_OK); }, wxID_OK);
    wxGetApp().UpdateDlgDarkUI(this);
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
    // API keys are intentionally not written to the slicer configuration.
}

void QidiAdminDialog::check_connection()
{
    const QidiAdminConnection value = connection();
    m_check->Disable();
    m_status->SetLabel(_L("Checking Qidi Admin Server…"));
    wxWeakRef<QidiAdminDialog> weak_this(this);
    m_pending_request = QidiAdminGateway::fetch_status(value, [weak_this](QidiAdminResult result) {
        wxTheApp->CallAfter([weak_this, result = std::move(result)]() {
            if (weak_this)
                weak_this->show_result(result);
        });
    });
    if (!m_pending_request)
        show_result({false, 0, {}, _L("Connection settings are incomplete.").ToUTF8().data()});
}

void QidiAdminDialog::show_result(const QidiAdminResult& result)
{
    if (m_check)
        m_check->Enable();
    if (result.ok) {
        m_status->SetLabel(wxString::Format(_L("Connected — HTTP %u."), result.status));
        return;
    }
    const wxString detail = from_u8(result.error.empty() ? result.body : result.error);
    m_status->SetLabel(wxString::Format(_L("Connection failed%s"), detail.empty() ? "." : ": " + detail));
}

} // namespace Slic3r::GUI
