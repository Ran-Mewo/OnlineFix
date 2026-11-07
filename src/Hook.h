#pragma once

// ─────────────────────────────────────────────────────────────────
//  Hook — attach/detach a batch of Detours in one transaction, under a process-wide lock. Several bridges install hooks from their own threads
//  (Mono's loader thread, the dll-load worker); Detours transactions are process-global, so two running at once corrupt each other. This serialises
//  them. Each entry is {trampoline, detour}: the trampoline holds the real target going in and the callable original coming out, exactly like
//  DetourAttach.
// ─────────────────────────────────────────────────────────────────

#include <initializer_list>
#include <utility>

namespace Hook {

    using Pair = std::pair<void**, void*>;

    // Returns true only if the transaction committed. An empty list is a no-op success.
    bool Attach(std::initializer_list<Pair> hooks);
    bool Detach(std::initializer_list<Pair> hooks);

    // Same, over a runtime-built set (NativeNet decides which stacks to hook at call time).
    bool Attach(const Pair* hooks, size_t count);

} // namespace Hook
