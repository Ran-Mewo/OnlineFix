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

    // Steam's own install path, so payload logs sit under <Steam>/opensteamtool
    // next to OpenSteamTool's. Read from the registry since handshake injection
    // can't seed the game's environment.
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
        // Prefer OpenSteamTool's env override, then Steam's registered root, then
        // this DLL's own folder when launched standalone.
        std::filesystem::path base;
        wchar_t steam[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"OPENSTEAMTOOL_STEAM_PATH", steam, MAX_PATH)) {
            base = steam;
        } else if (auto root = SteamRootFromRegistry(); !root.empty()) {
            base = root;
        } else {
            wchar_t dll[MAX_PATH] = {};
            if (!GetModuleFileNameW(self, dll, MAX_PATH)) return;
            base = std::filesystem::path(dll).parent_path();
        }
        auto dir = base / "opensteamtool" / "payload";

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
