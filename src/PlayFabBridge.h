#pragma once

// ─────────────────────────────────────────────────────────────────
//  PlayFabBridge — makes PlayFab-gated games log in without a real Steam license. The backend rejects LoginWithSteam because our session can't be
//  validated against a licensed title, so the login is rerouted to LoginWithCustomID, PlayFab's anonymous login, keyed on the Steam ID: every player
//  still gets a stable, distinct PlayFab account and a real session token.
//
//    • Unity/Mono: detour PlayFabClientAPI.LoginWithSteam and call LoginWithCustomID instead (via MonoBridge).
//    • Native (static OpenSSL etc.): rerouted at the TLS layer instead — see TlsIntercept.
// ─────────────────────────────────────────────────────────────────

namespace PlayFabBridge {

    // Unity/Mono path. Subscribes to Mono's assembly-load event and hooks PlayFabClientAPI.LoginWithSteam as soon as the PlayFab assembly is present.
    // Event-driven; no polling. Call once MonoBridge::Ready().
    void InstallMono();

} // namespace PlayFabBridge
