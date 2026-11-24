#pragma once

#include "eng/config.h" // ENG_HAVE_STEAM, as libeng.a was built

#include "eng/net/Transport.h"

#include <functional>

#ifdef ENG_HAVE_STEAM
#  include <steam/steam_api.h>
#  include <steam/isteamnetworkingmessages.h>
#endif

namespace eng {

// Transport over ISteamNetworkingMessages: peers are SteamID64s, traffic
// is NAT-punched P2P with automatic fallback to Valve's relay network
// (which also hides players' IPs from each other). Steam handles
// reliability/ordering/fragmentation itself.
//
// Requires an initialized SteamPlatform for its whole lifetime.
class SteamTransport : public Transport {
public:
    // `acceptPeer` decides whether an unsolicited session from a stranger is
    // allowed — typically "is this SteamID in my lobby?". Sessions we
    // initiate (by sending first) are always accepted.
    explicit SteamTransport(std::function<bool(PeerId)> acceptPeer);
    ~SteamTransport() override;

    void send(PeerId peer, const void* data, size_t size, Delivery delivery) override;
    using Transport::send;
    void poll(std::vector<TransportEvent>& out) override;
    void disconnect(PeerId peer) override;
    int pingMs(PeerId peer) const override;

private:
    std::function<bool(PeerId)> m_acceptPeer;
    std::vector<TransportEvent> m_pendingEvents;

#ifdef ENG_HAVE_STEAM
    STEAM_CALLBACK(SteamTransport, onSessionRequest, SteamNetworkingMessagesSessionRequest_t);
    STEAM_CALLBACK(SteamTransport, onSessionFailed, SteamNetworkingMessagesSessionFailed_t);
#endif
};

}  // namespace eng
