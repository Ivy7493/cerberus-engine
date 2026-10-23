#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eng {

// Opaque peer address. SteamTransport uses the peer's SteamID64; UdpTransport
// packs IPv4 address and port. 0 is never a valid peer.
using PeerId = uint64_t;

enum class Delivery : uint8_t {
    // May drop or reorder; for state that's resent every tick anyway.
    Unreliable,
    // Arrives exactly once, in order relative to other reliable messages
    // to the same peer.
    Reliable,
};

struct TransportEvent {
    enum class Type : uint8_t { Data, Disconnected };
    Type type = Type::Data;
    PeerId peer = 0;
    std::vector<uint8_t> data;
};

// Message-oriented, connectionless-feeling transport: a host doesn't
// "accept" anything at this layer — any peer that sends gets its messages
// delivered, and the game protocol's own hello/welcome decides who's
// actually in the session. Disconnected fires when a peer times out or its
// session fails, so the game can drop that player.
class Transport {
public:
    virtual ~Transport() = default;

    virtual void send(PeerId peer, const void* data, size_t size, Delivery delivery) = 0;
    void send(PeerId peer, const std::vector<uint8_t>& data, Delivery delivery) {
        send(peer, data.data(), data.size(), delivery);
    }

    // Pumps the transport (resends, keepalives, timeouts) and appends every
    // event since the last call to `out`. Call once per frame/tick.
    virtual void poll(std::vector<TransportEvent>& out) = 0;

    // Forget a peer: stop resending to it and drop its future traffic
    // until it sends again.
    virtual void disconnect(PeerId peer) = 0;

    // Rough round-trip time in ms for display (0 if unknown).
    virtual int pingMs(PeerId peer) const = 0;
};

}  // namespace eng
