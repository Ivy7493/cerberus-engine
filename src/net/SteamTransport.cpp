#include "eng/net/SteamTransport.h"

#include "eng/core/Log.h"

namespace eng {

#ifdef ENG_HAVE_STEAM

namespace {

constexpr int kChannel = 0;

SteamNetworkingIdentity identityFor(PeerId peer) {
    SteamNetworkingIdentity id;
    id.SetSteamID64(peer);
    return id;
}

}  // namespace

SteamTransport::SteamTransport(std::function<bool(PeerId)> acceptPeer) : m_acceptPeer(std::move(acceptPeer)) {}

SteamTransport::~SteamTransport() = default;

void SteamTransport::send(PeerId peer, const void* data, size_t size, Delivery delivery) {
    if (peer == 0) return;
    // AutoRestartBrokenSession: if the peer dropped and came back (e.g.
    // rejoined after a crash), re-establish instead of failing forever.
    int flags = k_nSteamNetworkingSend_AutoRestartBrokenSession |
                (delivery == Delivery::Reliable ? k_nSteamNetworkingSend_Reliable
                                                : k_nSteamNetworkingSend_UnreliableNoNagle);
    SteamNetworkingMessages()->SendMessageToUser(identityFor(peer), data, static_cast<uint32>(size), flags, kChannel);
}

void SteamTransport::poll(std::vector<TransportEvent>& out) {
    // Session callbacks fired during SteamAPI_RunCallbacks (SteamPlatform::
    // update) since the last poll.
    for (auto& e : m_pendingEvents) out.push_back(std::move(e));
    m_pendingEvents.clear();

    SteamNetworkingMessage_t* msgs[64];
    int n;
    while ((n = SteamNetworkingMessages()->ReceiveMessagesOnChannel(kChannel, msgs, 64)) > 0) {
        for (int i = 0; i < n; ++i) {
            SteamNetworkingMessage_t* m = msgs[i];
            const auto* bytes = static_cast<const uint8_t*>(m->m_pData);
            out.push_back({TransportEvent::Type::Data, m->m_identityPeer.GetSteamID64(),
                           std::vector<uint8_t>(bytes, bytes + m->m_cbSize)});
            m->Release();
        }
    }
}

void SteamTransport::disconnect(PeerId peer) {
    SteamNetworkingMessages()->CloseSessionWithUser(identityFor(peer));
}

int SteamTransport::pingMs(PeerId peer) const {
    SteamNetConnectionRealTimeStatus_t status{};
    SteamNetworkingMessages()->GetSessionConnectionInfo(identityFor(peer), nullptr, &status);
    return status.m_nPing > 0 ? status.m_nPing : 0;
}

void SteamTransport::onSessionRequest(SteamNetworkingMessagesSessionRequest_t* req) {
    PeerId peer = req->m_identityRemote.GetSteamID64();
    if (m_acceptPeer && m_acceptPeer(peer)) {
        SteamNetworkingMessages()->AcceptSessionWithUser(req->m_identityRemote);
    } else {
        ENG_LOG("[steam] rejected P2P session from " << peer << " (not in lobby)");
    }
}

void SteamTransport::onSessionFailed(SteamNetworkingMessagesSessionFailed_t* fail) {
    PeerId peer = fail->m_info.m_identityRemote.GetSteamID64();
    ENG_LOG("[steam] P2P session with " << peer << " failed: " << fail->m_info.m_szEndDebug);
    m_pendingEvents.push_back({TransportEvent::Type::Disconnected, peer, {}});
}

#else  // !ENG_HAVE_STEAM

SteamTransport::SteamTransport(std::function<bool(PeerId)> acceptPeer) : m_acceptPeer(std::move(acceptPeer)) {}
SteamTransport::~SteamTransport() = default;
void SteamTransport::send(PeerId, const void*, size_t, Delivery) {}
void SteamTransport::poll(std::vector<TransportEvent>&) {}
void SteamTransport::disconnect(PeerId) {}
int SteamTransport::pingMs(PeerId) const { return 0; }

#endif

}  // namespace eng
