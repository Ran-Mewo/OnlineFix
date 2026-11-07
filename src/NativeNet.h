#pragma once

#include <string>

// ─────────────────────────────────────────────────────────────────
//  NativeNet — engine-agnostic HTTP interception for games with no managed surface to patch (Unreal shipping builds strip the PlayFab SDK symbols, so
//  there is no LoginWithSteam method to detour). Since PlayFab is a plain REST API, we intercept at the HTTP-client layer instead, above TLS, where
//  the request path and body are plaintext.
//
//  We hook the three HTTP stacks a Windows game realistically uses:
//    • WinHTTP   (UE's default platform HTTP on Windows)
//    • WinINet   (older / alternate stack)
//    • libcurl   (UE's CurlHttp manager, if statically linked)
//  Each stack-specific hook funnels into one Rewriter chain, so the PlayFab logic lives once in PlayFabBridge and is reused everywhere.
//
//  Layering: NativeNet knows HTTP, nothing about PlayFab. Feature bridges register a Rewriter that recognises their endpoint and mutates the request.
//  Mirrors the EosBridge discipline: match on stable contract (here, the URL path), change the minimum, pass the rest through untouched.
// ─────────────────────────────────────────────────────────────────

namespace NativeNet {

    // A single outbound request, seen in the clear. `path` is the URL path (and query) — e.g. "/Client/LoginWithSteam"; hosts vary by PlayFab title
    // so we match on path, not host. `body` is the request payload (PlayFab uses small JSON objects).
    struct HttpRequest {
        std::string path;
        std::string body;
    };

    // Mutate the request in place; return true if anything changed. Called potentially twice per WinHTTP request (once at open with an empty body to
    // settle the path, once at send with the real body), so a Rewriter must be idempotent: a path it already rewrote must not match again. Runs on
    // the game's calling thread.
    using Rewriter = bool (*)(HttpRequest& req);

    // Register a rewriter. Call before Install (or after — new requests pick it up either way). Not thread-safe against concurrent Install; bridges
    // register during single-threaded payload startup.
    void AddRewriter(Rewriter fn);

    // Detour whichever HTTP stacks are present in the process. Idempotent. No-ops cleanly for stacks that aren't loaded; logs what it hooked. (Called
    // off the loader lock — it runs a Detours transaction.)
    void Install();

    // Hook any HTTP stack that has since loaded. The module dispatcher calls this when winhttp/wininet/curl appears after startup. Idempotent.
    void AttemptHooks();

} // namespace NativeNet
