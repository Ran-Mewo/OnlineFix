#include "NativeNet.h"
#include "PayloadLog.h"

#include "Hook.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// HINTERNET typedef'd so we don't include winhttp.h (avoids the implicit lib dependency). All HTTP funcs are resolved via GetProcAddress, so we never
// drag winhttp/wininet into games that don't use them.
using HINTERNET = void*;

// ─────────────────────────────────────────────────────────────────
//  Per stack. PlayFab routes by URL path, so a rewrite must touch both the path (fixed at open) and the body (sent later); we run the rewriter at
//  open AND send, keyed on the handle.
//    WinHTTP:  WinHttpOpenRequest / WinHttpSendRequest
//    WinINet:  HttpOpenRequestW / HttpSendRequestW (mirror)
//    libcurl:  curl_easy_setopt(CURLOPT_URL / CURLOPT_POSTFIELDS)
// ─────────────────────────────────────────────────────────────────

namespace {
    std::atomic_bool g_installed{false};

    std::mutex                 g_rwMutex;
    std::vector<NativeNet::Rewriter> g_rewriters;

    // path remembered at WinHttpOpenRequest, keyed by request handle.
    std::mutex                                   g_pathMutex;
    std::unordered_map<HINTERNET, std::wstring>  g_reqPath;

    // Bodies we substitute, kept alive until the send returns. WinHttp streams the body, so a thread_local keeps ours valid for the call.
    thread_local std::string t_subBody;

    std::wstring Widen(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }
    std::string Narrow(const wchar_t* s) {
        if (!s) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string a(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s, -1, a.data(), n, nullptr, nullptr);
        return a;
    }

    // Run the rewriter chain. Returns true if any rewriter changed req.
    bool RunRewriters(NativeNet::HttpRequest& req) {
        std::lock_guard<std::mutex> lock(g_rwMutex);
        bool changed = false;
        for (auto fn : g_rewriters) changed |= fn(req);
        return changed;
    }

    // ── WinHTTP hooks ───────────────────────────────────────────────
    using WinHttpOpenRequest_t = HINTERNET (WINAPI*)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD);
    using WinHttpSendRequest_t = BOOL (WINAPI*)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
    using WinHttpCloseHandle_t = BOOL (WINAPI*)(HINTERNET);

    WinHttpOpenRequest_t oWinHttpOpenRequest = nullptr;
    WinHttpSendRequest_t oWinHttpSendRequest = nullptr;
    WinHttpCloseHandle_t oWinHttpCloseHandle = nullptr;

    HINTERNET WINAPI hkWinHttpOpenRequest(HINTERNET hConnect, LPCWSTR verb, LPCWSTR object,
        LPCWSTR version, LPCWSTR referrer, LPCWSTR* accept, DWORD flags)
    {
        // Path is fixed here; reroute it now (PlayFab routes by path) and stash the original so the send hook can rewrite the body.
        std::string origPath = Narrow(object);
        LPCWSTR effObject = object;
        std::wstring newObjectW;
        if (!origPath.empty()) {
            NativeNet::HttpRequest req{origPath, ""};
            if (RunRewriters(req) && req.path != origPath) {
                newObjectW = Widen(req.path);
                effObject = newObjectW.c_str();
                PayloadLog::Write("NativeNet[WinHTTP]: rerouted path " + origPath + " -> " + req.path);
            }
        }
        HINTERNET h = oWinHttpOpenRequest(hConnect, verb, effObject, version, referrer, accept, flags);
        if (h && !origPath.empty()) {
            std::lock_guard<std::mutex> lock(g_pathMutex);
            g_reqPath[h] = Widen(origPath);
        }
        return h;
    }

    BOOL WINAPI hkWinHttpSendRequest(HINTERNET hRequest, LPCWSTR headers, DWORD headersLen,
        LPVOID optional, DWORD optionalLen, DWORD totalLen, DWORD_PTR context)
    {
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(g_pathMutex);
            if (auto it = g_reqPath.find(hRequest); it != g_reqPath.end()) path = it->second;
        }

        NativeNet::HttpRequest req;
        req.path = Narrow(path.c_str());
        if (optional && optionalLen) req.body.assign((const char*)optional, optionalLen);

        if (RunRewriters(req)) {
            t_subBody = req.body;
            PayloadLog::Write("NativeNet[WinHTTP]: rewrote request path=" + req.path + " newlen=" + std::to_string(t_subBody.size()));
            // Pass the new size as both optional and total len so WinHTTP's Content-Length matches the rewritten body.
            DWORD newLen = (DWORD)t_subBody.size();
            return oWinHttpSendRequest(hRequest, headers, headersLen, t_subBody.empty() ? nullptr : t_subBody.data(), newLen, newLen, context);
        }
        return oWinHttpSendRequest(hRequest, headers, headersLen, optional, optionalLen, totalLen, context);
    }

    BOOL WINAPI hkWinHttpCloseHandle(HINTERNET h) {
        {
            std::lock_guard<std::mutex> lock(g_pathMutex);
            g_reqPath.erase(h);
        }
        return oWinHttpCloseHandle(h);
    }

    bool CollectWinHttp(std::vector<Hook::Pair>& out) {
        HMODULE m = GetModuleHandleW(L"winhttp.dll");
        if (!m) return false;  // game doesn't use WinHTTP
        oWinHttpOpenRequest = (WinHttpOpenRequest_t)GetProcAddress(m, "WinHttpOpenRequest");
        oWinHttpSendRequest = (WinHttpSendRequest_t)GetProcAddress(m, "WinHttpSendRequest");
        oWinHttpCloseHandle = (WinHttpCloseHandle_t)GetProcAddress(m, "WinHttpCloseHandle");
        if (!oWinHttpOpenRequest || !oWinHttpSendRequest || !oWinHttpCloseHandle) {
            PayloadLog::Write("NativeNet[WinHTTP]: exports missing");
            return false;
        }
        out.push_back({ &(PVOID&)oWinHttpOpenRequest, (void*)hkWinHttpOpenRequest });
        out.push_back({ &(PVOID&)oWinHttpSendRequest, (void*)hkWinHttpSendRequest });
        out.push_back({ &(PVOID&)oWinHttpCloseHandle, (void*)hkWinHttpCloseHandle });
        return true;
    }

    // ── WinINet hooks ───────────────────────────────────────────────
    // HttpOpenRequestW sets the path; HttpSendRequestW carries the body in lpOptional. Same model as WinHTTP.
    using HttpOpenRequestW_t = HINTERNET (WINAPI*)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD, DWORD_PTR);
    using HttpSendRequestW_t = BOOL (WINAPI*)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD);

    HttpOpenRequestW_t oHttpOpenRequestW = nullptr;
    HttpSendRequestW_t oHttpSendRequestW = nullptr;

    HINTERNET WINAPI hkHttpOpenRequestW(HINTERNET hConnect, LPCWSTR verb, LPCWSTR object,
        LPCWSTR version, LPCWSTR referrer, LPCWSTR* accept, DWORD flags, DWORD_PTR context)
    {
        HINTERNET h = oHttpOpenRequestW(hConnect, verb, object, version, referrer, accept, flags, context);
        if (h && object) {
            std::lock_guard<std::mutex> lock(g_pathMutex);
            g_reqPath[h] = object;
        }
        return h;
    }

    BOOL WINAPI hkHttpSendRequestW(HINTERNET hRequest, LPCWSTR headers, DWORD headersLen, LPVOID optional, DWORD optionalLen)
    {
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(g_pathMutex);
            if (auto it = g_reqPath.find(hRequest); it != g_reqPath.end()) path = it->second;
        }
        NativeNet::HttpRequest req;
        req.path = Narrow(path.c_str());
        if (optional && optionalLen) req.body.assign((const char*)optional, optionalLen);

        if (RunRewriters(req)) {
            t_subBody = req.body;
            PayloadLog::Write("NativeNet[WinINet]: rewrote request path=" + req.path);
            return oHttpSendRequestW(hRequest, headers, headersLen, t_subBody.empty() ? nullptr : t_subBody.data(), (DWORD)t_subBody.size());
        }
        return oHttpSendRequestW(hRequest, headers, headersLen, optional, optionalLen);
    }

    bool CollectWinINet(std::vector<Hook::Pair>& out) {
        HMODULE m = GetModuleHandleW(L"wininet.dll");
        if (!m) return false;
        oHttpOpenRequestW = (HttpOpenRequestW_t)GetProcAddress(m, "HttpOpenRequestW");
        oHttpSendRequestW = (HttpSendRequestW_t)GetProcAddress(m, "HttpSendRequestW");
        if (!oHttpOpenRequestW || !oHttpSendRequestW) {
            PayloadLog::Write("NativeNet[WinINet]: exports missing");
            return false;
        }
        out.push_back({ &(PVOID&)oHttpOpenRequestW, (void*)hkHttpOpenRequestW });
        out.push_back({ &(PVOID&)oHttpSendRequestW, (void*)hkHttpSendRequestW });
        return true;
    }

    // ── libcurl hooks ───────────────────────────────────────────────
    // URL arrives via CURLOPT_URL then body via CURLOPT_POSTFIELDS; we rewrite on the body set and fix up POSTFIELDSIZE for the new length.
    using curl_easy_setopt_t = int (*)(void* handle, int option, ...);
    curl_easy_setopt_t oCurlEasySetopt = nullptr;

    struct CurlReq { std::string url, body; };
    std::mutex                                g_curlMutex;
    std::unordered_map<void*, CurlReq>        g_curlReq;
    std::unordered_map<void*, std::string>    g_curlSubBody;  // keep substituted body alive

    constexpr int CURLOPT_URL          = 10002;
    constexpr int CURLOPT_POSTFIELDS   = 10015;

    int CurlSetopt(void* handle, int option, void* value); // fwd

    bool CollectCurl(std::vector<Hook::Pair>& out) {
        // libcurl is usually statically linked into the game exe, so the export lives on the main module, not a curl DLL. Try both.
        HMODULE candidates[] = { GetModuleHandleW(nullptr),
                                 GetModuleHandleW(L"libcurl.dll"),
                                 GetModuleHandleW(L"libcurl-x64.dll") };
        for (HMODULE m : candidates) {
            if (!m) continue;
            if (auto p = (curl_easy_setopt_t)GetProcAddress(m, "curl_easy_setopt")) {
                oCurlEasySetopt = p;
                break;
            }
        }
        if (!oCurlEasySetopt) return false;  // not linked / not exported
        out.push_back({ &(PVOID&)oCurlEasySetopt, (void*)CurlSetopt });
        return true;
    }

    int CurlSetopt(void* handle, int option, void* value) {
        // String options pass a const char*; we only inspect URL/body.
        if (option == CURLOPT_URL && value) {
            std::lock_guard<std::mutex> lock(g_curlMutex);
            g_curlReq[handle].url = (const char*)value;
        } else if (option == CURLOPT_POSTFIELDS && value) {
            std::string urlCopy;
            {
                std::lock_guard<std::mutex> lock(g_curlMutex);
                g_curlReq[handle].body = (const char*)value;
                urlCopy = g_curlReq[handle].url;
            }
            // Derive a path from the URL (strip scheme://host).
            NativeNet::HttpRequest req;
            req.body = (const char*)value;
            size_t slash = urlCopy.find('/', urlCopy.find("://") == std::string::npos ? 0
                                            : urlCopy.find("://") + 3);
            req.path = (slash == std::string::npos) ? urlCopy : urlCopy.substr(slash);

            if (RunRewriters(req)) {
                std::string* held;
                {
                    std::lock_guard<std::mutex> lock(g_curlMutex);
                    held = &(g_curlSubBody[handle] = req.body);
                }
                PayloadLog::Write("NativeNet[curl]: rewrote POSTFIELDS path=" + req.path);
                // POSTFIELDS with a NUL-containing/longer body needs the size set explicitly; UE sets COPYPOSTFIELDS or SIZE itself, but to be safe
                // we set the new body and its size.
                int r = oCurlEasySetopt(handle, CURLOPT_POSTFIELDS, (void*)held->c_str());
                // CURLOPT_POSTFIELDSIZE_LARGE = 30120; pass new length.
                oCurlEasySetopt(handle, 30120, (void*)(intptr_t)held->size());
                return r;
            }
        }
        return oCurlEasySetopt(handle, option, value);
    }

    // Hook every HTTP stack present and not yet hooked, in one transaction. Safe to call repeatedly as modules load: a stack
    // marks itself hooked only after the commit succeeds, so a failed attempt is retried on the next call rather than lost.
    std::atomic_bool g_winhttpHooked{false};
    std::atomic_bool g_wininetHooked{false};
    std::atomic_bool g_curlHooked{false};

    int AttemptHooksImpl() {
        std::vector<Hook::Pair> hooks;
        const bool wh = !g_winhttpHooked.load() && CollectWinHttp(hooks);
        const bool wi = !g_wininetHooked.load() && CollectWinINet(hooks);
        const bool cu = !g_curlHooked.load()    && CollectCurl(hooks);
        if (hooks.empty()) return 0;
        if (!Hook::Attach(hooks.data(), hooks.size())) {
            PayloadLog::Write("NativeNet: hook transaction failed");
            return 0;
        }
        int newly = 0;
        if (wh) { g_winhttpHooked.store(true); ++newly; PayloadLog::Write("NativeNet: hooked WinHTTP"); }
        if (wi) { g_wininetHooked.store(true); ++newly; PayloadLog::Write("NativeNet: hooked WinINet"); }
        if (cu) { g_curlHooked.store(true);    ++newly; PayloadLog::Write("NativeNet: hooked libcurl"); }
        return newly;
    }
}

namespace NativeNet {

    void AddRewriter(Rewriter fn) {
        std::lock_guard<std::mutex> lock(g_rwMutex);
        g_rewriters.push_back(fn);
    }

    void Install() {
        bool expected = false;
        if (!g_installed.compare_exchange_strong(expected, true)) return;
        AttemptHooksImpl();
    }

    // Called again whenever an HTTP stack loads later, so a lazily-loaded winhttp/wininet/curl still gets hooked.
    void AttemptHooks() { AttemptHooksImpl(); }

} // namespace NativeNet
