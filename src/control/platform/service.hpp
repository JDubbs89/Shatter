#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace shatter::control::platform {

// OS-specific control transport and detached process launch. This interface is
// independent of the audio backend: Linux uses a Unix socket, Windows a named
// pipe. Audio device handles and processing never belong in this module.
class ControlConnection {
public:
    virtual ~ControlConnection() = default;
    virtual bool receive(std::string& message, std::string& error) = 0;
    virtual bool send(std::string_view message, std::string& error) = 0;
};

class ControlServer {
public:
    virtual ~ControlServer() = default;
    virtual std::unique_ptr<ControlConnection> accept(std::string& error) = 0;
};

std::unique_ptr<ControlServer> listen_for_control(std::string& error);
std::string request_control(std::string_view command);
bool launch_detached_engine(std::string_view executable, std::string& error);
unsigned short terminal_width() noexcept;

} // namespace shatter::control::platform
