cmake_minimum_required(VERSION 3.20)
project(OnlineFix VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static runtime so the shipped DLL has no extra runtime dependencies.
# MSVC: /MT via CMAKE_MSVC_RUNTIME_LIBRARY. MinGW: -static-libgcc/-libstdc++
# applied as link options on the OnlineFix target below.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ── Detours (fetched, built static) ────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    detours
    GIT_REPOSITORY https://github.com/microsoft/Detours.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake-noop  # repo ships no CMake build of its own
)
FetchContent_MakeAvailable(detours)

add_library(detours STATIC
    ${detours_SOURCE_DIR}/src/detours.cpp
    ${detours_SOURCE_DIR}/src/modules.cpp
    ${detours_SOURCE_DIR}/src/disasm.cpp
    ${detours_SOURCE_DIR}/src/image.cpp
    ${detours_SOURCE_DIR}/src/creatwth.cpp
    ${detours_SOURCE_DIR}/src/disolx64.cpp  # EOS shipping games are x64
)
target_include_directories(detours PUBLIC "${detours_SOURCE_DIR}/src")
set_target_properties(detours PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(detours PRIVATE /w)
else()
    # MinGW / other GCC-like fronts: silence Detours' upstream noise.
    target_compile_options(detours PRIVATE -w)
endif()

# ── OnlineFix.dll ──────────────────────────────────────────────────
add_library(OnlineFix SHARED
    src/dllmain.cpp
    src/EosBridge.cpp
    src/PayloadLog.cpp
)
set_target_properties(OnlineFix PROPERTIES PREFIX "" OUTPUT_NAME "OnlineFix")
target_include_directories(OnlineFix PRIVATE src)
# psapi: EnumProcessModules/GetModuleBaseNameW. advapi32: registry access
# for the payload-log Steam-path lookup. MSVC links advapi32 automatically;
# MinGW needs it spelled out.
target_link_libraries(OnlineFix PRIVATE detours psapi advapi32)

if(MINGW)
    # GetProcAddress reinterpret_casts trip -Wcast-function-type under GCC;
    # the pattern is intentional and harmless.
    target_compile_options(OnlineFix PRIVATE -Wno-cast-function-type)
    # Match the MSVC /MT outcome: no extra runtime DLL deps at load time.
    target_link_options(OnlineFix PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Logging is compiled in only for Debug, same convention as OpenSteamTool.
target_compile_definitions(OnlineFix PRIVATE
    $<$<CONFIG:Debug>:OPENSTEAMTOOL_LOGGING_ENABLED>
)
if(MINGW)
    # mingw-w64 gates localtime_s behind the C11 Annex K opt-in.
    target_compile_definitions(OnlineFix PRIVATE
        $<$<CONFIG:Debug>:__STDC_WANT_LIB_EXT1__=1>
    )
endif()
