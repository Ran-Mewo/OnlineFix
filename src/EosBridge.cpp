// ─────────────────────────────────────────────────────────────────
//  EOS games run multiplayer through Epic's servers in-process, so the Steam
//  AppId 480 trick can't reach them. This DLL hooks the EOS SDK directly:
//  redirect the Connect login from a Steam session ticket (which fails without
//  a real license) to anonymous Device ID auth, and drop the Epic-presence
//  requirement on lobbies. The display name is still read from Steam.
// ─────────────────────────────────────────────────────────────────

#include "EosBridge.h"
#include "EosTypes.h"
#include "PayloadLog.h"

#include <atomic>
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

    // Locate the versioned ISteamFriends accessor the SDK ships
    // (SteamAPI_SteamFriends_v017, _v018, ...) without guessing the number.
    FARPROC FindExportByPrefix(HMODULE mod, const char* prefix) {
        auto base = reinterpret_cast<const BYTE*>(mod);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress) return nullptr;

        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
        const auto* names   = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        const auto* ordinals= reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
        const auto* funcs   = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
        const size_t prefixLen = std::strlen(prefix);

        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            const char* name = reinterpret_cast<const char*>(base + names[i]);
            if (std::strncmp(name, prefix, prefixLen) == 0)
                return reinterpret_cast<FARPROC>(const_cast<BYTE*>(base + funcs[ordinals[i]]));
        }
        return nullptr;
    }

    // EOS Device ID login needs a display name; use the Steam persona name.
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

        // Prefer the legacy flat accessor; newer SDKs only ship the versioned one.
        void* friends = nullptr;
        if (auto pFriends = reinterpret_cast<void* (*)()>(GetProcAddress(sa, "SteamFriends")))
            friends = pFriends();
        if (!friends) {
            // Legacy accessor doesn't exist; utilizing the versioned one.
            if (auto pFriends = reinterpret_cast<void* (*)()>(
                    FindExportByPrefix(sa, "SteamAPI_SteamFriends_v")))
                friends = pFriends();
        }
        if (!friends) {
            PayloadLog::Write("SteamPersonaName: no ISteamFriends accessor found");
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
        ctx->hadUserInfo = opts && opts->UserLoginInfo;
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
