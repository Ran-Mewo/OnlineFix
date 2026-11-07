#include "PhotonBridge.h"
#include "MonoBridge.h"
#include "PhotonEmulator.h"
#include "SteamRelay.h"
#include "SteamApi.h"
// #include "SteamAuthBridge.h"   // disabled, see SteamAuthBridge.cpp.disabled
#include "AppIdStrategy.h"
#include "PayloadLog.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────
//  Serverless Photon: every Photon datagram the game sends is diverted to PhotonEmulator in-process and answered on the same socket. Installs
//  event-driven off Mono's assembly-load hook, the moment the Photon assembly appears.
// ─────────────────────────────────────────────────────────────────

namespace {
    std::atomic_bool g_setupDone{false};

    // ── Plaintext payloads ──────────────────────────────────────────
    // SerializeOperationToMessage encrypts only when `encrypt && !IsTransportEncrypted()`, so reporting the transport as encrypted keeps every
    // operation in plaintext and the emulator needs no crypto. The key exchange itself still runs (the client waits on it); its secret goes
    // unused. This is a replacement hook: we never call the original.
    bool HkIsTransportEncrypted(void*) { return true; }

    // ── Region ping ─────────────────────────────────────────────────
    // PingMono pings each region over its own UDP socket, which nothing answers here, and charges every miss MaxMillisecondsPerPing. The
    // regions we hand out are this process, so a ping opens no socket and completes at once.
    bool HkPingStart(void*, void*) { return false; }
    bool HkPingDone(void* self) {
        bool successful = true;
        MonoBridge::SetField(self, "Successful", &successful);
        return true;
    }

    // ── Socket redirect ─────────────────────────────────────────────
    // SocketUdp.Send(byte[], int) carries each outgoing ENet datagram; IPhotonSocket.HandleReceivedDatagram(byte[], int, bool) is where the
    // receive thread hands inbound ones to the peer. Both are named and shaped the same across Photon versions. We replace Send and call
    // HandleReceivedDatagram directly — no trampoline for either, so these hooks install even when Detours can't allocate within ±2 GB of
    // Mono's JIT region (the Mono-method size limit the previous build hit on R.E.P.O).
    using HandleRecv_t = void (*)(void* self, void* byteArray, int len, bool reuse);
    HandleRecv_t oHandleRecv = nullptr;

    int HkSockSend(void* self, void* byteArray, int len) {
        const auto bytes = MonoBridge::ReadByteArray(byteArray, len > 0 ? (size_t)len : 0);
        PhotonEmulator::OnClientDatagram(self, bytes.data(), bytes.size());
        return len;   // "sent": nothing reaches the network
    }

    void InjectTo(void* socket, const uint8_t* data, size_t len) {
        if (!socket || !oHandleRecv) return;
        if (void* arr = MonoBridge::NewByteArray(data, len)) oHandleRecv(socket, arr, (int)len, false);
    }

    bool Patch(const char* ns, const char* cls, const char* method, int args, void* replacement) {
        void* klass = MonoBridge::FindClass(ns, cls);
        void* m     = klass ? MonoBridge::FindMethod(klass, method, args) : nullptr;
        const bool ok = m && MonoBridge::PatchMethod(m, replacement);
        if (!ok) PayloadLog::Write(std::string("Photon[Mono]: can't patch ") + cls + "." + method);
        return ok;
    }

    void* NativeEntry(const char* ns, const char* cls, const char* method, int args) {
        void* klass = MonoBridge::FindClass(ns, cls);
        void* m     = klass ? MonoBridge::FindMethod(klass, method, args) : nullptr;
        void* fn = m ? MonoBridge::NativeEntry(m) : nullptr;
        if (!fn) PayloadLog::Write(std::string("Photon[Mono]: can't resolve ") + cls + "." + method);
        return fn;
    }

    void BringUpRelay() {
        SteamRelay::Callbacks cb;
        cb.onMessage  = PhotonEmulator::OnRelayMessage;
        cb.onPeerLeft = PhotonEmulator::OnRelayPeerLeft;
        cb.onRegistry = PhotonEmulator::OnRegistry;
        if (!SteamRelay::Init(cb)) PayloadLog::Write("Photon: SteamRelay init failed — play stays on this machine");
    }

    // Mono assembly-load callback: acts once the Photon assembly is present.
    void OnAssembly() {
        if (!MonoBridge::Ready() || !MonoBridge::FindClass("Photon.Realtime", "LoadBalancingClient")) return;
        bool expected = false;
        if (!g_setupDone.compare_exchange_strong(expected, true)) return;

        PayloadLog::Write("Photon[Mono]: Photon.Realtime present");
        // Photon games rely on a Steam lobby + invites under the unowned appid, so tell OST to report 480 (Spacewar).
        AppIdStrategy::Set(AppIdStrategy::Force480);
        PhotonEmulator::SetInjector(InjectTo);
        // Both need the game's Steam client, which is often loaded only after Photon's assembly.
        SteamApi::WhenLoaded([] {
            // SteamAuthBridge::Install();   // disabled: a 480 session gets a real auth ticket from Steam, so none is forged
            BringUpRelay();
        });

        const bool plaintext = Patch("ExitGames.Client.Photon", "EnetPeer", "IsTransportEncrypted", 0,
                                     reinterpret_cast<void*>(HkIsTransportEncrypted));
        oHandleRecv = reinterpret_cast<HandleRecv_t>(NativeEntry("ExitGames.Client.Photon", "IPhotonSocket", "HandleReceivedDatagram", 3));
        // The send hook goes last: from then on the emulator answers, so injection must already work.
        const bool send      = oHandleRecv && Patch("ExitGames.Client.Photon", "SocketUdp", "Send", 2, reinterpret_cast<void*>(HkSockSend));
        const bool ping      = Patch("Photon.Realtime", "PingMono", "Done", 0, reinterpret_cast<void*>(HkPingDone))
                            && Patch("Photon.Realtime", "PingMono", "StartPing", 1, reinterpret_cast<void*>(HkPingStart));
        PayloadLog::Write(std::string("Photon[Mono]: plaintext=") + (plaintext ? "1" : "0")
                          + " receive=" + (oHandleRecv ? "1" : "0") + " send=" + (send ? "1" : "0") + " ping=" + (ping ? "1" : "0"));
    }
}

namespace PhotonBridge {

    void InstallMono() {
        MonoBridge::OnAssemblyLoaded(OnAssembly);
    }

} // namespace PhotonBridge
