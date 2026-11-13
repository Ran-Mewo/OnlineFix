#include "PlayFabBridge.h"
#include "MonoBridge.h"
#include "SteamApi.h"
#include "PayloadLog.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace {
    std::atomic_bool g_monoInstalled{false};

    std::string SteamIdString() {
        uint64_t id = SteamApi::LocalSteamId();
        return id ? std::to_string(id) : std::string();
    }

    // ─────────────────────────────────────────────────────────────────
    //  Unity / Mono path
    // ─────────────────────────────────────────────────────────────────
    //  LoginWithSteam is static, so its native entry takes the declared parameters in order with no `this`. The hook replaces it with the real
    //  LoginWithCustomID, keyed on the Steam ID and given the game's own callbacks, so the game gets a genuine PlayFab session.

    using LoginWithSteam_t = void (*)(void* request, void* resultCb, void* errorCb, void* customData, void* extraHeaders);
    LoginWithSteam_t oLoginWithSteam = nullptr;

    void*  g_clientApiClass     = nullptr;  // PlayFab.PlayFabClientAPI
    void*  g_loginCustomIdMethod= nullptr;  // LoginWithCustomID(...)
    void*  g_customIdReqClass   = nullptr;  // PlayFab.ClientModels.LoginWithCustomIDRequest

    void hkLoginWithSteam(void* request, void* resultCb, void* errorCb, void* customData, void* extraHeaders)
    {
        MonoBridge::AttachThread();
        PayloadLog::Write("PlayFab[Mono]: LoginWithSteam intercepted -> LoginWithCustomID");

        // Build LoginWithCustomIDRequest { CustomId = <steamid>, CreateAccount = true }.
        void* req = MonoBridge::NewObject(g_customIdReqClass);
        if (!req || !g_loginCustomIdMethod) {
            PayloadLog::Write("PlayFab[Mono]: cannot build CustomID request; falling through");
            if (oLoginWithSteam) oLoginWithSteam(request, resultCb, errorCb, customData, extraHeaders);
            return;
        }

        std::string id = SteamIdString();
        if (void* s = MonoBridge::NewString(id.c_str()))
            MonoBridge::SetField(req, "CustomId", &s);            // string ref → pass &ref
        // CreateAccount is bool or bool? depending on the SDK; setting both bytes makes either true, whatever order bool? stores hasValue and value.
        uint8_t createAccount[2] = { 1, 1 };
        MonoBridge::SetField(req, "CreateAccount", createAccount);

        // LoginWithCustomID(request, resultCallback, errorCallback, customData, extraHeaders)
        void* args[5] = { req, resultCb, errorCb, customData, extraHeaders };
        MonoBridge::Invoke(g_loginCustomIdMethod, nullptr, args);
    }

    // Fired by Mono on every assembly load (and once at registration). Installs the LoginWithSteam hook as soon as the PlayFab assembly is present.
    // Idempotent: latches g_monoInstalled on success so repeated events are no-ops. No polling.
    void OnAssembly() {
        if (g_monoInstalled.load() || !MonoBridge::Ready()) return;
        void* api = MonoBridge::FindClass("PlayFab", "PlayFabClientAPI");
        if (!api) return;  // PlayFab assembly not loaded yet; a later event retries

        void* loginCustomId = MonoBridge::FindMethod(api, "LoginWithCustomID", -1);
        void* customIdReq   = MonoBridge::FindClass("PlayFab.ClientModels", "LoginWithCustomIDRequest");
        void* loginSteam    = MonoBridge::FindMethod(api, "LoginWithSteam", -1);
        if (!loginSteam || !loginCustomId || !customIdReq) {
            PayloadLog::Write("PlayFab[Mono]: PlayFab type present but symbols missing");
            return;
        }
        g_clientApiClass      = api;
        g_loginCustomIdMethod = loginCustomId;
        g_customIdReqClass    = customIdReq;

        if (MonoBridge::DetourMethod(loginSteam, reinterpret_cast<void*>(hkLoginWithSteam), reinterpret_cast<void**>(&oLoginWithSteam))) {
            g_monoInstalled.store(true);
            PayloadLog::Write("PlayFab[Mono]: LoginWithSteam hooked");
        }
    }
}

namespace PlayFabBridge {

    void InstallMono() {
        // Event-driven: react to the PlayFab assembly loading. OnAssembly fires immediately for already-loaded assemblies and on each future load, so
        // there is no polling.
        MonoBridge::OnAssemblyLoaded(OnAssembly);
    }

} // namespace PlayFabBridge
