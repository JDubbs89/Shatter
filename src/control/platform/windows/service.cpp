#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>

#include "control/platform/service.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace shatter::control::platform {
namespace {

std::string windows_error(DWORD code)
{
    std::array<char, 512> buffer{};
    const DWORD count = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    std::string message = count == 0 ? "Windows error " + std::to_string(code)
                                     : std::string(buffer.data(), count);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
        message.pop_back();
    return message;
}

PSID current_user_sid(std::vector<std::byte>& storage, std::string& error)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = "cannot inspect the current user: " + windows_error(GetLastError());
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    storage.resize(size);
    const BOOL queried = GetTokenInformation(token, TokenUser, storage.data(), size, &size);
    const DWORD query_error = queried ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (!queried) {
        error = "cannot inspect the current user: " + windows_error(query_error);
        return {};
    }
    return reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid;
}

std::wstring control_pipe_name(PSID user_sid, std::string& error)
{
    LPSTR sid = nullptr;
    if (!ConvertSidToStringSidA(user_sid, &sid)) {
        error = "cannot identify the current user: " + windows_error(GetLastError());
        return {};
    }
    const std::string name = std::string("\\\\.\\pipe\\shatter-") + sid;
    LocalFree(sid);
    return std::wstring(name.begin(), name.end());
}

struct PipeSecurity {
    std::vector<std::byte> acl_storage;
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};

    bool initialize(PSID user_sid, std::string& error)
    {
        const DWORD acl_size = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(user_sid);
        acl_storage.resize(acl_size);
        auto* acl = reinterpret_cast<PACL>(acl_storage.data());
        if (!InitializeAcl(acl, acl_size, ACL_REVISION) ||
            !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, user_sid) ||
            !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)) {
            error = "cannot secure the control pipe: " + windows_error(GetLastError());
            return false;
        }
        return true;
    }
};

HANDLE create_pipe(const std::wstring& name, const SECURITY_ATTRIBUTES& security, bool first, std::string& error)
{
    const DWORD flags = PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    HANDLE pipe = CreateNamedPipeW(name.c_str(), flags,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 4096, 4096, 1000, const_cast<SECURITY_ATTRIBUTES*>(&security));
    if (pipe == INVALID_HANDLE_VALUE) error = windows_error(GetLastError());
    return pipe;
}

class NamedPipeConnection final : public ControlConnection {
public:
    explicit NamedPipeConnection(HANDLE pipe) : pipe_(pipe) {}
    ~NamedPipeConnection() override
    {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(pipe_);
            DisconnectNamedPipe(pipe_);
            CloseHandle(pipe_);
        }
    }

    bool receive(std::string& message, std::string& error) override
    {
        std::array<char, 512> buffer{};
        DWORD received = 0;
        if (!ReadFile(pipe_, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &received, nullptr)) {
            const DWORD code = GetLastError();
            if (code != ERROR_BROKEN_PIPE) error = windows_error(code);
            return false;
        }
        if (received == 0) return false;
        message.assign(buffer.data(), received);
        if (!message.empty() && message.back() == '\n') message.pop_back();
        return true;
    }

    bool send(std::string_view message, std::string& error) override
    {
        std::size_t sent = 0;
        while (sent < message.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(message.size() - sent, MAXDWORD));
            DWORD written = 0;
            if (!WriteFile(pipe_, message.data() + sent, chunk, &written, nullptr) || written == 0) {
                error = windows_error(GetLastError());
                return false;
            }
            sent += written;
        }
        return true;
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

class NamedPipeServer final : public ControlServer {
public:
    ~NamedPipeServer() override
    {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    }

    bool start(std::string& error)
    {
        sid_storage_.clear();
        PSID sid = current_user_sid(sid_storage_, error);
        if (sid == nullptr) return false;
        if (!security_.initialize(sid, error)) return false;
        name_ = control_pipe_name(sid, error);
        if (name_.empty()) return false;
        pipe_ = create_pipe(name_, security_.attributes, true, error);
        if (pipe_ == INVALID_HANDLE_VALUE)
            error = "could not create the per-user Shatter control pipe (another instance may be running): " + error;
        return pipe_ != INVALID_HANDLE_VALUE;
    }

    std::unique_ptr<ControlConnection> accept(std::string& error) override
    {
        if (pipe_ == INVALID_HANDLE_VALUE) return nullptr;
        const HANDLE connected_pipe = pipe_;
        const BOOL connected = ConnectNamedPipe(connected_pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        pipe_ = INVALID_HANDLE_VALUE;
        if (!connected) {
            error = windows_error(GetLastError());
            CloseHandle(connected_pipe);
            return nullptr;
        }
        HANDLE next = create_pipe(name_, security_.attributes, false, error);
        if (next == INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(connected_pipe);
            CloseHandle(connected_pipe);
            return nullptr;
        }
        pipe_ = next;
        return std::make_unique<NamedPipeConnection>(connected_pipe);
    }

private:
    std::wstring name_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::vector<std::byte> sid_storage_;
    PipeSecurity security_;
};

std::wstring widen(std::string_view text)
{
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::wstring quote_argument(std::wstring_view value)
{
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(character);
        } else {
            quoted.append(slashes, L'\\');
            quoted.push_back(character);
        }
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

} // namespace

std::unique_ptr<ControlServer> listen_for_control(std::string& error)
{
    auto server = std::make_unique<NamedPipeServer>();
    if (!server->start(error)) return nullptr;
    return server;
}

std::string request_control(std::string_view command)
{
    std::string error;
    std::vector<std::byte> sid_storage_;
    const PSID sid = current_user_sid(sid_storage_, error);
    if (sid == nullptr) return {};
    const std::wstring name = control_pipe_name(sid, error);
    if (name.empty() || !WaitNamedPipeW(name.c_str(), 1000)) return {};
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return {};
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    const std::string message = std::string(command) + "\n";
    std::size_t sent = 0;
    while (sent < message.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(message.size() - sent, MAXDWORD));
        if (!WriteFile(pipe, message.data() + sent, chunk, &written, nullptr) || written == 0) {
            CloseHandle(pipe);
            return {};
        }
        sent += written;
    }
    std::string response;
    std::array<char, 4096> buffer{};
    while (response.size() < 65536) {
        DWORD received = 0;
        if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr) || received == 0)
            break;
        response.append(buffer.data(), received);
        if (response.ends_with(".\n")) { response.resize(response.size() - 2); break; }
    }
    CloseHandle(pipe);
    return response;
}

namespace {
bool launch_engine(const std::wstring& application, std::string& error)
{
    std::wstring command_line = quote_argument(application) + L" --engine";
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    SECURITY_ATTRIBUTES inherit_handles{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE null_handle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit_handles, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_handle == INVALID_HANDLE_VALUE) {
        error = "could not open the null device: " + windows_error(GetLastError());
        return false;
    }
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_handle;
    startup.StartupInfo.hStdOutput = null_handle;
    startup.StartupInfo.hStdError = null_handle;

    // Redirecting standard handles does not prevent other inheritable handles
    // (including a caller's output pipes) from keeping the CLI pipeline open.
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<std::byte> attribute_storage(attribute_size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) {
        error = "could not initialize engine handle inheritance: " + windows_error(GetLastError());
        CloseHandle(null_handle);
        return false;
    }
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   &null_handle, sizeof(null_handle), nullptr, nullptr)) {
        error = "could not restrict engine handle inheritance: " + windows_error(GetLastError());
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(null_handle);
        return false;
    }
    const BOOL created = CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, TRUE,
        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr, &startup.StartupInfo, &process);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(null_handle);
    if (!created) {
        error = windows_error(create_error);
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace

bool launch_detached_engine(std::string_view executable, std::string& error)
{
    const auto application = widen(executable);
    if (application.empty()) { error = "could not convert the executable path to Windows text"; return false; }
    return launch_engine(application, error);
}

bool restart_current_engine(std::string& error)
{
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) { error = windows_error(GetLastError()); return false; }
    return launch_engine(std::wstring(path.data(), length), error);
}

bool notify_control(std::string_view command, std::string& error)
{
    std::vector<std::byte> sid_storage;
    const PSID sid = current_user_sid(sid_storage, error);
    if (sid == nullptr) return false;
    const auto name = control_pipe_name(sid, error);
    if (name.empty() || !WaitNamedPipeW(name.c_str(), 100)) return false;
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    const std::string message = std::string(command) + "\n";
    DWORD written = 0;
    const bool sent = WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), &written, nullptr) &&
                      written == message.size();
    if (!sent) error = windows_error(GetLastError());
    CloseHandle(pipe);
    return sent;
}

unsigned short terminal_width() noexcept
{
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(output, &info)) return 100;
    const SHORT width = static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1);
    return width > 0 ? static_cast<unsigned short>(width) : 100;
}

} // namespace shatter::control::platform
