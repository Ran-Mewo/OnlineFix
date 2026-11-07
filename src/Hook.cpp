#include "Hook.h"
#include "PayloadLog.h"

#include <windows.h>
#include <detours.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace {
    std::mutex g_txMutex;   // Detours transactions are process-global; serialise ours

    bool Run(const Hook::Pair* hooks, size_t count, bool attach) {
        if (!count) return true;
        std::lock_guard<std::mutex> lock(g_txMutex);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        for (size_t i = 0; i < count; ++i) {
            if (attach) DetourAttach(hooks[i].first, hooks[i].second);
            else        DetourDetach(hooks[i].first, hooks[i].second);
        }
        PVOID* failed = nullptr;
        const LONG error = DetourTransactionCommitEx(&failed);
        if (error != NO_ERROR) {
            char at[32];
            std::snprintf(at, sizeof(at), "%p", failed ? *failed : nullptr);
            PayloadLog::Write("Hook: transaction failed, error " + std::to_string(error) + " at " + at);
        }
        return error == NO_ERROR;
    }
}

namespace Hook {

    bool Attach(std::initializer_list<Pair> hooks) { return Run(hooks.begin(), hooks.size(), true); }
    bool Detach(std::initializer_list<Pair> hooks) { return Run(hooks.begin(), hooks.size(), false); }
    bool Attach(const Pair* hooks, size_t count)   { return Run(hooks, count, true); }

} // namespace Hook
