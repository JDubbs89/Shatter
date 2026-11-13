#include "control/platform/service.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace shatter::control::platform {
namespace {

std::string control_socket_path()
{
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_dir == nullptr || *runtime_dir == '\0')
        throw std::runtime_error("XDG_RUNTIME_DIR is not set");

    const std::string directory = std::string(runtime_dir) + "/shatter";
    const std::string path = directory + "/control.sock";
    if (path.size() >= sizeof(sockaddr_un{}.sun_path))
        throw std::runtime_error("XDG_RUNTIME_DIR path is too long for a Unix socket");

    if (mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST)
        throw std::runtime_error("cannot create runtime directory: " + std::string(std::strerror(errno)));

    struct stat info {};
    if (lstat(directory.c_str(), &info) < 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 0077) != 0)
        throw std::runtime_error("runtime directory must be a private directory owned by the current user");
    return path;
}

int connect_to_engine(const std::string& path)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        throw std::runtime_error("cannot create control socket: " + std::string(std::strerror(errno)));

    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (connect(fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) < 0) {
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

class SocketConnection final : public ControlConnection {
public:
    explicit SocketConnection(int fd) : fd_(fd) {}
    ~SocketConnection() override { if (fd_ >= 0) close(fd_); }

    bool receive(std::string& message, std::string& error) override
    {
        std::array<char, 512> buffer{};
        const ssize_t received = recv(fd_, buffer.data(), buffer.size() - 1, 0);
        if (received <= 0) {
            if (received < 0) error = std::strerror(errno);
            return false;
        }
        message.assign(buffer.data(), static_cast<std::size_t>(received));
        if (!message.empty() && message.back() == '\n') message.pop_back();
        return true;
    }

    bool send(std::string_view message, std::string& error) override
    {
        std::size_t sent = 0;
        while (sent < message.size()) {
            const ssize_t result = ::send(fd_, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
            if (result <= 0) {
                error = result == 0 ? "control connection closed" : std::strerror(errno);
                return false;
            }
            sent += static_cast<std::size_t>(result);
        }
        return true;
    }

private:
    int fd_ = -1;
};

class SocketServer final : public ControlServer {
public:
    explicit SocketServer(std::string path) : path_(std::move(path)) {}
    ~SocketServer() override
    {
        if (fd_ >= 0) close(fd_);
        if (owns_path_) unlink(path_.c_str());
    }

    bool start(std::string& error)
    {
        struct stat existing {};
        if (lstat(path_.c_str(), &existing) == 0) {
            if (!S_ISSOCK(existing.st_mode) || existing.st_uid != getuid()) {
                error = "refusing to replace an unexpected control socket path";
                return false;
            }
            const int active_fd = connect_to_engine(path_);
            if (active_fd >= 0) {
                close(active_fd);
                error = "a Shatter engine is already listening";
                return false;
            }
            if (errno != ECONNREFUSED && errno != ENOENT) {
                error = "could not verify existing control socket: " + std::string(std::strerror(errno));
                return false;
            }
            if (unlink(path_.c_str()) < 0) {
                error = "could not remove stale control socket: " + std::string(std::strerror(errno));
                return false;
            }
        }

        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            error = "could not create control socket: " + std::string(std::strerror(errno));
            return false;
        }
        struct sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path_.c_str(), sizeof(address.sun_path) - 1);
        if (bind(fd_, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) < 0) {
            error = "could not listen on control socket: " + std::string(std::strerror(errno));
            return false;
        }
        owns_path_ = true;
        if (chmod(path_.c_str(), 0600) < 0 || listen(fd_, 8) < 0) {
            error = "could not listen on control socket: " + std::string(std::strerror(errno));
            return false;
        }
        return true;
    }

    std::unique_ptr<ControlConnection> accept(std::string& error) override
    {
        int client_fd;
        do { client_fd = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC); } while (client_fd < 0 && errno == EINTR);
        if (client_fd < 0) error = std::strerror(errno);
        return client_fd < 0 ? nullptr : std::make_unique<SocketConnection>(client_fd);
    }

private:
    std::string path_;
    int fd_ = -1;
    bool owns_path_ = false;
};

} // namespace

std::unique_ptr<ControlServer> listen_for_control(std::string& error)
{
    try {
        const std::string path = control_socket_path();
        auto server = std::make_unique<SocketServer>(path);
        if (!server->start(error)) return nullptr;
        return server;
    } catch (const std::exception& exception) {
        error = exception.what();
        return nullptr;
    }
}

std::string request_control(std::string_view command)
{
    const int fd = connect_to_engine(control_socket_path());
    if (fd < 0) return {};

    const timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    const std::string message = std::string(command) + "\n";
    std::size_t sent = 0;
    while (sent < message.size()) {
        const ssize_t result = ::send(fd, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
        if (result <= 0) { close(fd); return {}; }
        sent += static_cast<std::size_t>(result);
    }

    std::string response;
    std::array<char, 4096> buffer{};
    while (response.size() < 65536) {
        const ssize_t received = recv(fd, buffer.data(), buffer.size(), 0);
        if (received <= 0) break;
        response.append(buffer.data(), static_cast<std::size_t>(received));
        if (response.ends_with(".\n")) { response.resize(response.size() - 2); break; }
    }
    close(fd);
    return response;
}

bool launch_detached_engine(std::string_view executable, std::string& error)
{
    const pid_t child = fork();
    if (child < 0) {
        error = std::strerror(errno);
        return false;
    }
    if (child == 0) {
        if (setsid() < 0) _exit(126);
        const int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        const std::string executable_path(executable);
        char* const arguments[] = {const_cast<char*>(executable_path.c_str()),
                                   const_cast<char*>("--engine"), nullptr};
        execvp(executable_path.c_str(), arguments);
        _exit(127);
    }
    return true;
}

bool notify_control(std::string_view command, std::string& error)
{
    const auto path = control_socket_path();
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { error = std::strerror(errno); return false; }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    bool sent = false;
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        const std::string message = std::string(command) + "\n";
        sent = ::send(fd, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT) ==
               static_cast<ssize_t>(message.size());
    }
    if (!sent) error = std::strerror(errno);
    close(fd);
    return sent;
}

bool restart_current_engine(std::string& error)
{
    char* const arguments[] = {const_cast<char*>("shatter"), const_cast<char*>("--engine"), nullptr};
    execv("/proc/self/exe", arguments);
    error = std::strerror(errno);
    return false;
}

unsigned short terminal_width() noexcept
{
    struct winsize size {};
    return ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 ? size.ws_col : 100;
}

} // namespace shatter::control::platform
