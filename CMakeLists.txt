cmake_minimum_required(VERSION 3.20)
project(OnlineFix VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static runtime so the shipped DLL has no extra runtime dependencies.
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
    GIT_TAG        1dbd4ea281d73d478aa8d09a8b83fbd265f715f7  # pinned commit: a moving main could change the ABI/build out from under us
    GIT_SHALLOW    FALSE
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
    target_compile_options(detours PRIVATE -w)
endif()

# ── OnlineFix.dll ──────────────────────────────────────────────────
add_library(OnlineFix SHARED
    src/dllmain.cpp
    src/EosBridge.cpp
    src/MonoBridge.cpp
    src/NativeNet.cpp
    src/PlayFabBridge.cpp
    src/PhotonBridge.cpp
    src/PhotonProtocol.cpp
    src/PhotonEnet.cpp
    src/PhotonServer.cpp
    src/PhotonEmulator.cpp
    src/AppIdStrategy.cpp
    src/SteamRelay.cpp
    # src/SteamAuthBridge.cpp   # disabled: a 480 session gets a real auth ticket (SteamAuthBridge.cpp.disabled)
    src/SteamApi.cpp
    src/SteamEnv.cpp
    src/Hook.cpp
    src/PayloadLog.cpp
)
set_target_properties(OnlineFix PROPERTIES PREFIX "" OUTPUT_NAME "OnlineFix")
target_include_directories(OnlineFix PRIVATE src)
# advapi32 (registry) is implicit under MSVC but must be explicit for MinGW.
# HTTP stacks (winhttp/wininet) and Steam APIs are resolved at runtime via
# GetProcAddress, so they are deliberately NOT linked here — that keeps them
# out of games that don't use them and makes presence checks meaningful.
target_link_libraries(OnlineFix PRIVATE detours psapi advapi32)

if(MINGW)
    target_compile_options(OnlineFix PRIVATE -Wno-cast-function-type)  # GetProcAddress casts
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

# ── Standalone Photon protocol self-test ───────────────────────────
# Builds only when ONLINEFIX_BUILD_TESTS is set. Checks the Photon codecs and emulator without a game or injection.
option(ONLINEFIX_BUILD_TESTS "Build OnlineFix unit tests" OFF)
if(ONLINEFIX_BUILD_TESTS)
    add_executable(PhotonProtocolTest
        src/PhotonProtocol.cpp
        src/PhotonEnet.cpp
        test/PhotonProtocolTest.cpp
    )
    target_include_directories(PhotonProtocolTest PRIVATE src)

    # The whole serverless Photon stack driven end to end by scripted clients, SteamRelay stubbed in the test.
    add_executable(PhotonEmulatorTest
        src/PhotonEmulator.cpp
        src/PhotonServer.cpp
        src/PhotonProtocol.cpp
        src/PhotonEnet.cpp
        src/PayloadLog.cpp
        test/PhotonEmulatorTest.cpp
    )
    target_include_directories(PhotonEmulatorTest PRIVATE src)
    target_compile_definitions(PhotonEmulatorTest PRIVATE OPENSTEAMTOOL_LOGGING_ENABLED)
endif()
