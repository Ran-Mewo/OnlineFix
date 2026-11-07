// ─────────────────────────────────────────────────────────────────
//  EOS games run multiplayer through Epic's servers in-process, so the Steam
//  AppId 480 trick can't reach them. This DLL hooks the EOS SDK directly:
//  redirect the Connect login from a Steam session ticket (which fails without
//  a real license) to anonymous Device ID auth, and drop the Epic-presence
//  requirement on lobbies. The display name is still read from Steam.
// ─────────────────────────────────────────────────────────────────

#include "EosBridge.h"
#include "EosTypes.h"
#include "AppIdStrategy.h"
#include "SteamApi.h"
#include "Hook.h"
#include "PayloadLog.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <vector>

namespace {
    std::atomic_bool g_installed{false};

    EOS_Connect_Login_t          oLogin          = nullptr;
    EOS_Connect_CreateDeviceId_t oCreateDeviceId = nullptr;
    EOS_IPOContainer_Add_t       oIPOAdd         = nullptr;
    EOS_Lobby_OpFn_t             oCreateLobby    = nullptr;
    EOS_Lobby_OpFn_t             oJoinLobby      = nullptr;
    EOS_Lobby_OpFn_t             oJoinLobbyById  = nullptr;

    // Carries the game's own ApiVersions across the Connect_Login ->
    // CreateDeviceId -> Login chain. Freed in the final callback.
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

    // EOS Device ID login needs a display name; use the Steam persona name.
    std::string SteamPersonaName() {
        auto pName = reinterpret_cast<const char* (*)(void*)>(
            SteamApi::Export("SteamAPI_ISteamFriends_GetPersonaName"));
        void* friends = SteamApi::Interface("SteamAPI_SteamFriends", "SteamFriends");
        if (!pName || !friends) {
            PayloadLog::Write("SteamPersonaName: no ISteamFriends accessor");
            return "Unknown Player";
        }
        const char* name = pName(friends);
        return (name && *name) ? name : "Unknown Player";
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

    // Preserve every ApiVersion the game passed; swap only the credential type.
    void hkLogin(EOS_HConnect h, const EOS_Connect_LoginOptions* opts,
                 void* cbData, EOS_Connect_OnLoginCb cb)
    {
        auto* ctx = new LoginCtx{};
        ctx->handle      = h;
        ctx->cb          = cb;
        ctx->cbData      = cbData;
        ctx->loginApiVersion = opts ? opts->ApiVersion : 1;
        ctx->credsApiVersion = (opts && opts->Credentials) ? opts->Credentials->ApiVersion : 1;
        // UserLoginInfo was added in LoginOptions API v2; on a v1 struct that pointer field doesn't exist, so never read it there.
        ctx->hadUserInfo = opts && opts->ApiVersion >= 2 && opts->UserLoginInfo;
        ctx->userInfoApiVersion = ctx->hadUserInfo ? opts->UserLoginInfo->ApiVersion : 0;
        const char* gameName = ctx->hadUserInfo ? opts->UserLoginInfo->DisplayName : nullptr;
        ctx->displayName = (gameName && *gameName) ? gameName : SteamPersonaName();

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

    // Clear bPresenceEnabled — it needs an Epic account we don't have.
    // flagOffset: where the flag sits in this options struct.
    // minApiVer: the ApiVersion that first added the flag; older options lack it.
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

        // The login reroute is the whole point and needs both exports; the lobby presence strips are a bonus, so hook only those a given
        // SDK version actually ships. That keeps the bridge working on older/newer EOS SDKs instead of all-or-nothing.
        if (!Resolve(eos, "EOS_Connect_Login", oLogin) | !Resolve(eos, "EOS_Connect_CreateDeviceId", oCreateDeviceId)) {
            g_installed.store(false);
            return;
        }
        Resolve(eos, "EOS_IntegratedPlatformOptionsContainer_Add", oIPOAdd);
        Resolve(eos, "EOS_Lobby_CreateLobby",   oCreateLobby);
        Resolve(eos, "EOS_Lobby_JoinLobby",     oJoinLobby);
        Resolve(eos, "EOS_Lobby_JoinLobbyById", oJoinLobbyById);

        std::vector<Hook::Pair> hooks{ { reinterpret_cast<void**>(&oLogin), reinterpret_cast<void*>(hkLogin) } };
        if (oIPOAdd)        hooks.push_back({ reinterpret_cast<void**>(&oIPOAdd),        reinterpret_cast<void*>(hkIPOAdd) });
        if (oCreateLobby)   hooks.push_back({ reinterpret_cast<void**>(&oCreateLobby),   reinterpret_cast<void*>(hkCreateLobby) });
        if (oJoinLobby)     hooks.push_back({ reinterpret_cast<void**>(&oJoinLobby),     reinterpret_cast<void*>(hkJoinLobby) });
        if (oJoinLobbyById) hooks.push_back({ reinterpret_cast<void**>(&oJoinLobbyById), reinterpret_cast<void*>(hkJoinLobbyById) });

        if (!Hook::Attach(hooks.data(), hooks.size())) {
            PayloadLog::Write("EOS: hook transaction failed");
            g_installed.store(false);
            return;
        }
        PayloadLog::Write("EOS hooks installed");
        // EOS games need the real appid throughout, otherwise the game can abort launch or EOS multiplayer stops working. EOS needs no 480 cert.
        AppIdStrategy::Set(AppIdStrategy::ForceReal);
    }
}
