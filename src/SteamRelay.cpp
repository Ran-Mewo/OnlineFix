#include "SteamRelay.h"
#include "SteamApi.h"
#include "Hook.h"
#include "PayloadLog.h"

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// ─────────────────────────────────────────────────────────────────
//  The relay between OnlineFix machines, driven through steam_api's flat C API. Every Steam function is resolved from steam_api64.dll by name, so
//  we link nothing and tolerate SDK drift.
//
//  Callbacks: Unity's Steam wrappers (Facepunch, Steamworks.NET) pump Steam via ManualDispatch every frame, so we DETOUR
//  SteamAPI_ManualDispatch_GetNextCallback and ride that pump — reading lobby/session callbacks (never freeing them: the game owns FreeLastCallback),
//  hiding the ones about our own registry lobby from the game, and draining relay receive on each tick. No dedicated poll thread, no sleeps.
//
//  Anti-interference: we advertise as appid 480 (Spacewar) like every other 480-spoofed game, so a raw lobby list returns EVERY Spacewar lobby. We
//  tag our lobbies with "ofx_game"=<realAppId> and filter the list server-side (AddRequestLobbyListStringFilter), so we only ever see and contact
//  peers running THIS game under OnlineFix.
// ─────────────────────────────────────────────────────────────────

namespace {
    // ── Steam types (ABI-stable layouts) ────────────────────────────
    using HSteamPipe     = int32_t;
    using SteamAPICall_t = uint64_t;

    struct SteamNetworkingIdentity { char opaque[136]; };

    // Prefix of SteamNetConnectionInfo_t, which can grow across SDK versions: only ever read through Steam's own pointers, never filled by Steam.
    struct SteamNetConnectionInfo_t {
        SteamNetworkingIdentity m_identityRemote;
        int64_t  m_nUserData;
        uint32_t m_hListenSocket;
        uint8_t  m_addrRemote[18];
        uint16_t m__pad1;
        uint32_t m_idPOPRemote;
        uint32_t m_idPOPRelay;
        int32_t  m_eState;
        int32_t  m_eEndReason;
        char     m_szEndDebug[128];
    };
    static_assert(offsetof(SteamNetConnectionInfo_t, m_szEndDebug) == 184, "SteamNetConnectionInfo_t layout drifted");

    struct SteamNetworkingMessage_t {
        void*                    m_pData;
        int                      m_cbSize;
        uint32_t                 m_conn;
        SteamNetworkingIdentity  m_identityPeer;
        int64_t                  m_nConnUserData;
        int64_t                  m_usecTimeReceived;
        int64_t                  m_nMessageNumber;
        void                   (*m_pfnFreeData)(SteamNetworkingMessage_t*);
        void                   (*m_pfnRelease)(SteamNetworkingMessage_t*);
        int                      m_nChannel;
        int                      m_nFlags;
        int64_t                  m_nUserData;
        uint16_t                 m_idxLane;
        uint16_t                 _pad;
    };

    // ManualDispatch callback envelope + the callbacks we observe.
    struct CallbackMsg_t { int32_t m_hSteamUser; int m_iCallback; uint8_t* m_pubParam; int m_cubParam; };
    struct SteamAPICallCompleted_t { SteamAPICall_t m_hAsyncCall; int m_iCallback; uint32_t m_cubParam; };
    struct LobbyCreated_t   { int m_eResult; uint64_t m_ulSteamIDLobby; };
    struct LobbyEnter_t     { uint64_t m_ulSteamIDLobby; uint32_t m_rgfChatPermissions; uint8_t m_bLocked; uint32_t m_EChatRoomEnterResponse; };
    struct LobbyMatchList_t { uint32_t m_nLobbiesMatching; };
    struct LobbyDataUpdate_t { uint64_t m_ulSteamIDLobby; uint64_t m_ulSteamIDMember; uint8_t m_bSuccess; };
    struct LobbyChatUpdate_t {
        uint64_t m_ulSteamIDLobby;
        uint64_t m_ulSteamIDUserChanged;
        uint64_t m_ulSteamIDMakingChange;
        uint32_t m_rgfChatMemberStateChange;
    };

    constexpr int k_iAPICallCompleted = 703;
    constexpr int k_iLobbyCreated     = 513;
    constexpr int k_iLobbyEnter       = 504;
    constexpr int k_iLobbyMatchList   = 510;
    constexpr int k_iSessionRequest   = 1251;  // k_iSteamNetworkingMessagesCallbacks(1250) + 1
    constexpr int k_iSessionFailed    = 1252;  // ...Callbacks + 2: relay handshake failed
    constexpr int k_iLobbyChatUpdate   = 506;  // k_iSteamMatchmakingCallbacks(500) + 6
    constexpr int k_iLobbyChatMsg      = 507;
    constexpr int k_iLobbyGameCreated  = 509;
    constexpr int k_iLobbyKicked       = 512;
    constexpr uint32_t k_EChatMemberStateChangeEntered = 0x0001;
    constexpr uint32_t k_EChatMemberStateChangeGone    = 0x0002 | 0x0004 | 0x0008 | 0x0010;  // left, disconnected, kicked, banned
    // Games use the low SteamNetworkingMessages channels for their own traffic, so ours sit far above them.
    constexpr int kChTunnel = 0x4F460000;      // OnlineFix traffic between machines
    constexpr int kChPing   = 0x4F460001;      // diagnostic hello (never forwarded)
    constexpr int k_iCallbackHidden   = 0;      // neutral id: no game handler matches it
    constexpr int k_iLobbyDataUpdate  = 505;

    constexpr int kSendReliable        = 8;     // k_nSteamNetworkingSend_Reliable
    constexpr int kSendUnreliable      = 0;
    constexpr int kLobbyTypeInvisible  = 3;     // k_ELobbyTypeInvisible
    constexpr int kLobbyCmpEqual       = 0;     // k_ELobbyComparisonEqual
    constexpr int kMaxLobbyMembers     = 250;   // Steam's cap: every joiner co-joins to authorize its relay session

    constexpr char kTagKey[]       = "ofx_game";   // value = real appid; namespaces our 480 lobbies
    constexpr char kTagFlag[]      = "ofx";
    constexpr char kTagHost[]      = "ofx_host";   // host SteamId (the owner is unreadable in list results)
    constexpr char kTagPublished[] = "ofx_data";   // the host's published blob, hex
    constexpr char kTagGameLobby[] = "ofx_lobby";  // the host game's Steam lobby
    constexpr size_t kMaxLobbyValue = 8192;         // k_cubChatMetadataMax

    // ── Resolved exports ────────────────────────────────────────────
    using SetId64_t  = void (*)(SteamNetworkingIdentity*, uint64_t);
    using GetId64_t  = uint64_t (*)(const SteamNetworkingIdentity*);
    SetId64_t pSetIdentitySteamID64 = nullptr;
    GetId64_t pGetIdentitySteamID64 = nullptr;

    using SendMessageToUser_t   = int  (*)(void*, const SteamNetworkingIdentity*, const void*, uint32_t, int, int);
    using ReceiveOnChannel_t    = int  (*)(void*, int, SteamNetworkingMessage_t**, int);
    using AcceptSession_t       = bool (*)(void*, const SteamNetworkingIdentity*);
    SendMessageToUser_t pSendMessageToUser = nullptr;
    ReceiveOnChannel_t  pReceiveOnChannel  = nullptr;
    AcceptSession_t     pAcceptSession      = nullptr;
    // Synchronous session state. Its info/status out-params are optional and left null, so no SDK-sized struct is ever filled.
    using GetSessionInfo_t = int (*)(void*, const SteamNetworkingIdentity*, SteamNetConnectionInfo_t*, void*);
    GetSessionInfo_t    pGetSessionInfo     = nullptr;

    // SteamNetworkingUtils: prime the SDR relay network so P2P Messages route.
    void* g_utils = nullptr;
    using InitRelay_t = void (*)(void*);
    InitRelay_t pInitRelay = nullptr;
    using GetRelayStatus_t = int (*)(void*, void*);   // ESteamNetworkingAvailability
    GetRelayStatus_t pGetRelayStatus = nullptr;

    using CreateLobby_t      = SteamAPICall_t (*)(void*, int, int);
    using RequestLobbyList_t = SteamAPICall_t (*)(void*);
    using JoinLobby_t        = SteamAPICall_t (*)(void*, uint64_t);
    using RequestLobbyData_t = bool      (*)(void*, uint64_t);
    using SetLobbyData_t     = bool      (*)(void*, uint64_t, const char*, const char*);
    using SetLobbyType_t     = bool      (*)(void*, uint64_t, int);   // ELobbyType
    using SetLobbyJoinable_t = bool      (*)(void*, uint64_t, bool);
    using GetLobbyData_t     = const char* (*)(void*, uint64_t, const char*);
    using GetLobbyOwner_t    = uint64_t  (*)(void*, uint64_t);
    using GetLobbyByIndex_t  = uint64_t  (*)(void*, int);
    using LeaveLobby_t       = void      (*)(void*, uint64_t);
    using AddStrFilter_t     = void      (*)(void*, const char*, const char*, int);
    using AddDistFilter_t    = void      (*)(void*, int);   // ELobbyDistanceFilter
    CreateLobby_t      pCreateLobby      = nullptr;
    RequestLobbyList_t pRequestLobbyList = nullptr;
    JoinLobby_t        pJoinLobby        = nullptr;
    RequestLobbyData_t pRequestLobbyData = nullptr;
    SetLobbyData_t     pSetLobbyData     = nullptr;
    SetLobbyType_t     pSetLobbyType     = nullptr;
    SetLobbyJoinable_t pSetLobbyJoinable = nullptr;
    GetLobbyData_t     pGetLobbyData     = nullptr;
    GetLobbyOwner_t    pGetLobbyOwner    = nullptr;
    GetLobbyByIndex_t  pGetLobbyByIndex  = nullptr;
    LeaveLobby_t       pLeaveLobby       = nullptr;
    AddStrFilter_t     pAddStrFilter     = nullptr;
    AddDistFilter_t    pAddDistFilter    = nullptr;
    constexpr int      kLobbyDistWorldwide = 3;   // k_ELobbyDistanceFilterWorldwide

    // ManualDispatch: we detour GetNextCallback, and call GetAPICallResult to fetch our own call results (CreateLobby/RequestLobbyList).
    using GetNextCallback_t = bool (*)(HSteamPipe, CallbackMsg_t*);
    using GetAPICallResult_t = bool (*)(HSteamPipe, SteamAPICall_t, void*, int, int, bool*);
    GetNextCallback_t  oGetNextCallback  = nullptr;
    GetAPICallResult_t pGetAPICallResult = nullptr;

    // ── State ───────────────────────────────────────────────────────
    SteamRelay::Callbacks g_cb;
    std::atomic_bool      g_inited{false};
    std::atomic_bool      g_deferredInit{false};   // detour armed, waiting on SteamAPI_Init
    void*                 g_messages = nullptr;
    void*                 g_matchmaking = nullptr;
    SteamRelay::SteamId   g_localId = 0;
    std::atomic<uint64_t> g_ourLobby{0};             // the registry lobby we host
    std::atomic<uint64_t> g_gameLobby{0};            // the Steam lobby the game last entered itself
    std::atomic<uint64_t> g_hideEnter{0};            // the Steam lobby we're joining for the game (JoinGameLobby)
    std::atomic<uint64_t> g_pendingCreateCall{0};
    std::atomic<uint64_t> g_pendingListCall{0};
    std::mutex            g_mutex;                   // what follows is shared with the game threads that publish and send
    std::string           g_published;               // hex of the blob we publish
    std::map<SteamRelay::SteamId, uint64_t> g_hostLobby;   // discovered host -> its registry lobby
    std::atomic<uint64_t> g_joinedHostLobby{0};      // host lobby WE joined (as a joiner)
    uint64_t              g_lobbyForGame = 0;        // the host's Steam lobby we joined for the game, and that host
    SteamRelay::SteamId   g_lobbyForGameHost = 0;
    std::vector<SteamRelay::HostListing> g_discovered;  // this browse so far (pump thread only)
    std::unordered_set<uint64_t>         g_pendingData; // result lobbies awaiting RequestLobbyData (pump thread only)

    template <typename Fn>
    bool Res(const char* name, Fn& slot) {
        slot = reinterpret_cast<Fn>(SteamApi::Export(name));
        if (!slot) PayloadLog::Write(std::string("SteamRelay: missing export ") + name);
        return slot != nullptr;
    }

    SteamNetworkingIdentity MakeIdentity(SteamRelay::SteamId id) {
        SteamNetworkingIdentity ident{};
        if (pSetIdentitySteamID64) pSetIdentitySteamID64(&ident, id);
        return ident;
    }

    void Accept(SteamRelay::SteamId peer) {
        if (!pAcceptSession || !peer) return;
        const SteamNetworkingIdentity ident = MakeIdentity(peer);
        pAcceptSession(g_messages, &ident);
    }

    // Lobby data values are C strings, so the published blob travels as hex.
    std::string ToHex(const std::vector<uint8_t>& bytes) {
        static const char kDigits[] = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (uint8_t b : bytes) { out.push_back(kDigits[b >> 4]); out.push_back(kDigits[b & 15]); }
        return out;
    }
    std::vector<uint8_t> FromHex(const char* hex) {
        auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        std::vector<uint8_t> out;
        for (size_t i = 0; hex && hex[i] && hex[i + 1]; i += 2) out.push_back((uint8_t)(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
        return out;
    }

    // The per-game discriminator for the ofx_game discovery tag. SteamAppId is forced to 480 (the networking cert must bind there), so the
    // real appid arrives out of band in OpenSteamTool_RealAppId_<pid>; the env is only a fallback for older OST builds.
    std::string GameTag() {
        char nm[64];
        std::snprintf(nm, sizeof(nm), "OpenSteamTool_RealAppId_%lu", (unsigned long)GetCurrentProcessId());
        if (HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, nm)) {
            uint32_t real = 0;
            if (auto* v = static_cast<uint32_t*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(uint32_t)))) {
                real = *v;
                UnmapViewOfFile(v);
            }
            CloseHandle(h);
            if (real) return std::to_string(real);
        }
        char buf[32] = {0};
        DWORD n = GetEnvironmentVariableA("SteamAppId", buf, sizeof(buf));
        return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
    }

    // The registry lobby is Invisible: returned by search yet never shown as the lobby you're in, so the game's own lobby stays the one your
    // persona carries — the one "Invite to Game" invites into. (A user can be in one regular lobby plus invisible ones.) Search also needs joinable.
    void MakeDiscoverable(uint64_t lobby) {
        if (pSetLobbyType)     pSetLobbyType(g_matchmaking, lobby, kLobbyTypeInvisible);
        if (pSetLobbyJoinable) pSetLobbyJoinable(g_matchmaking, lobby, true);
    }

    // The registry lobby is created the first time there is something to show in it.
    void EnsureRegistryLobby() {
        if (!g_inited.load() || g_ourLobby.load() || g_pendingCreateCall.load() || !pCreateLobby) return;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_published.empty()) return;
        }
        g_pendingCreateCall.store(pCreateLobby(g_matchmaking, kLobbyTypeInvisible, kMaxLobbyMembers));
        PayloadLog::Write("SteamRelay: creating registry lobby (call=" + std::to_string(g_pendingCreateCall.load()) + ")");
    }

    // The Steam interface pointers (messages/matchmaking/user) only exist once the game has called SteamAPI_Init. The Photon assembly can load before
    // that, so Init may run too early; resolve them here and finish bringup the moment they (and a logged-in user) are available. Called from the
    // GetNextCallback pump, which the game only drives post-init.
    bool FinishInitIfReady() {
        if (g_inited.load()) return true;
        if (!g_messages)    g_messages    = SteamApi::Interface("SteamAPI_SteamNetworkingMessages_SteamAPI");
        if (!g_matchmaking) g_matchmaking = SteamApi::Interface("SteamAPI_SteamMatchmaking");
        if (!g_messages || !g_matchmaking) return false;
        const SteamRelay::SteamId id = SteamApi::LocalSteamId();
        if (!id) return false;   // Steam context up but user not logged in yet
        g_localId = id;
        // Prime the SDR relay network so the P2P Messages cert (now minted under 480 by the OST 5621 rewrite) is fetched and the relay route comes
        // up.
        if (!g_utils) g_utils = SteamApi::Interface("SteamAPI_SteamNetworkingUtils_SteamAPI");
        if (g_utils && pInitRelay) {
            pInitRelay(g_utils);
            PayloadLog::Write("SteamRelay: InitRelayNetworkAccess primed");
        }
        g_deferredInit.store(false);
        g_inited.store(true);
        // SteamAppId is what steam_api bound the connection — and so the relay cert — to; it should read 480.
        char envApp[32] = {0};
        GetEnvironmentVariableA("SteamAppId", envApp, sizeof(envApp));
        PayloadLog::Write("SteamRelay: initialised, localId=" + std::to_string(g_localId) + " gameTag=" + GameTag() + " SteamAppId=" + envApp);
        EnsureRegistryLobby();   // something may have been published before Steam was up
        return true;
    }

    // ESteamNetworkingAvailability: 100=Current(relay up), 2=Attempting, 3=Retrying, -1=NeverTried, -2=CannotTry, -101=Previously, -102=Failed.
    int RelayStatus() { return (g_utils && pGetRelayStatus) ? pGetRelayStatus(g_utils, nullptr) : -999; }

    void LogRelayStatus(const char* where) {
        PayloadLog::Write(std::string("SteamRelay: relay status @") + where + " = " + std::to_string(RelayStatus()));
    }

    // ── Callback handlers (called from the GetNextCallback detour) ──
    void PublishToLobby(uint64_t lobby) {
        if (!lobby || !pSetLobbyData) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        pSetLobbyData(g_matchmaking, lobby, kTagPublished, g_published.c_str());
    }

    void PublishGameLobby() {
        const uint64_t ours = g_ourLobby.load(), game = g_gameLobby.load();
        if (ours && game && pSetLobbyData) pSetLobbyData(g_matchmaking, ours, kTagGameLobby, std::to_string(game).c_str());
    }

    // The game joined a Steam lobby on its own. If it's the one we joined for it, forget it, so LeaveHostLobby doesn't take the game out of it.
    void NoteGameLobby(uint64_t lobby) {
        g_gameLobby.store(lobby);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_lobbyForGame == lobby) g_lobbyForGame = 0;
        }
        PublishGameLobby();
    }

    // Friends can only "Join Game" through a player in the host's Steam lobby, but games that join the host's room by its name never join that
    // Steam lobby, so we join it for them. The game isn't sent this LobbyEnter, as it otherwise gets games stuck in a ghost lobby if they join it on their own later.
    void JoinGameLobby(SteamRelay::SteamId host, uint64_t registry) {
        const char* id = pGetLobbyData ? pGetLobbyData(g_matchmaking, registry, kTagGameLobby) : nullptr;
        const uint64_t lobby = id ? strtoull(id, nullptr, 10) : 0;
        if (!lobby || lobby == g_gameLobby.load()) return;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_lobbyForGame == lobby) return;
            g_lobbyForGame     = lobby;
            g_lobbyForGameHost = host;
        }
        g_hideEnter.store(lobby);
        pJoinLobby(g_matchmaking, lobby);
        PayloadLog::Write("SteamRelay: joining host " + std::to_string(host) + "'s Steam lobby " + std::to_string(lobby));
    }

    void OnEnteredLobby(uint64_t lobby, uint32_t enterResp = 1) {
        if (!lobby) return;
        if (lobby == g_ourLobby.load()) {
            const std::string tag = GameTag();
            if (pSetLobbyData && !tag.empty()) {
                pSetLobbyData(g_matchmaking, lobby, kTagFlag, "1");
                pSetLobbyData(g_matchmaking, lobby, kTagKey, tag.c_str());
                pSetLobbyData(g_matchmaking, lobby, kTagHost, std::to_string(g_localId).c_str());
            }
            PublishToLobby(lobby);
            PublishGameLobby();
            MakeDiscoverable(lobby);
            PayloadLog::Write("SteamRelay: hosting registry lobby " + std::to_string(lobby) + " tagged " + kTagKey + "=" + tag);
            LogRelayStatus("host");
            return;
        }
        const uint64_t owner = pGetLobbyOwner ? pGetLobbyOwner(g_matchmaking, lobby) : 0;
        if (owner != g_localId) Accept(owner);   // pre-open to host
        PayloadLog::Write("SteamRelay: entered lobby " + std::to_string(lobby) + " owner=" + std::to_string(owner)
                          + " enterResp=" + std::to_string(enterResp));  // 1 = success
    }

    // Record a result lobby once its metadata is readable (GetLobbyData is empty for list results until RequestLobbyData lands). The host tag is
    // always set on a registry lobby, so it doubles as the "data arrived" marker; the published blob may legitimately be empty.
    bool RecordHost(uint64_t lob) {
        const char* host = pGetLobbyData ? pGetLobbyData(g_matchmaking, lob, kTagHost) : nullptr;
        if (!host || !*host) return false;
        const SteamRelay::SteamId hid = strtoull(host, nullptr, 10);
        if (!hid || hid == g_localId) return true;   // our own lobby: nothing to record, but nothing to wait for
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_hostLobby[hid] = lob;   // so a joiner can co-join it for relay authorization
        }
        g_discovered.push_back({ hid, FromHex(pGetLobbyData(g_matchmaking, lob, kTagPublished)) });
        return true;
    }

    void ReportRegistry() {
        if (g_cb.onRegistry) g_cb.onRegistry(g_discovered, g_pendingData.empty());
    }

    void OnLobbyMatchList(uint32_t count) {
        g_discovered.clear();
        g_pendingData.clear();
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t lob = pGetLobbyByIndex ? pGetLobbyByIndex(g_matchmaking, (int)i) : 0;
            if (lob && !RecordHost(lob) && pRequestLobbyData && pRequestLobbyData(g_matchmaking, lob))
                g_pendingData.insert(lob);
        }
        PayloadLog::Write("SteamRelay: registry lists " + std::to_string(count) + " lobbies, " + std::to_string(g_discovered.size())
                          + " readable, " + std::to_string(g_pendingData.size()) + " pending");
        ReportRegistry();
    }

    // A pending lobby's metadata arrived — record it and report, so the listing fills in progressively.
    void OnLobbyDataUpdate(uint64_t lob, bool success) {
        if (!g_pendingData.erase(lob)) return;   // not one of our browse fetches
        if (success) RecordHost(lob);
        ReportRegistry();
    }

    // SDR SteamNetworkingMessagesSessionFailed_t: m_info begins with the remote identity. Surfaces relay handshake failures (should NOT happen now
    // that the cert is minted under 480) with the relay status for context. ESteamNetworkingConnectionState: 0=None 1=Connecting 2=FindingRoute
    // 3=Connected 4=ClosedByPeer 5=ProblemDetectedLocally.
    void LogSessionState(SteamRelay::SteamId peer, const char* where) {
        if (!pGetSessionInfo || !peer) return;
        SteamNetworkingIdentity ident = MakeIdentity(peer);
        PayloadLog::Write(std::string("SteamRelay: session @") + where + " peer=" + std::to_string(peer)
                          + " state=" + std::to_string(pGetSessionInfo(g_messages, &ident, nullptr, nullptr)));
    }

    void OnSessionFailed(const SteamNetConnectionInfo_t* info) {
        const uint64_t id = (info && pGetIdentitySteamID64) ? pGetIdentitySteamID64(&info->m_identityRemote) : 0;
        char why[sizeof(info->m_szEndDebug) + 1] = {0};
        int state = 0, endReason = 0;
        if (info) {
            state = info->m_eState;
            endReason = info->m_eEndReason;
            std::memcpy(why, info->m_szEndDebug, sizeof(info->m_szEndDebug));
        }
        PayloadLog::Write("SteamRelay: SDR session FAILED with " + std::to_string(id) + " state=" + std::to_string(state)
                          + " endReason=" + std::to_string(endReason) + " relayStatus=" + std::to_string(RelayStatus()) + " reason='" + why + "'");
        if (id && g_cb.onPeerLeft) g_cb.onPeerLeft(id);
    }

    void OnSessionRequest(uint64_t id) {
        Accept(id);
        PayloadLog::Write("SteamRelay: accepted session from " + std::to_string(id));
    }

    // A member entered or left our registry lobby. On a join we (the host) open the relay session FROM our side — pre-accept the peer and send a
    // hello — so it establishes promptly; the hello rides a diagnostic channel so it never reaches the game. Leaving the lobby means the peer's
    // process is gone (a joiner stays in it for as long as it plays).
    void OnLobbyMemberChange(uint64_t lobby, uint64_t member, uint32_t change) {
        if (lobby != g_ourLobby.load() || !member || member == g_localId) return;
        if (change & k_EChatMemberStateChangeGone) {
            PayloadLog::Write("SteamRelay: registry-lobby member left " + std::to_string(member) + " (change=" + std::to_string(change) + ")");
            if (g_cb.onPeerLeft) g_cb.onPeerLeft(member);
            return;
        }
        if (!(change & k_EChatMemberStateChangeEntered)) return;
        Accept(member);
        const SteamNetworkingIdentity ident = MakeIdentity(member);
        static const char kHello[] = "OFXHELLO";
        int sent = pSendMessageToUser ? pSendMessageToUser(g_messages, &ident, kHello, (uint32_t)sizeof(kHello), kSendReliable, kChPing) : -1;
        PayloadLog::Write("SteamRelay: registry-lobby member joined " + std::to_string(member)
                          + " — pre-accepted + hello result=" + std::to_string(sent));  // 1 = OK
    }

    void DrainReceive() {
        if (!g_inited.load() || !pReceiveOnChannel) return;
        for (int ch : { kChTunnel, kChPing }) {
            SteamNetworkingMessage_t* msgs[32];
            const int n = pReceiveOnChannel(g_messages, ch, msgs, 32);
            for (int i = 0; i < n; ++i) {
                auto* m = msgs[i];
                const uint64_t from = pGetIdentitySteamID64 ? pGetIdentitySteamID64(&m->m_identityPeer) : 0;
                const auto* data = static_cast<const uint8_t*>(m->m_pData);
                const size_t len = (m->m_cbSize > 0) ? (size_t)m->m_cbSize : 0;
                if (ch == kChPing)
                    PayloadLog::Write("SteamRelay: hello from " + std::to_string(from) + " — relay path OK");
                else if (g_cb.onMessage && data && len)
                    g_cb.onMessage(from, data, len);
                if (m->m_pfnRelease) m->m_pfnRelease(m);
            }
        }
    }

    // Our lobbies: the registry lobby we host, the host lobby we co-join, and the ones a browse is reading. Their callbacks are hidden from the
    // game — a game that tracks "its" lobby adopts any lobby it hears about (sets its data, locks it, leaves it), which wrecks the registry.
    bool IsOurLobby(uint64_t lobby) {
        if (!lobby) return false;
        if (lobby == g_ourLobby.load() || lobby == g_joinedHostLobby.load() || g_pendingData.count(lobby)) return true;
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [_, l] : g_hostLobby) if (l == lobby) return true;
        return false;
    }

    void AdoptRegistryLobby(uint64_t lobby) {
        if (g_ourLobby.exchange(lobby) != lobby) OnEnteredLobby(lobby);   // tags and publishes it
    }

    // A call result fetched at the size Steam reports (cb) and read through our prefix T, so a result that grows in a newer SDK still matches.
    template <class T>
    bool CallResult(HSteamPipe pipe, SteamAPICall_t call, int callback, size_t cb, T& out) {
        if (!call || cb < sizeof(T)) return false;
        std::vector<uint8_t> buf(cb);
        bool failed = true;
        if (!pGetAPICallResult(pipe, call, buf.data(), (int)cb, callback, &failed) || failed) return false;
        return std::memcpy(&out, buf.data(), sizeof(T)), true;
    }

    // Our CreateLobby also broadcasts LobbyCreated, which can arrive before its call result names the lobby; the result is already there by then.
    bool IsOurCreation(HSteamPipe pipe, uint64_t lobby, size_t cb) {
        LobbyCreated_t r{};
        return CallResult(pipe, g_pendingCreateCall.load(), k_iLobbyCreated, cb, r) && r.m_ulSteamIDLobby == lobby;
    }

    // The lobby a lobby callback is about: every one of them starts with it, except LobbyCreated which leads with its result.
    uint64_t LobbyOf(const CallbackMsg_t* msg) {
        switch (msg->m_iCallback) {
            case k_iLobbyCreated: return reinterpret_cast<const LobbyCreated_t*>(msg->m_pubParam)->m_ulSteamIDLobby;
            case k_iLobbyEnter: case k_iLobbyDataUpdate: case k_iLobbyChatUpdate:
            case k_iLobbyChatMsg: case k_iLobbyGameCreated: case k_iLobbyKicked:
                return *reinterpret_cast<const uint64_t*>(msg->m_pubParam);
            default:
                return 0;
        }
    }

    // Ride the game's per-frame ManualDispatch pump. Read our callbacks, hiding those about our registry lobby (the game still frees every message),
    // and drain receive.
    bool __cdecl hkGetNextCallback(HSteamPipe pipe, CallbackMsg_t* msg) {
        bool got = oGetNextCallback(pipe, msg);
        // Finish a deferred bringup now that the game is pumping callbacks (guaranteed post-SteamAPI_Init), before we touch any interface ptr.
        if (g_deferredInit.load() && !g_inited.load()) FinishInitIfReady();
        __try {
            if (got && msg && msg->m_pubParam) {
                const uint64_t lobby = LobbyOf(msg);
                if (msg->m_iCallback == k_iLobbyCreated && !IsOurLobby(lobby) && IsOurCreation(pipe, lobby, msg->m_cubParam)) AdoptRegistryLobby(lobby);
                bool hide = IsOurLobby(lobby);   // decided before the handlers below retire a lobby from the pending set

                if (msg->m_iCallback == k_iLobbyEnter) {
                    auto* le = reinterpret_cast<LobbyEnter_t*>(msg->m_pubParam);
                    if (lobby == g_hideEnter.load()) { g_hideEnter.store(0); hide = true; }
                    else if (!hide && le->m_EChatRoomEnterResponse == 1) NoteGameLobby(lobby);
                    OnEnteredLobby(le->m_ulSteamIDLobby, le->m_EChatRoomEnterResponse);
                } else if (msg->m_iCallback == k_iLobbyDataUpdate) {
                    auto* u = reinterpret_cast<LobbyDataUpdate_t*>(msg->m_pubParam);
                    OnLobbyDataUpdate(u->m_ulSteamIDLobby, u->m_bSuccess != 0);
                } else if (msg->m_iCallback == k_iLobbyChatUpdate) {
                    auto* u = reinterpret_cast<LobbyChatUpdate_t*>(msg->m_pubParam);
                    OnLobbyMemberChange(u->m_ulSteamIDLobby, u->m_ulSteamIDUserChanged, u->m_rgfChatMemberStateChange);
                } else if (msg->m_iCallback == k_iSessionRequest) {
                    if (pGetIdentitySteamID64)
                        OnSessionRequest(pGetIdentitySteamID64(reinterpret_cast<const SteamNetworkingIdentity*>(msg->m_pubParam)));
                } else if (msg->m_iCallback == k_iSessionFailed) {
                    OnSessionFailed(reinterpret_cast<const SteamNetConnectionInfo_t*>(msg->m_pubParam));
                } else if (msg->m_iCallback == k_iAPICallCompleted) {
                    auto* c = reinterpret_cast<SteamAPICallCompleted_t*>(msg->m_pubParam);
                    if (c->m_hAsyncCall == g_pendingListCall.load() && c->m_iCallback == k_iLobbyMatchList) {
                        LobbyMatchList_t r{};
                        OnLobbyMatchList(CallResult(pipe, c->m_hAsyncCall, c->m_iCallback, c->m_cubParam, r) ? r.m_nLobbiesMatching : 0);
                        g_pendingListCall.store(0);
                    } else if (c->m_hAsyncCall == g_pendingCreateCall.load() && c->m_iCallback == k_iLobbyCreated) {
                        LobbyCreated_t r{};
                        if (CallResult(pipe, c->m_hAsyncCall, c->m_iCallback, c->m_cubParam, r) && r.m_eResult == 1)
                            AdoptRegistryLobby(r.m_ulSteamIDLobby);
                        g_pendingCreateCall.store(0);
                    }
                }
                // Neutralizing the id makes the game's dispatcher skip it; FreeLastCallback tracks the pipe internally, unaffected.
                if (hide) msg->m_iCallback = k_iCallbackHidden;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        DrainReceive();
        return got;
    }
}

namespace SteamRelay {

    bool Init(const Callbacks& cb) {
        if (g_inited.load()) return true;
        g_cb = cb;

        if (!SteamApi::Module()) { PayloadLog::Write("SteamRelay: steam_api not loaded"); return false; }

        Res("SteamAPI_SteamNetworkingIdentity_SetSteamID64", pSetIdentitySteamID64);
        Res("SteamAPI_SteamNetworkingIdentity_GetSteamID64", pGetIdentitySteamID64);

        // Flat function pointers are plain steam_api64 exports, available even before SteamAPI_Init. The interface *instances* (below) are not, so
        // resolve those separately in FinishInitIfReady. SDR transport (ISteamNetworkingMessages, iface 46). This is the path whose per-app cert
        // steamclient mints via CMsgClientNetworkingCertRequest (eMsg 5621) — which OST now rewrites to appid 480 so a non-owner gets a valid,
        // universally-usable relay cert. (Legacy ISteamNetworking iface 12 never emits 5621 and can't be relayed for a non-owner, so we don't use
        // it.)
        bool ok = Res("SteamAPI_ISteamNetworkingMessages_SendMessageToUser", pSendMessageToUser)
                & Res("SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel", pReceiveOnChannel)
                & Res("SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser", pAcceptSession)
                & Res("SteamAPI_ISteamMatchmaking_CreateLobby", pCreateLobby)
                & Res("SteamAPI_ISteamMatchmaking_RequestLobbyList", pRequestLobbyList)
                & Res("SteamAPI_ISteamMatchmaking_JoinLobby", pJoinLobby)
                & Res("SteamAPI_ISteamMatchmaking_LeaveLobby", pLeaveLobby)
                & Res("SteamAPI_ISteamMatchmaking_RequestLobbyData", pRequestLobbyData)
                & Res("SteamAPI_ISteamMatchmaking_SetLobbyData", pSetLobbyData)
                & Res("SteamAPI_ISteamMatchmaking_GetLobbyData", pGetLobbyData)
                & Res("SteamAPI_ISteamMatchmaking_GetLobbyOwner", pGetLobbyOwner)
                & Res("SteamAPI_ISteamMatchmaking_GetLobbyByIndex", pGetLobbyByIndex)
                & Res("SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter", pAddStrFilter);
        if (!ok) return false;

        // Optional extras: registry-lobby discoverability, worldwide browse, relay diagnostics.
        Res("SteamAPI_ISteamMatchmaking_SetLobbyType", pSetLobbyType);
        Res("SteamAPI_ISteamMatchmaking_SetLobbyJoinable", pSetLobbyJoinable);
        Res("SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter", pAddDistFilter);
        Res("SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess", pInitRelay);
        Res("SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus", pGetRelayStatus);
        Res("SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo", pGetSessionInfo);

        // Callback pump: detour the game's per-frame ManualDispatch fetch.
        if (!(Res("SteamAPI_ManualDispatch_GetNextCallback", oGetNextCallback) & Res("SteamAPI_ManualDispatch_GetAPICallResult", pGetAPICallResult)))
            return false;
        if (!Hook::Attach({ { reinterpret_cast<void**>(&oGetNextCallback), reinterpret_cast<void*>(hkGetNextCallback) } })) {
            PayloadLog::Write("SteamRelay: GetNextCallback detour failed");
            return false;
        }

        // With the pump detoured, finishing bringup no longer needs Steam to already be initialised: if the interfaces aren't ready (Photon assembly
        // loaded pre-SteamAPI_Init), the first callback tick resolves them. Event-driven — no poll thread, no sleep.
        if (!FinishInitIfReady()) {
            g_deferredInit.store(true);
            PayloadLog::Write("SteamRelay: Steam interfaces not ready — deferring init to first callback pump");
        }
        return true;
    }

    void Publish(const std::vector<uint8_t>& blob) {
        std::string hex = ToHex(blob);
        if (hex.size() >= kMaxLobbyValue) {
            PayloadLog::Write("SteamRelay: published data (" + std::to_string(blob.size()) + " bytes) exceeds a lobby value — not updated");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_published = std::move(hex);
        }
        if (const uint64_t lobby = g_ourLobby.load()) PublishToLobby(lobby);
        else                                          EnsureRegistryLobby();
    }

    bool RequestRegistry() {
        if (!g_inited.load() || g_pendingListCall.load()) return g_pendingListCall.load() != 0;
        // Worldwide (the default is regional, which hides hosts elsewhere), and only THIS game's OnlineFix lobbies.
        const std::string tag = GameTag();
        if (pAddDistFilter) pAddDistFilter(g_matchmaking, kLobbyDistWorldwide);
        if (pAddStrFilter && !tag.empty()) pAddStrFilter(g_matchmaking, kTagKey, tag.c_str(), kLobbyCmpEqual);
        g_pendingListCall.store(pRequestLobbyList(g_matchmaking));
        return g_pendingListCall.load() != 0;
    }

    // The resulting LobbyEnters are hidden from the game (see hkGetNextCallback).
    void JoinHostLobby(SteamId host) {
        if (!g_inited.load() || !pJoinLobby) return;
        uint64_t lobby = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_hostLobby.find(host);
            if (it == g_hostLobby.end()) return;
            lobby = it->second;
        }
        if (g_joinedHostLobby.exchange(lobby) != lobby) {
            pJoinLobby(g_matchmaking, lobby);
            PayloadLog::Write("SteamRelay: joining host lobby " + std::to_string(lobby) + " (host " + std::to_string(host) + ") for relay authorization");
        }
        JoinGameLobby(host, lobby);
    }

    void LeaveHostLobby(SteamId host) {
        uint64_t lobby = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_lobbyForGameHost == host) lobby = std::exchange(g_lobbyForGame, 0);
        }
        if (lobby) pLeaveLobby(g_matchmaking, lobby);
    }

    bool Send(SteamId peer, const uint8_t* data, size_t len, bool reliable) {
        if (!g_inited.load() || !peer || !pSendMessageToUser) return false;
        SteamNetworkingIdentity ident = MakeIdentity(peer);
        const int r = pSendMessageToUser(g_messages, &ident, data, (uint32_t)len, reliable ? kSendReliable : kSendUnreliable, kChTunnel);
        static std::atomic_bool s_probed{false};
        if (!s_probed.exchange(true)) LogSessionState(peer, "first-send");
        return r == 1;   // k_EResultOK
    }

} // namespace SteamRelay
