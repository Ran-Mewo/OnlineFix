// End-to-end test of the serverless Photon stack: scripted game clients speak real ENet datagrams to PhotonEmulator through the whole
// NameServer -> Master -> GameServer flow, and SteamRelay is stubbed so "other machines" are scripted too. Every expectation is a behaviour
// the Realtime client depends on (LoadBalancingClient / LoadBalancingPeer), so a failure here is a game that would misbehave.
//   cmake -B build -A x64 -DONLINEFIX_BUILD_TESTS=ON
//   cmake --build build --config Debug --target PhotonEmulatorTest
// Returns 0 on success, 1 on any failed expectation.

#include "PhotonEmulator.h"
#include "PhotonEnet.h"
#include "PhotonProtocol.h"
#include "SteamRelay.h"

#include <crtdbg.h>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

using Photon::Message;
using Photon::ParameterTable;
using Photon::Type;
using Photon::Value;

// ── SteamRelay stub: records what would cross the network ──────────
namespace {
    struct Sent { SteamRelay::SteamId to; std::vector<uint8_t> frame; bool reliable; };
    std::vector<Sent>    g_sent;
    std::vector<uint8_t> g_published;
    int                  g_registryReads = 0;
    std::vector<SteamRelay::SteamId> g_joinedHosts, g_leftHosts;
}
namespace SteamRelay {
    bool Init(const Callbacks&) { return true; }
    void Publish(const std::vector<uint8_t>& blob) { g_published = blob; }
    bool RequestRegistry() { ++g_registryReads; return true; }
    void JoinHostLobby(SteamId host) { g_joinedHosts.push_back(host); }
    void LeaveHostLobby(SteamId host) { g_leftHosts.push_back(host); }
    bool Send(SteamId peer, const uint8_t* data, size_t len, bool reliable) {
        g_sent.push_back({ peer, std::vector<uint8_t>(data, data + len), reliable });
        return true;
    }
}

// ── Test plumbing ───────────────────────────────────────────────────
namespace {
    int g_failures = 0;
    void Expect(bool ok, const std::string& what) {
        if (!ok) { ++g_failures; std::printf("  FAIL: %s\n", what.c_str()); }
    }

    constexpr const char* kApp    = "app-guid";
    constexpr const char* kAppVer = "1.0";
    const std::string     kAppKey = std::string(kApp) + "/" + kAppVer;

    // A game client: one connection at a time, reconnecting for each server like LoadBalancingClient does.
    struct Client {
        std::string          name;
        std::string          token;
        std::vector<Message> inbox;      // every response / event received, in order
        bool                 verified = false;
        bool                 disconnected = false;
        uint8_t              disconnectReason = 0;
        void*                socket = nullptr;   // a fresh SocketUdp per connection
        std::map<uint8_t, uint32_t> seq;

        void Send(std::vector<PhotonEnet::Command> cmds) {
            PhotonEnet::Packet p;
            p.peerId = 1; p.timestamp = 1000; p.challenge = 0x5f78965b;
            p.commands = std::move(cmds);
            const auto bytes = PhotonEnet::Serialize(p);
            PhotonEmulator::OnClientDatagram(socket, bytes.data(), bytes.size());
        }
        PhotonEnet::Command Reliable(uint8_t type, uint8_t channel, std::vector<uint8_t> payload) {
            PhotonEnet::Command c;
            c.type = type; c.channel = channel; c.flags = PhotonEnet::Reliable; c.reliableSeq = ++seq[channel];
            c.payload = std::move(payload);
            return c;
        }
        void Connect(bool nameServer) { Connect(std::string(nameServer ? "NameServer" : kApp)); }
        void Connect(const std::string& initIdentity);
        void Disconnect() { Send({ Reliable(PhotonEnet::Disconnect, 0xff, {}) }); }
        void Op(uint8_t code, const ParameterTable& params, uint8_t channel = 0) {
            Send({ Reliable(PhotonEnet::SendReliable, channel, Photon::WriteOperationRequest(code, params)) });
        }
        void Fragmented(uint8_t code, const ParameterTable& params, size_t chunk) {
            const auto msg = Photon::WriteOperationRequest(code, params);
            const uint32_t count = (uint32_t)((msg.size() + chunk - 1) / chunk);
            const uint32_t start = seq[0] + 1;
            std::vector<PhotonEnet::Command> cmds;
            for (uint32_t i = 0; i < count; ++i) {
                PhotonEnet::Command c = Reliable(PhotonEnet::SendFragment, 0, {});
                const size_t off = i * chunk, n = std::min(chunk, msg.size() - off);
                c.payload.assign(msg.begin() + off, msg.begin() + off + n);
                c.fragment = { start, count, i, (uint32_t)msg.size(), (uint32_t)off };
                cmds.push_back(std::move(c));
            }
            std::swap(cmds.front(), cmds.back());   // out of order on purpose
            Send(std::move(cmds));
        }

        const Message* Last(uint8_t msgType, uint8_t code) const {
            for (auto it = inbox.rbegin(); it != inbox.rend(); ++it)
                if (it->msgType == msgType && it->code == code) return &*it;
            return nullptr;
        }
        const Message* Response(uint8_t op) const { return Last(Photon::OperationResponse, op); }
        const Message* Event(uint8_t code) const { return Last(Photon::Event, code); }
        int Count(uint8_t msgType, uint8_t code) const {
            int n = 0;
            for (const Message& m : inbox) n += (m.msgType == msgType && m.code == code);
            return n;
        }
        void Clear() { inbox.clear(); }

        // NameServer auth, then the master, like ConnectUsingSettings.
        void ToMaster(const std::string& userId) {
            Connect(true);
            Op(230, { { 220, Value::Str(kAppVer) }, { 224, Value::Str(kApp) }, { 225, Value::Str(userId) } });
            const Message* auth = Response(230);
            Expect(auth && auth->returnCode == 0, name + ": name-server auth succeeds");
            Expect(auth && auth->params.count(230), name + ": name-server auth gives the master address");
            if (auth && auth->params.count(221)) token = auth->params.at(221).str;
            Disconnect();
            Connect(false);
            Op(230, { { 221, Value::Str(token) } });
            auth = Response(230);
            Expect(auth && auth->returnCode == 0, name + ": master auth succeeds");
            if (auth && auth->params.count(221)) token = auth->params.at(221).str;
        }
        // A master room op, then the game server with the token it handed out — returns that game server's room-op response.
        const Message* EnterRoom(uint8_t op, const ParameterTable& masterParams, const ParameterTable& gameParams) {
            Op(op, masterParams);
            const Message* r = Response(op);
            if (!r || r->returnCode != 0) return r;
            token = r->params.count(221) ? r->params.at(221).str : token;
            Disconnect();
            Connect(false);
            Clear();
            Op(230, { { 221, Value::Str(token) } });
            const Message* auth = Response(230);
            Expect(auth && auth->returnCode == 0, name + ": game-server auth succeeds");
            if (auth && auth->params.count(221)) token = auth->params.at(221).str;
            Op(op == 227 ? 227 : 226, gameParams);
            return Response(op == 227 ? 227 : 226);
        }
    };

    std::map<void*, Client*> g_owner;
    uintptr_t                g_nextSocket = 0x1000;

    // The Init names the app it connects for: "NameServer" from ConnectToNameServer, the AppId on a fixed-region connect, or with AuthMode
    // AuthOnce, also the token.
    void Client::Connect(const std::string& id) {
        socket = reinterpret_cast<void*>(g_nextSocket += 0x10);
        g_owner[socket] = this;
        seq.clear(); verified = false; disconnected = false;
        Send({ Reliable(PhotonEnet::Connect, 0xff, std::vector<uint8_t>(32, 0)) });
        std::vector<uint8_t> init = { 0xF3, Photon::Init, 0x01, 0x08 };
        init.insert(init.end(), id.begin(), id.end());
        Send({ Reliable(PhotonEnet::SendReliable, 0, init) });
    }

    void Inject(void* socket, const uint8_t* data, size_t len) {
        auto owner = g_owner.find(socket);
        Client* c = owner == g_owner.end() ? nullptr : owner->second;
        PhotonEnet::Packet p;
        if (!c || !PhotonEnet::Parse(data, len, p)) return;
        for (const auto& cmd : p.commands) {
            if (cmd.type == PhotonEnet::VerifyConnect) c->verified = true;
            if (cmd.type == PhotonEnet::Disconnect) { c->disconnected = true; c->disconnectReason = cmd.reserved; }
            if (cmd.type != PhotonEnet::SendReliable) continue;
            Message m;
            if (Photon::ParseMessage(cmd.payload.data(), cmd.payload.size(), m)) c->inbox.push_back(std::move(m));
        }
    }

    Value Table(std::initializer_list<std::pair<Value, Value>> kv) {
        Value t = Value::Table();
        for (const auto& [k, v] : kv) t.Set(k, v);
        return t;
    }
    std::vector<int32_t> Ints(const Value* v) {
        std::vector<int32_t> out;
        if (v && v->type == Type::IntegerArray) for (const Value& n : v->array) out.push_back(n.i32);
        return out;
    }
    const Value* P(const Message* m, uint8_t key) {
        if (!m) return nullptr;
        auto it = m->params.find(key);
        return it == m->params.end() ? nullptr : &it->second;
    }

    std::vector<uint8_t> Frame(uint8_t kind, uint16_t conn, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> f{ kind, (uint8_t)(conn >> 8), (uint8_t)conn, 0, 1 };
        f.insert(f.end(), body.begin(), body.end());
        return f;
    }
    // A copy: later sends reallocate g_sent.
    std::optional<Sent> LastSent(uint8_t kind) {
        for (auto it = g_sent.rbegin(); it != g_sent.rend(); ++it) if (!it->frame.empty() && it->frame[0] == kind) return *it;
        return std::nullopt;
    }

    const Value kRoomProps = Table({
        { Value::U8(255), Value::U8(4) },                // MaxPlayers
        { Value::U8(253), Value::Bool(true) },           // IsOpen
        { Value::U8(254), Value::Bool(true) },           // IsVisible
        { Value::U8(250), Value::Strings({ "L" }) },     // PropsListedInLobby
        { Value::Str("L"), Value::Str("en") },
        { Value::Str("secret"), Value::I32(7) },         // not listed: must stay out of the lobby
    });
    const Value kRoomFlags = Value::I32(0x01 | 0x02 | 0x20);   // CheckUserOnJoin | DeleteCacheOnLeave | BroadcastPropsChangeToAll
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);   // a debug-iterator assertion prints instead of popping a dialog
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    PhotonEmulator::SetInjector(Inject);
    Client a{ "A" }, b{ "B" }, c{ "C" }, d{ "D" }, e{ "E" }, j{ "J" }, k{ "K" }, l{ "L" }, x{ "X" }, v{ "V" }, o{ "O" };

    std::printf("host creates a room\n");
    a.ToMaster("userA");
    Expect(a.verified, "A: Connect is answered with VerifyConnect");
    const Message* created = a.EnterRoom(227, { { 255, Value::Str("room1") } },
        { { 255, Value::Str("room1") }, { 248, kRoomProps }, { 191, kRoomFlags },
          { 249, Table({ { Value::U8(255), Value::Str("Host") } }) }, { 250, Value::Bool(true) } });
    Expect(created && created->returnCode == 0, "A: CreateGame succeeds");
    Expect(P(created, 254) && P(created, 254)->i32 == 1, "A: is actor 1");
    Expect(Ints(P(created, 252)) == std::vector<int32_t>{ 1 }, "A: actor list is [1]");
    const Value* gameProps = P(created, 248);
    Expect(gameProps && gameProps->Find(Value::U8(248)) && gameProps->Find(Value::U8(248))->i32 == 1, "A: room props carry MasterClientId 1");
    Expect(gameProps && gameProps->Find(Value::Str("secret")), "A: room props carry every custom property");
    Expect(P(created, 191) && P(created, 191)->i32 == kRoomFlags.i32, "A: response carries the room flags");
    const Value* players = P(created, 249);
    Expect(players && players->Find(Value::I32(1)), "A: player properties keyed by int actor number");
    Expect(a.Event(255) && P(a.Event(255), 254)->i32 == 1, "A: gets its own Join event");
    Expect(a.token.rfind("ofx|M|", 0) == 0, "A: game-server auth hands back a master token for afterwards");

    std::printf("a fixed-region client (Photon Voice) and an AuthOnce client\n");
    {
        v.Connect(false);   // ConnectUsingSettings with a FixedRegion opens the name server with the AppId, not "NameServer"
        v.Op(230, { { 220, Value::Str(kAppVer) }, { 224, Value::Str("voice-app") }, { 210, Value::Str("us") } });
        const Message* auth = v.Response(230);
        Expect(auth && auth->returnCode == 0 && P(auth, 230) && !P(auth, 230)->str.empty(),
               "V: a name server reached by AppId still gives the master address");
        o.Connect("ofx|M|" + kAppKey);   // AuthOnce: the token rides in the Init and no Authenticate follows
        Expect(o.Response(230) && o.Response(230)->returnCode == 0, "O: a token in the Init is accepted without an Authenticate");
        o.Op(228, {});
        Expect(o.Response(228) && o.Response(228)->returnCode == 0, "O: and the connection is on the master");
    }

    std::printf("the room is published as a lobby listing\n");
    {
        Photon::Reader r(g_published.data(), g_published.size());
        const Value apps = r.ReadValue();
        const Value* lobbies = apps.Find(Value::Str(kAppKey));
        const Value* rooms = lobbies ? lobbies->Find(Value::Str("")) : nullptr;
        const Value* room1 = rooms ? rooms->Find(Value::Str("room1")) : nullptr;
        Expect(r.ok() && room1, "published: room1 under its application, in the default lobby");
        Expect(room1 && room1->Find(Value::U8(252)) && room1->Find(Value::U8(252))->AsInt() == 1, "published: live player count 1");
        Expect(room1 && room1->Find(Value::U8(255)) && room1->Find(Value::U8(255))->u8 == 4, "published: MaxPlayers 4");
        Expect(room1 && room1->Find(Value::Str("L")), "published: listed custom property");
        Expect(room1 && !room1->Find(Value::Str("secret")), "published: unlisted custom property withheld");
    }

    std::printf("properties\n");
    a.Clear();
    a.Op(252, { { 251, Table({ { Value::Str("k"), Value::I32(5) } }) }, { 250, Value::Bool(true) } });
    Expect(a.Response(252) && a.Response(252)->returnCode == 0, "SetProperties is answered");
    const Message* changed = a.Event(253);
    Expect(changed && !P(changed, 253), "BroadcastPropsChangeToAll: room change echoed to the sender, no target actor");
    a.Clear();
    a.Op(252, { { 251, Table({ { Value::Str("ready"), Value::Bool(true) } }) }, { 254, Value::I32(1) }, { 250, Value::Bool(true) } });
    changed = a.Event(253);
    Expect(changed && P(changed, 253) && P(changed, 253)->i32 == 1, "player change names its target in TargetActorNr (253)");
    Expect(changed && P(changed, 254) && P(changed, 254)->i32 == 1, "player change names the sender in ActorNr (254)");
    a.Clear();
    a.Op(252, { { 251, Table({ { Value::Str("k"), Value::I32(9) } }) }, { 231, Table({ { Value::Str("k"), Value::I32(4) } }) },
                { 250, Value::Bool(true) } });
    Expect(a.Response(252) && a.Response(252)->returnCode == -2, "a stale check-and-swap is refused");
    Expect(!a.Event(253), "a refused change is not broadcast");

    std::printf("a second client joins through the master\n");
    b.ToMaster("userB");
    const Message* joined = b.EnterRoom(226, { { 255, Value::Str("room1") } }, { { 255, Value::Str("room1") },
                                         { 249, Table({ { Value::U8(255), Value::Str("Guest") } }) } });
    Expect(joined && joined->returnCode == 0, "B: JoinGame succeeds");
    Expect(P(joined, 254) && P(joined, 254)->i32 == 2, "B: is actor 2");
    Expect(Ints(P(joined, 252)) == std::vector<int32_t>{ 1, 2 }, "B: actor list is [1, 2]");
    const Value* all = P(joined, 249);
    const Value* hostProps = all ? all->Find(Value::I32(1)) : nullptr;
    Expect(hostProps && hostProps->Find(Value::U8(255)) && hostProps->Find(Value::U8(255))->str == "Host",
           "B: learns the existing player's properties (name)");
    Expect(P(joined, 248) && P(joined, 248)->Find(Value::U8(255)), "B: room props carry MaxPlayers for a non-creator");
    Expect(P(joined, 248) && P(joined, 248)->Find(Value::Str("k")) && P(joined, 248)->Find(Value::Str("k"))->i32 == 5,
           "B: SetProperties merged rather than replaced");
    Expect(a.Event(255) && P(a.Event(255), 254)->i32 == 2, "A: told B joined");
    Expect(b.Event(255) && P(b.Event(255), 254)->i32 == 2, "B: gets its own Join event");

    std::printf("events\n");
    a.Clear(); b.Clear();
    a.Op(253, { { 244, Value::U8(7) }, { 245, Table({ { Value::U8(0), Value::I32(1001) } }) }, { 247, Value::U8(4) } });
    Expect(b.Event(7) && P(b.Event(7), 254)->i32 == 1, "RaiseEvent reaches the others, naming the sender");
    Expect(!a.Event(7), "ReceiverGroup Others leaves the sender out");
    Expect(!a.Response(253), "RaiseEvent gets no response");
    a.Op(253, { { 244, Value::U8(8) }, { 246, Value::U8(1) } });
    Expect(a.Event(8) && b.Event(8), "ReceiverGroup All includes the sender");
    b.Clear();
    a.Op(253, { { 244, Value::U8(9) }, { 252, Value::Ints({ 2 }) } });
    Expect(b.Event(9), "target actors receive the event");
    a.Clear(); b.Clear();
    b.Op(248, { { 238, Value::Bytes({ 3 }) } });
    a.Op(253, { { 244, Value::U8(10) }, { 240, Value::U8(3) } });
    Expect(b.Event(10), "an interest-group event reaches the subscriber");
    b.Op(248, { { 239, Value::Bytes({}) } });
    b.Clear();
    a.Op(253, { { 244, Value::U8(11) }, { 240, Value::U8(3) } });
    Expect(!b.Event(11), "an empty Remove unsubscribes from every group");

    std::printf("the event cache replays to late joiners\n");
    c.ToMaster("userC");
    c.EnterRoom(226, { { 255, Value::Str("room1") } }, { { 255, Value::Str("room1") } });
    Expect(c.Event(7) != nullptr, "C: receives the cached event");
    {
        size_t joinAt = SIZE_MAX, cachedAt = SIZE_MAX;
        for (size_t i = 0; i < c.inbox.size(); ++i) {
            if (c.inbox[i].msgType == Photon::Event && c.inbox[i].code == 255 && joinAt == SIZE_MAX) joinAt = i;
            if (c.inbox[i].msgType == Photon::Event && c.inbox[i].code == 7) cachedAt = i;
        }
        Expect(joinAt < cachedAt, "C: the cached event follows its Join event");
    }
    a.Op(253, { { 244, Value::U8(7) }, { 245, Table({ { Value::U8(0), Value::I32(1001) } }) }, { 247, Value::U8(6) } });
    d.ToMaster("userD");
    const Message* full = d.EnterRoom(226, { { 255, Value::Str("room1") } }, { { 255, Value::Str("room1") } });
    Expect(full && full->returnCode == 0, "D: joins (4 of 4)");
    Expect(!d.Event(7), "RemoveFromRoomCache with a data filter removed the cached event");

    std::printf("a full room refuses\n");
    e.ToMaster("userE");
    e.Op(226, { { 255, Value::Str("room1") } });
    Expect(e.Response(226) && e.Response(226)->returnCode == 32765, "E: GameFull from the master");

    std::printf("leaving and disconnecting\n");
    a.Clear(); b.Clear();
    d.Op(254, {});
    Expect(d.Response(254) && d.Response(254)->returnCode == 0, "D: Leave is answered");
    Expect(a.Event(254) && P(a.Event(254), 254)->i32 == 4, "A: told D left");
    Expect(a.Event(254) && !P(a.Event(254), 203), "no MasterClientId when the master is unchanged");
    a.Disconnect();
    const Message* left = b.Event(254);
    Expect(left && P(left, 254)->i32 == 1, "B: a disconnected host leaves like a Leave");
    Expect(left && P(left, 203) && P(left, 203)->i32 == 2, "B: master hands over to the lowest actor");

    std::printf("a client back from a room is on the master again\n");
    d.Disconnect();
    d.Connect(false);
    d.Clear();
    d.Op(230, { { 221, Value::Str(d.token) } });
    d.Op(229, {});
    Expect(d.Response(229) && d.Response(229)->returnCode == 0, "D: JoinLobby after leaving a room is a master op");
    Expect(!d.Event(230), "D: the first GameList waits on the registry read");
    PhotonEmulator::OnRegistry({}, true);

    std::printf("lobby and JoinRandom\n");
    const Message* list = d.Event(230);
    const Value* rooms = P(list, 222);
    Expect(rooms && rooms->Find(Value::Str("room1")), "D: GameList shows room1");
    Expect(rooms && rooms->Find(Value::Str("room1")) && rooms->Find(Value::Str("room1"))->Find(Value::U8(252))->AsInt() == 2,
           "D: with its live player count");
    const int readsBefore = g_registryReads;
    e.Clear();
    e.Op(225, { { 248, Table({ { Value::Str("L"), Value::Str("en") }, { Value::U8(255), Value::U8(4) } }) } });
    Expect(!e.Response(225) && g_registryReads >= readsBefore && g_registryReads > 0, "E: JoinRandom waits on a registry read");
    PhotonEmulator::OnRegistry({}, true);
    Expect(e.Response(225) && e.Response(225)->returnCode == 0 && P(e.Response(225), 255)->str == "room1",
           "E: JoinRandom matches room1 on its listed property and MaxPlayers");
    e.Clear();
    e.Op(225, { { 248, Table({ { Value::Str("L"), Value::Str("fr") } }) } });
    PhotonEmulator::OnRegistry({}, true);
    Expect(e.Response(225) && e.Response(225)->returnCode == 32760, "E: no match is NoRandomMatchFound");
    e.Clear();
    e.Op(225, { { 248, Table({ { Value::Str("L"), Value::Str("fr") } }) }, { 215, Value::U8(1) } });
    PhotonEmulator::OnRegistry({}, true);
    Expect(e.Response(225) && e.Response(225)->returnCode == 0 && !P(e.Response(225), 255)->str.empty(),
           "E: JoinRandomOrCreate falls back to a new room");
    e.Clear();
    e.Op(226, { { 255, Value::Str("nope") } });
    PhotonEmulator::OnRegistry({}, true);
    Expect(e.Response(226) && e.Response(226)->returnCode == 32758, "E: joining an unknown room is GameDoesNotExist");
    e.Clear();
    e.Op(229, {});
    PhotonEmulator::OnRegistry({}, true);   // its first list
    e.Op(225, { { 248, Table({ { Value::Str("L"), Value::Str("en") } }) } });
    Expect(!e.Response(225), "E: still waiting on the registry");
    e.Clear();
    b.Op(252, { { 251, Table({ { Value::U8(254), Value::Bool(false) } }) }, { 250, Value::Bool(true) } });   // room1 turns invisible
    const Message* update = e.Event(229);
    Expect(update && P(update, 222) && P(update, 222)->Find(Value::Str("room1"))
           && P(update, 222)->Find(Value::Str("room1"))->Find(Value::U8(251)), "E: an invisible room is removed from the lobby list");
    PhotonEmulator::OnRegistry({}, true);
    Expect(e.Response(225) && e.Response(225)->returnCode == 32760, "E: JoinRandom skips invisible rooms");

    std::printf("a room on another machine\n");
    {
        Value listing = Table({ { Value::Str(kAppKey), Table({ { Value::Str(""), Table({ { Value::Str("far"), Table({ { Value::U8(252), Value::U8(1) } }) } }) } }) } });
        Photon::Writer w;
        w.WriteValue(listing);
        j.ToMaster("userJ");
        j.Clear();
        g_sent.clear();
        j.Op(226, { { 255, Value::Str("far") } });
        PhotonEmulator::OnRegistry({ { 77, w.buffer() } }, true);
        const auto ping = LastSent(5);   // FPing
        Expect(ping && ping->to == 77, "J: pings the room's host before sending the client there");
        Expect(!j.Response(226), "J: no answer before the host replies");
        if (!ping) return 1;
        std::vector<uint8_t> pong(ping->frame.begin() + 5, ping->frame.begin() + 9);   // our tick, echoed
        pong.insert(pong.end(), { 0, 0, 0x10, 0 });                                    // the host's tick
        const auto pongFrame = Frame(6, 0, pong);
        PhotonEmulator::OnRelayMessage(77, pongFrame.data(), pongFrame.size());
        const Message* routed = j.Response(226);
        Expect(routed && routed->returnCode == 0 && P(routed, 221) && P(routed, 221)->str.rfind("ofx|G|77|", 0) == 0, "J: routed to host 77");
        if (P(routed, 221)) j.token = P(routed, 221)->str;
        j.Disconnect();
        j.Connect(false);
        j.Clear();
        g_sent.clear();
        j.Op(230, { { 221, Value::Str(j.token) } });
        const auto open = LastSent(1);   // FOpen
        Expect(open && open->to == 77 && std::string(open->frame.begin() + 5, open->frame.end()) == kAppKey + "\nuserJ",
               "J: opens a connection on the host with its app and user id");
        Expect(!g_joinedHosts.empty() && g_joinedHosts.back() == 77, "J: joins the host's registry and Steam lobbies when it connects there");
        j.Op(226, { { 255, Value::Str("far") } });
        const auto req = LastSent(2);    // FRequest
        Message tunnelled;
        Expect(req && req->to == 77 && Photon::ParseMessage(req->frame.data() + 5, req->frame.size() - 5, tunnelled) && tunnelled.code == 226,
               "J: the room op is tunnelled to the host");
        const uint16_t conn = open ? (uint16_t)(open->frame[1] << 8 | open->frame[2]) : 0;
        const auto replyFrame = Frame(4, conn, Photon::WriteOperationResponse(226, 0, { { 254, Value::I32(2) } }));
        PhotonEmulator::OnRelayMessage(77, replyFrame.data(), replyFrame.size());
        Expect(j.Response(226) && j.Response(226)->returnCode == 0, "J: the host's reply reaches the client");
        Expect(g_leftHosts.empty(), "J: stays in the host's Steam lobby while it plays there");
        j.Disconnect();
        Expect(g_leftHosts == std::vector<SteamRelay::SteamId>{ 77 }, "J: leaves the host's Steam lobby with its last connection there");
        j.Connect(false);
        j.Op(230, { { 221, Value::Str(j.token) } });
        g_leftHosts.clear();
        PhotonEmulator::OnRelayPeerLeft(77);
        Expect(j.disconnected && j.disconnectReason == 2, "J: losing the host is a server timeout to the client");
        Expect(g_leftHosts == std::vector<SteamRelay::SteamId>{ 77 }, "J: leaves the Steam lobby of a host that is gone");
    }

    std::printf("rooms are listed only in their own lobby\n");
    {
        const Value four = Table({ { Value::U8(255), Value::U8(4) } });
        const ParameterTable custom = { { 213, Value::Str("custom") }, { 212, Value::U8(0) } };
        auto plus = [](ParameterTable t, const ParameterTable& more) { t.insert(more.begin(), more.end()); return t; };
        // PUN names the lobby to the master and the game server alike.
        k.ToMaster("userK");
        const Message* pub = k.EnterRoom(227, plus(custom, { { 255, Value::Str("pub") } }), plus(custom, { { 255, Value::Str("pub") }, { 248, four } }));
        Expect(pub && pub->returnCode == 0, "K: creates a room in lobby 'custom'");
        // A Realtime client inside a lobby creates without naming one: Photon's master puts the room in that lobby.
        l.ToMaster("userL");
        l.Op(229, { { 213, Value::Str("browse") }, { 212, Value::U8(0) } });
        PhotonEmulator::OnRegistry({}, true);
        const Message* mine = l.EnterRoom(227, { { 255, Value::Str("mine") } }, { { 255, Value::Str("mine") }, { 248, four } });
        Expect(mine && mine->returnCode == 0, "L: creates a room without naming a lobby");
        Photon::Reader r(g_published.data(), g_published.size());
        const Value apps = r.ReadValue();
        auto published = [&](const char* lobby, const char* room) {
            const Value* lobbies = apps.Find(Value::Str(kAppKey));
            const Value* rooms = lobbies ? lobbies->Find(Value::Str(lobby)) : nullptr;
            return rooms && rooms->Find(Value::Str(room));
        };
        Expect(published("custom/0", "pub"), "published: a room under the lobby it was created in");
        Expect(published("browse/0", "mine") && !published("", "mine"), "published: an unnamed create goes in its client's lobby");

        // Another machine's rooms, in two lobbies.
        Value listing = Table({ { Value::Str(kAppKey), Table({ { Value::Str(""), Table({ { Value::Str("plain"), four } }) },
                                                                { Value::Str("custom/0"), Table({ { Value::Str("public"), four } }) } }) } });
        Photon::Writer w;
        w.WriteValue(listing);
        x.ToMaster("userX");
        x.Clear();
        x.Op(229, custom);
        PhotonEmulator::OnRegistry({ { 88, w.buffer() } }, true);
        const Value* named = P(x.Event(230), 222);
        Expect(named && named->Find(Value::Str("public")) && named->Find(Value::Str("pub")) && !named->Find(Value::Str("plain")),
               "X: a named lobby lists its own rooms, here and elsewhere, and no others");
        x.Clear();
        x.Op(229, {});
        PhotonEmulator::OnRegistry({ { 88, w.buffer() } }, true);
        const Value* standard = P(x.Event(230), 222);
        Expect(standard && standard->Find(Value::Str("plain")) && !standard->Find(Value::Str("public")) && !standard->Find(Value::Str("pub")),
               "X: the default lobby doesn't list a named lobby's rooms");
        x.Clear();
        x.Op(225, custom);
        PhotonEmulator::OnRegistry({ { 88, w.buffer() } }, true);
        Expect(x.Response(225) && x.Response(225)->returnCode == 0 && P(x.Response(225), 255)->str == "pub",
               "X: JoinRandom finds a room inside its lobby");
        x.Clear();
        x.Op(225, { { 213, Value::Str("other") }, { 212, Value::U8(0) } });
        PhotonEmulator::OnRegistry({ { 88, w.buffer() } }, true);
        Expect(x.Response(225) && x.Response(225)->returnCode == 32760, "X: JoinRandom stays within its lobby");
    }

    std::printf("a client of another machine in our room\n");
    {
        b.Clear();
        g_sent.clear();
        const std::string who = kAppKey + "\nuserR";
        const auto openFrame = Frame(1, 9, std::vector<uint8_t>(who.begin(), who.end()));
        PhotonEmulator::OnRelayMessage(55, openFrame.data(), openFrame.size());
        const auto joinFrame = Frame(2, 9, Photon::WriteOperationRequest(226, { { 255, Value::Str("room1") } }));
        PhotonEmulator::OnRelayMessage(55, joinFrame.data(), joinFrame.size());
        Message m;
        bool answered = false;
        for (const Sent& sent : g_sent)
            if (sent.to == 55 && sent.frame[0] == 4 && Photon::ParseMessage(sent.frame.data() + 5, sent.frame.size() - 5, m)
                && m.msgType == Photon::OperationResponse && m.code == 226)
                answered = m.returnCode == 0;
        Expect(answered, "remote: the join is answered over the relay");
        Expect(b.Event(255) != nullptr, "B: told a remote player joined");
        b.Clear();
        PhotonEmulator::OnRelayPeerLeft(55);
        Expect(b.Event(254) != nullptr, "B: a remote player whose machine is gone leaves the room");
    }

    std::printf("fragmented operations\n");
    {
        c.Clear();
        std::vector<uint8_t> big(3000);
        for (size_t i = 0; i < big.size(); ++i) big[i] = (uint8_t)i;
        b.Fragmented(253, { { 244, Value::U8(20) }, { 245, Value::Bytes(big) } }, 1000);
        const Message* ev = c.Event(20);
        Expect(ev && P(ev, 245) && P(ev, 245)->bytes == big, "a fragmented RaiseEvent arrives whole");
    }

    std::printf("%s (%d failed)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
