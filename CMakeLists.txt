# Deadcast, for the TrimUI Brick (NextUI tg5040, aarch64) only: built by
# shell/brick/build.sh with the cross toolchain (shell/brick/tg5040.cmake). Vulkan renders
# (core/rend/vulkan), SDL2 (the device's) reads the pad and plays the audio, the ARM64 dynarec runs
# the SH4.
cmake_minimum_required(VERSION 3.22.1)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
	set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM} CACHE STRING "Compiler launcher for C.")
	set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM} CACHE STRING "Compiler launcher for CXX.")
endif()

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/shell/cmake")

project(deadcast VERSION 1.2.0)
# no path of the build machine in the binary
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
	message(FATAL_ERROR "Deadcast builds for the Brick (aarch64) only: shell/brick/build.sh")
endif()

option(ENABLE_LOG "Enable full logging" OFF)
option(ENABLE_GDB_SERVER "Build with GDB debugging support" OFF)
option(ENABLE_DC_PROFILER "Build with support for target machine (SH4) profiler" OFF)
option(ENABLE_FC_PROFILER "Build with support for host app (Flycast) profiler" OFF)
set(USE_VULKAN ON)

include(CMakeRC)

# The version, and the commit built
find_package(Git)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
	execute_process(
		COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE GIT_HASH
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
else()
	set(GIT_HASH "000000000")
endif()
set(GIT_VERSION "${PROJECT_VERSION}-${GIT_HASH}")
string(TIMESTAMP BUILD_TIMESTAMP UTC)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/core/version.h.in" "${CMAKE_CURRENT_SOURCE_DIR}/core/version.h" @ONLY)

add_executable(${PROJECT_NAME} core/emulator.cpp)

target_compile_options(${PROJECT_NAME} PRIVATE
		$<$<COMPILE_LANGUAGE:CXX>:-fno-strict-aliasing>
		$<$<COMPILE_LANGUAGE:CXX>:-Wall>)
target_compile_definitions(${PROJECT_NAME} PRIVATE
		$<$<BOOL:${ENABLE_LOG}>:DEBUGFAST>
		PAGE_SIZE=4096
		FLYCAST_DATADIR="/usr/local/share/deadcast/"
		FLYCAST_SYSCONFDIR="/usr/local/etc/deadcast/")
target_include_directories(${PROJECT_NAME} PRIVATE core core/deps core/deps/stb core/deps/json core/deps/asio/asio/include)

option(BUILD_SHARED_LIBS "Build shared library" OFF)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "Build the xxhsum binary")
add_subdirectory(core/deps/xxHash/cmake_unofficial)
target_link_libraries(${PROJECT_NAME} PRIVATE xxHash::xxhash)

add_subdirectory(core/deps/glm)
target_link_libraries(${PROJECT_NAME} PRIVATE glm::glm)

# The sysroot's: ALSA, SDL2 (shell/brick/cmake), zlib; the device's curl (shell/brick/devlibs.sh)
find_package(ALSA REQUIRED)
target_compile_definitions(${PROJECT_NAME} PRIVATE USE_ALSA)
target_include_directories(${PROJECT_NAME} PRIVATE ${ALSA_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${ALSA_LIBRARIES})

find_package(SDL2 2.0.18 REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2)
target_compile_definitions(${PROJECT_NAME} PRIVATE USE_SDL USE_SDL_AUDIO)
add_subdirectory(core/sdl)

find_package(CURL REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)

find_package(ZLIB REQUIRED)
set(WITH_SYSTEM_ZLIB ON CACHE BOOL "Use system provided zlib library")
target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)

find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads rt ${CMAKE_DL_LIBS})

# CHD: libchdr with its zstd
option(ZSTD_BUILD_SHARED "BUILD SHARED LIBRARIES" OFF)
option(ZSTD_BUILD_PROGRAMS "BUILD PROGRAMS" OFF)
option(ZSTD_LEGACY_SUPPORT "LEGACY SUPPORT" OFF)
add_subdirectory(core/deps/libchdr/deps/zstd-1.5.6/build/cmake EXCLUDE_FROM_ALL)
target_link_libraries(${PROJECT_NAME} PRIVATE libzstd_static)
add_subdirectory(core/deps/libchdr EXCLUDE_FROM_ALL)
target_link_libraries(${PROJECT_NAME} PRIVATE chdr-static)
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/libchdr/include)
target_sources(${PROJECT_NAME} PRIVATE
		core/deps/chdpsr/cdipsr.cpp
		core/deps/chdpsr/cdipsr.h)

add_subdirectory(core/deps/nowide EXCLUDE_FROM_ALL)
target_link_libraries(${PROJECT_NAME} PRIVATE nowide::nowide)

option(UPNPC_BUILD_SHARED "Build shared library" OFF)
option(UPNPC_BUILD_TESTS "Build test executables" OFF)
option(UPNPC_BUILD_SAMPLE "Build sample executables" OFF)
option(UPNPC_NO_INSTALL "Disable installation" ON)
add_subdirectory(core/deps/miniupnpc)
target_link_libraries(${PROJECT_NAME} PRIVATE miniupnpc::miniupnpc)

option(ENABLE_COMMONCRYPTO "Enable use of CommonCrypto" OFF)
option(ENABLE_GNUTLS "Enable use of GnuTLS" OFF)
option(ENABLE_MBEDTLS "Enable use of mbed TLS" OFF)
option(ENABLE_OPENSSL "Enable use of OpenSSL" OFF)
option(ENABLE_WINDOWS_CRYPTO "Enable use of Windows cryptography libraries" OFF)
option(ENABLE_BZIP2 "Enable use of BZip2" OFF)
option(ENABLE_LZMA "Enable use of LZMA" OFF)
option(ENABLE_ZSTD "Enable use of Zstandard" ON)
option(BUILD_TOOLS "Build tools in the src directory (zipcmp, zipmerge, ziptool)" OFF)
option(BUILD_REGRESS "Build regression tests" OFF)
option(BUILD_EXAMPLES "Build examples" OFF)
option(BUILD_DOC "Build documentation" OFF)
option(LIBZIP_DO_INSTALL "Install libzip and the related files" OFF)
add_subdirectory(core/deps/libzip EXCLUDE_FROM_ALL)
target_link_libraries(${PROJECT_NAME} PRIVATE libzip::zip)

set(TINYGETTEXT_WO_ICONV ON)
add_subdirectory(core/deps/tinygettext EXCLUDE_FROM_ALL)
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/tinygettext/include)
target_link_libraries(${PROJECT_NAME} PRIVATE tinygettext::tinygettext)

# the modem's and the broadband adapter's network
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/picotcp/include core/deps/picotcp/modules)
target_sources(${PROJECT_NAME} PRIVATE
		core/deps/picotcp/include/arch/pico_linux.h
		core/deps/picotcp/include/arch/pico_posix.h
		core/deps/picotcp/include/heap.h
		core/deps/picotcp/include/pico_addressing.h
		core/deps/picotcp/include/pico_config.h
		core/deps/picotcp/include/pico_constants.h
		core/deps/picotcp/include/pico_defines.h
		core/deps/picotcp/include/pico_device.h
		core/deps/picotcp/include/pico_eth.h
		core/deps/picotcp/include/pico_frame.h
		core/deps/picotcp/include/pico_md5.h
		core/deps/picotcp/include/pico_module_eth.h
		core/deps/picotcp/include/pico_protocol.h
		core/deps/picotcp/include/pico_queue.h
		core/deps/picotcp/include/pico_socket.h
		core/deps/picotcp/include/pico_socket_multicast.h
		core/deps/picotcp/include/pico_stack.h
		core/deps/picotcp/include/pico_tree.h
		core/deps/picotcp/modules/pico_arp.c
		core/deps/picotcp/modules/pico_dev_ppp.c
		core/deps/picotcp/modules/pico_dhcp_common.c
		core/deps/picotcp/modules/pico_dhcp_common.h
		core/deps/picotcp/modules/pico_dhcp_server.c
		core/deps/picotcp/modules/pico_dhcp_server.h
		core/deps/picotcp/modules/pico_dns_client.c
		core/deps/picotcp/modules/pico_dns_common.c
		core/deps/picotcp/modules/pico_ethernet.c
		core/deps/picotcp/modules/pico_fragments.c
		core/deps/picotcp/modules/pico_icmp4.c
		core/deps/picotcp/modules/pico_ipv4.c
		core/deps/picotcp/modules/pico_socket_tcp.c
		core/deps/picotcp/modules/pico_socket_udp.c
		core/deps/picotcp/modules/pico_strings.c
		core/deps/picotcp/modules/pico_tcp.c
		core/deps/picotcp/modules/pico_udp.c
		core/deps/picotcp/stack/pico_device.c
		core/deps/picotcp/stack/pico_frame.c
		core/deps/picotcp/stack/pico_md5.c
		core/deps/picotcp/stack/pico_protocol.c
		core/deps/picotcp/stack/pico_socket.c
		core/deps/picotcp/stack/pico_socket_multicast.c
		core/deps/picotcp/stack/pico_stack.c
		core/deps/picotcp/stack/pico_tree.c)

target_compile_definitions(${PROJECT_NAME} PRIVATE _7ZIP_ST)
target_sources(${PROJECT_NAME} PRIVATE core/deps/lzma/7zArcIn.c core/deps/lzma/7zBuf.c core/deps/lzma/7zCrc.c core/deps/lzma/7zCrcOpt.c core/deps/lzma/7zDec.c core/deps/lzma/7zFile.c core/deps/lzma/7zStream.c core/deps/lzma/Alloc.c core/deps/lzma/Bcj2.c core/deps/lzma/Bra86.c core/deps/lzma/Bra.c core/deps/lzma/BraIA64.c core/deps/lzma/CpuArch.c core/deps/lzma/Delta.c core/deps/lzma/LzFind.c core/deps/lzma/Lzma2Dec.c core/deps/lzma/Lzma86Dec.c core/deps/lzma/Lzma86Enc.c core/deps/lzma/LzmaDec.c core/deps/lzma/LzmaEnc.c core/deps/lzma/LzmaLib.c core/deps/lzma/Sort.c)
add_subdirectory(core/deps/libelf)
target_link_libraries(${PROJECT_NAME} PRIVATE elf)

# the menus
target_compile_definitions(${PROJECT_NAME} PRIVATE
	IMGUI_DISABLE_DEMO_WINDOWS
	IMGUI_DISABLE_DEBUG_TOOLS
	IMGUI_DEFINE_MATH_OPERATORS)
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/imgui core/deps/imgui/backends)
target_sources(${PROJECT_NAME} PRIVATE
	core/deps/imgui/imgui.cpp
	core/deps/imgui/imgui_demo.cpp
	core/deps/imgui/imgui_draw.cpp
	core/deps/imgui/imgui_cjk.cpp
	core/deps/imgui/imgui_stdlib.cpp
	core/deps/imgui/imgui_tables.cpp
	core/deps/imgui/imgui_widgets.cpp
	core/deps/imgui/misc/freetype/imgui_freetype.cpp)
set(IMGUI_BACKENDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/core/deps/imgui/backends")

set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory(core/deps/freetype EXCLUDE_FROM_ALL)
target_link_libraries(${PROJECT_NAME} PRIVATE freetype)

if(ENABLE_FC_PROFILER)
	target_include_directories(${PROJECT_NAME} PRIVATE core/deps/implot)
	target_sources(${PROJECT_NAME} PRIVATE
		core/deps/implot/implot.h
		core/deps/implot/implot.cpp
		core/deps/implot/implot_internal.h
		core/deps/implot/implot_items.cpp)
endif()
target_sources(${PROJECT_NAME} PRIVATE core/deps/xbrz/xbrz.cpp)
target_sources(${PROJECT_NAME} PRIVATE core/deps/md5/md5.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE flycast::res)

add_subdirectory(core/archive)
add_subdirectory(core/cfg)
add_subdirectory(core/hw)
add_subdirectory(core/imgread)
add_subdirectory(core/input)
add_subdirectory(core/log)
add_subdirectory(core/network)
add_subdirectory(core/oslib)
add_subdirectory(core/audio)
add_subdirectory(core/reios)
add_subdirectory(core/deadcast)
# the menu and pages, shared with the recomp modules
set(DEADUI_APP_NAME Deadcast)
set(DEADUI_HOME_ENV DEADCAST_HOME)
set(DEADUI_LINK_PREFIX dclink)
set(DEADUI_STORE_URL https://raw.githubusercontent.com/tyrannotorus/emulator-dreamcast-deadcast/store/catalog.cfg)
set(DEADUI_CONSOLE dreamcast)
add_subdirectory(ui)
target_link_libraries(${PROJECT_NAME} PRIVATE deadui)
add_subdirectory(core/achievements)
add_subdirectory(core/ui)
add_subdirectory(core/lua)
add_subdirectory(core/profiler)
add_subdirectory(core/wsi)

include(resources/resources.cmake)

target_sources(${PROJECT_NAME} PRIVATE
		core/linux/common.cpp
		core/linux/context.cpp
		core/linux/posix_vmem.cpp
		core/linux/unwind_info.cpp
		core/linux-dist/evdev.cpp
		core/linux-dist/evdev.h
		core/linux-dist/icon.h
		core/linux-dist/main.cpp)

# RetroAchievements
target_sources(${PROJECT_NAME} PRIVATE
		core/deps/rcheevos/src/rc_client_raintegration.c
		core/deps/rcheevos/src/rc_client.c
		core/deps/rcheevos/src/rc_compat.c
		core/deps/rcheevos/src/rc_util.c
		core/deps/rcheevos/src/rc_version.c
		core/deps/rcheevos/src/rapi/rc_api_common.c
		core/deps/rcheevos/src/rapi/rc_api_editor.c
		core/deps/rcheevos/src/rapi/rc_api_info.c
		core/deps/rcheevos/src/rapi/rc_api_runtime.c
		core/deps/rcheevos/src/rapi/rc_api_user.c
		core/deps/rcheevos/src/rcheevos/alloc.c
		core/deps/rcheevos/src/rcheevos/condition.c
		core/deps/rcheevos/src/rcheevos/condset.c
		core/deps/rcheevos/src/rcheevos/consoleinfo.c
		core/deps/rcheevos/src/rcheevos/format.c
		core/deps/rcheevos/src/rcheevos/lboard.c
		core/deps/rcheevos/src/rcheevos/memref.c
		core/deps/rcheevos/src/rcheevos/operand.c
		core/deps/rcheevos/src/rcheevos/rc_validate.c
		core/deps/rcheevos/src/rcheevos/richpresence.c
		core/deps/rcheevos/src/rcheevos/runtime_progress.c
		core/deps/rcheevos/src/rcheevos/runtime.c
		core/deps/rcheevos/src/rcheevos/trigger.c
		core/deps/rcheevos/src/rcheevos/value.c
		core/deps/rcheevos/src/rhash/aes.c
		core/deps/rcheevos/src/rhash/cdreader.c
		core/deps/rcheevos/src/rhash/hash.c
		core/deps/rcheevos/src/rhash/md5.c
		core/deps/rcheevos/src/rurl/url.c)
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/rcheevos/include)
target_compile_definitions(${PROJECT_NAME} PRIVATE USE_RACHIEVEMENTS RC_DISABLE_LUA)

target_sources(${PROJECT_NAME} PRIVATE
		core/build.h
		core/chd.h
		core/cheats.cpp
		core/cheats.h
		core/emulator.h
		core/nullDC.cpp
		core/serialize.cpp
		core/serialize.h
		core/stbi.h
		core/stdclass.cpp
		core/stdclass.h
		core/types.h
		core/debug/gdb_server.h)
if(ENABLE_GDB_SERVER)
	target_sources(${PROJECT_NAME} PRIVATE core/debug/gdb_server.cpp)
	target_compile_definitions(${PROJECT_NAME} PRIVATE GDB_SERVER)
endif()

# Vulkan: the Brick's loader is opened at run time; the headers are the host's (vulkan-headers,
# copied by shell/brick/build.sh: the sysroot's are too old), no window system
target_compile_definitions(${PROJECT_NAME} PUBLIC VK_NO_PROTOTYPES)
if(NOT DEADCAST_VULKAN_INCLUDE)
	message(FATAL_ERROR "DEADCAST_VULKAN_INCLUDE: the folder with the Vulkan headers (vulkan/vulkan.hpp)")
endif()
target_include_directories(${PROJECT_NAME} PRIVATE "${DEADCAST_VULKAN_INCLUDE}")
add_subdirectory(core/rend)

# the SH4 dynarec, ARM64
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/vixl)
target_sources(${PROJECT_NAME} PRIVATE
	core/deps/vixl/aarch64/abi-aarch64.h
	core/deps/vixl/aarch64/assembler-aarch64.cc
	core/deps/vixl/aarch64/assembler-aarch64.h
	core/deps/vixl/aarch64/assembler-sve-aarch64.cc
	core/deps/vixl/aarch64/constants-aarch64.h
	core/deps/vixl/aarch64/instructions-aarch64.cc
	core/deps/vixl/aarch64/instructions-aarch64.h
	core/deps/vixl/aarch64/macro-assembler-aarch64.cc
	core/deps/vixl/aarch64/macro-assembler-aarch64.h
	core/deps/vixl/aarch64/macro-assembler-sve-aarch64.cc
	core/deps/vixl/aarch64/operands-aarch64.cc
	core/deps/vixl/aarch64/operands-aarch64.h
	core/deps/vixl/aarch64/registers-aarch64.cc
	core/deps/vixl/aarch64/registers-aarch64.h
	core/deps/vixl/aarch64/pointer-auth-aarch64.cc
	core/deps/vixl/aarch64/simulator-constants-aarch64.h
	core/deps/vixl/assembler-base-vixl.h
	core/deps/vixl/code-buffer-vixl.cc
	core/deps/vixl/code-buffer-vixl.h
	core/deps/vixl/code-generation-scopes-vixl.h
	core/deps/vixl/compiler-intrinsics-vixl.cc
	core/deps/vixl/compiler-intrinsics-vixl.h
	core/deps/vixl/cpu-features.cc
	core/deps/vixl/cpu-features.h
	core/deps/vixl/globals-vixl.h
	core/deps/vixl/invalset-vixl.h
	core/deps/vixl/macro-assembler-interface.h
	core/deps/vixl/platform-vixl.h
	core/deps/vixl/pool-manager.h
	core/deps/vixl/pool-manager-impl.h
	core/deps/vixl/utils-vixl.cc
	core/deps/vixl/utils-vixl.h)
target_sources(${PROJECT_NAME} PRIVATE core/rec-ARM64/rec_arm64.cpp core/rec-ARM64/arm64_regalloc.h)

# netplay
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/ggpo/include core/deps/ggpo/lib/ggpo)
target_sources(${PROJECT_NAME} PRIVATE
	core/deps/ggpo/lib/ggpo/bitvector.cpp
	core/deps/ggpo/lib/ggpo/bitvector.h
	core/deps/ggpo/lib/ggpo/game_input.cpp
	core/deps/ggpo/lib/ggpo/game_input.h
	core/deps/ggpo/lib/ggpo/input_queue.cpp
	core/deps/ggpo/lib/ggpo/input_queue.h
	core/deps/ggpo/lib/ggpo/main.cpp
	core/deps/ggpo/lib/ggpo/platform_linux.cpp
	core/deps/ggpo/lib/ggpo/platform_linux.h
	core/deps/ggpo/lib/ggpo/poll.cpp
	core/deps/ggpo/lib/ggpo/ggpo_poll.h
	core/deps/ggpo/lib/ggpo/ring_buffer.h
	core/deps/ggpo/lib/ggpo/static_buffer.h
	core/deps/ggpo/lib/ggpo/sync.cpp
	core/deps/ggpo/lib/ggpo/sync.h
	core/deps/ggpo/lib/ggpo/timesync.cpp
	core/deps/ggpo/lib/ggpo/timesync.h
	core/deps/ggpo/lib/ggpo/ggpo_types.h
	core/deps/ggpo/lib/ggpo/backends/backend.h
	core/deps/ggpo/lib/ggpo/backends/p2p.cpp
	core/deps/ggpo/lib/ggpo/backends/p2p.h
	core/deps/ggpo/lib/ggpo/backends/spectator.cpp
	core/deps/ggpo/lib/ggpo/backends/spectator.h
	core/deps/ggpo/lib/ggpo/backends/synctest.cpp
	core/deps/ggpo/lib/ggpo/backends/synctest.h
	core/deps/ggpo/lib/ggpo/network/udp_msg.h
	core/deps/ggpo/lib/ggpo/network/udp_proto.cpp
	core/deps/ggpo/lib/ggpo/network/udp_proto.h
	core/deps/ggpo/lib/ggpo/network/udp.cpp
	core/deps/ggpo/lib/ggpo/network/udp.h)
