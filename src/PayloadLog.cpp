#include "PayloadLog.h"

#ifdef OPENSTEAMTOOL_LOGGING_ENABLED

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace {
    std::filesystem::path g_path;
    std::mutex            g_mutex;
    std::atomic_bool      g_ready{false};

    // Steam's own install path, so payload logs sit under <Steam>\opensteamtool
    // next to OpenSteamTool's. Read from the registry since handshake injection
    // can't seed the game's environment.
    //
    // The value here is a candidate, not an answer: Proton's per-game Wine
    // prefix ships a stub at C:\Program Files (x86)\Steam that has this exact
    // key set, but the directory is a per-prefix shell with no OST in it. The
    // caller filters that case by requiring <Steam>\opensteamtool\ to exist.
    std::filesystem::path SteamRootFromRegistry() {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return {};
        wchar_t buf[MAX_PATH] = {};
        DWORD bytes = sizeof(buf);
        DWORD type = 0;
        const LONG r = RegQueryValueExW(key, L"SteamPath", nullptr, &type,
                                        reinterpret_cast<LPBYTE>(buf), &bytes);
        RegCloseKey(key);
        if (r != ERROR_SUCCESS || type != REG_SZ) return {};
        return buf;
    }
}

namespace PayloadLog {
    void Init(HMODULE self) {
        (void)self;

        std::filesystem::path dir;

        // 1. OST explicit override. Trust it without checking the directory:
        //    OST set it because it wants logs there, even if it hasn't created
        //    the tree yet.
        wchar_t steam[MAX_PATH] = {};
        if (DWORD n = GetEnvironmentVariableW(L"OPENSTEAMTOOL_STEAM_PATH", steam, MAX_PATH);
            n > 0 && n < MAX_PATH) {
            dir = std::filesystem::path(steam) / "opensteamtool" / "payload";
        }

        // 2. Registry candidate. Only accept it when <Steam>\opensteamtool\
        //    actually exists, which is true on a Windows host with OST
        //    installed and false on Proton's per-prefix Steam stub.
        if (dir.empty()) {
            if (auto root = SteamRootFromRegistry(); !root.empty()) {
                auto candidate = root / "opensteamtool";
                std::error_code ec;
                if (std::filesystem::is_directory(candidate, ec)) {
                    dir = candidate / "payload";
                }
            }
        }

        // 3. Standalone (Proton, non-OST hosts, anything else): %APPDATA%
        //    with %TEMP% as a fallback when APPDATA is unset.
        if (dir.empty()) {
            wchar_t userDir[MAX_PATH] = {};
            DWORD n2 = GetEnvironmentVariableW(L"APPDATA", userDir, MAX_PATH);
            if (n2 == 0 || n2 >= MAX_PATH)
                n2 = GetEnvironmentVariableW(L"TEMP", userDir, MAX_PATH);
            if (n2 == 0 || n2 >= MAX_PATH) return;
            dir = std::filesystem::path(userDir) / "onlinefix";
        }

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        g_path = dir / (std::to_string(GetCurrentProcessId()) + ".log");
        g_ready.store(true);
    }

    void Write(const std::string& line) {
        if (!g_ready.load()) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        std::ofstream f(g_path, std::ios::app | std::ios::binary);
        if (!f) return;
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        f << "[" << ts << "] [tid=" << GetCurrentThreadId() << "] " << line << "\n";
    }
}

#endif  // OPENSTEAMTOOL_LOGGING_ENABLED
