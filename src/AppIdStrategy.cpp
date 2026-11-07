#include "AppIdStrategy.h"
#include "PayloadLog.h"

#include <windows.h>
#include <cstdio>
#include <mutex>

namespace AppIdStrategy {

    void Set(Strategy s) {
        static std::mutex          mtx;
        static volatile uint32_t*  view = nullptr;
        std::lock_guard<std::mutex> lock(mtx);
        if (!view) {
            char name[64];
            sprintf_s(name, "OpenSteamTool_AppIdStrategy_%lu", GetCurrentProcessId());
            // Pagefile-backed section; the handle is intentionally leaked so the block (and OST's mapped view of it) lives for the whole process.
            HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(uint32_t), name);
            if (hMap)
                view = static_cast<volatile uint32_t*>(MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(uint32_t)));
        }
        if (view) {
            *view = static_cast<uint32_t>(s);
            PayloadLog::Write("AppIdStrategy: set " + std::to_string((uint32_t)s));
        }
    }

} // namespace AppIdStrategy
