#pragma once
#include "transport/transport.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <stdexcept>
#include <utility>

namespace netcode::ipv4 {
#ifdef _WIN32
using SocketHandle = SOCKET;
using AddressSize = int;
constexpr SocketHandle invalid_socket = INVALID_SOCKET;
constexpr int send_flags = 0;
constexpr int datagram_receive_flags = 0;
inline int last_socket_error() { return WSAGetLastError(); }
inline bool retry_io(int error) { return error == WSAEWOULDBLOCK || error == WSAEINTR; }
inline bool connection_pending(int error) { return error == WSAEWOULDBLOCK; }
inline void close_socket(SocketHandle socket) { closesocket(socket); }
#else
using SocketHandle = int;
using AddressSize = socklen_t;
constexpr SocketHandle invalid_socket = -1;
constexpr int send_flags = MSG_NOSIGNAL;
constexpr int datagram_receive_flags = MSG_TRUNC;
inline int last_socket_error() { return errno; }
inline bool retry_io(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR; }
inline bool connection_pending(int error) { return error == EINPROGRESS || error == EINTR; }
inline void close_socket(SocketHandle socket) { ::close(socket); }
#endif

[[noreturn]] inline void socket_error(const char* operation)
{
    const int error = last_socket_error();
    throw std::runtime_error(std::string(operation) + " failed (socket error " + std::to_string(error) + ")");
}

#ifdef _WIN32
class Winsock {
public:
    Winsock()
    {
        WSADATA data{};
        if (const int error = WSAStartup(MAKEWORD(2, 2), &data)) {
            throw std::runtime_error("WSAStartup failed (" + std::to_string(error) + ")");
        }
    }
    ~Winsock() { WSACleanup(); }
};
#endif

class Socket {
public:
    Socket() = default;
    explicit Socket(SocketHandle handle) : handle_(handle) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, invalid_socket)) {}
    Socket& operator=(Socket&& other) noexcept
    {
        std::swap(handle_, other.handle_);
        return *this;
    }
    ~Socket() { if (*this) close_socket(handle_); }
    explicit operator bool() const { return handle_ != invalid_socket; }
    operator SocketHandle() const { return handle_; }

private:
    SocketHandle handle_ = invalid_socket;
};

inline void nonblocking(SocketHandle socket)
{
#ifdef _WIN32
    u_long value = 1;
    if (ioctlsocket(socket, FIONBIO, &value) < 0) socket_error("ioctlsocket");
#else
    const int flags = fcntl(socket, F_GETFL);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0) socket_error("fcntl");
#endif
}

inline Socket make_socket(int type, int protocol)
{
    Socket socket{::socket(AF_INET, type, protocol)};
    if (!socket) socket_error("socket");
    nonblocking(socket);
    return socket;
}

inline sockaddr_in endpoint(const DirectConfig& config)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (!config.port || inet_pton(AF_INET, config.address.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("Expected an IPv4 address and nonzero port");
    }
    return address;
}

}
