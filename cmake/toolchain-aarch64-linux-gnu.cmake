# Cross-compilation toolchain for Linux AArch64 (ARM64 + NEON)
# Supports aarch64-linux-gnu-gcc/g++ sysroot toolchains cleanly, with automatic
# fallback to `zig cc/c++ -target aarch64-linux-gnu` when GNU cross-compilers
# are not installed locally.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Always populate CMAKE_FIND_ROOT_PATH so ONLY mode never falls back to host /usr/lib64
list(APPEND CMAKE_FIND_ROOT_PATH "/usr/aarch64-linux-gnu")

find_program(OPENAVX_AARCH64_GCC NAMES aarch64-linux-gnu-gcc)
find_program(OPENAVX_AARCH64_GXX NAMES aarch64-linux-gnu-g++)

if(OPENAVX_AARCH64_GCC AND OPENAVX_AARCH64_GXX)
    set(CMAKE_C_COMPILER "${OPENAVX_AARCH64_GCC}")
    set(CMAKE_CXX_COMPILER "${OPENAVX_AARCH64_GXX}")
else()
    find_program(OPENAVX_ZIG NAMES zig)
    if(OPENAVX_ZIG)
        set(CMAKE_C_COMPILER "${OPENAVX_ZIG}")
        set(CMAKE_C_COMPILER_ARG1 "cc -target aarch64-linux-gnu")
        set(CMAKE_CXX_COMPILER "${OPENAVX_ZIG}")
        set(CMAKE_CXX_COMPILER_ARG1 "c++ -target aarch64-linux-gnu")
        # Zig's linker driver segfaults on CMake's `-Xlinker --dependency-file=...`
        set(CMAKE_C_LINKER_DEPFILE_SUPPORTED FALSE CACHE BOOL "" FORCE)
        set(CMAKE_CXX_LINKER_DEPFILE_SUPPORTED FALSE CACHE BOOL "" FORCE)
    else()
        find_program(OPENAVX_CLANG NAMES clang)
        find_program(OPENAVX_CLANGXX NAMES clang++)
        if(OPENAVX_CLANG AND OPENAVX_CLANGXX)
            set(CMAKE_C_COMPILER "${OPENAVX_CLANG}")
            set(CMAKE_C_COMPILER_TARGET "aarch64-linux-gnu")
            set(CMAKE_CXX_COMPILER "${OPENAVX_CLANGXX}")
            set(CMAKE_CXX_COMPILER_TARGET "aarch64-linux-gnu")
        else()
            message(FATAL_ERROR
                "No AArch64 cross-compiler found. Install 'aarch64-linux-gnu-gcc'/'aarch64-linux-gnu-g++' "
                "or 'zig' (for `zig cc -target aarch64-linux-gnu`).")
        endif()
    endif()
endif()

find_program(OPENAVX_QEMU_AARCH64 NAMES qemu-aarch64 qemu-aarch64-static)
if(OPENAVX_QEMU_AARCH64 AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${OPENAVX_QEMU_AARCH64}")
endif()

# Never leak host x86_64 libraries or headers into ARM64 targets
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
