#include "eng/net/UdpTransport.h"

#include "eng/net/ByteStream.h"

#ifndef _WIN32
#  include <arpa/inet.h>
#endif

#include <algorithm>

namespace eng {

namespace {

enum Kind : uint8_t { kUnreliable = 0, kReliable = 1, kAck = 2, kPing = 3, kPong = 4, kBye = 5 };

constexpr size_t kMaxDatagram = 65507;
constexpr size_t kMaxOutOfOrder = 1024;  // drop a peer flooding far-ahead seqs rather than grow forever

}  // namespace

UdpTransport::UdpTransport(uint16_t port) {
    if (port != 0) m_socket.bind(port);
}

PeerId UdpTransport::idFor(const sockaddr_in& addr) {
    return (static_cast<uint64_t>(ntohl(addr.sin_addr.s_addr)) << 16) | ntohs(addr.sin_port);
}

sockaddr_in UdpTransport::addrFor(PeerId id) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(static_cast<uint32_t>(id >> 16));
    addr.sin_port = htons(static_cast<uint16_t>(id & 0xFFFF));
    return addr;
}

PeerId UdpTransport::resolve(const std::string& host, uint16_t port) {
    return idFor(UdpSocket::makeAddress(host, port));
}

UdpTransport::Peer& UdpTransport::peerFor(PeerId id) {
    auto it = m_peers.find(id);
    if (it != m_peers.end()) return it->second;
    Peer& p = m_peers[id];
    p.addr = addrFor(id);
    p.lastHeard = Clock::now();
    p.lastPing = Clock::time_point{};
    return p;
}

void UdpTransport::sendRaw(Peer& peer, const std::vector<uint8_t>& packet) {
    m_socket.sendTo(packet.data(), packet.size(), peer.addr);
}

void UdpTransport::send(PeerId id, const void* data, size_t size, Delivery delivery) {
    if (id == 0 || size + 5 > kMaxDatagram) return;
    Peer& peer = peerFor(id);
    ByteWriter w;
    if (delivery == Delivery::Reliable) {
        uint32_t seq = peer.nextSendSeq++;
        w.u8(kReliable);
        w.u32(seq);
        w.bytes(data, size);
        sendRaw(peer, w.data());
        peer.unacked[seq] = {std::move(w.data()), Clock::now()};
    } else {
        w.u8(kUnreliable);
        w.bytes(data, size);
        sendRaw(peer, w.data());
    }
}

void UdpTransport::disconnect(PeerId id) {
    auto it = m_peers.find(id);
    if (it == m_peers.end()) return;
    std::vector<uint8_t> bye{kBye};
    sendRaw(it->second, bye);
    m_peers.erase(it);
}

int UdpTransport::pingMs(PeerId id) const {
    auto it = m_peers.find(id);
    return it == m_peers.end() ? 0 : static_cast<int>(it->second.rttMs);
}

void UdpTransport::poll(std::vector<TransportEvent>& out) {
    auto now = Clock::now();
    static thread_local std::vector<uint8_t> buf(kMaxDatagram);

    sockaddr_in from{};
    int n;
    while ((n = m_socket.receiveFrom(buf.data(), buf.size(), from)) >= 0) {
        if (n < 1) continue;
        PeerId id = idFor(from);
        uint8_t kind = buf[0];
        if (kind == kBye) {
            if (m_peers.erase(id)) out.push_back({TransportEvent::Type::Disconnected, id, {}});
            continue;
        }
        Peer& peer = peerFor(id);
        peer.lastHeard = now;
        ByteReader r(buf.data() + 1, static_cast<size_t>(n - 1));

        switch (kind) {
        case kUnreliable:
            out.push_back({TransportEvent::Type::Data, id, std::vector<uint8_t>(buf.begin() + 1, buf.begin() + n)});
            break;
        case kReliable: {
            uint32_t seq = r.u32();
            if (!r.ok()) break;
            ByteWriter ack;
            ack.u8(kAck);
            ack.u32(seq);
            sendRaw(peer, ack.data());
            // Signed distance so seq wraparound (after 4 billion messages)
            // still orders correctly.
            auto ahead = static_cast<int32_t>(seq - peer.nextRecvSeq);
            if (ahead < 0) break;  // duplicate of something already delivered
            std::vector<uint8_t> payload(buf.begin() + 5, buf.begin() + n);
            if (ahead > 0) {
                if (peer.outOfOrder.size() < kMaxOutOfOrder) peer.outOfOrder.emplace(seq, std::move(payload));
                break;
            }
            out.push_back({TransportEvent::Type::Data, id, std::move(payload)});
            ++peer.nextRecvSeq;
            for (auto it = peer.outOfOrder.find(peer.nextRecvSeq); it != peer.outOfOrder.end();
                 it = peer.outOfOrder.find(peer.nextRecvSeq)) {
                out.push_back({TransportEvent::Type::Data, id, std::move(it->second)});
                peer.outOfOrder.erase(it);
                ++peer.nextRecvSeq;
            }
            break;
        }
        case kAck: {
            uint32_t seq = r.u32();
            if (r.ok()) peer.unacked.erase(seq);
            break;
        }
        case kPing: {
            uint64_t stamp = r.u64();
            if (!r.ok()) break;
            ByteWriter pong;
            pong.u8(kPong);
            pong.u64(stamp);
            sendRaw(peer, pong.data());
            break;
        }
        case kPong: {
            uint64_t stamp = r.u64();
            if (!r.ok()) break;
            auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(now - m_epoch).count();
            double sample = (static_cast<double>(nowUs) - static_cast<double>(stamp)) / 1000.0;
            if (sample >= 0.0) peer.rttMs = peer.rttMs == 0.0 ? sample : peer.rttMs * 0.8 + sample * 0.2;
            break;
        }
        default:
            break;
        }
    }

    for (auto it = m_peers.begin(); it != m_peers.end();) {
        Peer& peer = it->second;
        if (std::chrono::duration<double>(now - peer.lastHeard).count() > kTimeoutSeconds) {
            out.push_back({TransportEvent::Type::Disconnected, it->first, {}});
            it = m_peers.erase(it);
            continue;
        }
        auto resendAfter = std::chrono::milliseconds(std::max(50, static_cast<int>(peer.rttMs * 1.5)));
        for (auto& [seq, pending] : peer.unacked) {
            if (now - pending.lastSent > resendAfter) {
                sendRaw(peer, pending.packet);
                pending.lastSent = now;
            }
        }
        if (now - peer.lastPing > std::chrono::milliseconds(500)) {
            ByteWriter ping;
            ping.u8(kPing);
            ping.u64(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - m_epoch).count()));
            sendRaw(peer, ping.data());
            peer.lastPing = now;
        }
        ++it;
    }
}

}  // namespace eng
