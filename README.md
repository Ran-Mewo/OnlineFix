# OnlineFix

A companion DLL for [OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool) that fixes online play for games whose matchmaking runs outside Steam.

OpenSteamTool itself ships no payload — it only provides the generic injection layer. This OnlineFix DLL is loaded by that layer directly into the game process when it opens its Steam pipe.

## Build

### Visual Studio 2022 (Windows, primary)
Requires CMake 3.20+ and Visual Studio 2022 (MSVC x64).
```powershell
cmake -B build -A x64
cmake --build build --config Release
```
Output: `build/Release/OnlineFix.dll`.

### Cross-compile (Linux/macOS)
Requires CMake, Ninja (or `make`), and mingw-w64 (`x86_64-w64-mingw32-*`).
```sh
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake
cmake --build build-mingw
```
Output: `build-mingw/OnlineFix.dll`. Add `-DCMAKE_BUILD_TYPE=Debug` to enable
payload logging (mirrors the VS Debug config).

## Usage
1. Drop `OnlineFix.dll` next to `steam.exe` (or anywhere; use an absolute path below).
2. Add an entry to `opensteamtool.toml`:
   ```toml
   [[inject]]
   path = "OnlineFix.dll"
   when_cmdline = "-onlinefix"
   ```
3. Launch the game with `-onlinefix` in its Steam launch options.

## Disclaimer
For research and educational purposes only. You are responsible for complying with local laws, platform terms of service, and software licenses.
