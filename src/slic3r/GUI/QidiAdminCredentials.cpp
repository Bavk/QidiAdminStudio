#include "QidiAdminCredentials.hpp"

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>

#include <wx/string.h>

#pragma comment(lib, "Advapi32.lib")
#endif

namespace Slic3r::GUI::QidiAdminCredentials {

std::string load_api_key()
{
#ifdef _WIN32
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(L"QidiAdminStudio.ApiKey", CRED_TYPE_GENERIC, 0, &credential))
        return {};
    const wxString value(reinterpret_cast<const wchar_t*>(credential->CredentialBlob),
                         credential->CredentialBlobSize / sizeof(wchar_t));
    const std::string api_key = value.ToUTF8().data();
    CredFree(credential);
    return api_key;
#else
    return {};
#endif
}

bool save_api_key(const std::string& api_key)
{
#ifdef _WIN32
    if (api_key.empty()) {
        const BOOL deleted = CredDeleteW(L"QidiAdminStudio.ApiKey", CRED_TYPE_GENERIC, 0);
        return deleted || GetLastError() == ERROR_NOT_FOUND;
    }
    const wxString value = wxString::FromUTF8(api_key);
    CREDENTIALW credential {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(L"QidiAdminStudio.ApiKey");
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = static_cast<DWORD>(value.length() * sizeof(wchar_t));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(value.wc_str()));
    credential.UserName = const_cast<LPWSTR>(L"Qidi Admin Studio");
    return CredWriteW(&credential, 0) != FALSE;
#else
    (void)api_key;
    return true;
#endif
}

} // namespace Slic3r::GUI::QidiAdminCredentials
