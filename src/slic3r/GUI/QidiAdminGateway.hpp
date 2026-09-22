#ifndef slic3r_GUI_QidiAdminGateway_hpp_
#define slic3r_GUI_QidiAdminGateway_hpp_

#include <functional>
#include <string>

#include "slic3r/Utils/Http.hpp"

namespace Slic3r::GUI {

// Connection settings deliberately contain only the server address and token.
// They must be stored in AppConfig / the platform credential store, never in a
// 3MF project or a printer preset that may be shared with other people.
struct QidiAdminConnection {
    std::string endpoint;
    std::string api_key;
    bool        verify_tls {true};
};

struct QidiAdminResult {
    bool        ok {false};
    unsigned    status {0};
    std::string body;
    std::string error;
};

class QidiAdminGateway {
public:
    using ResultCallback = std::function<void(QidiAdminResult)>;

    // A conservative validation step prevents accidental use of malformed or
    // credential-bearing URLs before a request leaves the slicer.
    static bool        is_valid_endpoint(const std::string& endpoint);
    static std::string normalized_endpoint(const std::string& endpoint);

    // These operations are asynchronous. Keep the returned request alive when
    // cancellation from a UI owner is needed.
    static Http::Ptr fetch_status(const QidiAdminConnection& connection, ResultCallback callback);
    static Http::Ptr fetch_camera_snapshot(const QidiAdminConnection& connection, ResultCallback callback);
    static Http::Ptr fetch_materials(const QidiAdminConnection& connection, ResultCallback callback);
    static Http::Ptr fetch_macros(const QidiAdminConnection& connection, ResultCallback callback);
    static Http::Ptr fetch_maintenance_tasks(const QidiAdminConnection& connection, ResultCallback callback);
    static Http::Ptr fetch_command_queue(const QidiAdminConnection& connection, ResultCallback callback);
    static Http::Ptr cancel_queued_command(const QidiAdminConnection& connection, int command_id, ResultCallback callback);
    static Http::Ptr preflight(const QidiAdminConnection& connection,
                               const std::string& filename,
                               const std::string& printer_id,
                               ResultCallback callback);
    static Http::Ptr enqueue_command(const QidiAdminConnection& connection,
                                     const std::string& script,
                                     int priority,
                                     ResultCallback callback);

private:
    static Http::Ptr request(const QidiAdminConnection& connection,
                             const std::string& path,
                             const std::string* json_body,
                             ResultCallback callback);
};

} // namespace Slic3r::GUI

#endif
