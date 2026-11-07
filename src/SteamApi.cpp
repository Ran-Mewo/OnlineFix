#include "SteamApi.h"
#include "Pe.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
    std::mutex                         g_mutex;
    std::vector<std::function<void()>> g_waiting;   // run once steam_api arrives
}

namespace SteamApi {

    HMODULE Module() {
        if (HMODULE m = GetModuleHandleW(L"steam_api64.dll")) return m;
        return GetModuleHandleW(L"steam_api.dll");
    }

    void WhenLoaded(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!Module()) { g_waiting.push_back(std::move(fn)); return; }
        }
        fn();
    }

    void OnLoaded() {
        std::vector<std::function<void()>> due;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            due.swap(g_waiting);
        }
        for (auto& fn : due) fn();
    }

    void* Export(const char* name) {
        HMODULE m = Module();
        return m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
    }

    void* Interface(const char* base, const char* legacy) {
        HMODULE m = Module();
        if (!m) return nullptr;

        // Pick the highest "<base>_v###" the SDK actually exports, so a newer interface version needs no code change.
        const size_t len = std::strlen(base);
        long best = -1;
        const char* bestName = nullptr;
        Pe::ForEachExport(m, [&](const char* name) {
            if (std::strncmp(name, base, len) != 0 || std::strncmp(name + len, "_v", 2) != 0) return;
            char* end = nullptr;
            const long v = std::strtol(name + len + 2, &end, 10);
            if (end != name + len + 2 && *end == '\0' && v > best) { best = v; bestName = name; }
        });

        using Accessor = void* (*)();
        Accessor fn = bestName ? reinterpret_cast<Accessor>(GetProcAddress(m, bestName)) : nullptr;
        if (!fn) fn = reinterpret_cast<Accessor>(GetProcAddress(m, base));
        if (!fn && legacy) fn = reinterpret_cast<Accessor>(GetProcAddress(m, legacy));
        return fn ? fn() : nullptr;
    }

    uint64_t LocalSteamId() {
        auto getId = reinterpret_cast<uint64_t (*)(void*)>(Export("SteamAPI_ISteamUser_GetSteamID"));
        void* user = Interface("SteamAPI_SteamUser", "SteamUser");
        return getId && user ? getId(user) : 0;
    }

} // namespace SteamApi
