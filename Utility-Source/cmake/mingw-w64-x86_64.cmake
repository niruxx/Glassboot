# Cross-compile from Linux with MinGW-w64 (used to compile-check / build glassboot-cli.exe).
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DGLASSBOOT_BUILD_GUI=OFF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Single self-contained .exe
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
