#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <wrl/client.h>

#include "backend/iaudio_backend.hpp"
#include "mixer/engine.hpp"
#include "mixer/health.hpp"
#include "backend/wasapi/wasapi_types.hpp"

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shatter::backend {
namespace {

using Microsoft::WRL::ComPtr;

std::string hresult_message(HRESULT result)
{
    wchar_t buffer[512]{};
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0, buffer, static_cast<DWORD>(std::size(buffer)), nullptr);
    std::string message;
    if (count != 0) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(count),
                                              nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            message.resize(static_cast<std::size_t>(bytes));
            WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(count), message.data(),
                                bytes, nullptr, nullptr);
        }
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
            message.pop_back();
    }
    std::ostringstream output;
    output << "HRESULT 0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    if (!message.empty()) output << " (" << message << ')';
    return output.str();
}

class ComApartment {
public:
    explicit ComApartment(std::string& error)
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(result)) {
            initialized_ = true;
            return;
        }
        // A caller may already have initialized COM as an STA. Calls are synchronous and
        // all COM objects created below stay on this same thread.
        if (result == RPC_E_CHANGED_MODE) {
            changed_mode_ = true;
            return;
        }
        error = "cannot initialize Windows audio COM apartment: " + hresult_message(result);
    }

    ~ComApartment()
    {
        if (initialized_) CoUninitialize();
    }

    bool ready() const { return initialized_ || changed_mode_; }

private:
    bool initialized_ = false;
    bool changed_mode_ = false;
};

std::string to_utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                        bytes, nullptr, nullptr);
    return result;
}

std::string process_binary(DWORD process_id)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) return {};
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL queried = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (!queried) return {};
    path.resize(length);
    const auto separator = path.find_last_of(L"\\/");
    return to_utf8(separator == std::wstring::npos ? std::wstring_view(path)
                                                    : std::wstring_view(path).substr(separator + 1));
}

bool create_device_enumerator(ComPtr<IMMDeviceEnumerator>& enumerator, std::string& error)
{
    const HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(enumerator.GetAddressOf()));
    if (FAILED(result)) {
        error = "cannot connect to the Windows audio device service: " + hresult_message(result);
        return false;
    }
    return true;
}

bool endpoint_id(IMMDevice* device, std::string& id, std::string& error)
{
    LPWSTR value = nullptr;
    const HRESULT result = device->GetId(&value);
    if (FAILED(result)) {
        error = "cannot read a Windows playback endpoint ID: " + hresult_message(result);
        return false;
    }
    id = to_utf8(value == nullptr ? std::wstring_view{} : std::wstring_view(value));
    CoTaskMemFree(value);
    return true;
}

bool endpoint_name(IMMDevice* device, std::string& name, std::string& error)
{
    ComPtr<IPropertyStore> properties;
    HRESULT result = device->OpenPropertyStore(STGM_READ, properties.GetAddressOf());
    if (FAILED(result)) {
        error = "cannot read a Windows playback endpoint name: " + hresult_message(result);
        return false;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    result = properties->GetValue(PKEY_Device_FriendlyName, &value);
    if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
        name = to_utf8(value.pwszVal);
    PropVariantClear(&value);
    if (FAILED(result)) {
        error = "cannot read a Windows playback endpoint name: " + hresult_message(result);
        return false;
    }
    return true;
}

bool enumerate_native_endpoints(std::vector<WasapiPlaybackEndpoint>& endpoints, std::string& error)
{
    ComApartment apartment(error);
    if (!apartment.ready()) return false;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (!create_device_enumerator(enumerator, error)) return false;

    std::string default_id;
    ComPtr<IMMDevice> default_device;
    const HRESULT default_result = enumerator->GetDefaultAudioEndpoint(
        eRender, eMultimedia, default_device.GetAddressOf());
    if (SUCCEEDED(default_result)) {
        if (!endpoint_id(default_device.Get(), default_id, error)) return false;
    } else if (default_result != E_NOTFOUND) {
        error = "cannot determine the default Windows playback endpoint: " +
            hresult_message(default_result);
        return false;
    }

    ComPtr<IMMDeviceCollection> collection;
    HRESULT result = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.GetAddressOf());
    if (FAILED(result)) {
        error = "cannot enumerate Windows playback endpoints: " + hresult_message(result);
        return false;
    }
    UINT count = 0;
    result = collection->GetCount(&count);
    if (FAILED(result)) {
        error = "cannot count Windows playback endpoints: " + hresult_message(result);
        return false;
    }
    endpoints.clear();
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        result = collection->Item(index, device.GetAddressOf());
        if (FAILED(result)) {
            error = "cannot read a Windows playback endpoint: " + hresult_message(result);
            return false;
        }
        WasapiPlaybackEndpoint endpoint;
        if (!endpoint_id(device.Get(), endpoint.id, error) ||
            !endpoint_name(device.Get(), endpoint.name, error)) return false;
        if (endpoint.name.empty()) endpoint.name = endpoint.id;
        endpoint.is_default = endpoint.id == default_id;
        endpoints.push_back(std::move(endpoint));
    }
    error.clear();
    return true;
}

bool enumerate_playback_sessions(std::vector<WasapiPlaybackSession>& sessions, std::string& error)
{
    ComApartment apartment(error);
    if (!apartment.ready()) return false;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (!create_device_enumerator(enumerator, error)) return false;
    ComPtr<IMMDeviceCollection> devices;
    HRESULT result = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.GetAddressOf());
    if (FAILED(result)) {
        error = "cannot enumerate Windows playback endpoints: " + hresult_message(result);
        return false;
    }
    UINT device_count = 0;
    result = devices->GetCount(&device_count);
    if (FAILED(result)) {
        error = "cannot count Windows playback endpoints: " + hresult_message(result);
        return false;
    }

    sessions.clear();
    for (UINT device_index = 0; device_index < device_count; ++device_index) {
        ComPtr<IMMDevice> device;
        result = devices->Item(device_index, device.GetAddressOf());
        if (FAILED(result)) continue;
        ComPtr<IAudioSessionManager2> manager;
        result = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(manager.GetAddressOf()));
        if (FAILED(result)) continue;
        ComPtr<IAudioSessionEnumerator> session_list;
        result = manager->GetSessionEnumerator(session_list.GetAddressOf());
        if (FAILED(result)) continue;
        int session_count = 0;
        result = session_list->GetCount(&session_count);
        if (FAILED(result)) continue;
        for (int session_index = 0; session_index < session_count; ++session_index) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(session_list->GetSession(session_index, control.GetAddressOf()))) continue;
            AudioSessionState state{};
            if (FAILED(control->GetState(&state)) || state != AudioSessionStateActive) continue;
            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control.As(&control2))) continue;
            DWORD process_id = 0;
            if (FAILED(control2->GetProcessId(&process_id))) process_id = 0;

            WasapiPlaybackSession session;
            session.process_id = process_id;
            session.process_binary = process_binary(process_id);
            LPWSTR display_name = nullptr;
            if (SUCCEEDED(control->GetDisplayName(&display_name)) && display_name != nullptr)
                session.application = to_utf8(display_name);
            CoTaskMemFree(display_name);
            if (session.application.empty()) session.application = session.process_binary;
            if (session.application.empty())
                session.application = "Process " + std::to_string(session.process_id);
            LPWSTR identifier = nullptr;
            if (SUCCEEDED(control2->GetSessionInstanceIdentifier(&identifier)) && identifier != nullptr)
                session.session_id = to_utf8(identifier);
            CoTaskMemFree(identifier);
            sessions.push_back(std::move(session));
        }
    }
    error.clear();
    return true;
}

std::string unsupported_message()
{
    return "WASAPI currently supports playback session and endpoint discovery only";
}

class WasapiBackend final : public IAudioBackend {
public:
    static std::string unsupported_operation()
    {
        return "WASAPI currently supports playback session and endpoint discovery only";
    }
    bool enumerate_endpoints(std::vector<Endpoint>& endpoints, std::string& error) const override
    {
        if (!started_.load(std::memory_order_acquire)) {
            error = "WASAPI backend is not running";
            return false;
        }
        std::vector<WasapiPlaybackEndpoint> devices;
        std::vector<WasapiPlaybackSession> sessions;
        if (!enumerate_native_endpoints(devices, error) || !enumerate_playback_sessions(sessions, error))
            return false;
        std::vector<Endpoint> result;
        for (const auto& device : devices)
            result.push_back({device.id, device.id, device.name,
                              EndpointKind::playback_device, device.is_default, {}});
        for (const auto& session : sessions) {
            const std::string id = session.session_id.empty()
                ? "PID " + std::to_string(session.process_id) : session.session_id;
            result.push_back({id, id, session.application, EndpointKind::playback_stream, {}, {}});
        }
        has_active_endpoint_.store(!devices.empty(), std::memory_order_release);
        endpoints = std::move(result);
        error.clear();
        return true;
    }

    bool start(std::string& error) override
    {
        if (started_.load(std::memory_order_acquire)) return true;
        std::vector<WasapiPlaybackEndpoint> endpoints;
        if (!enumerate_native_endpoints(endpoints, error)) return false;
        has_active_endpoint_.store(!endpoints.empty(), std::memory_order_release);
        started_.store(true, std::memory_order_release);
        return true;
    }

    void stop() override
    {
        started_.store(false, std::memory_order_release);
        has_active_endpoint_.store(false, std::memory_order_release);
    }

    std::string list_inputs(std::size_t) const
    {
        if (!started_.load(std::memory_order_acquire)) return "WASAPI backend is not running\n";
        std::vector<WasapiPlaybackSession> sessions;
        std::string error;
        if (!enumerate_playback_sessions(sessions, error))
            return "could not list WASAPI playback sessions: " + error + "\n";
        std::ostringstream output;
        output << "Windows WASAPI playback sessions\n";
        output << "App selector | Application | Session ID\n";
        for (const auto& session : sessions) {
            const std::string selector = session.process_binary.empty()
                ? session.application : session.process_binary;
            const std::string session_id = session.session_id.empty()
                ? "PID " + std::to_string(session.process_id) : session.session_id;
            output << selector << " | " << session.application << " | " << session_id << '\n';
        }
        if (sessions.empty()) output << "No active playback sessions found\n";
        return output.str();
    }

    std::string list_outputs(std::size_t) const
    {
        if (!started_.load(std::memory_order_acquire)) return "WASAPI backend is not running\n";
        std::vector<WasapiPlaybackEndpoint> endpoints;
        std::string error;
        if (!enumerate_native_endpoints(endpoints, error))
            return "could not list WASAPI playback endpoints: " + error + "\n";
        has_active_endpoint_.store(!endpoints.empty(), std::memory_order_release);
        std::ostringstream output;
        output << "Windows WASAPI playback endpoints\n";
        output << "Endpoint selector | Name | Status\n";
        for (const auto& endpoint : endpoints)
            output << endpoint.id << " | " << endpoint.name << " | "
                   << (endpoint.is_default ? "default" : "available") << '\n';
        if (!endpoints.empty() && std::none_of(endpoints.begin(), endpoints.end(),
                [](const auto& endpoint) { return endpoint.is_default; }))
            output << "System default endpoint: unavailable\n";
        if (endpoints.empty()) output << "No active playback endpoints found\n";
        return output.str();
    }

    std::string list_meters(std::string_view) const
    { return unsupported_message() + "\nLive meters are unavailable.\n"; }
    std::string fader_status() const { return unsupported_message() + "\n"; }

    std::string health_status() const
    {
        std::vector<WasapiPlaybackEndpoint> endpoints;
        std::string error;
        const bool running = started_.load(std::memory_order_acquire);
        const bool queried = running && enumerate_native_endpoints(endpoints, error);
        if (running)
            has_active_endpoint_.store(queried && !endpoints.empty(), std::memory_order_release);
        const auto state = mixer::health_state(running, queried && !endpoints.empty(), 0);
        std::ostringstream output;
        output << "Engine health: " << mixer::health_state_name(state) << '\n';
        if (!running) output << "Reason: WASAPI backend did not start\n";
        else if (!queried) output << "Issue: Windows audio endpoint query failed: " << error << '\n';
        else if (endpoints.empty()) output << "Issue: no active Windows playback endpoint is available\n";
        else output << "Issue: none detected\n";
        output << "Xruns since engine start: unavailable (WASAPI discovery backend has no Shatter ring buffers)\n";
        output << "I/O latency: unavailable (WASAPI playback routing is not implemented yet)\n";
        return output.str();
    }

    std::string profile_stop(std::string& error)
    { error = unsupported_message(); return {}; }
    bool available() const noexcept override
    {
        return started_.load(std::memory_order_acquire) &&
               has_active_endpoint_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> started_{false};
    mutable std::atomic<bool> has_active_endpoint_{false};
};

} // namespace

} // namespace shatter::backend

namespace shatter::mixer {
std::unique_ptr<IControl> create_engine()
{
    return std::make_unique<Engine<backend::WasapiBackend>>();
}
} // namespace shatter::mixer
