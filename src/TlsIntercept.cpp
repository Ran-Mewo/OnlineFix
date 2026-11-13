#include "TlsIntercept.h"
#include "PayloadLog.h"
#include "Hook.h"
#include "SteamApi.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace {
    bool IsHttpRequestStart(const char* p, size_t n) {
        static const char* kVerbs[] = { "GET ", "POST ", "PUT ", "HEAD ", "DELETE ", "PATCH ", "OPTIONS " };
        for (const char* v : kVerbs) {
            const size_t len = std::strlen(v);
            if (n >= len && std::memcmp(p, v, len) == 0) return true;
        }
        return false;
    }

    // Log the request line of a plaintext HTTP record, whichever TLS provider surfaced it — a record not starting with a verb is a body continuation,
    // which we skip. Debug-only visibility into what endpoints a game calls.
    void LogHttp(const char* tag, const char* data, size_t n) {
        if (!data || n < 5 || !IsHttpRequestStart(data, n)) return;
        const char* eol = static_cast<const char*>(std::memchr(data, '\r', n));
        PayloadLog::Write(std::string("TLS[") + tag + "] " + std::string(data, eol ? (size_t)(eol - data) : (n < 200 ? n : 200)));
    }

    // ── Schannel (sspicli EncryptMessage) ───────────────────────────
    struct SecBuffer     { unsigned long cbBuffer, BufferType; void* pvBuffer; };
    struct SecBufferDesc { unsigned long ulVersion, cBuffers; SecBuffer* pBuffers; };
    constexpr unsigned long kSecBufferData = 1;

    using EncryptMessage_t = long (WINAPI*)(void*, unsigned long, SecBufferDesc*, unsigned long);
    EncryptMessage_t oEncryptMessage = nullptr;

    long WINAPI hkEncryptMessage(void* ctx, unsigned long fQOP, SecBufferDesc* msg, unsigned long seqNo) {
        __try {
            if (msg && msg->pBuffers)
                for (unsigned long i = 0; i < msg->cBuffers; ++i)
                    if (msg->pBuffers[i].BufferType == kSecBufferData && msg->pBuffers[i].pvBuffer) {
                        LogHttp("Schannel", static_cast<const char*>(msg->pBuffers[i].pvBuffer), msg->pBuffers[i].cbBuffer);
                        break;
                    }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return oEncryptMessage(ctx, fQOP, msg, seqNo);
    }

    void InstallSchannel() {
        HMODULE sspi = GetModuleHandleW(L"sspicli.dll");
        if (!sspi) sspi = LoadLibraryW(L"Secur32.dll");   // forwards EncryptMessage to sspicli
        oEncryptMessage = sspi ? reinterpret_cast<EncryptMessage_t>(GetProcAddress(sspi, "EncryptMessage")) : nullptr;
        if (!oEncryptMessage) { PayloadLog::Write("TlsIntercept: EncryptMessage not found"); return; }
        if (Hook::Attach({ { reinterpret_cast<void**>(&oEncryptMessage), reinterpret_cast<void*>(hkEncryptMessage) } }))
            PayloadLog::Write("TlsIntercept: hooked Schannel EncryptMessage");
    }

    // ── OpenSSL (static SSL_write, unexported) ──────────────────────
    using SSLWrite_t = int (*)(void* ssl, const void* buf, int num);
    SSLWrite_t oSSLWrite = nullptr;

    // PlayFab LoginWithSteam validates the Steam ticket server-side against the title's appid, which a 480 session can't satisfy for an unowned game.
    // Reroute it to LoginWithCustomID (keyed on the SteamID, no ticket): swap the path, replace the ticket with CustomId, drop the ticket flag, and
    // keep TitleId/CreateAccount/InfoRequestParameters so the game still gets the profile it asked for. Fix Content-Length for the shorter body.
    bool RewriteLogin(const char* data, size_t n, std::string& out) {
        const char* sep = nullptr;
        for (size_t j = 0; j + 3 < n; ++j)
            if (std::memcmp(data + j, "\r\n\r\n", 4) == 0) { sep = data + j + 4; break; }
        if (!sep) return false;
        std::string head(data, sep - data);
        std::string body(sep, data + n - sep);
        if (head.find("/Client/LoginWithSteam") == std::string::npos) return false;

        // Don't rewrite a body split across records, or we'd corrupt it. (The header offset is still valid here; nothing has moved yet.)
        size_t cl = head.find("Content-Length:");
        if (cl == std::string::npos) cl = head.find("content-length:");
        if (cl != std::string::npos) {
            size_t vs = cl + 15; while (vs < head.size() && head[vs] == ' ') ++vs;
            if ((size_t)std::strtoul(head.c_str() + vs, nullptr, 10) > body.size()) return false;
        }

        head.replace(head.find("LoginWithSteam"), 14, "LoginWithCustomID");

        const std::string cid = "\"CustomId\":\"" + std::to_string(SteamApi::LocalSteamId()) + "\"";
        if (size_t sp = body.find("\"SteamTicket\":\""); sp != std::string::npos) {
            size_t ve = body.find('"', sp + 15);
            if (ve != std::string::npos) body.replace(sp, ve + 1 - sp, cid);
        } else if (!body.empty() && body[0] == '{') {
            body.insert(1, cid + ",");
        }
        for (const char* f : { "\"TicketIsServiceSpecific\":true,", "\"TicketIsServiceSpecific\":false,",
                               ",\"TicketIsServiceSpecific\":true", ",\"TicketIsServiceSpecific\":false" })
            if (size_t tp = body.find(f); tp != std::string::npos) { body.erase(tp, std::strlen(f)); break; }

        // Re-find Content-Length: the path rewrite above shifted every offset after it.
        size_t cl2 = head.find("Content-Length:");
        if (cl2 == std::string::npos) cl2 = head.find("content-length:");
        if (cl2 != std::string::npos) {
            size_t vs = cl2 + 15; while (vs < head.size() && head[vs] == ' ') ++vs;
            head.replace(vs, head.find("\r\n", vs) - vs, std::to_string(body.size()));
        }
        out = head + body;
        return true;
    }

    // SEH guard in its own function with no unwinding locals: hkSSLWrite holds a std::string, which can't share a frame with __try (C2712), and this
    // must build no std::string temporaries inside the __try either.
    void SafeInspect(const char* buf, int num, std::string& out) {
        __try {
            LogHttp("OpenSSL", buf, (size_t)num);
            RewriteLogin(buf, (size_t)num, out);
        } __except (EXCEPTION_EXECUTE_HANDLER) { out.clear(); }
    }

    int hkSSLWrite(void* ssl, const void* buf, int num) {
        std::string sub;
        if (buf && num > 0) SafeInspect(static_cast<const char*>(buf), num, sub);
        if (!sub.empty()) {
            PayloadLog::Write("TLS[OpenSSL] rerouted LoginWithSteam -> LoginWithCustomID");
            oSSLWrite(ssl, sub.data(), (int)sub.size());
            return num;   // report the caller's length as sent
        }
        return oSSLWrite(ssl, buf, num);
    }

    // Find OpenSSL 1.1.x SSL_write by its body `test r8d,r8d; jns; mov edx, 0xD0(SSL_F_SSL_WRITE)` — the num<0 check plus the write func code, unique
    // in the image. The entry is the fixed 13-byte prologue (`mov eax,imm; call __chkstk; sub rsp,rax`) before it.
    uint8_t* FindSslWrite(HMODULE mod) {
        if (!mod) return nullptr;
        auto base = reinterpret_cast<uint8_t*>(mod);
        auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uint8_t* p = base + sec[i].VirtualAddress;
            const size_t n = sec[i].Misc.VirtualSize;
            for (size_t o = 0; o + 10 < n; ++o) {
                if (p[o]==0x45 && p[o+1]==0x85 && p[o+2]==0xC0 && p[o+3]==0x79 &&
                    p[o+5]==0xBA && p[o+6]==0xD0 && p[o+7]==0 && p[o+8]==0 && p[o+9]==0) {
                    uint8_t* entry = p + o - 13;
                    if (entry >= base && entry[0]==0xB8 && entry[5]==0xE8 && entry[10]==0x48 && entry[11]==0x2B && entry[12]==0xE0)
                        return entry;
                }
            }
        }
        return nullptr;
    }

    void InstallOpenSSL() {
        uint8_t* fn = FindSslWrite(GetModuleHandleW(nullptr));   // UE statically links OpenSSL into the game exe
        if (!fn) return;   // not a static-OpenSSL game
        oSSLWrite = reinterpret_cast<SSLWrite_t>(fn);
        if (Hook::Attach({ { reinterpret_cast<void**>(&oSSLWrite), reinterpret_cast<void*>(hkSSLWrite) } }))
            PayloadLog::Write("TlsIntercept: hooked OpenSSL SSL_write");
    }
}

namespace TlsIntercept {

    void Install() {
        static std::atomic_bool done{false};
        bool expected = false;
        if (!done.compare_exchange_strong(expected, true)) return;
        InstallSchannel();
        InstallOpenSSL();
    }

} // namespace TlsIntercept
