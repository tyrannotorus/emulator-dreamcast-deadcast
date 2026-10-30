# Cross-build for NextUI's tg5040 handhelds (TrimUI Brick: aarch64 Cortex-A53) with the host's
# clang and lld, against the sysroot and GCC 8.3 runtime of LoveRetro's tg5040 toolchain.
# DEADCAST_TG5040_ROOT names the toolchain's root, the folder holding aarch64-nextui-linux-gnu/
# (shell/brick/toolchain.sh copies it out of the toolchain image).
#   cmake -S . -B <build> -DCMAKE_TOOLCHAIN_FILE=shell/brick/tg5040.cmake -DDEADCAST_TG5040_ROOT=<root>
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
if(NOT DEADCAST_TG5040_ROOT AND DEFINED ENV{DEADCAST_TG5040_ROOT})
	set(DEADCAST_TG5040_ROOT $ENV{DEADCAST_TG5040_ROOT})
endif()
if(NOT EXISTS "${DEADCAST_TG5040_ROOT}/aarch64-nextui-linux-gnu/libc")
	message(FATAL_ERROR "tg5040: DEADCAST_TG5040_ROOT (${DEADCAST_TG5040_ROOT}) is not the toolchain's root")
endif()
# try_compile projects get the toolchain file again; pass the root on to them.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES DEADCAST_TG5040_ROOT)
set(CMAKE_SYSROOT "${DEADCAST_TG5040_ROOT}/aarch64-nextui-linux-gnu/libc")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-nextui-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-nextui-linux-gnu)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_ASM_COMPILER_TARGET aarch64-nextui-linux-gnu)
set(_tg5040_flags "--gcc-toolchain=${DEADCAST_TG5040_ROOT} -mcpu=cortex-a53")
# Sections per function, so the link drops what nothing calls: among it the MIME upload that needs a
# newer libcurl than the device's 7.54 (shell/brick/devlibs.sh).
set(CMAKE_C_FLAGS_INIT "${_tg5040_flags} -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "${_tg5040_flags} -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${_tg5040_flags}")
# The device's libstdc++ is older than the toolchain's headers may assume: link the C++ runtime in.
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_tg5040_flags} -fuse-ld=lld -static-libstdc++ -static-libgcc -Wl,--gc-sections")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_tg5040_flags} -fuse-ld=lld -static-libstdc++ -static-libgcc")
# GCC 8's std::filesystem lives in a library of its own, and glibc 2.28's threads too (before 2.34).
set(CMAKE_CXX_STANDARD_LIBRARIES "-lstdc++fs -lpthread")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
