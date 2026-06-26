# Toolchain file for building OnlineFix.dll with mingw-w64 on Linux/macOS.
#
#   cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake
#   cmake --build build-mingw
#
# Override the prefix for a 32-bit toolchain if ever targeting an x86 EOS game:
#   cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake \
#         -DMINGW_PREFIX=i686-w64-mingw32-

set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_PREFIX "x86_64-w64-mingw32-" CACHE STRING "mingw-w64 toolchain prefix")

set(CMAKE_C_COMPILER   "${MINGW_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${MINGW_PREFIX}g++")
set(CMAKE_RC_COMPILER  "${MINGW_PREFIX}windres")
set(CMAKE_AR           "${MINGW_PREFIX}ar"      CACHE FILEPATH "")
set(CMAKE_RANLIB       "${MINGW_PREFIX}ranlib"  CACHE FILEPATH "")

# Standard cross-compile lookup behavior: programs from host, libs and headers
# from target sysroot only.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
