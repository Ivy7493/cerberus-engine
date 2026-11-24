#pragma once

#include "eng/config.h" // ENG_HAVE_STEAM, as libeng.a was built

#include <cstdint>
#include <string>
#include <vector>

#ifdef ENG_HAVE_STEAM
#  include <steam/steam_api.h>
#endif

namespace eng {

// Steamworks bootstrap + lobbies, generalised from arena's
// SteamLobbyManager. Lobbies here are the *session directory*: the lobby
// owner is the game host, members are the players, and invites/joins go
// through Steam's own friends overlay. Actual gameplay traffic goes over
// SteamTransport, addressed by the member SteamIDs this hands out.
//
// Every call is a no-op (and available() false) when Steam failed to init
// or the engine was built without ENG_HAVE_STEAM — games must keep working
// without it (local UDP testing, non-Steam builds).
//
// MinGW note (inherited from arena): Valve's steam_api64.dll is MSVC-built,
// and the OOP accessors that *return* a CSteamID by value disagree with
// MinGW about that return's calling convention — intermittent crashes on
// Windows. Anything returning a SteamID goes through the flat C API
// (SteamAPI_ISteam*_*) instead, which returns a plain uint64.
class SteamPlatform {
public:
    SteamPlatform();
    ~SteamPlatform();

    SteamPlatform(const SteamPlatform&) = delete;
    SteamPlatform& operator=(const SteamPlatform&) = delete;

    bool available() const { return m_initialized; }
    std::string localPersonaName() const;
    uint64_t localSteamId() const;
    std::string personaName(uint64_t steamId) const;

    // Pumps Steam callbacks. Call once per frame.
    void update();

    // ── Lobby ────────────────────────────────────────────────────────────────
    void createLobby(int maxMembers, bool friendsOnly = true);
    void joinLobby(uint64_t lobbyId);
    void leaveLobby();
    // Steam overlay invite dialog. Only works when the overlay is hooked into
    // this process, i.e. the game was launched *by Steam* — started from
    // itch/a terminal there's no overlay and this silently does nothing.
    // Prefer an in-game friend list + inviteToLobby(), which always works.
    void inviteFriends();
    bool overlayEnabled() const;

    // Sends a Steam chat invite for the current lobby. If the friend's game
    // is running it gets an Invited event (see below); otherwise they see
    // the invite in Steam chat.
    bool inviteToLobby(uint64_t friendId);

    struct Friend {
        uint64_t id = 0;
        std::string name;
        bool online = false;
        bool inThisGame = false;  // running this app id right now
        uint64_t lobby = 0;       // lobby they're in for this game, 0 if none (joinable)
    };
    // Immediate friends: in-game first, then online, then offline; by name.
    std::vector<Friend> friends() const;

    // Short, typeable lobby code (base36 of the lobby id) for sharing out of
    // band when invites aren't an option. decode returns 0 on garbage.
    static std::string encodeLobbyCode(uint64_t lobbyId);
    static uint64_t decodeLobbyCode(const std::string& code);
    void setLobbyJoinable(bool joinable);

    uint64_t currentLobby() const;
    uint64_t lobbyOwner() const;
    bool isLobbyOwner() const { return available() && lobbyOwner() == localSteamId(); }
    std::vector<uint64_t> lobbyMembers() const;
    bool isLobbyMember(uint64_t steamId) const;

    void setLobbyData(const std::string& key, const std::string& value);
    std::string lobbyData(const std::string& key) const;

    // Parses "+connect_lobby <id>" (what Steam passes when a friend accepts
    // an invite while the game isn't running). Returns 0 if absent.
    static uint64_t lobbyFromCommandLine(int argc, char** argv);

    struct Event {
        enum class Type : uint8_t {
            LobbyCreated,    // we own a fresh lobby (lobby = its id)
            LobbyCreateFailed,
            LobbyEntered,    // we joined someone's (or our own) lobby
            LobbyJoinFailed,
            MemberJoined,    // user = who
            MemberLeft,      // user = who (left, disconnected, kicked)
            JoinRequested,   // friend invite accepted / "Join game" clicked (lobby = target);
                             // the game decides whether to tear down its session and joinLobby()
            Invited,         // a friend invited us in-game (lobby = target, user = who)
        };
        Type type;
        uint64_t lobby = 0;
        uint64_t user = 0;
    };
    // Lobby events since the last call, oldest first.
    std::vector<Event> drainEvents();

private:
    bool m_initialized = false;
    std::vector<Event> m_events;

#ifdef ENG_HAVE_STEAM
    uint64_t m_lobby = 0;

    void onLobbyCreated(LobbyCreated_t* result, bool ioFailure);
    void onLobbyEnterResult(LobbyEnter_t* result, bool ioFailure);
    CCallResult<SteamPlatform, LobbyCreated_t> m_lobbyCreated;
    CCallResult<SteamPlatform, LobbyEnter_t> m_lobbyEnterResult;

    STEAM_CALLBACK(SteamPlatform, onJoinRequested, GameLobbyJoinRequested_t);
    STEAM_CALLBACK(SteamPlatform, onLobbyChatUpdate, LobbyChatUpdate_t);
    STEAM_CALLBACK(SteamPlatform, onLobbyInvite, LobbyInvite_t);
#endif
};

}  // namespace eng
