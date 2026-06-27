#pragma once

#include <windows.h>
#include <string>

// Per-process line logger for the injected payload. Debug builds only.
// Logs go to <Steam>\opensteamtool\payload\<pid>.log when OST is detected
// (env override, or registry Steam path with the opensteamtool\ directory
// actually present), otherwise to %APPDATA%\onlinefix\<pid>.log (TEMP
// fallback). One file per process.
#ifdef OPENSTEAMTOOL_LOGGING_ENABLED
namespace PayloadLog {
    void Init(HMODULE self);
    void Write(const std::string& line);
}
#else
namespace PayloadLog {
    inline void Init(HMODULE) {}
    inline void Write(const std::string&) {}
}
#endif
