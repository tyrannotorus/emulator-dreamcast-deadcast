cmake_minimum_required(VERSION 3.24)
project(deadcast_recomp VERSION 1.0.0 LANGUAGES C CXX)

# Deadcast's recomp modules: recompiled builds of single Dreamcast games (frontend/recomp.h), each an
# executable Deadcast runs instead of its emulation. engine/ translates a game's SH-4 code to C++ at
# build time and provides the runtime it runs on (from Rez Recompiled); src/framework/ is the port
# layer every module shares, Deadcast's menu included (src/framework/deadcast.cpp, ../frontend); each
# module's folder (rez/) holds its game's description (game/<id>.toml) and its own native code.
# A module is built from its builder's own disc: shell/brick/recomp.sh.

# Engine defaults (set before the engine's own option() calls, which then keep them).
set(DREAM_WERROR OFF CACHE BOOL "Warnings as errors in engine targets")
# The generated code is split once per hardware thread so it compiles in parallel;
# -DDREAM_GEN_SHARDS=1 keeps it in one file.
cmake_host_system_information(RESULT _dc_threads QUERY NUMBER_OF_LOGICAL_CORES)
if(_dc_threads LESS 1)
  set(_dc_threads 1)
elseif(_dc_threads GREATER 64)
  set(_dc_threads 64)
endif()
set(DREAM_GEN_SHARDS ${_dc_threads} CACHE STRING "Parts the generated game code is split into")

# NextUI handhelds (TrimUI Brick): the renderer's window on /dev/fb0 (engine/render,
# DREAM_RENDER_NEXTUI), the handheld's SDL2 for sound, Deadcast's menu. A host build (no
# DREAMCOMP_NEXTUI) is for the translator only.
option(DREAMCOMP_NEXTUI "Build the modules for a NextUI handheld" OFF)
if(DREAMCOMP_NEXTUI)
  set(DREAM_RENDER_NEXTUI ON CACHE BOOL "" FORCE)
endif()
# Every build computes the same floating point: no implicit fused multiply-adds (clang fuses a*b+c
# by default on ARM64, which changes FTRV's and FIPR's rounding). FMAC is fused explicitly with
# std::fmaf (sh4/ops.h). The game's targets get the same in dreamcomp_add_port.
set(DREAMCOMP_FP_OPTIONS $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-ffp-contract=off>
                         $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-ffp-contract=off>)
add_compile_options(${DREAMCOMP_FP_OPTIONS})
add_subdirectory(engine)
if(NOT DREAMCOMP_NEXTUI)
  return()
endif()
include(cmake/Port.cmake)
# The engine's warnings for the port layer too (dream_target_defaults also changes floating point,
# which the port layer keeps as it is).
set(DREAMCOMP_WARNINGS
    "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wno-sign-conversion>")

# Deadcast's frontend (its menu and pages, ../frontend) with Deadcast's FreeType, the device's libcurl
# (shell/brick/devlibs.sh) and the sysroot's zlib
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
add_subdirectory(${PROJECT_SOURCE_DIR}/../core/deps/freetype ${CMAKE_BINARY_DIR}/freetype EXCLUDE_FROM_ALL)
add_subdirectory(${PROJECT_SOURCE_DIR}/../frontend ${CMAKE_BINARY_DIR}/frontend)

# The port layer linked into the game executable. An OBJECT library, because its extensions
# register themselves from static constructors that a static library would drop.
add_library(dreamcomp_core OBJECT src/framework/core.cpp src/framework/settings.cpp
                                  src/framework/hud.cpp src/framework/sharp2d.cpp
                                  src/framework/deadcast.cpp)
target_include_directories(dreamcomp_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dreamcomp_core PUBLIC dream::runtime dream::render deadcast_frontend)
target_compile_features(dreamcomp_core PUBLIC cxx_std_20)
target_compile_options(dreamcomp_core PRIVATE ${DREAMCOMP_WARNINGS})
target_compile_definitions(dreamcomp_core PRIVATE DREAMCOMP_NEXTUI)
if(TARGET dream_render_vk)
  target_link_libraries(dreamcomp_core PUBLIC dream::render_vk)
  target_compile_definitions(dreamcomp_core PRIVATE DREAM_WITH_RENDERER)
endif()

# The modules
add_subdirectory(rez/game)
