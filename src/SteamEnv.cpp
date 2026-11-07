#include "SteamEnv.h"
#include "Hook.h"
#include "PayloadLog.h"

#include <windows.h>
#include <atomic>
#include <string>

namespace {
    struct Kept {
        const wchar_t* name;
        std::wstring   value;   // as launched; empty when Steam didn't set it
    };
    Kept g_kept[] = { { L"SteamAppId", {} }, { L"SteamGameId", {} } };

    using SetEnvW_t = BOOL (WINAPI*)(LPCWSTR, LPCWSTR);
    using SetEnvA_t = BOOL (WINAPI*)(LPCSTR, LPCSTR);
    SetEnvW_t oSetEnvW = nullptr;
    SetEnvA_t oSetEnvA = nullptr;

    const std::wstring* LaunchValue(const wchar_t* name) {
        for (const Kept& k : g_kept)
            if (!k.value.empty() && _wcsicmp(name, k.name) == 0) return &k.value;
        return nullptr;
    }

    // The value to actually set: the launch value for a kept variable, the caller's otherwise.
    const wchar_t* Effective(const wchar_t* name, const wchar_t* value) {
        const std::wstring* kept = name ? LaunchValue(name) : nullptr;
        if (!kept || (value && *kept == value)) return value;
        static std::atomic_bool logged{false};
        if (!logged.exchange(true))
            PayloadLog::Write("SteamEnv: the game rewrote its Steam identity; holding the launch value");
        return kept->c_str();
    }

    BOOL WINAPI hkSetEnvW(LPCWSTR name, LPCWSTR value) { return oSetEnvW(name, Effective(name, value)); }

    BOOL WINAPI hkSetEnvA(LPCSTR name, LPCSTR value) {
        wchar_t wide[64];
        if (!name || !MultiByteToWideChar(CP_ACP, 0, name, -1, wide, 64) || !LaunchValue(wide)) return oSetEnvA(name, value);
        return oSetEnvW(wide, LaunchValue(wide)->c_str());
    }
}

namespace SteamEnv {

    void Install() {
        bool any = false;
        for (Kept& k : g_kept) {
            wchar_t buf[64];
            const DWORD n = GetEnvironmentVariableW(k.name, buf, 64);
            if (n && n < 64) { k.value.assign(buf, n); any = true; }
        }
        if (!any) return;   // not launched by Steam with an identity: nothing to hold

        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        oSetEnvW = reinterpret_cast<SetEnvW_t>(GetProcAddress(kernel, "SetEnvironmentVariableW"));
        oSetEnvA = reinterpret_cast<SetEnvA_t>(GetProcAddress(kernel, "SetEnvironmentVariableA"));
        if (!oSetEnvW || !oSetEnvA) return;
        const bool ok = Hook::Attach({ { reinterpret_cast<void**>(&oSetEnvW), reinterpret_cast<void*>(hkSetEnvW) },
                                       { reinterpret_cast<void**>(&oSetEnvA), reinterpret_cast<void*>(hkSetEnvA) } });
        PayloadLog::Write(std::string("SteamEnv: holding SteamAppId/SteamGameId at launch values") + (ok ? "" : " — hook failed"));
    }

} // namespace SteamEnv
