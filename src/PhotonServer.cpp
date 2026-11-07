#include "PhotonServer.h"
#include "PayloadLog.h"

#include <algorithm>
#include <map>
#include <set>

// ─────────────────────────────────────────────────────────────────
//  Room semantics, matched to what the Realtime client reads back (LoadBalancingClient.GameEnteredOnGameServer, OnEvent, ReadoutProperties) and to
//  Photon's documented server behaviour.
// ─────────────────────────────────────────────────────────────────

namespace {
    using Photon::Message;
    using Photon::ParameterTable;
    using Photon::Type;
    using Photon::Value;
    using PhotonServer::Delivery;
    using PhotonServer::PeerId;

    // ── Photon LoadBalancing constants ──────────────────────────────
    enum Op : uint8_t {
        OpLeave = 254, OpRaiseEvent = 253, OpSetProperties = 252, OpGetProperties = 251, OpChangeGroups = 248,
        OpCreateGame = 227, OpJoinGame = 226, OpJoinRandomGame = 225,
    };
    enum Event : uint8_t { EvJoin = 255, EvLeave = 254, EvPropertiesChanged = 253 };
    enum Param : uint8_t {
        PRoomName = 255, PActorNr = 254, PTargetActorNr = 253, PActorList = 252, PProperties = 251, PBroadcast = 250,
        PPlayerProperties = 249, PGameProperties = 248, PCache = 247, PReceiverGroup = 246, PData = 245, PCode = 244,
        PGroup = 240, PRemoveGroups = 239, PAddGroups = 238, PExpectedValues = 231, PJoinMode = 215, PLobbyName = 213, PLobbyType = 212,
        PMasterClientId = 203, PRoomOptionFlags = 191,
        // Clients older than RoomOptionFlags send the same options as separate booleans.
        PCleanupCacheOnLeave = 241, PSuppressRoomEvents = 237, PPublishUserId = 239,
    };
    enum RoomKey : uint8_t {   // GamePropertyKey
        KMaxPlayers = 255, KIsVisible = 254, KIsOpen = 253, KPlayerCount = 252, KLobbyProps = 250, KMasterClientId = 248,
        KMaxPlayersInt = 243,
    };
    constexpr uint8_t KActorUserId = 253;   // ActorProperties.UserId
    enum Error : int16_t {
        Ok = 0, InvalidOperation = -2, NotAllowedInState = -3,
        GameIdAlreadyExists = 32766, GameFull = 32765, GameClosed = 32764, GameDoesNotExist = 32758,
    };
    enum Flag : int32_t {   // RoomOptionFlags
        DeleteCacheOnLeave = 0x02, SuppressRoomEvents = 0x04, PublishUserId = 0x08, DeleteNullProps = 0x10,
        BroadcastPropsChangeToAll = 0x20, SuppressPlayerInfo = 0x40,
    };
    enum Receivers : int32_t { ReceiverOthers = 0, ReceiverAll = 1, ReceiverMaster = 2 };
    enum Caching : int32_t {
        AddToRoomCache = 4, AddToRoomCacheGlobal = 5, RemoveFromRoomCache = 6, RemoveFromRoomCacheForActorsLeft = 7,
    };
    constexpr int32_t JoinModeCreateIfNotExists = 1;

    // ── State ───────────────────────────────────────────────────────
    struct Actor {
        PeerId            peer = 0;
        Value             props = Value::Table();
        std::set<uint8_t> groups;   // interest groups it receives
    };

    struct CachedEvent {
        uint8_t        code  = 0;
        int32_t        owner = 0;   // 0: room-global, outlives its sender
        ParameterTable event;
    };

    struct Room {
        Value                    props = Value::Table();   // custom and well-known keys, as the creator sent them
        std::string              lobby;                     // the lobby it is listed in (LobbyOf)
        int32_t                  flags = 0;
        std::map<int32_t, Actor> actors;                    // by actor number, so begin() is the lowest
        int32_t                  nextActor = 1;
        std::vector<CachedEvent> cache;

        const Value* Prop(uint8_t key) const { return props.Find(Value::U8(key)); }
        int32_t Master() const { const Value* m = Prop(KMasterClientId); return m ? m->AsInt() : 0; }
        void    SetMaster(int32_t nr) { props.Set(Value::U8(KMasterClientId), Value::I32(nr)); }
        bool    IsOpen() const { const Value* v = Prop(KIsOpen); return !v || v->type != Type::Boolean || v->b; }
        bool    IsFull() const {
            const Value* v = Prop(KMaxPlayersInt);   // newer clients add an int beside the byte for rooms over 255
            if (!v) v = Prop(KMaxPlayers);
            const int32_t max = v ? v->AsInt() : 0;   // 0: unlimited
            return max > 0 && (int32_t)actors.size() >= max;
        }
    };

    using App = std::map<std::string, Room>;   // one application's rooms, by name

    struct Seat {   // a connection sits in at most one room
        std::string app, room;
        int32_t     actor = 0;
    };

    std::map<std::string, App> g_apps;
    std::map<PeerId, Seat>     g_seats;
    PhotonServer::SendFn       g_send;
    PhotonServer::ChangedFn    g_changed;

    // ── Helpers ─────────────────────────────────────────────────────
    const Value* Param(const Message& m, uint8_t key) {
        auto it = m.params.find(key);
        return it == m.params.end() ? nullptr : &it->second;
    }
    int32_t IntParam(const Message& m, uint8_t key) {
        const Value* v = Param(m, key);
        return v ? v->AsInt() : 0;
    }
    bool BoolParam(const Message& m, uint8_t key, bool fallback = false) {
        const Value* v = Param(m, key);
        return (v && v->type == Type::Boolean) ? v->b : fallback;
    }
    const Value* TableParam(const Message& m, uint8_t key) {
        const Value* v = Param(m, key);
        return (v && v->type == Type::Hashtable) ? v : nullptr;
    }

    void Respond(PeerId to, Delivery d, uint8_t op, int16_t code, const ParameterTable& params = {}, const std::string& debug = {}) {
        if (g_send) g_send(to, { d.channel, true }, Photon::WriteOperationResponse(op, code, params, debug));
    }
    void Emit(const Actor& to, Delivery d, uint8_t code, const ParameterTable& event) {
        if (g_send) g_send(to.peer, d, Photon::WriteEvent(code, event));
    }
    void Changed() { if (g_changed) g_changed(); }

    Value ActorList(const Room& room) {
        std::vector<int32_t> nrs;
        for (const auto& [nr, a] : room.actors) nrs.push_back(nr);
        return Value::Ints(nrs);
    }

    // Every player's properties, keyed by actor number — the client casts each key to int.
    Value PlayerProps(const Room& room) {
        Value all = Value::Table();
        for (const auto& [nr, a] : room.actors) all.Set(Value::I32(nr), a.props);
        return all;
    }

    Room* SeatOf(PeerId peer, int32_t& actor) {
        auto s = g_seats.find(peer);
        if (s == g_seats.end()) return nullptr;
        auto app = g_apps.find(s->second.app);
        if (app == g_apps.end()) return nullptr;
        auto room = app->second.find(s->second.room);
        if (room == app->second.end()) return nullptr;
        actor = s->second.actor;
        return &room->second;
    }

    // ── Entering a room ─────────────────────────────────────────────
    int32_t FlagsOf(const Message& m) {
        if (Param(m, PRoomOptionFlags)) return IntParam(m, PRoomOptionFlags);
        int32_t f = 0;
        if (BoolParam(m, PCleanupCacheOnLeave, true)) f |= DeleteCacheOnLeave;
        if (BoolParam(m, PSuppressRoomEvents))         f |= SuppressRoomEvents;
        if (BoolParam(m, PPublishUserId))              f |= PublishUserId;
        return f;
    }

    void Join(const Seat& where, Room& room, PeerId peer, const std::string& userId, Delivery d, const Message& m) {
        const int32_t nr = room.nextActor++;
        Actor& actor = room.actors[nr];
        actor.peer = peer;
        if (const Value* p = TableParam(m, PPlayerProperties)) actor.props = *p;
        if (room.flags & PublishUserId) actor.props.Set(Value::U8(KActorUserId), Value::Str(userId));
        g_seats[peer] = { where.app, where.room, nr };
        if (!room.Master()) room.SetMaster(nr);

        const bool playerInfo = !(room.flags & SuppressPlayerInfo);
        ParameterTable resp;
        resp[PActorNr]         = Value::I32(nr);
        resp[PActorList]       = ActorList(room);
        resp[PGameProperties]  = room.props;
        resp[PRoomOptionFlags] = Value::I32(room.flags);
        if (playerInfo) resp[PPlayerProperties] = PlayerProps(room);
        Respond(peer, d, m.code, Ok, resp);

        // The joiner gets its own Join event too: that is what completes the join on its side, unless room events are suppressed.
        if (!(room.flags & SuppressRoomEvents)) {
            ParameterTable ev;
            ev[PActorNr]   = Value::I32(nr);
            ev[PActorList] = ActorList(room);
            if (playerInfo) ev[PPlayerProperties] = actor.props;
            for (const auto& [_, a] : room.actors) Emit(a, { d.channel, true }, EvJoin, ev);
        }
        for (const CachedEvent& c : room.cache) Emit(actor, { d.channel, true }, c.code, c.event);

        PayloadLog::Write("PhotonServer: actor " + std::to_string(nr) + " joined '" + where.room + "' ("
                          + std::to_string(room.actors.size()) + " in room)");
        Changed();
    }

    void EnterRoom(const std::string& app, PeerId peer, const std::string& userId, Delivery d, const Message& m) {
        int32_t seated = 0;
        if (SeatOf(peer, seated)) return Respond(peer, d, m.code, NotAllowedInState, {}, "Already in a room");
        const Value* nameParam = Param(m, PRoomName);
        const std::string name = (nameParam && nameParam->type == Type::String) ? nameParam->str : "";
        if (name.empty()) return Respond(peer, d, m.code, InvalidOperation, {}, "No room name");   // the master always assigns one

        App& rooms = g_apps[app];
        auto it = rooms.find(name);
        if (it != rooms.end()) {
            if (m.code == OpCreateGame)  return Respond(peer, d, m.code, GameIdAlreadyExists, {}, "A game with that name already exists");
            if (!it->second.IsOpen())    return Respond(peer, d, m.code, GameClosed, {}, "Game closed");
            if (it->second.IsFull())     return Respond(peer, d, m.code, GameFull, {}, "Game full");
            return Join({ app, name }, it->second, peer, userId, d, m);
        }
        if (m.code != OpCreateGame && IntParam(m, PJoinMode) != JoinModeCreateIfNotExists)
            return Respond(peer, d, m.code, GameDoesNotExist, {}, "Game does not exist");

        Room& room = rooms[name];
        if (const Value* p = TableParam(m, PGameProperties)) room.props = *p;
        room.lobby = PhotonServer::LobbyOf(m.params);
        room.flags = FlagsOf(m);
        Join({ app, name }, room, peer, userId, d, m);
    }

    // ── Leaving ─────────────────────────────────────────────────────
    void Leave(PeerId peer, Delivery d, bool respond) {
        auto seat = g_seats.find(peer);
        if (seat == g_seats.end()) {
            if (respond) Respond(peer, d, OpLeave, NotAllowedInState, {}, "Not in a room");
            return;
        }
        const Seat s = seat->second;
        g_seats.erase(seat);
        if (respond) Respond(peer, d, OpLeave, Ok);   // the client goes back to the master once it has this

        App& rooms = g_apps[s.app];
        auto it = rooms.find(s.room);
        if (it == rooms.end()) return;
        Room& room = it->second;
        room.actors.erase(s.actor);
        if (room.flags & DeleteCacheOnLeave)
            std::erase_if(room.cache, [&](const CachedEvent& c) { return c.owner == s.actor; });
        PayloadLog::Write("PhotonServer: actor " + std::to_string(s.actor) + " left '" + s.room + "'");

        if (room.actors.empty()) {
            rooms.erase(it);
        } else {
            const bool masterLeft = room.Master() == s.actor;
            if (masterLeft) room.SetMaster(room.actors.begin()->first);   // the lowest remaining actor number
            if (!(room.flags & SuppressRoomEvents)) {
                ParameterTable ev;
                ev[PActorNr]   = Value::I32(s.actor);
                ev[PActorList] = ActorList(room);
                if (masterLeft) ev[PMasterClientId] = Value::I32(room.Master());   // only on a change: the client reports it as a switch
                for (const auto& [_, a] : room.actors) Emit(a, { d.channel, true }, EvLeave, ev);
            }
        }
        Changed();
    }

    // ── In-room operations ──────────────────────────────────────────
    // RemoveFromRoomCache filter: code 0 matches any event, target actors select whose events, and a data hashtable matches events whose own data
    // hashtable holds every one of its entries.
    bool CacheMatches(const CachedEvent& c, const Message& m) {
        if (const int32_t code = IntParam(m, PCode); code && c.code != code) return false;
        if (const Value* owners = Param(m, PActorList); owners && owners->type == Type::IntegerArray
            && std::none_of(owners->array.begin(), owners->array.end(), [&](const Value& n) { return n.i32 == c.owner; }))
            return false;
        if (const Value* filter = TableParam(m, PData)) {
            auto data = c.event.find(PData);
            if (data == c.event.end() || data->second.type != Type::Hashtable) return false;
            for (const auto& [k, v] : filter->hashtable) {
                const Value* have = data->second.Find(k);
                if (!have || *have != v) return false;
            }
        }
        return true;
    }

    // Explicit target actors win; then an interest group (0 is everyone); then the receiver group.
    std::vector<const Actor*> Recipients(const Room& room, int32_t sender, const Message& m) {
        std::vector<const Actor*> out;
        if (const Value* targets = Param(m, PActorList); targets && targets->type == Type::IntegerArray) {
            for (const Value& n : targets->array)
                if (auto a = room.actors.find(n.i32); a != room.actors.end()) out.push_back(&a->second);
            return out;
        }
        if (const int32_t group = IntParam(m, PGroup)) {
            for (const auto& [nr, a] : room.actors)
                if (nr != sender && a.groups.count((uint8_t)group)) out.push_back(&a);
            return out;
        }
        const int32_t receivers = IntParam(m, PReceiverGroup);
        if (receivers == ReceiverMaster) {
            if (auto a = room.actors.find(room.Master()); a != room.actors.end()) out.push_back(&a->second);
            return out;
        }
        for (const auto& [nr, a] : room.actors)
            if (receivers == ReceiverAll || nr != sender) out.push_back(&a);
        return out;
    }

    void RaiseEvent(PeerId peer, Delivery d, const Message& m) {
        int32_t sender = 0;
        Room* room = SeatOf(peer, sender);
        if (!room) return Respond(peer, d, OpRaiseEvent, NotAllowedInState, {}, "Not in a room");

        const int32_t caching = IntParam(m, PCache);
        if (caching == RemoveFromRoomCache) {
            std::erase_if(room->cache, [&](const CachedEvent& c) { return CacheMatches(c, m); });
            return;
        }
        if (caching == RemoveFromRoomCacheForActorsLeft) {
            std::erase_if(room->cache, [&](const CachedEvent& c) { return c.owner && !room->actors.count(c.owner); });
            return;
        }

        const uint8_t code = (uint8_t)IntParam(m, PCode);
        ParameterTable ev;
        ev[PActorNr] = Value::I32(sender);
        if (const Value* data = Param(m, PData)) ev[PData] = *data;
        if (caching == AddToRoomCache || caching == AddToRoomCacheGlobal)
            room->cache.push_back({ code, caching == AddToRoomCache ? sender : 0, ev });
        for (const Actor* to : Recipients(*room, sender, m)) Emit(*to, d, code, ev);
    }

    void SetProperties(PeerId peer, Delivery d, const Message& m) {
        int32_t sender = 0;
        Room* room = SeatOf(peer, sender);
        if (!room) return Respond(peer, d, OpSetProperties, NotAllowedInState, {}, "Not in a room");
        const Value* props = TableParam(m, PProperties);
        if (!props) return Respond(peer, d, OpSetProperties, InvalidOperation, {}, "No properties");

        const int32_t target = IntParam(m, PActorNr);   // 0: the room
        Value* table = &room->props;
        if (target) {
            auto a = room->actors.find(target);
            if (a == room->actors.end()) return Respond(peer, d, OpSetProperties, InvalidOperation, {}, "No such actor");
            table = &a->second.props;
        }

        // Check-and-swap: every expected value must still be current, an expected null meaning "not set".
        const Value* expected = TableParam(m, PExpectedValues);
        const bool checked = expected && !expected->hashtable.empty();
        if (checked)
            for (const auto& [k, v] : expected->hashtable) {
                const Value* have = table->Find(k);
                if (have ? *have != v : v.type != Type::Null)
                    return Respond(peer, d, OpSetProperties, InvalidOperation, {}, "CAS update failed");
            }
        for (const auto& [k, v] : props->hashtable) {
            if (v.type == Type::Null && (room->flags & DeleteNullProps)) table->Erase(k);
            else table->Set(k, v);
        }
        Respond(peer, d, OpSetProperties, Ok);

        // A client applies its own change only once it comes back as this event when the room broadcasts changes to all, or when it used
        // check-and-swap, so it must reach the sender too in those cases.
        if (BoolParam(m, PBroadcast)) {
            ParameterTable ev;
            ev[PProperties] = *props;
            ev[PActorNr]    = Value::I32(sender);
            if (target) ev[PTargetActorNr] = Value::I32(target);
            const bool echo = checked || (room->flags & BroadcastPropsChangeToAll);
            for (const auto& [nr, a] : room->actors)
                if (echo || nr != sender) Emit(a, { d.channel, true }, EvPropertiesChanged, ev);
        }
        if (!target) Changed();
    }

    void GetProperties(PeerId peer, Delivery d) {
        int32_t sender = 0;
        Room* room = SeatOf(peer, sender);
        if (!room) return Respond(peer, d, OpGetProperties, NotAllowedInState, {}, "Not in a room");
        ParameterTable resp;
        resp[PGameProperties]   = room->props;
        resp[PPlayerProperties] = PlayerProps(*room);
        Respond(peer, d, OpGetProperties, Ok, resp);
    }

    // A null array leaves the set alone; an empty one means every group (for Add: every group in use in the room).
    void ChangeGroups(PeerId peer, const Message& m) {
        int32_t nr = 0;
        Room* room = SeatOf(peer, nr);
        if (!room) return;
        std::set<uint8_t>& groups = room->actors[nr].groups;
        if (const Value* rm = Param(m, PRemoveGroups); rm && rm->type == Type::ByteArray) {
            if (rm->bytes.empty()) groups.clear();
            for (uint8_t g : rm->bytes) groups.erase(g);
        }
        if (const Value* add = Param(m, PAddGroups); add && add->type == Type::ByteArray) {
            if (add->bytes.empty())
                for (const auto& [other, a] : room->actors)
                    if (other != nr) groups.insert(a.groups.begin(), a.groups.end());
            groups.insert(add->bytes.begin(), add->bytes.end());
        }
    }

    // What a master server shows for a room in its lobby: the well-known keys the client reads (RoomInfo.InternalCacheProperties), the live
    // player count, and the custom properties the creator listed for the lobby.
    Value LobbyProps(const Room& room) {
        Value out = Value::Table();
        for (uint8_t key : { KMaxPlayers, KMaxPlayersInt, KIsOpen, KIsVisible })
            if (const Value* v = room.Prop(key)) out.Set(Value::U8(key), *v);
        out.Set(Value::U8(KPlayerCount), Value::U8((uint8_t)std::min<size_t>(room.actors.size(), 255)));
        if (const Value* listed = room.Prop(KLobbyProps); listed && listed->type == Type::StringArray)
            for (const Value& name : listed->array)
                if (const Value* v = room.props.Find(name)) out.Set(name, *v);
        return out;
    }
}

namespace PhotonServer {

    void Init(SendFn send, ChangedFn roomsChanged) {
        g_send    = std::move(send);
        g_changed = std::move(roomsChanged);
    }

    void OnRequest(const std::string& app, PeerId peer, const std::string& userId, Delivery d, const Message& m) {
        switch (m.code) {
            case OpCreateGame:
            case OpJoinGame:
            case OpJoinRandomGame: EnterRoom(app, peer, userId, d, m); break;
            case OpLeave:          Leave(peer, d, true); break;
            case OpRaiseEvent:     RaiseEvent(peer, d, m); break;
            case OpSetProperties:  SetProperties(peer, d, m); break;
            case OpGetProperties:  GetProperties(peer, d); break;
            case OpChangeGroups:   ChangeGroups(peer, m); break;
            default:
                Respond(peer, d, m.code, InvalidOperation, {}, "Operation " + std::to_string(m.code) + " is not supported");
                PayloadLog::Write("PhotonServer: unsupported op " + std::to_string(m.code));
                break;
        }
    }

    void OnDisconnect(PeerId peer) { Leave(peer, {}, false); }

    std::string LobbyOf(const ParameterTable& params) {
        const auto name = params.find(PLobbyName), type = params.find(PLobbyType);
        if (name == params.end() || name->second.type != Type::String || name->second.str.empty()) return {};
        return name->second.str + "/" + std::to_string(type == params.end() ? 0 : type->second.AsInt());
    }

    Value Rooms() {
        Value apps = Value::Table();
        for (const auto& [app, rooms] : g_apps) {
            if (rooms.empty()) continue;
            Value lobbies = Value::Table();
            for (const auto& [name, room] : rooms) {
                const Value lobby = Value::Str(room.lobby);
                if (!lobbies.Find(lobby)) lobbies.Set(lobby, Value::Table());
                lobbies.Find(lobby)->Set(Value::Str(name), LobbyProps(room));
            }
            apps.Set(Value::Str(app), std::move(lobbies));
        }
        return apps;
    }

} // namespace PhotonServer
