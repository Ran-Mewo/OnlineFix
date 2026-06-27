// ─────────────────────────────────────────────────────────────────
//  Not every game sold on Steam runs its multiplayer through Steam. Some
//  ship a third-party online layer. Most such games use EOS which loads
//  inside the game process and talks to its own servers; Steam is only
//  the launcher. The Steam AppId 480 trick doesn't work since it never sees that traffic,
//  so the only way we can make these games work is in-process, i.e. right next to the SDK.
//  That's what this DLL is for.
//
//  Regular EOS works like this: the game logs into EOS with a Steam session ticket
//  (credential type 18). The server validates that ticket against a genuine
//  Steam license for the app, and so it fails the moment ownership isn't
//  real. We try to sidestep ownership entirely by redirecting the Connect login
//  to an anonymous Device ID auth, which carries no account or entitlement check.
//  The display name is still read from Steam so friends see a real name.
//
//  Device ID auth has no Epic presence behind it, so the lobby hooks
//  below drop the presence requirement that would otherwise reject us.
// ─────────────────────────────────────────────────────────────────

#include "EosBridge.h"
#include "EosTypes.h"
#include "PayloadLog.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <detours.h>

namespace {
    std::atomic_bool g_installed{false};

    EOS_Connect_Login_t          oLogin          = nullptr;
    EOS_Connect_CreateDeviceId_t oCreateDeviceId = nullptr;
    EOS_IPOContainer_Add_t       oIPOAdd         = nullptr;
    EOS_Lobby_OpFn_t             oCreateLobby    = nullptr;
    EOS_Lobby_OpFn_t             oJoinLobby      = nullptr;
    EOS_Lobby_OpFn_t             oJoinLobbyById  = nullptr;

    // Spans Connect_Login -> CreateDeviceId -> Login. Freed in the final
    // Login callback (success) or the device-id callback (failure path).
    // We carry the game's own ApiVersions through so the re-login matches the
    // SDK the game was built against — never hardcode them.
    struct LoginCtx {
        EOS_HConnect            handle;
        EOS_Connect_OnLoginCb   cb;
        void*                   cbData;
        int32_t                 loginApiVersion;
        int32_t                 credsApiVersion;
        int32_t                 userInfoApiVersion;  // 0 if game passed none
        bool                    hadUserInfo;
        std::string             displayName;
    };

    // EOS Device ID login requires a display name. Steam is already loaded
    // in the game process by the time EOS comes up, so walk steam_api.
    //
    // Two interface-acquisition paths because not every Steamworks SDK
    // release ships the legacy `SteamFriends()` flat accessor — newer SDKs
    // only expose the `SteamAPI_ISteamFriends_*` per-method helpers and the
    // `SteamInternal_FindOrCreateUserInterface` factory.
    std::string SteamPersonaName() {
        HMODULE sa = GetModuleHandleW(L"steam_api64.dll");
        if (!sa) sa = GetModuleHandleW(L"steam_api.dll");
        if (!sa) {
            PayloadLog::Write("SteamPersonaName: steam_api{,64}.dll not loaded");
            return "Unknown Player";
        }

        auto pName = reinterpret_cast<const char* (*)(void*)>(
            GetProcAddress(sa, "SteamAPI_ISteamFriends_GetPersonaName"));
        if (!pName) {
            PayloadLog::Write("SteamPersonaName: SteamAPI_ISteamFriends_GetPersonaName missing");
            return "Unknown Player";
        }

        void* friends = nullptr;
        std::string via = "(none)";

        // Path 1: legacy flat accessor — present in older SDKs that still
        // ship `SteamFriends()`.
        if (auto pFriends = reinterpret_cast<void* (*)()>(
                GetProcAddress(sa, "SteamFriends"))) {
            friends = pFriends();
            if (friends) via = "SteamFriends() legacy";
        }

        // Path 2: factory for newer SDKs that no longer ship the flat
        // accessor. SteamInternal_FindOrCreateUserInterface takes a version
        // string ("SteamFriends018", etc.) and returns the corresponding
        // vtable. We don't know which version the game baked in, and there
        // is no API to enumerate what the steam_api dll supports, so scan a
        // descending integer range. Any hit works for our use because
        // GetPersonaName() lives in a vtable slot that has never moved
        // across versions — the Steamworks ABI rule is append-only.
        // First hit wins.
        if (!friends) {
            auto pGetHUser = reinterpret_cast<int (*)()>(
                GetProcAddress(sa, "SteamAPI_GetHSteamUser"));
            auto pCreate = reinterpret_cast<void* (*)(int, const char*)>(
                GetProcAddress(sa, "SteamInternal_FindOrCreateUserInterface"));
            if (pGetHUser && pCreate) {
                const int hUser = pGetHUser();
                // Scan ascending — we only need GetPersonaName(), which has
                // existed since V1 and (under append-only ABI) lives in the
                // same vtable slot at every version. The lowest version the
                // dll still ships is the cheapest to acquire and the most
                // stable to use. 15 is the oldest baseline still seen in
                // the wild; 30 is generous headroom over today's max (~020)
                // for the rare cases where older versions have been dropped
                // from the runtime.
                char buf[16];
                for (int v = 15; v <= 30; ++v) {
                    std::snprintf(buf, sizeof(buf), "SteamFriends%03d", v);
                    friends = pCreate(hUser, buf);
                    if (friends) { via = buf; break; }
                }
            }
        }

        if (!friends) {
            PayloadLog::Write("SteamPersonaName: no ISteamFriends accessor worked "
                              "(legacy + factory both null)");
            return "Unknown Player";
        }

        const char* name = pName(friends);
        if (!name || !*name) {
            PayloadLog::Write(std::string("SteamPersonaName: GetPersonaName() empty via ")
                              + via);
            return "Unknown Player";
        }
        PayloadLog::Write(std::string("SteamPersonaName: ") + name + " (via " + via + ")");
        return name;
    }

    void OnLoginDone(const EOS_Connect_LoginCallbackInfo* info) {
        auto* ctx = static_cast<LoginCtx*>(info->ClientData);
        EOS_Connect_LoginCallbackInfo out = *info;
        out.ClientData = ctx->cbData;
        if (ctx->cb) ctx->cb(&out);
        delete ctx;
    }

    void OnCreateDeviceIdDone(const EOS_Connect_CreateDeviceIdCallbackInfo* info) {
        auto* ctx = static_cast<LoginCtx*>(info->ClientData);
        const bool ready = info->ResultCode == EOS_Success
                        || info->ResultCode == EOS_DuplicateNotAllowed;
        if (!ready) {
            EOS_Connect_LoginCallbackInfo fail = {};
            fail.ResultCode = info->ResultCode;
            fail.ClientData = ctx->cbData;
            if (ctx->cb) ctx->cb(&fail);
            delete ctx;
            return;
        }

        EOS_Connect_Credentials   creds{ ctx->credsApiVersion, nullptr, EOS_ECT_DEVICEID_ACCESS_TOKEN };
        EOS_Connect_UserLoginInfo who {};
        who.ApiVersion  = ctx->hadUserInfo ? ctx->userInfoApiVersion : 1;
        who.DisplayName = ctx->displayName.c_str();
        who.NsaIdToken  = nullptr;
        EOS_Connect_LoginOptions  opts{ ctx->loginApiVersion, &creds, &who };
        oLogin(ctx->handle, &opts, ctx, OnLoginDone);
    }

    // Redirect the game's Steam-credentialed Connect login to anonymous Device
    // ID auth. We preserve every ApiVersion the game passed and swap only the
    // credential type, so the call stays correct across SDK versions.
    void hkLogin(EOS_HConnect h, const EOS_Connect_LoginOptions* opts,
                 void* cbData, EOS_Connect_OnLoginCb cb)
    {
        auto* ctx = new LoginCtx{};
        ctx->handle      = h;
        ctx->cb          = cb;
        ctx->cbData      = cbData;
        ctx->loginApiVersion = opts ? opts->ApiVersion : 1;
        ctx->credsApiVersion = (opts && opts->Credentials) ? opts->Credentials->ApiVersion : 1;
        ctx->hadUserInfo = opts && opts->UserLoginInfo;
        ctx->userInfoApiVersion = ctx->hadUserInfo ? opts->UserLoginInfo->ApiVersion : 0;
        // Keep the game's display name if it supplied one; otherwise pull Steam's.
        const char* gameName = ctx->hadUserInfo ? opts->UserLoginInfo->DisplayName : nullptr;
        ctx->displayName = (gameName && *gameName) ? gameName : SteamPersonaName();

        // Record the versions we carried through; a future SDK that shifts them should show up here.
        PayloadLog::Write("Connect_Login intercepted: login.v=" + std::to_string(ctx->loginApiVersion)
                          + " creds.v=" + std::to_string(ctx->credsApiVersion)
                          + " userInfo.v=" + std::to_string(ctx->userInfoApiVersion)
                          + " name=" + ctx->displayName);

        EOS_Connect_CreateDeviceIdOptions create{ EOS_CONNECT_CREATEDEVICEID_API_LATEST, "PC" };
        oCreateDeviceId(h, &create, ctx, OnCreateDeviceIdDone);
    }

    EOS_EResult hkIPOAdd(EOS_HIntegratedPlatformOptionsContainer, const void*) {
        return EOS_Success;
    }

    // bPresenceEnabled requires an Epic account we don't have. The field
    // offset differs per struct because the preceding members differ.
    void StripPresence(const void* opts, size_t flagOffset, int32_t minApiVer) {
        if (!opts) return;
        if (*reinterpret_cast<const int32_t*>(opts) < minApiVer) return;
        auto* flag = reinterpret_cast<EOS_Bool*>(reinterpret_cast<uintptr_t>(opts) + flagOffset);
        if (*flag) *flag = 0;
    }

    void hkCreateLobby(EOS_HLobby h, const void* opts, void* cd, void* cb) {
        StripPresence(opts, offsetof(EOS_Lobby_CreateLobbyOptions_Partial, bPresenceEnabled), 2);
        oCreateLobby(h, opts, cd, cb);
    }
    void hkJoinLobby(EOS_HLobby h, const void* opts, void* cd, void* cb) {
        StripPresence(opts, offsetof(EOS_Lobby_JoinLobbyOptions_Partial, bPresenceEnabled), 2);
        oJoinLobby(h, opts, cd, cb);
    }
    void hkJoinLobbyById(EOS_HLobby h, const void* opts, void* cd, void* cb) {
        StripPresence(opts, offsetof(EOS_Lobby_JoinLobbyByIdOptions_Partial, bPresenceEnabled), 1);
        oJoinLobbyById(h, opts, cd, cb);
    }

    template <typename Fn>
    bool Resolve(HMODULE m, const char* name, Fn& slot) {
        slot = reinterpret_cast<Fn>(GetProcAddress(m, name));
        if (!slot) PayloadLog::Write(std::string("missing EOS export: ") + name);
        return slot != nullptr;
    }
}

namespace EosBridge {
    void InstallOn(HMODULE eos) {
        bool expected = false;
        if (!eos || !g_installed.compare_exchange_strong(expected, true)) return;

        bool ok = Resolve(eos, "EOS_Connect_Login",                          oLogin)
                & Resolve(eos, "EOS_Connect_CreateDeviceId",                 oCreateDeviceId)
                & Resolve(eos, "EOS_IntegratedPlatformOptionsContainer_Add", oIPOAdd)
                & Resolve(eos, "EOS_Lobby_CreateLobby",                      oCreateLobby)
                & Resolve(eos, "EOS_Lobby_JoinLobby",                        oJoinLobby)
                & Resolve(eos, "EOS_Lobby_JoinLobbyById",                    oJoinLobbyById);
        if (!ok) { g_installed.store(false); return; }

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&oLogin),         reinterpret_cast<PVOID>(hkLogin));
        DetourAttach(reinterpret_cast<PVOID*>(&oIPOAdd),        reinterpret_cast<PVOID>(hkIPOAdd));
        DetourAttach(reinterpret_cast<PVOID*>(&oCreateLobby),   reinterpret_cast<PVOID>(hkCreateLobby));
        DetourAttach(reinterpret_cast<PVOID*>(&oJoinLobby),     reinterpret_cast<PVOID>(hkJoinLobby));
        DetourAttach(reinterpret_cast<PVOID*>(&oJoinLobbyById), reinterpret_cast<PVOID>(hkJoinLobbyById));
        LONG err = DetourTransactionCommit();
        if (err != NO_ERROR) {
            PayloadLog::Write("DetourTransactionCommit failed err=" + std::to_string(err));
            g_installed.store(false);
            return;
        }
        PayloadLog::Write("EOS hooks installed");
    }
}
