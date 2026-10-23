#pragma once

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netinet/in.h>
#endif

#include <cstddef>
#include <cstdint>
#include <string>

namespace eng {

// A minimal non-blocking UDP socket wrapper (POSIX + Winsock2).
class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void bind(uint16_t port);
    void sendTo(const void* data, size_t size, const sockaddr_in& addr);

    // Non-blocking receive. Returns bytes received, or -1 if no datagram available.
    int receiveFrom(void* buffer, size_t bufferSize, sockaddr_in& fromAddr);

    static sockaddr_in makeAddress(const std::string& ip, uint16_t port);

private:
#ifdef _WIN32
    SOCKET m_fd = INVALID_SOCKET;
#else
    int m_fd = -1;
#endif
};

}  // namespace eng
