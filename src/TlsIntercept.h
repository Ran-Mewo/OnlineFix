#pragma once

// ─────────────────────────────────────────────────────────────────
//  TlsIntercept — reads plaintext HTTP at the TLS layer, below any HTTP client.
//
//  Games reach PlayFab through whatever HTTP stack they ship (WinHTTP, WinINet, a statically linked libcurl, the Xbox SDK's libHttpClient), so hooking
//  a specific client by its exported API misses the ones that export nothing. Every one of them still hands plaintext to a TLS provider before it hits the
//  socket, so that provider is the one place all of them share. We hook the provider's encrypt/decrypt and recover the HTTP request from the bytes
//  about to be sealed, which is stack-agnostic and, for Schannel, version-agnostic (the SSPI contract is stable).
//
//  Providers:
//    • Schannel — EncryptMessage/DecryptMessage (exported from sspicli, used by WinHTTP/WinINet/.NET/libHttpClient). Installed here.
//    • OpenSSL — SSL_write (static in UE's libcurl, unexported), located by its OpenSSL 1.1.x body signature. Covers UE games that bypass Schannel.
// ─────────────────────────────────────────────────────────────────

namespace TlsIntercept {

    // Hook the TLS providers present in the process. Idempotent; safe before the provider is loaded (Schannel's sspicli always is).
    void Install();

} // namespace TlsIntercept
