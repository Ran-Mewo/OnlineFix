#include "PhotonEmulator.h"
#include "PhotonEnet.h"
#include "PhotonProtocol.h"
#include "PhotonServer.h"
#include "PayloadLog.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {
    using Photon::Message;
    using Photon::ParameterTable;
    using Photon::Type;
    using Photon::Value;
    using PhotonServer::Delivery;
    using PhotonServer::PeerId;
    using SteamRelay::SteamId;

    // ── Photon constants ────────────────────────────────────────────
    enum Op : uint8_t {
        OpLeave = 254, OpRaiseEvent = 253, OpAuthenticateOnce = 231, OpAuthenticate = 230, OpJoinLobby = 229, OpLeaveLobby = 228,
        OpCreateGame = 227, OpJoinGame = 226, OpJoinRandomGame = 225, OpGetRegions = 220, OpServerSettings = 218, OpGetGameList = 217,
    };
    enum Param : uint8_t {
        PRoomName = 255, PGameProperties = 248, PAddress = 230, PUserId = 225, PApplicationId = 224, PGameList = 222, PToken = 221,
        PAppVersion = 220, PJoinMode = 215, PLobbyName = 213, PLobbyType = 212, PRegion = 210,
    };
    enum Event : uint8_t { EvGameList = 230, EvGameListUpdate = 229 };
    enum Error : int16_t {
        Ok = 0, InvalidOperation = -2,
        GameIdAlreadyExists = 32766, GameFull = 32765, GameClosed = 32764, NoRandomMatchFound = 32760, GameDoesNotExist = 32758,
    };
    enum RoomKey : uint8_t { KMaxPlayers = 255, KIsVisible = 254, KIsOpen = 253, KPlayerCount = 252, KRemoved = 251, KMaxPlayersInt = 243 };
    constexpr int32_t JoinModeCreateIfNotExists = 1;

    // Addresses we hand out only have to parse as host:port — every datagram the client sends is diverted here anyway.
    constexpr const char* kMasterAddress     = "127.0.0.1:5055";
    constexpr const char* kGameServerAddress = "127.0.0.1:5056";
    constexpr const char* kRegion            = "us";

    constexpr DWORD  kRoomListRefreshMs = 5000;        // how often a lobby client's room list is re-read from the registry
    constexpr DWORD  kMatchmakingMs     = 10000;       // how long a master request may wait on the registry, then on the host
    constexpr size_t kMaxMessage        = 16u << 20;   // largest fragmented message we reassemble

    // A real Photon server's InitEncryption key. The client derives a shared secret from it and only then calls the connection ready; the secret
    // is never used, because payload encryption is switched off client-side.
    const uint8_t kServerKey[96] = {
        0xa9,0xf2,0xa5,0x30,0x05,0x4d,0x79,0x58,0x52,0x5d,0xe4,0x02,0x27,0x23,0x6c,0xf2,
        0x8d,0x62,0x5a,0x24,0xd4,0x94,0xa2,0xa0,0x79,0x64,0x71,0x06,0xbd,0x07,0xf7,0x6f,
        0xb7,0x20,0x7e,0xc7,0xb4,0xb9,0x9a,0x8a,0x81,0x9e,0xe7,0x02,0x84,0xfe,0xd6,0x01,
        0x18,0x3b,0x70,0x08,0x93,0xac,0x44,0x15,0x7c,0xb4,0xd2,0x20,0x20,0x44,0xf2,0x13,
        0xfa,0x86,0x6d,0xb9,0xa3,0x84,0xc4,0x96,0x02,0x1a,0x0c,0xa3,0x08,0x1f,0x14,0xda,
        0xfc,0xd6,0xd7,0xb8,0xb5,0x78,0xce,0x47,0xfb,0xaf,0x2c,0x1e,0x44,0xc2,0x8f,0xd1,
    };

    // ── Tunnel frames between machines ──────────────────────────────
    // [kind][connection: u16 BE][channel][reliable] body. A joiner Opens a connection on the room's host (body: app '\n' userId), sends its
    // client's Requests over it and Closes it; the host sends Messages back on it. Ping/Pong carry clock ticks, so a joiner's clients see the
    // host's server time.
    enum Frame : uint8_t { FOpen = 1, FRequest, FClose, FMessage, FPing, FPong };
    constexpr size_t kFrameHeader = 5;

    // ── State ───────────────────────────────────────────────────────
    // Name server and master are one server here: the client only reads the master address from an auth response while it is on the name
    // server, so a connection only has to be told apart from a game-server one — which its token does.
    enum class Phase : uint8_t { Master, GameServer };

    struct Partial {   // a fragmented message being reassembled
        std::vector<uint8_t> data;
        std::vector<bool>    got;
        uint32_t             missing = 0;
    };

    // One client connection (one SocketUdp). A client opens a fresh connection for each server it is sent to; the token we issue at each step
    // carries what the next server is — for a game server, which machine hosts the room — and which application the client is.
    struct Session {
        void*       socket = nullptr;
        uint32_t    challenge = 0;
        Phase       phase = Phase::Master;
        std::string app;          // AppId/AppVersion: each is its own namespace, as on Photon Cloud
        SteamId     host = 0;     // game server: the machine hosting the room (0 = this one)
        PeerId      peer = 0;     // game server here: our id on PhotonServer
        uint16_t    tunnel = 0;   // game server elsewhere: our connection id on the host
        bool        inRoom = false;
        bool        inLobby = false;
        ParameterTable lobby;     // LobbyName/LobbyType of the lobby it joined (none: the default lobby)
        DWORD       lobbySince = 0;
        Value       shownRooms;   // the room list a lobby client last got (Null: none yet)
        std::map<uint8_t, uint32_t>      outSeq;   // last reliable sequence number per channel
        std::map<uint64_t, Partial>      partial;
        std::vector<PhotonEnet::Command> outbox;
    };

    // A master request that has to wait: on a registry read to find its room, then on the clock of the host it resolved to.
    struct Pending {
        void*       socket = nullptr;
        Delivery    d;
        Message     request;
        DWORD       deadline = 0;
        SteamId     host = 0;   // resolved to this machine, awaiting its clock
        std::string room;
    };

    // A client of another machine, connected to a room we host.
    struct RemoteConn {
        PeerId      peer = 0;
        std::string app, userId;
    };

    struct Found { std::string room; Value props; SteamId host = 0; std::string lobby; };

    std::mutex                       g_mutex;
    PhotonEmulator::InjectFn         g_inject = nullptr;
    std::map<void*, Session>         g_sessions;
    std::string                      g_userId;           // this machine's Photon UserId, shared by its clients
    uint16_t                         g_nextClientId = 1; // ENet peer ids we assign
    PeerId                           g_nextPeer = 1;
    bool                             g_serverUp = false;
    std::map<PeerId, void*>          g_localPeers;       // PhotonServer peer -> local session
    std::map<PeerId, std::pair<SteamId, uint16_t>>          g_remotePeers;   // PhotonServer peer -> (machine, connection)
    std::map<std::pair<SteamId, uint16_t>, RemoteConn>      g_remoteConns;
    uint16_t                         g_nextTunnel = 1;
    std::map<uint16_t, void*>        g_tunnels;          // our connection id on a host -> local session
    std::map<SteamId, int32_t>       g_clock;            // host -> its tick minus ours
    std::map<SteamId, Value>         g_registry;         // host -> what it published: { app: { lobby: { room: lobby props } } }
    bool                             g_reading = false;  // a registry read is in flight
    DWORD                            g_lastRead = 0;
    std::vector<Pending>             g_pending;
    std::map<std::string, ParameterTable> g_createdIn;   // app '\n' room -> the lobby an unnamed create came from (see Admit)

    // ── Helpers ─────────────────────────────────────────────────────
    const Value* Param(const Message& m, uint8_t key) {
        auto it = m.params.find(key);
        return it == m.params.end() ? nullptr : &it->second;
    }
    std::string StrParam(const Message& m, uint8_t key) {
        const Value* v = Param(m, key);
        return (v && v->type == Type::String) ? v->str : std::string();
    }
    int32_t IntParam(const Message& m, uint8_t key) {
        const Value* v = Param(m, key);
        return v ? v->AsInt() : 0;
    }
    // A request's LobbyName/LobbyType (none: the default lobby).
    ParameterTable NamedLobby(const Message& m) {
        ParameterTable lobby;
        for (uint8_t key : { PLobbyName, PLobbyType }) if (const Value* v = Param(m, key)) lobby[key] = *v;
        return lobby;
    }

    void Put32(std::vector<uint8_t>& b, uint32_t v) {
        b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v);
    }
    uint32_t Get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

    // A GUID-shaped id, as Photon assigns user ids and room names.
    std::string NewId() {
        static std::mt19937_64 rng{ std::random_device{}() };
        const uint64_t a = rng(), b = rng();
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx", (uint32_t)(a >> 32), (uint32_t)(a >> 16) & 0xffff,
                      (uint32_t)a & 0xffff, (uint32_t)(b >> 48), (unsigned long long)(b & 0xffffffffffffULL));
        return buf;
    }

    std::string Hex(const std::vector<uint8_t>& b, size_t max) {
        std::string s;
        char t[4];
        for (size_t i = 0; i < b.size() && i < max; ++i) { std::snprintf(t, sizeof(t), "%02x ", b[i]); s += t; }
        return s;
    }

    const char* PhaseName(Phase p) { return p == Phase::Master ? "master" : "gameserver"; }

    // ── ENet: outbound ──────────────────────────────────────────────
    PhotonEnet::Command Ack(const PhotonEnet::Command& c, uint32_t sentTime) {
        PhotonEnet::Command ack;
        ack.type    = c.IsUnsequenced() ? PhotonEnet::AckUnsequenced : PhotonEnet::Ack;   // the client matches the two kinds separately
        ack.channel = c.channel;
        Put32(ack.payload, c.reliableSeq);
        Put32(ack.payload, sentTime);
        return ack;
    }

    PhotonEnet::Command& Queue(Session& s, uint8_t type, uint8_t channel, std::vector<uint8_t> payload) {
        PhotonEnet::Command c;
        c.type        = type;
        c.channel     = channel;
        c.flags       = PhotonEnet::Reliable;
        c.reliableSeq = ++s.outSeq[channel];
        c.payload     = std::move(payload);
        s.outbox.push_back(std::move(c));
        return s.outbox.back();
    }

    // Everything reaches a local client as a reliable command: delivery is in-process and cannot be lost.
    void Deliver(Session& s, uint8_t channel, const std::vector<uint8_t>& message) { Queue(s, PhotonEnet::SendReliable, channel, message); }

    void Reply(Session& s, Delivery d, uint8_t op, int16_t code, const ParameterTable& params = {}, const std::string& debug = {}) {
        Deliver(s, d.channel, Photon::WriteOperationResponse(op, code, params, debug));
    }

    int32_t ClockOffset(SteamId host) {
        auto it = g_clock.find(host);
        return it == g_clock.end() ? 0 : it->second;
    }

    // The header timestamp is the server clock the client syncs PhotonNetwork.Time to. For a room on another machine it is that host's clock,
    // so everyone in the room agrees on the time.
    void Flush(Session& s) {
        if (s.outbox.empty() || !g_inject) return;
        PhotonEnet::Packet p;
        p.timestamp = GetTickCount() + ClockOffset(s.host);
        p.challenge = s.challenge;
        for (size_t i = 0; i < s.outbox.size(); i += 255) {   // the command count is one byte
            p.commands.assign(s.outbox.begin() + i, s.outbox.begin() + std::min<size_t>(i + 255, s.outbox.size()));
            const auto bytes = PhotonEnet::Serialize(p);
            g_inject(s.socket, bytes.data(), bytes.size());
        }
        s.outbox.clear();
    }

    void FlushAll() { for (auto& [_, s] : g_sessions) Flush(s); }

    void Tunnel(SteamId to, Frame kind, uint16_t conn, Delivery d, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> f{ kind, (uint8_t)(conn >> 8), (uint8_t)conn, d.channel, (uint8_t)d.reliable };
        f.insert(f.end(), body.begin(), body.end());
        SteamRelay::Send(to, f.data(), f.size(), d.reliable);
    }

    void Ping(SteamId host) {
        std::vector<uint8_t> body;
        Put32(body, GetTickCount());
        Tunnel(host, FPing, 0, {}, body);
    }

    // Whether a client is in a room is read off the responses it gets, wherever the room is.
    void Track(Session& s, const std::vector<uint8_t>& msg) {
        if (msg.size() < 5 || msg[0] != 0xF3 || (msg[1] & 0x7F) != Photon::OperationResponse) return;
        const uint8_t op = msg[2];
        if (op == OpLeave) s.inRoom = false;
        else if ((op == OpCreateGame || op == OpJoinGame || op == OpJoinRandomGame) && msg[3] == 0 && msg[4] == 0) s.inRoom = true;
    }

    // ── The local game server ───────────────────────────────────────
    void ShowRooms(Session& s);

    void ServerSend(PeerId to, Delivery d, const std::vector<uint8_t>& msg) {
        if (auto local = g_localPeers.find(to); local != g_localPeers.end()) {
            if (auto s = g_sessions.find(local->second); s != g_sessions.end()) {
                Track(s->second, msg);
                Deliver(s->second, d.channel, msg);
            }
        } else if (auto remote = g_remotePeers.find(to); remote != g_remotePeers.end()) {
            Tunnel(remote->second.first, FMessage, remote->second.second, d, msg);
        }
    }

    // Our rooms changed: republish them, and refresh the room list of any local client in the lobby.
    void PublishRooms() {
        const Value rooms = PhotonServer::Rooms();
        Photon::Writer w;
        if (!rooms.hashtable.empty()) w.WriteValue(rooms);
        SteamRelay::Publish(w.buffer());
        for (auto& [_, s] : g_sessions) if (s.inLobby) ShowRooms(s);
    }

    void EnsureServer() {
        if (g_serverUp) return;
        g_serverUp = true;
        PhotonServer::Init(ServerSend, PublishRooms);
    }

    // ── Rooms visible from here ─────────────────────────────────────
    // This machine's rooms of `app`, in every lobby, then every registry host's.
    std::vector<Found> AllRooms(const std::string& app) {
        std::vector<Found> out;
        auto collect = [&](const Value& published, SteamId host) {
            const Value* lobbies = published.Find(Value::Str(app));
            if (!lobbies) return;
            for (const auto& [lobby, rooms] : lobbies->hashtable)
                for (const auto& [name, props] : rooms.hashtable)
                    if (lobby.type == Type::String && name.type == Type::String) out.push_back({ name.str, props, host, lobby.str });
        };
        if (g_serverUp) collect(PhotonServer::Rooms(), 0);
        for (const auto& [host, published] : g_registry) collect(published, host);
        return out;
    }

    std::optional<Found> FindRoom(const std::string& app, const std::string& name) {
        for (Found& f : AllRooms(app)) if (f.room == name) return std::move(f);
        return std::nullopt;
    }

    bool IsVisible(const Value& props) {
        const Value* v = props.Find(Value::U8(KIsVisible));
        return !v || v->type != Type::Boolean || v->b;
    }

    // Why the master refuses a join, from what the room's listing shows; Ok when it can be joined.
    int16_t Refusal(const Value& props) {
        if (const Value* open = props.Find(Value::U8(KIsOpen)); open && open->type == Type::Boolean && !open->b) return GameClosed;
        const Value* max = props.Find(Value::U8(KMaxPlayersInt));
        if (!max) max = props.Find(Value::U8(KMaxPlayers));
        const Value* count = props.Find(Value::U8(KPlayerCount));
        if (max && count && max->AsInt() > 0 && count->AsInt() >= max->AsInt()) return GameFull;
        return Ok;
    }

    // A JoinRandom filter: every expected property, MaxPlayers included when the client asks for it, must show the same value.
    bool Matches(const Value& props, const Value* expected) {
        if (!expected || expected->type != Type::Hashtable) return true;
        for (const auto& [k, v] : expected->hashtable) {
            const Value* have = props.Find(k);
            if (!have || *have != v) return false;
        }
        return true;
    }

    // A lobby's listing: like Photon's master, rooms are listed only in the lobby they were created in.
    Value VisibleRooms(const std::string& app, const std::string& lobby) {
        Value list = Value::Table();
        for (const Found& f : AllRooms(app))
            if (f.lobby == lobby && IsVisible(f.props) && !list.Find(Value::Str(f.room))) list.Set(Value::Str(f.room), f.props);
        return list;
    }

    // A lobby client's room list. The first is a GameList; after that a GameListUpdate with only what changed, rooms that vanished flagged
    // Removed — the client keeps its own copy and drops a room only when told to.
    void ShowRooms(Session& s) {
        const bool first = s.shownRooms.type != Type::Hashtable;
        // Some games take the first list as the whole list and stop listening, so it waits, within reason, for the registry read in flight.
        if (first && g_reading && GetTickCount() - s.lobbySince < kMatchmakingMs) return;
        Value now = VisibleRooms(s.app, PhotonServer::LobbyOf(s.lobby));
        Value delta = Value::Table();
        for (const auto& [name, props] : now.hashtable) {
            const Value* before = first ? nullptr : s.shownRooms.Find(name);
            if (!before || *before != props) delta.Set(name, props);
        }
        if (!first)
            for (const auto& [name, _] : s.shownRooms.hashtable)
                if (!now.Find(name)) {
                    Value gone = Value::Table();
                    gone.Set(Value::U8(KRemoved), Value::Bool(true));
                    delta.Set(name, std::move(gone));
                }
        if (!first && delta.hashtable.empty()) return;
        Deliver(s, 0, Photon::WriteEvent(first ? EvGameList : EvGameListUpdate, { { PGameList, delta } }));
        s.shownRooms = std::move(now);
    }

    // Where rooms this machine creates live: with the room its clients are already in, so a side room — a voice room beside the game room — lands
    // on the same server, as on Photon Cloud. Empty when no client is in a room.
    std::optional<SteamId> CurrentHost() {
        for (const auto& [_, s] : g_sessions) if (s.inRoom) return s.host;
        return std::nullopt;
    }

    // ── Master: matchmaking ─────────────────────────────────────────
    struct Route { int16_t error = Ok; std::string room; SteamId host = 0; };

    // Where a master room request goes. Rooms on this machine are known at once; anything else needs a finished registry read (`complete`),
    // without which there is no answer yet.
    std::optional<Route> Decide(const Session& s, const Message& m, bool complete) {
        const std::string name = StrParam(m, PRoomName);
        const bool orCreate = IntParam(m, PJoinMode) == JoinModeCreateIfNotExists;
        const SteamId here = CurrentHost().value_or(0);

        if (m.code == OpCreateGame) {
            const std::string room = name.empty() ? NewId() : name;
            if (FindRoom(s.app, room)) return Route{ GameIdAlreadyExists };
            return Route{ Ok, room, here };
        }
        if (m.code == OpJoinGame) {
            std::optional<Found> found = FindRoom(s.app, name);
            if (!found && orCreate && CurrentHost()) return Route{ Ok, name, here };   // that host's server joins or creates it
            if (!found && !complete) return std::nullopt;
            if (!found) return orCreate ? Route{ Ok, name, here } : Route{ GameDoesNotExist };
            if (const int16_t why = Refusal(found->props)) return Route{ why };
            return Route{ Ok, name, found->host };
        }
        // JoinRandomGame
        if (!complete) return std::nullopt;
        const Value* expected = Param(m, PGameProperties);
        // Without a lobby of its own, a random join searches the one the client is in, as on Photon's master.
        const ParameterTable named = NamedLobby(m);
        const std::string lobby = PhotonServer::LobbyOf(named.empty() && s.inLobby ? s.lobby : named);
        for (const Found& f : AllRooms(s.app))
            if (f.lobby == lobby && IsVisible(f.props) && !Refusal(f.props) && Matches(f.props, expected)) return Route{ Ok, f.room, f.host };
        if (!orCreate) return Route{ NoRandomMatchFound };
        return Route{ Ok, name.empty() ? NewId() : name, here };
    }

    const char* ErrorText(int16_t code) {
        switch (code) {
            case GameIdAlreadyExists: return "A game with that name already exists";
            case GameFull:            return "Game full";
            case GameClosed:          return "Game closed";
            case NoRandomMatchFound:  return "No match found";
            case GameDoesNotExist:    return "Game does not exist";
            default:                  return "";
        }
    }

    // Sends the client on to its game server: the room name, the address, and the token that names the room's host.
    void Admit(Session& s, Delivery d, const Message& request, const Route& r) {
        const uint8_t op = request.code;
        PayloadLog::Write(std::string("PhotonEmu: master op ") + std::to_string(op) + " -> "
                          + (r.error ? std::string("error ") + std::to_string(r.error)
                                     : "room '" + r.room + "' on " + (r.host ? std::to_string(r.host) : std::string("this machine"))));
        if (r.error) return Reply(s, d, op, r.error, {}, ErrorText(r.error));
        ParameterTable resp;
        resp[PRoomName] = Value::Str(r.room);
        resp[PAddress]  = Value::Str(kGameServerAddress);
        resp[PToken]    = Value::Str("ofx|G|" + std::to_string(r.host) + "|" + s.app);
        // An unnamed create from inside a lobby goes in that lobby. The game server sees only its own request, so it is told there (GameServerOp).
        if (s.inLobby && !s.lobby.empty() && NamedLobby(request).empty()) g_createdIn[s.app + '\n' + r.room] = s.lobby;
        Reply(s, d, op, Ok, resp);
    }

    // Tries to answer a pending request; true once it is answered (or its client is gone).
    bool Advance(Pending& p, bool complete) {
        auto it = g_sessions.find(p.socket);
        if (it == g_sessions.end()) return true;
        Session& s = it->second;
        if (p.host) {
            if (!g_clock.count(p.host)) return false;
            Admit(s, p.d, p.request, Route{ Ok, p.room, p.host });
            return true;
        }
        std::optional<Route> r = Decide(s, p.request, complete);
        if (!r) return false;
        if (!r->error && r->host && !g_clock.count(r->host)) {
            // First contact with this host: get its clock — which also proves it is reachable — before sending the client there.
            p.host = r->host;
            p.room = r->room;
            SteamRelay::JoinHostLobby(r->host);
            Ping(r->host);
            return false;
        }
        Admit(s, p.d, p.request, *r);
        return true;
    }

    void ResolvePending(bool complete) {
        std::erase_if(g_pending, [&](Pending& p) { return Advance(p, complete); });
    }

    // Past its deadline a request is decided with whatever is known, and a host that never answered counts as gone.
    void ExpirePending() {
        const DWORD now = GetTickCount();
        std::erase_if(g_pending, [&](Pending& p) {
            if ((int32_t)(now - p.deadline) < 0) return false;
            if (p.host) {
                if (auto it = g_sessions.find(p.socket); it != g_sessions.end())
                    Reply(it->second, p.d, p.request.code, GameDoesNotExist, {}, "The room's host is unreachable");
                return true;
            }
            if (Advance(p, true)) return true;
            p.deadline = now + kMatchmakingMs;   // resolved only now: the host's clock reply gets its own time
            return false;
        });
    }

    // One read at a time — unless it has been outstanding so long that Steam evidently dropped part of it.
    void ReadRegistry() {
        if (g_reading && GetTickCount() - g_lastRead < kMatchmakingMs) return;
        g_lastRead = GetTickCount();
        g_reading  = SteamRelay::RequestRegistry();
        if (!g_reading) ResolvePending(true);   // nothing to wait for: answer from what is known
    }

    void Matchmake(Session& s, Delivery d, const Message& m) {
        Pending p{ s.socket, d, m, GetTickCount() + kMatchmakingMs };
        if (Advance(p, false)) return;
        const bool needsRegistry = !p.host;
        g_pending.push_back(std::move(p));
        if (needsRegistry) ReadRegistry();
    }

    // ── Sessions ────────────────────────────────────────────────────
    // "ofx|M|<app>" sends a client to the master, "ofx|G|<host>|<app>" to the game server on <host>. A client hands back the last token it got,
    // so the application — which only the first authentication names — travels with it.
    void ReadToken(Session& s, const std::string& t) {
        if (t.rfind("ofx|G|", 0) == 0) {
            const size_t bar = t.find('|', 6);
            if (bar == std::string::npos) return;
            s.phase = Phase::GameServer;
            s.host  = strtoull(t.c_str() + 6, nullptr, 10);
            s.app   = t.substr(bar + 1);
        } else if (t.rfind("ofx|M|", 0) == 0) {
            s.phase = Phase::Master;
            s.app   = t.substr(6);
        }
    }

    // A token of ours inside a message (with AuthMode AuthOnce the client puts it in its Init); empty when there is none.
    std::string TokenIn(const std::vector<uint8_t>& bytes) {
        static const char kPrefix[] = "ofx|";
        auto it = std::search(bytes.begin(), bytes.end(), kPrefix, kPrefix + 4);
        std::string token;
        for (; it != bytes.end() && *it >= 0x20 && *it < 0x7f; ++it) token.push_back((char)*it);
        return token;
    }

    // The game server this session talks to: ours, or a connection on the room host's.
    void OpenGameServer(Session& s) {
        if (s.peer || s.tunnel) return;
        if (!s.host) {
            EnsureServer();
            s.peer = g_nextPeer++;
            g_localPeers[s.peer] = s.socket;
            return;
        }
        s.tunnel = g_nextTunnel++;
        g_tunnels[s.tunnel] = s.socket;
        SteamRelay::JoinHostLobby(s.host);
        const std::string who = s.app + "\n" + g_userId;
        Tunnel(s.host, FOpen, s.tunnel, {}, std::vector<uint8_t>(who.begin(), who.end()));
    }

    // The client is done with this connection: it disconnected, or reconnected for its next server.
    void Close(Session& s) {
        if (s.peer) {
            PhotonServer::OnDisconnect(s.peer);
            g_localPeers.erase(s.peer);
        }
        if (s.tunnel) {
            Tunnel(s.host, FClose, s.tunnel, {}, {});
            g_tunnels.erase(s.tunnel);
        }
        std::erase_if(g_pending, [&](const Pending& p) { return p.socket == s.socket; });
        s.peer    = 0;
        s.tunnel  = 0;
        s.inRoom  = false;
        s.inLobby = false;
    }

    // Accepts the client. The master address is always included: the client reads it only while on the name server. A token names the server
    // the client goes to next, and after anything but a master room op that is the master — a client leaving a room reconnects there with the
    // last token it got.
    void Authenticated(Session& s, Delivery d, uint8_t op) {
        if (g_userId.empty()) g_userId = NewId();   // Photon assigns one when the client brings none
        ParameterTable resp;
        resp[PUserId]  = Value::Str(g_userId);
        resp[PAddress] = Value::Str(kMasterAddress);
        resp[PToken]   = Value::Str("ofx|M|" + s.app);
        if (s.phase == Phase::GameServer) OpenGameServer(s);
        Reply(s, d, op, Ok, resp);
    }

    // A first authentication names the application; every later one carries the token we issued.
    void Authenticate(Session& s, Delivery d, const Message& m) {
        if (const std::string appId = StrParam(m, PApplicationId); !appId.empty())
            s.app = appId + "/" + StrParam(m, PAppVersion);
        else
            ReadToken(s, StrParam(m, PToken));
        if (const std::string user = StrParam(m, PUserId); !user.empty()) g_userId = user;
        Authenticated(s, d, m.code);
    }

    void MasterOp(Session& s, Delivery d, const Message& m) {
        switch (m.code) {
            case OpGetRegions: {
                // One region, so the client's region list and best-region ping have something to work with.
                ParameterTable resp;
                resp[PRegion]  = Value::Strings({ kRegion });
                resp[PAddress] = Value::Strings({ kMasterAddress });
                return Reply(s, d, OpGetRegions, Ok, resp);
            }
            case OpServerSettings:
                return Reply(s, d, OpServerSettings, Ok);   // lobby statistics on or off: nothing to set here
            case OpJoinLobby:
                Reply(s, d, OpJoinLobby, Ok);
                s.inLobby    = true;
                s.lobby      = NamedLobby(m);
                s.lobbySince = GetTickCount();
                s.shownRooms = Value();
                ReadRegistry();
                ShowRooms(s);
                return;
            case OpLeaveLobby:
                s.inLobby = false;
                return Reply(s, d, OpLeaveLobby, Ok);
            case OpGetGameList:
                return Reply(s, d, OpGetGameList, Ok, { { PGameList, VisibleRooms(s.app, PhotonServer::LobbyOf(m.params)) } });
            case OpCreateGame:
            case OpJoinGame:
            case OpJoinRandomGame:
                return Matchmake(s, d, m);
            default:
                return Reply(s, d, m.code, InvalidOperation, {}, "Operation not supported");
        }
    }

    void GameServerOp(Session& s, Delivery d, const Message& m, const std::vector<uint8_t>& bytes) {
        OpenGameServer(s);
        const auto it = g_createdIn.empty() ? g_createdIn.end() : g_createdIn.find(s.app + '\n' + StrParam(m, PRoomName));
        if (it != g_createdIn.end()) {
            Message named = m;
            named.params.insert(it->second.begin(), it->second.end());
            g_createdIn.erase(it);
            return GameServerOp(s, d, named, Photon::WriteOperationRequest(named.code, named.params));
        }
        if (s.host) Tunnel(s.host, FRequest, s.tunnel, d, bytes);
        else        PhotonServer::OnRequest(s.app, s.peer, g_userId, d, m);
    }

    void OnMessage(Session& s, const std::vector<uint8_t>& bytes, Delivery d) {
        Message m;
        if (!Photon::ParseMessage(bytes.data(), bytes.size(), m)) {
            PayloadLog::Write("PhotonEmu: unreadable message (" + std::to_string(bytes.size()) + " bytes): " + Hex(bytes, 64));
            return;
        }
        switch (m.msgType) {
            case Photon::Init:
                Deliver(s, 0, { 0xF3, Photon::InitResponse, 0x00 });
                // With AuthMode AuthOnce a client authenticates by putting its token in the Init rather than sending Authenticate, then waits
                // for the server to accept it.
                if (const std::string token = TokenIn(bytes); !token.empty()) {
                    ReadToken(s, token);
                    Authenticated(s, d, OpAuthenticate);
                }
                return;
            case Photon::InternalOperationRequest:
                if (m.code == 0) {   // InitEncryption
                    ParameterTable resp;
                    resp[1] = Value::Bytes(std::vector<uint8_t>(kServerKey, kServerKey + sizeof(kServerKey)));
                    Deliver(s, d.channel, Photon::WriteInternalOperationResponse(0, Ok, resp));
                }
                return;
            case Photon::OperationRequest:
                break;
            default:
                return;
        }
        if (m.code != OpRaiseEvent) PayloadLog::Write(std::string("PhotonEmu: ") + PhaseName(s.phase) + " op " + std::to_string(m.code));
        if (m.code == OpAuthenticate || m.code == OpAuthenticateOnce) return Authenticate(s, d, m);
        if (s.phase == Phase::GameServer) GameServerOp(s, d, m, bytes);
        else                              MasterOp(s, d, m);
    }

    // ── ENet: inbound ───────────────────────────────────────────────
    // Fragments carry their offset into the whole message, so they reassemble in any order; it is complete once each has arrived.
    std::optional<std::vector<uint8_t>> Reassemble(Session& s, const PhotonEnet::Command& c) {
        const PhotonEnet::Fragment& f = c.fragment;
        if (!f.count || f.number >= f.count || f.totalLength > kMaxMessage || (uint64_t)f.offset + c.payload.size() > f.totalLength)
            return std::nullopt;
        const uint64_t key = (uint64_t)c.IsUnsequenced() << 32 | f.startSeq;   // the two kinds number their sequences separately
        Partial& p = s.partial[key];
        if (p.got.empty()) {
            p.data.resize(f.totalLength);
            p.got.resize(f.count);
            p.missing = f.count;
        }
        if (p.got.size() != f.count || p.data.size() != f.totalLength) return std::nullopt;
        if (!p.got[f.number]) {
            p.got[f.number] = true;
            --p.missing;
            std::copy(c.payload.begin(), c.payload.end(), p.data.begin() + f.offset);
        }
        if (p.missing) return std::nullopt;
        std::vector<uint8_t> whole = std::move(p.data);
        s.partial.erase(key);
        return whole;
    }

    void OnDatagram(Session& s, const PhotonEnet::Packet& pkt) {
        for (const PhotonEnet::Command& cmd : pkt.commands) {
            switch (cmd.type) {
                case PhotonEnet::Connect: {
                    Close(s);
                    void* socket = s.socket;
                    s = Session{};
                    s.socket    = socket;
                    s.challenge = pkt.challenge;
                    // VerifyConnect echoes the connect parameters with the first two bytes replaced by the peer id we assign.
                    std::vector<uint8_t> verify = cmd.payload;
                    if (verify.size() >= 2) {
                        const uint16_t id = g_nextClientId++;
                        verify[0] = (uint8_t)(id >> 8);
                        verify[1] = (uint8_t)id;
                    }
                    Queue(s, PhotonEnet::VerifyConnect, cmd.channel, std::move(verify));
                    break;
                }
                case PhotonEnet::Disconnect:
                    Close(s);
                    break;
                case PhotonEnet::SendReliable:
                case PhotonEnet::SendUnreliable:
                case PhotonEnet::SendUnsequenced:
                case PhotonEnet::SendReliableUnsequenced:
                    OnMessage(s, cmd.payload, { cmd.channel, cmd.IsReliable() });
                    break;
                case PhotonEnet::SendFragment:
                case PhotonEnet::SendFragmentUnsequenced:
                    if (auto whole = Reassemble(s, cmd)) OnMessage(s, *whole, { cmd.channel, true });
                    break;
                default:
                    break;   // Ping, ServerTime, acks: the ack below is all they need
            }
            if (cmd.IsReliable()) s.outbox.push_back(Ack(cmd, pkt.timestamp));   // after handling: a Connect starts the session over
        }
    }

    // ── Relay ───────────────────────────────────────────────────────
    void DropRemote(SteamId machine, uint16_t conn) {
        auto it = g_remoteConns.find({ machine, conn });
        if (it == g_remoteConns.end()) return;
        PhotonServer::OnDisconnect(it->second.peer);
        g_remotePeers.erase(it->second.peer);
        g_remoteConns.erase(it);
    }

    void OnFrame(SteamId from, Frame kind, uint16_t conn, Delivery d, const uint8_t* body, size_t n) {
        switch (kind) {
            // As the room host:
            case FOpen: {
                const std::string who(body, body + n);
                const size_t nl = who.find('\n');
                RemoteConn& rc = g_remoteConns[{ from, conn }];
                if (!rc.peer) {
                    EnsureServer();
                    rc.peer = g_nextPeer++;
                    g_remotePeers[rc.peer] = { from, conn };
                }
                rc.app    = who.substr(0, nl);
                rc.userId = nl == std::string::npos ? std::to_string(from) : who.substr(nl + 1);
                break;
            }
            case FRequest: {
                auto it = g_remoteConns.find({ from, conn });
                Message m;
                if (it != g_remoteConns.end() && Photon::ParseMessage(body, n, m) && m.msgType == Photon::OperationRequest)
                    PhotonServer::OnRequest(it->second.app, it->second.peer, it->second.userId, d, m);
                break;
            }
            case FClose:
                DropRemote(from, conn);
                break;
            case FPing:
                if (n >= 4) {
                    std::vector<uint8_t> pong(body, body + 4);
                    Put32(pong, GetTickCount());
                    Tunnel(from, FPong, 0, {}, pong);
                }
                break;
            // As a joiner:
            case FPong:
                if (n >= 8) {
                    const DWORD now = GetTickCount();
                    // The host read its clock halfway through the round trip.
                    g_clock[from] = (int32_t)(Get32(body + 4) + (now - Get32(body)) / 2 - now);
                    ResolvePending(false);
                }
                break;
            case FMessage: {
                auto t = g_tunnels.find(conn);
                if (t == g_tunnels.end()) break;
                auto s = g_sessions.find(t->second);
                if (s == g_sessions.end() || s->second.host != from) break;
                const std::vector<uint8_t> msg(body, body + n);
                Track(s->second, msg);
                Deliver(s->second, d.channel, msg);
                break;
            }
        }
    }
}

namespace PhotonEmulator {

    void SetInjector(InjectFn fn) { g_inject = fn; }

    void OnClientDatagram(void* socket, const uint8_t* data, size_t len) {
        if (!data) return;
        PhotonEnet::Packet pkt;
        if (!PhotonEnet::Parse(data, len, pkt)) {
            PayloadLog::Write("PhotonEmu: unreadable datagram (" + std::to_string(len) + " bytes): "
                              + Hex(std::vector<uint8_t>(data, data + std::min<size_t>(len, 64)), 64));
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        Session& s = g_sessions[socket];
        s.socket = socket;
        OnDatagram(s, pkt);
        if (s.inLobby && GetTickCount() - g_lastRead > kRoomListRefreshMs) ReadRegistry();
        if (s.inLobby && s.shownRooms.type != Type::Hashtable) ShowRooms(s);   // a first list still held goes out once its wait is over
        ExpirePending();
        FlushAll();
    }

    void OnRelayMessage(SteamId from, const uint8_t* data, size_t len) {
        if (!data || len < kFrameHeader) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        OnFrame(from, (Frame)data[0], (uint16_t)(data[1] << 8 | data[2]), { data[3], data[4] != 0 }, data + kFrameHeader, len - kFrameHeader);
        ExpirePending();
        FlushAll();
    }

    // A machine is gone. As its host: its clients leave their rooms. As its joiner: our clients in its room lose their server, as on a timeout.
    void OnRelayPeerLeft(SteamId machine) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_remoteConns.begin(); it != g_remoteConns.end();) {
            const auto key = (it++)->first;
            if (key.first == machine) DropRemote(key.first, key.second);
        }
        g_clock.erase(machine);
        for (auto& [_, s] : g_sessions) {
            if (!s.tunnel || s.host != machine) continue;
            Queue(s, PhotonEnet::Disconnect, 0xff, {}).reserved = 2;   // DisconnectByServerTimeout
            g_tunnels.erase(s.tunnel);
            s.tunnel = 0;
            s.inRoom = false;
        }
        FlushAll();
    }

    void OnRegistry(const std::vector<SteamRelay::HostListing>& hosts, bool complete) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_registry.clear();
        for (const SteamRelay::HostListing& h : hosts) {
            Photon::Reader r(h.published.data(), h.published.size());
            Value rooms = r.ReadValue();
            if (r.ok() && rooms.type == Type::Hashtable) g_registry[h.host] = std::move(rooms);
        }
        if (complete) g_reading = false;
        for (auto& [_, s] : g_sessions) if (s.inLobby) ShowRooms(s);
        if (complete) ResolvePending(true);
        ExpirePending();
        FlushAll();
    }

} // namespace PhotonEmulator
