#include "eng/net/SteamPlatform.h"

#include "eng/core/Log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef ENG_HAVE_STEAM
#  include <steam/steam_api_flat.h>
#endif

namespace eng {

uint64_t SteamPlatform::lobbyFromCommandLine(int argc, char** argv) {
    for (int i = 0; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "+connect_lobby") == 0) return std::strtoull(argv[i + 1], nullptr, 10);
    return 0;
}

std::string SteamPlatform::encodeLobbyCode(uint64_t lobbyId) {
    static const char* kDigits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (lobbyId == 0) return "";
    std::string out;
    while (lobbyId) {
        out.insert(out.begin(), kDigits[lobbyId % 36]);
        lobbyId /= 36;
    }
    return out;
}

uint64_t SteamPlatform::decodeLobbyCode(const std::string& code) {
    uint64_t v = 0;
    int digits = 0;
    for (char c : code) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c == ' ' || c == '-') continue;
        else return 0;
        if (++digits > 13) return 0;  // 36^13 > 2^64: too long to be a lobby id
        v = v * 36 + static_cast<uint64_t>(d);
    }
    return v;
}

std::vector<SteamPlatform::Event> SteamPlatform::drainEvents() {
    std::vector<Event> out;
    out.swap(m_events);
    return out;
}

#ifdef ENG_HAVE_STEAM

SteamPlatform::SteamPlatform() {
    // Bracketed by logs: if startup dies inside SteamAPI_InitEx (or the
    // steam_api DLL it loads) the crash log ends on the "calling" line.
    ENG_LOG("[steam] calling SteamAPI_InitEx...");
    SteamErrMsg errMsg{};
    m_initialized = SteamAPI_InitEx(&errMsg) == k_ESteamAPIInitResult_OK;
    if (!m_initialized) {
        ENG_LOG("[steam] init failed - Steam features disabled: " << errMsg);
        return;
    }
    ENG_LOG("[steam] SteamAPI_InitEx OK");
    // Start fetching the relay network config now rather than on the first
    // P2P send, so the first connection doesn't stall a few seconds.
    SteamNetworkingUtils()->InitRelayNetworkAccess();
}

SteamPlatform::~SteamPlatform() {
    if (!m_initialized) return;
    leaveLobby();
    SteamAPI_Shutdown();
}

std::string SteamPlatform::localPersonaName() const {
    if (!m_initialized) return "";
    const char* name = SteamFriends()->GetPersonaName();
    return name ? name : "";
}

uint64_t SteamPlatform::localSteamId() const {
    if (!m_initialized) return 0;
    return SteamAPI_ISteamUser_GetSteamID(SteamUser());
}

std::string SteamPlatform::personaName(uint64_t steamId) const {
    if (!m_initialized) return "";
    const char* name = SteamFriends()->GetFriendPersonaName(CSteamID(static_cast<uint64>(steamId)));
    return name ? name : "";
}

void SteamPlatform::update() {
    if (m_initialized) SteamAPI_RunCallbacks();
}

void SteamPlatform::createLobby(int maxMembers, bool friendsOnly) {
    if (!m_initialized) return;
    leaveLobby();
    SteamAPICall_t call = SteamMatchmaking()->CreateLobby(
        friendsOnly ? k_ELobbyTypeFriendsOnly : k_ELobbyTypePublic, maxMembers);
    m_lobbyCreated.Set(call, this, &SteamPlatform::onLobbyCreated);
}

void SteamPlatform::onLobbyCreated(LobbyCreated_t* result, bool ioFailure) {
    if (ioFailure || result->m_eResult != k_EResultOK) {
        ENG_LOG("[steam] CreateLobby failed (result=" << (ioFailure ? -1 : int(result->m_eResult)) << ")");
        m_events.push_back({Event::Type::LobbyCreateFailed});
        return;
    }
    m_lobby = result->m_ulSteamIDLobby;
    ENG_LOG("[steam] lobby created: " << m_lobby);
    m_events.push_back({Event::Type::LobbyCreated, m_lobby});
}

void SteamPlatform::joinLobby(uint64_t lobbyId) {
    if (!m_initialized || lobbyId == 0) return;
    leaveLobby();
    SteamAPICall_t call = SteamMatchmaking()->JoinLobby(CSteamID(static_cast<uint64>(lobbyId)));
    m_lobbyEnterResult.Set(call, this, &SteamPlatform::onLobbyEnterResult);
}

void SteamPlatform::onLobbyEnterResult(LobbyEnter_t* result, bool ioFailure) {
    if (ioFailure || result->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
        ENG_LOG("[steam] JoinLobby failed");
        m_events.push_back({Event::Type::LobbyJoinFailed});
        return;
    }
    m_lobby = result->m_ulSteamIDLobby;
    ENG_LOG("[steam] entered lobby " << m_lobby);
    m_events.push_back({Event::Type::LobbyEntered, m_lobby});
}

void SteamPlatform::leaveLobby() {
    if (!m_initialized || m_lobby == 0) return;
    SteamMatchmaking()->LeaveLobby(CSteamID(static_cast<uint64>(m_lobby)));
    m_lobby = 0;
}

void SteamPlatform::inviteFriends() {
    if (!m_initialized || m_lobby == 0) return;
    SteamFriends()->ActivateGameOverlayInviteDialog(CSteamID(static_cast<uint64>(m_lobby)));
}

bool SteamPlatform::overlayEnabled() const {
    return m_initialized && SteamAPI_ISteamUtils_IsOverlayEnabled(SteamUtils());
}

bool SteamPlatform::inviteToLobby(uint64_t friendId) {
    if (!m_initialized || m_lobby == 0) return false;
    return SteamAPI_ISteamMatchmaking_InviteUserToLobby(SteamMatchmaking(), m_lobby, friendId);
}

std::vector<SteamPlatform::Friend> SteamPlatform::friends() const {
    std::vector<Friend> out;
    if (!m_initialized) return out;
    ISteamFriends* f = SteamFriends();
    uint32 appId = SteamUtils()->GetAppID();
    int count = SteamAPI_ISteamFriends_GetFriendCount(f, k_EFriendFlagImmediate);
    for (int i = 0; i < count; ++i) {
        Friend fr;
        fr.id = SteamAPI_ISteamFriends_GetFriendByIndex(f, i, k_EFriendFlagImmediate);
        if (fr.id == 0) continue;
        fr.name = personaName(fr.id);
        fr.online = SteamAPI_ISteamFriends_GetFriendPersonaState(f, fr.id) != k_EPersonaStateOffline;
        FriendGameInfo_t game{};
        if (SteamAPI_ISteamFriends_GetFriendGamePlayed(f, fr.id, &game) && game.m_gameID.AppID() == appId) {
            fr.inThisGame = true;
            fr.lobby = game.m_steamIDLobby.ConvertToUint64();
        }
        out.push_back(std::move(fr));
    }
    std::sort(out.begin(), out.end(), [](const Friend& a, const Friend& b) {
        if (a.inThisGame != b.inThisGame) return a.inThisGame;
        if (a.online != b.online) return a.online;
        return a.name < b.name;
    });
    return out;
}

void SteamPlatform::setLobbyJoinable(bool joinable) {
    if (!m_initialized || m_lobby == 0) return;
    SteamMatchmaking()->SetLobbyJoinable(CSteamID(static_cast<uint64>(m_lobby)), joinable);
}

uint64_t SteamPlatform::currentLobby() const { return m_lobby; }

uint64_t SteamPlatform::lobbyOwner() const {
    if (!m_initialized || m_lobby == 0) return 0;
    return SteamAPI_ISteamMatchmaking_GetLobbyOwner(SteamMatchmaking(), m_lobby);
}

std::vector<uint64_t> SteamPlatform::lobbyMembers() const {
    std::vector<uint64_t> out;
    if (!m_initialized || m_lobby == 0) return out;
    ISteamMatchmaking* mm = SteamMatchmaking();
    int count = mm->GetNumLobbyMembers(CSteamID(static_cast<uint64>(m_lobby)));
    for (int i = 0; i < count; ++i) {
        uint64_t id = SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(mm, m_lobby, i);
        if (id != 0) out.push_back(id);
    }
    return out;
}

bool SteamPlatform::isLobbyMember(uint64_t steamId) const {
    for (uint64_t id : lobbyMembers())
        if (id == steamId) return true;
    return false;
}

void SteamPlatform::setLobbyData(const std::string& key, const std::string& value) {
    if (!m_initialized || m_lobby == 0) return;
    SteamMatchmaking()->SetLobbyData(CSteamID(static_cast<uint64>(m_lobby)), key.c_str(), value.c_str());
}

std::string SteamPlatform::lobbyData(const std::string& key) const {
    if (!m_initialized || m_lobby == 0) return "";
    const char* v = SteamMatchmaking()->GetLobbyData(CSteamID(static_cast<uint64>(m_lobby)), key.c_str());
    return v ? v : "";
}

void SteamPlatform::onJoinRequested(GameLobbyJoinRequested_t* request) {
    // Friend clicked "Join game" in the overlay / accepted an invite while
    // we were already running. Surfaced rather than acted on: the game may
    // need to end its current session first.
    m_events.push_back({Event::Type::JoinRequested, request->m_steamIDLobby.ConvertToUint64()});
}

void SteamPlatform::onLobbyInvite(LobbyInvite_t* invite) {
    m_events.push_back({Event::Type::Invited, invite->m_ulSteamIDLobby, invite->m_ulSteamIDUser});
}

void SteamPlatform::onLobbyChatUpdate(LobbyChatUpdate_t* update) {
    if (update->m_ulSteamIDLobby != m_lobby) return;
    uint32 change = update->m_rgfChatMemberStateChange;
    if (change & k_EChatMemberStateChangeEntered)
        m_events.push_back({Event::Type::MemberJoined, m_lobby, update->m_ulSteamIDUserChanged});
    else
        m_events.push_back({Event::Type::MemberLeft, m_lobby, update->m_ulSteamIDUserChanged});
}

#else  // !ENG_HAVE_STEAM

SteamPlatform::SteamPlatform() = default;
SteamPlatform::~SteamPlatform() = default;
std::string SteamPlatform::localPersonaName() const { return ""; }
uint64_t SteamPlatform::localSteamId() const { return 0; }
std::string SteamPlatform::personaName(uint64_t) const { return ""; }
void SteamPlatform::update() {}
void SteamPlatform::createLobby(int, bool) {}
void SteamPlatform::joinLobby(uint64_t) {}
void SteamPlatform::leaveLobby() {}
void SteamPlatform::inviteFriends() {}
void SteamPlatform::setLobbyJoinable(bool) {}
bool SteamPlatform::overlayEnabled() const { return false; }
bool SteamPlatform::inviteToLobby(uint64_t) { return false; }
std::vector<SteamPlatform::Friend> SteamPlatform::friends() const { return {}; }
uint64_t SteamPlatform::currentLobby() const { return 0; }
uint64_t SteamPlatform::lobbyOwner() const { return 0; }
std::vector<uint64_t> SteamPlatform::lobbyMembers() const { return {}; }
bool SteamPlatform::isLobbyMember(uint64_t) const { return false; }
void SteamPlatform::setLobbyData(const std::string&, const std::string&) {}
std::string SteamPlatform::lobbyData(const std::string&) const { return ""; }

#endif

}  // namespace eng
