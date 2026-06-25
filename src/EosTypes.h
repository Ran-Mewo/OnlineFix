#pragma once

#include <cstdint>

// ─────────────────────────────────────────────────────────────────
//  Partial mirrors of the EOS SDK structs we touch. Each one declares the
//  struct's leading fields only as far as the last field we read or write,
//  and omits the rest.
//
//  This is safe because the EOS ABI is append-only: new fields are only ever
//  added at the end and bump the struct's ApiVersion, so existing field offsets
//  never move and a leading slice stays correct on every later SDK. To stay
//  version-agnostic we also never invent an ApiVersion: options coming from the
//  game keep whatever version the game passed, and the one struct we construct
//  ourselves (CreateDeviceId) uses its own *_API_LATEST.
// ─────────────────────────────────────────────────────────────────

using EOS_EResult = int32_t;
using EOS_Bool    = int32_t;
using EOS_HConnect = void*;
using EOS_HLobby   = void*;
using EOS_HIntegratedPlatformOptionsContainer = void*;
using EOS_ProductUserId    = void*;
using EOS_ContinuanceToken = void*;

constexpr EOS_EResult EOS_Success             = 0;
constexpr EOS_EResult EOS_DuplicateNotAllowed = 24;  // device id already exists
constexpr int32_t EOS_ECT_DEVICEID_ACCESS_TOKEN = 10;

constexpr int32_t EOS_CONNECT_CREATEDEVICEID_API_LATEST = 1;

#pragma pack(push, 8)

struct EOS_Connect_Credentials {
    int32_t  ApiVersion;
    const char* Token;
    int32_t  Type;
};
struct EOS_Connect_UserLoginInfo {
    int32_t  ApiVersion;
    const char* DisplayName;
    const char* NsaIdToken;  // v2+; null on every non-Switch platform
};
struct EOS_Connect_LoginOptions {
    int32_t  ApiVersion;
    const EOS_Connect_Credentials*   Credentials;
    const EOS_Connect_UserLoginInfo* UserLoginInfo;
};
struct EOS_Connect_LoginCallbackInfo {
    EOS_EResult ResultCode;
    void*       ClientData;
    EOS_ProductUserId    LocalUserId;
    EOS_ContinuanceToken ContinuanceToken;
};
struct EOS_Connect_CreateDeviceIdOptions {
    int32_t  ApiVersion;
    const char* DeviceModel;
};
struct EOS_Connect_CreateDeviceIdCallbackInfo {
    EOS_EResult ResultCode;
    void*       ClientData;
};

// Declared only up to bPresenceEnabled; the original pointer is passed through
// which ensures the omitted trailing fields we didn't touch stay intact.
struct EOS_Lobby_CreateLobbyOptions_Partial {
    int32_t           ApiVersion;
    EOS_ProductUserId LocalUserId;
    uint32_t          MaxLobbyMembers;
    int32_t           PermissionLevel;
    EOS_Bool          bPresenceEnabled;  // v2+
};
struct EOS_Lobby_JoinLobbyOptions_Partial {
    int32_t           ApiVersion;
    void*             LobbyDetailsHandle;
    EOS_ProductUserId LocalUserId;
    EOS_Bool          bPresenceEnabled;  // v2+
};
struct EOS_Lobby_JoinLobbyByIdOptions_Partial {
    int32_t           ApiVersion;
    const char*       LobbyId;
    EOS_ProductUserId LocalUserId;
    EOS_Bool          bPresenceEnabled;  // v1+
};

#pragma pack(pop)

using EOS_Connect_OnLoginCb          = void(*)(const EOS_Connect_LoginCallbackInfo*);
using EOS_Connect_OnCreateDeviceIdCb = void(*)(const EOS_Connect_CreateDeviceIdCallbackInfo*);

using EOS_Connect_Login_t          = void(*)(EOS_HConnect, const EOS_Connect_LoginOptions*, void*, EOS_Connect_OnLoginCb);
using EOS_Connect_CreateDeviceId_t = void(*)(EOS_HConnect, const EOS_Connect_CreateDeviceIdOptions*, void*, EOS_Connect_OnCreateDeviceIdCb);
using EOS_IPOContainer_Add_t       = EOS_EResult(*)(EOS_HIntegratedPlatformOptionsContainer, const void*);
// CreateLobby / JoinLobby / JoinLobbyById share shape (handle, opts, cd, completion).
using EOS_Lobby_OpFn_t             = void(*)(EOS_HLobby, const void*, void*, void*);
