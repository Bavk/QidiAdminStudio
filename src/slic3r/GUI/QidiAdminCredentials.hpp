#ifndef slic3r_GUI_QidiAdminCredentials_hpp_
#define slic3r_GUI_QidiAdminCredentials_hpp_

#include <string>

namespace Slic3r::GUI::QidiAdminCredentials {

std::string load_api_key();
bool        save_api_key(const std::string& api_key);

} // namespace Slic3r::GUI::QidiAdminCredentials

#endif
