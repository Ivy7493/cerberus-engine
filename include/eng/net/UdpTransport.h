#pragma once

#include "eng/net/Transport.h"
#include "eng/net/UdpSocket.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace eng {

// Plain-UDP Transport with a minimal reliability layer (per-message seq,
// individual acks, resend-until-acked, in-order delivery) plus keepalive
// pings and a timeout. Meant for LAN/localhost testing — several game
// instances on one machine can't all log into Steam, so local multiplayer
// testing goes over this instead of SteamTransport. No NAT traversal, no
// encryption; large messages rely on IP fragmentation (fine on loopback).
class UdpTransport : public Transport {
public:
    // port 0 = ephemeral (clients); hosts bind a known port.
    explicit UdpTransport(uint16_t port = 0);

    // Resolves host:port into the PeerId to send() to.
    static PeerId resolve(const std::string& host, uint16_t port);

    void send(PeerId peer, const void* data, size_t size, Delivery delivery) override;
    using Transport::send;
    void poll(std::vector<TransportEvent>& out) override;
    void disconnect(PeerId peer) override;
    int pingMs(PeerId peer) const override;

    static constexpr double kTimeoutSeconds = 6.0;

private:
    using Clock = std::chrono::steady_clock;

    struct Pending {
        std::vector<uint8_t> packet;  // full datagram incl. header
        Clock::time_point lastSent;
    };
    struct Peer {
        sockaddr_in addr{};
        uint32_t nextSendSeq = 0;
        uint32_t nextRecvSeq = 0;
        std::map<uint32_t, Pending> unacked;
        std::map<uint32_t, std::vector<uint8_t>> outOfOrder;
        Clock::time_point lastHeard;
        Clock::time_point lastPing;
        double rttMs = 0.0;
    };

    Peer& peerFor(PeerId id);
    static PeerId idFor(const sockaddr_in& addr);
    static sockaddr_in addrFor(PeerId id);
    void sendRaw(Peer& peer, const std::vector<uint8_t>& packet);

    UdpSocket m_socket;
    std::unordered_map<PeerId, Peer> m_peers;
    Clock::time_point m_epoch = Clock::now();
};

}  // namespace eng
