# OnlineFix

A companion DLL for [OpenSteamTool](https://github.com/Ran-Mewo/OpenSteamTool) that fixes online play for games whose matchmaking runs outside Steam.

Games that matchmake through Steam lobbies only need OpenSteamTool's `-onlinefix`. OnlineFix covers the rest: OpenSteamTool loads it into the game through `[[inject]]`, and it picks the fix for each backend from what the game's modules export, so it works across engine and SDK versions.

| Backend | What OnlineFix does | Tested on |
|---------|---------------------|-----------|
| **EOS** | Logs in to EOS Connect with an anonymous Device ID | Backrooms: Escape Together |
| **Photon** | Runs the Photon server inside the game. The room's host serves it and joiners reach the host over the Steam relay, so there's no Photon Cloud or port forwarding | R.E.P.O, Content Warning |
| **PlayFab** (Unity/Mono) | Reroutes `PlayFabClientAPI.LoginWithSteam` to `LoginWithCustomID`, keyed on the Steam ID | Not yet |
| **PlayFab** (Unreal/native) | Reroutes the same login at the TLS layer, under the game's statically-linked OpenSSL, so it works with whatever HTTP stack the game ships | Grounded 2 |

It also tells OpenSteamTool which AppId the game should report, through the `OpenSteamTool_AppIdStrategy_<pid>` shared memory block: the real one for EOS, whose ticket auth needs it, and 480 for Photon.

Notes on the Photon protocol the emulator speaks are in [research/photon](research/photon/).

## Install
1. Open PowerShell and run:
   ```powershell
   irm https://raw.githubusercontent.com/Ran-Mewo/scripts/refs/heads/main/opensteam-install.ps1 | iex
   ```
   It closes Steam and installs OpenSteamTool, OnlineFix, Millennium and the LuaTools plugin.
2. To download a game, open its Steam store page, click **Add via LuaTools** at the top, then download it from your library.
3. To play online, add `-onlinefix` to the game's launch options.

> [!NOTE]
> If Windows Defender gets in the way (unlikely), add your Steam folder (`C:\Program Files (x86)\Steam`) to its exclusions and temporarily turn off real-time protection.

## Build

### Visual Studio 2022 (Windows, primary)
Requires CMake 3.20+ and Visual Studio 2022 (MSVC x64).
```powershell
cmake -B build -A x64
cmake --build build --config Release
```
Output: `build/Release/OnlineFix.dll`.

### Tests
```powershell
cmake -B build -A x64 -DONLINEFIX_BUILD_TESTS=ON
cmake --build build --config Debug
build/Debug/PhotonProtocolTest.exe   # Photon wire format round-trips
build/Debug/PhotonEmulatorTest.exe   # scripted clients through the whole Photon flow, other machines included
```

### Cross-compile (Linux/macOS)
Requires CMake, Ninja (or `make`), and mingw-w64 (`x86_64-w64-mingw32-*`).
```sh
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake
cmake --build build-mingw
```
Output: `build-mingw/OnlineFix.dll`. Add `-DCMAKE_BUILD_TYPE=Debug` to enable logging, as in the VS Debug config.

## Usage
1. Drop `OnlineFix.dll` next to `steam.exe` (or anywhere; use an absolute path below).
2. Add an entry to `opensteamtool.toml`:
   ```toml
   [[inject]]
   path = "OnlineFix.dll"
   when_cmdline = "-onlinefix"
   all_games = true
   ```
3. Launch the game with `-onlinefix` in its Steam launch options.

Debug builds log each game process to `opensteamtool/payload/<pid>.log` in the Steam folder.

## Disclaimer
For research and educational purposes only. You are responsible for complying with local laws, platform terms of service, and software licenses.
