cmake_minimum_required(VERSION 3.20)
project(ff-port C)

# ccache, when installed: the generated code is large and mostly unchanged between builds.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The recompiled code is generated from the user's own copy of the game.
set(FF_EXE "${CMAKE_SOURCE_DIR}/gamedata/pc/Freedom.Exe" CACHE FILEPATH "Path to the retail Freedom.Exe")
if(NOT EXISTS "${FF_EXE}")
    message(FATAL_ERROR "Freedom.Exe not found at ${FF_EXE}; pass -DFF_EXE=/path/to/Freedom.Exe")
endif()

# The port's version: the one place it is set. Vita releases are tagged v<this> on
# FF_UPDATE_REPO (docs/VITA_RELEASE.md); the update check installs a release only when
# its version is higher, so raise it for every release. major.minor.patch, each 0-9 for
# minor and patch (the Vita's APP_VER is NN.NN: 0.1.0 -> 00.10, 1.2.3 -> 01.23).
set(FF_PORT_VERSION "0.1.0")
set(FF_UPDATE_REPO "HenryKun55/ff-vita" CACHE STRING "GitHub owner/repo whose releases the Vita update check reads")
if(NOT FF_PORT_VERSION MATCHES "^([0-9]+)\\.([0-9])\\.([0-9])$")
    message(FATAL_ERROR "FF_PORT_VERSION must be major.minor.patch with one-digit minor and patch")
endif()
math(EXPR FF_VERSION_MAJOR "${CMAKE_MATCH_1}")
if(FF_VERSION_MAJOR LESS 10)
    set(FF_VERSION_MAJOR "0${FF_VERSION_MAJOR}")
endif()
set(FF_APP_VER "${FF_VERSION_MAJOR}.${CMAKE_MATCH_2}${CMAKE_MATCH_3}")

if(DEFINED VITA_ELF_CREATE)
    # PlayStation Vita (vitasdk toolchain file).
    set(VITA TRUE)
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
    set(VITA_TITLEID "FFRD00001" CACHE STRING "Vita title id")
    # ATTRIBUTE2=12: the extended memory mode (about 109 MiB more main memory). The
    # game's 160 MiB of guest memory, the host side and vitaGL do not fit without it.
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
    set(VITA_VERSION "${FF_APP_VER}")
    add_compile_definitions(__vita__)
    add_compile_options(-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums
                        -ffunction-sections -fdata-sections -fno-pic -fno-pie)
    add_link_options(-Wl,-q -Wl,--gc-sections -static -fno-short-enums -Wl,--no-warn-mismatch -Wl,-z,noexecstack)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
file(GLOB RECOMP_TOOLS "${CMAKE_SOURCE_DIR}/tools/recomp/*.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_TOOLS} "${FF_EXE}")
# Diagnostic entry probes (hex guest addresses), e.g. -DFF_PROBES="497620 4E7960".
set(FF_PROBES "" CACHE STRING "Guest functions to instrument with rt_probe")

message(STATUS "Recompiling ${FF_EXE}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "FF_PROBES=${FF_PROBES}"
            "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/recomp/gen.py" "${FF_EXE}" "${GEN_DIR}"
    RESULT_VARIABLE GEN_RESULT
    OUTPUT_VARIABLE GEN_OUTPUT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "recompiler failed:\n${GEN_OUTPUT}")
endif()
message(STATUS "${GEN_OUTPUT}")

file(STRINGS "${GEN_DIR}/files.txt" GEN_FILES)
list(TRANSFORM GEN_FILES PREPEND "${GEN_DIR}/")

add_library(ff_recomp STATIC ${GEN_FILES})
target_include_directories(ff_recomp PUBLIC runtime/include "${GEN_DIR}")
# Guest code relies on x86 semantics: wrapping signed arithmetic, type punning
# and signed char. Without these, GCC (Vita) and clang (macOS) optimize the
# recompiled code differently.
# -O2 by default; -Os makes a smaller eboot (the generated code is most of it).
set(FF_RECOMP_OPT "-O2" CACHE STRING "Optimization flag of the generated code")
target_compile_options(ff_recomp PRIVATE -Wall -Wextra ${FF_RECOMP_OPT} -fwrapv -fno-strict-aliasing -fsigned-char
                       -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
# Generated-code variants (see runtime/include/recomp.h). FF_X87_FLOAT trades x87
# exactness for speed; FF_X87_CHECK verifies the static x87 stack analysis.
option(FF_X87_FLOAT "Single-precision x87 registers in generated code (faster, inexact)" OFF)
option(FF_X87_CHECK "Check statically derived x87 stack effects at run time" OFF)
if(FF_X87_FLOAT)
    target_compile_definitions(ff_recomp PRIVATE FF_X87_FLOAT)
endif()
if(FF_X87_CHECK)
    target_compile_definitions(ff_recomp PRIVATE FF_X87_CHECK)
endif()

file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/src/*.c)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/src/*.c)

add_executable(ff ${RUNTIME_SOURCES} ${SHIM_SOURCES})
target_include_directories(ff PRIVATE runtime/include shim/include "${GEN_DIR}")
target_compile_options(ff PRIVATE -Wall -Wextra -fwrapv -fno-strict-aliasing -fsigned-char)
target_compile_definitions(ff PRIVATE FF_PORT_VERSION="${FF_PORT_VERSION}")
# Sampling profiler (runtime/src/profile.c). No cost when OFF.
option(FF_PROFILE "Build with the sampling profiler" OFF)
if(FF_PROFILE)
    target_compile_definitions(ff_recomp PUBLIC FF_PROFILE)
    target_compile_definitions(ff PRIVATE FF_PROFILE)
    # Store watch (runtime/include/recomp.h RT_WATCH): who writes a watched range.
    # Call counts of every generated function (calls.txt beside the profile).
    option(FF_CALLCOUNT "Count the calls of every generated function" OFF)
    if(FF_CALLCOUNT)
        target_compile_definitions(ff_recomp PUBLIC FF_CALLCOUNT)
        target_compile_definitions(ff PRIVATE FF_CALLCOUNT)
    endif()
    option(FF_WATCH "Report the guest functions storing into a watched range" OFF)
    if(FF_WATCH)
        target_compile_definitions(ff_recomp PUBLIC FF_WATCH)
        target_compile_definitions(ff PRIVATE FF_WATCH)
    endif()
endif()

if(VITA)
    target_include_directories(ff PRIVATE "$ENV{VITASDK}/arm-vita-eabi/include/SDL2")
    # Hot code layout (tools/recomp/hot.py): the most called generated functions first.
    option(FF_HOT_ORDER "Place the hot generated functions together (tools/recomp/hot_order.ld)" ON)
    set(FF_HOT_ORDER_FILE "${CMAKE_SOURCE_DIR}/tools/recomp/hot_order.ld")
    if(FF_HOT_ORDER AND EXISTS "${FF_HOT_ORDER_FILE}")
        target_link_options(ff PRIVATE "-Wl,--section-ordering-file,${FF_HOT_ORDER_FILE}")
        set_property(TARGET ff APPEND PROPERTY LINK_DEPENDS "${FF_HOT_ORDER_FILE}")
    endif()
    set(FF_VITAGL_DIR "${CMAKE_SOURCE_DIR}/deps/vita" CACHE PATH "Prefix of the private vitaGL build")
    if(EXISTS "${FF_VITAGL_DIR}/lib/libvitaGL.a")
        message(STATUS "Using the private vitaGL in ${FF_VITAGL_DIR}")
        target_include_directories(ff BEFORE PRIVATE "${FF_VITAGL_DIR}/include")
        set(FF_VITAGL_LIB "${FF_VITAGL_DIR}/lib/libvitaGL.a")
    else()
        message(STATUS "Private vitaGL not built: using the SDK's (tools/build_vitagl_vita.sh)")
        set(FF_VITAGL_LIB vitaGL)
    endif()
    target_link_libraries(ff PRIVATE ff_recomp
        SDL2 ${FF_VITAGL_LIB} vitashark SceShaccCgExt mathneon taihen_stub
        SceShaccCg_stub SceKernelDmacMgr_stub SceCtrl_stub SceTouch_stub SceHid_stub SceMotion_stub
        SceAudio_stub SceAudioIn_stub SceDisplay_stub SceGxm_stub SceCommonDialog_stub SceSysmodule_stub
        ScePower_stub SceAppMgr_stub SceAppUtil_stub SceIme_stub SceLibKernel_stub SceProcessmgr_stub
        SceNet_stub SceNetCtl_stub SceIofilemgr_stub
        curl ssl crypto png z zstd stdc++ pthread m c)
    # The update check (shim/src/vita_update.c) and its start-up screen (vita_bootui.c).
    target_compile_definitions(ff PRIVATE FF_UPDATE_REPO="${FF_UPDATE_REPO}")
    # The LiveArea pictures (pic0.png is also the boot picture) come from the key art in gamedata/art
    # (or the game's own logo), so they are made here instead of being kept in the repo.
    get_filename_component(FF_GAME_DIR "${FF_EXE}" DIRECTORY)
    set(FF_SCE_SYS "${CMAKE_BINARY_DIR}/sce_sys")
    file(COPY "${CMAKE_SOURCE_DIR}/platform/vita/sce_sys/" DESTINATION "${FF_SCE_SYS}")
    execute_process(
        # No version on the LiveArea: an update from the game doesn't replace sce_sys,
        # so it would go stale (the boot screen shows the running version).
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/make_vita_livearea.py" "${FF_GAME_DIR}" "${FF_SCE_SYS}"
        RESULT_VARIABLE FF_LIVEAREA_RESULT)
    if(NOT FF_LIVEAREA_RESULT EQUAL 0)
        message(FATAL_ERROR "tools/make_vita_livearea.py failed (it needs Pillow: pip install Pillow)")
    endif()
    # The start-up screen's fonts (Oswald, OFL).
    set(FF_BOOT_FONTS "${CMAKE_BINARY_DIR}/vita_font")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/make_vita_boot_font.py" "${FF_BOOT_FONTS}"
        RESULT_VARIABLE FF_FONT_RESULT)
    if(NOT FF_FONT_RESULT EQUAL 0)
        message(FATAL_ERROR "tools/make_vita_boot_font.py failed (it needs Pillow: pip install Pillow)")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/make_vita_livearea.py"
        "${CMAKE_SOURCE_DIR}/tools/make_vita_boot_font.py" "${CMAKE_SOURCE_DIR}/platform/vita/fonts/Oswald.ttf")

    # updater.bin: puts a downloaded release in place of the game's files
    # (platform/vita/updater). A separate program, so the game is closed while its files
    # are replaced; unsafe, as only unsafe homebrew may write to ux0:app.
    set(FF_UPDATER_DIR "${CMAKE_SOURCE_DIR}/platform/vita/updater")
    add_executable(ff_updater "${FF_UPDATER_DIR}/updater.c" "${FF_UPDATER_DIR}/debugScreen.c")
    target_compile_options(ff_updater PRIVATE -Wall -Wextra)
    target_link_libraries(ff_updater PRIVATE png z m
        SceDisplay_stub SceCtrl_stub SceAppMgr_stub ScePower_stub SceLibKernel_stub
        SceKernelThreadMgr_stub SceIofilemgr_stub SceProcessmgr_stub)
    vita_create_self(updater.bin ff_updater UNSAFE)

    vita_create_self(eboot.bin ff UNSAFE)
    vita_create_vpk(ff.vpk ${VITA_TITLEID} eboot.bin
        VERSION ${VITA_VERSION}
        NAME "Freedom Fighters"
        FILE ${FF_SCE_SYS} sce_sys
             "${CMAKE_BINARY_DIR}/updater.bin.out" updater.bin
             # CA certificates for the update check's HTTPS (curl.se/ca, MPL 2.0)
             "${FF_UPDATER_DIR}/cacert.pem" cacert.pem
             "${FF_BOOT_FONTS}/boot.font" font/boot.font
             "${FF_BOOT_FONTS}/boot_small.font" font/boot_small.font
             "${CMAKE_SOURCE_DIR}/platform/vita/fonts/OFL.txt" font/Oswald-OFL.txt)
else()
    find_package(SDL2 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(ff PRIVATE ff_recomp SDL2::SDL2 OpenGL::GL m)
    if(APPLE)
        target_link_libraries(ff PRIVATE "-framework CoreFoundation")
    else()
        target_link_libraries(ff PRIVATE pthread)
    endif()
endif()
