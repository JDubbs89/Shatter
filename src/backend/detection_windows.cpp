#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "backend/selection.hpp"

namespace shatter::backend {

Server detect_server()
{
    // Track the audio service, not endpoint/default-device changes. If SCM is
    // inaccessible, retain WASAPI discovery rather than disabling audio.
    const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) return {Kind::wasapi, "unknown"};
    const SC_HANDLE service = OpenServiceW(manager, L"Audiosrv", SERVICE_QUERY_STATUS);
    if (service == nullptr) { CloseServiceHandle(manager); return {Kind::wasapi, "unknown"}; }
    SERVICE_STATUS_PROCESS status{};
    DWORD size = 0;
    const BOOL queried = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &size);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!queried) return {Kind::wasapi, "unknown"};
    if (status.dwCurrentState != SERVICE_RUNNING) return {};
    return {Kind::wasapi, std::to_string(status.dwProcessId)};
}

} // namespace shatter::backend
