#include "PlayFabBridge.h"
#include "MonoBridge.h"
#include "NativeNet.h"
#include "SteamApi.h"
#include "PayloadLog.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace {
    std::atomic_bool g_monoInstalled{false};
    std::atomic_bool g_nativeInstalled{false};

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

    // ─────────────────────────────────────────────────────────────────
    //  Unreal / native path
    // ─────────────────────────────────────────────────────────────────
    //
    //  PlayFab REST: POST https://<title>.playfabapi.com/Client/LoginWithSteam {"SteamTicket":"...","TitleId":"...","CreateAccount":...}
    //  We reroute it to /Client/LoginWithCustomID {"CustomId":"<steamid>","CreateAccount":true,"TitleId":"..."}, and the real PlayFab server
    //  then issues a genuine session, so the game gets a valid SessionTicket/EntityToken with no fakery.

    constexpr char kSteamPath[]  = "/Client/LoginWithSteam";
    constexpr char kCustomPath[] = "/Client/LoginWithCustomID";

    // Pull "TitleId":"VALUE" out of the original JSON so the rewritten body keeps targeting the same title. Minimal scan — no JSON lib.
    std::string ExtractJsonString(const std::string& json, const char* key) {
        std::string needle = std::string("\"") + key + "\"";
        size_t k = json.find(needle);
        if (k == std::string::npos) return {};
        size_t colon = json.find(':', k + needle.size());
        if (colon == std::string::npos) return {};
        size_t q1 = json.find('"', colon + 1);
        if (q1 == std::string::npos) return {};
        size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
        return json.substr(q1 + 1, q2 - q1 - 1);
    }

    bool PlayFabRewriter(NativeNet::HttpRequest& req) {
        // Match the login path (open call) or a body already on the custom path (send call after we rerouted). Idempotent: once the path is the
        // custom one we only fix the body.
        const bool onSteamPath  = req.path.find(kSteamPath)  != std::string::npos;
        const bool onCustomPath = req.path.find(kCustomPath) != std::string::npos;
        if (!onSteamPath && !onCustomPath) return false;

        bool changed = false;

        // Reroute path at the open call (empty body).
        if (onSteamPath) {
            size_t pos = req.path.find(kSteamPath);
            req.path.replace(pos, std::strlen(kSteamPath), kCustomPath);
            changed = true;
        }

        // Rewrite the body when present (send call).
        if (!req.body.empty()) {
            std::string title = ExtractJsonString(req.body, "TitleId");
            std::string id = SteamIdString();
            std::string body = "{\"CustomId\":\"" + id + "\",\"CreateAccount\":true";
            if (!title.empty()) body += ",\"TitleId\":\"" + title + "\"";
            body += "}";
            if (req.body != body) { req.body = body; changed = true; }
        }
        return changed;
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

    void InstallNative() {
        bool expected = false;
        if (!g_nativeInstalled.compare_exchange_strong(expected, true)) return;
        NativeNet::AddRewriter(PlayFabRewriter);
        NativeNet::Install();
        PayloadLog::Write("PlayFab[Native]: REST rewriter registered");
    }

} // namespace PlayFabBridge
