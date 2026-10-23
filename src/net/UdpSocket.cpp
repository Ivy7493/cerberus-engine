#include "eng/net/UdpSocket.h"

#ifdef _WIN32
#  include <cstring>
#  include <stdexcept>

namespace eng {

static void wsaInit() {
    static bool done = false;
    if (!done) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            throw std::runtime_error("WSAStartup failed");
        done = true;
    }
}

UdpSocket::UdpSocket() {
    wsaInit();
    m_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_fd == INVALID_SOCKET)
        throw std::runtime_error("failed to create UDP socket");
    u_long mode = 1;
    ioctlsocket(m_fd, FIONBIO, &mode);
}

UdpSocket::~UdpSocket() {
    if (m_fd != INVALID_SOCKET)
        closesocket(m_fd);
}

void UdpSocket::bind(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        throw std::runtime_error("failed to bind UDP socket");
}

void UdpSocket::sendTo(const void* data, size_t size, const sockaddr_in& addr) {
    sendto(m_fd, static_cast<const char*>(data), static_cast<int>(size), 0,
           reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

int UdpSocket::receiveFrom(void* buffer, size_t bufferSize, sockaddr_in& fromAddr) {
    int addrLen = sizeof(fromAddr);
    int received = recvfrom(m_fd, static_cast<char*>(buffer), static_cast<int>(bufferSize),
                            0, reinterpret_cast<sockaddr*>(&fromAddr), &addrLen);
    return (received == SOCKET_ERROR) ? -1 : received;
}

sockaddr_in UdpSocket::makeAddress(const std::string& host, uint16_t port) {
    // getaddrinfo handles both a literal IP and a hostname (e.g. a dynamic-DNS
    // name like yourname.duckdns.org for a self-hosted server behind a
    // residential connection) — inet_pton alone only ever accepted literals.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0 && result != nullptr) {
        addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
        freeaddrinfo(result);
    }
    return addr;
}

}  // namespace eng

#else  // POSIX ----------------------------------------------------------------

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

namespace eng {

UdpSocket::UdpSocket() {
    m_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_fd < 0)
        throw std::runtime_error("failed to create UDP socket");
    int flags = fcntl(m_fd, F_GETFL, 0);
    fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
}

UdpSocket::~UdpSocket() {
    if (m_fd >= 0)
        close(m_fd);
}

void UdpSocket::bind(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        throw std::runtime_error("failed to bind UDP socket");
}

void UdpSocket::sendTo(const void* data, size_t size, const sockaddr_in& addr) {
    sendto(m_fd, data, size, 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

int UdpSocket::receiveFrom(void* buffer, size_t bufferSize, sockaddr_in& fromAddr) {
    socklen_t addrLen = sizeof(fromAddr);
    ssize_t received  = recvfrom(m_fd, buffer, bufferSize, 0,
                                  reinterpret_cast<sockaddr*>(&fromAddr), &addrLen);
    return (received < 0) ? -1 : static_cast<int>(received);
}

sockaddr_in UdpSocket::makeAddress(const std::string& host, uint16_t port) {
    // getaddrinfo handles both a literal IP and a hostname (e.g. a dynamic-DNS
    // name like yourname.duckdns.org for a self-hosted server behind a
    // residential connection) — inet_pton alone only ever accepted literals.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0 && result != nullptr) {
        addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
        freeaddrinfo(result);
    }
    return addr;
}

}  // namespace eng

#endif
