cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(ssx_vita C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Vita application configuration. Debug and release builds install as two
# separate apps (same data folder), to compare them side by side.
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(VITA_APP_NAME "SSX (2012) Debug")
  set(VITA_TITLEID "SSXD00001")
  set(VITA_VPKNAME "ssx_vita_debug")
else ()
  set(VITA_APP_NAME "SSX (2012)")
  set(VITA_TITLEID "SSXV00001")
  set(VITA_VPKNAME "ssx_vita")
endif ()
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# Optional: used for the "send" targets
set(PSVITAIP "" CACHE STRING "PSVita IP (for FTP access): -DPSVITAIP=...")

# Game-specific definitions
set(DATA_PATH "ux0:data/ssx/") # Path to data (with trailing /)
set(SO_PATH "${DATA_PATH}libgame.so") # Path to .so
set(GNUSTL_PATH "${DATA_PATH}libgnustl_shared.so") # libgame.so depends on it
add_definitions(-DDATA_PATH="${DATA_PATH}" -DSO_PATH="${SO_PATH}" -DGNUSTL_PATH="${GNUSTL_PATH}")

# Port type
option(NDK_PORT "This should be on if the game is done with Android NDK (has an android_main entry point)" OFF)
if (NDK_PORT)
  add_definitions(-DNDK_PORT)
endif()

option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" ON)
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
endif()

set(SHADER_FORMAT "GLSL") # Preferred shader format (one of 'GLSL', 'CG', 'GXP')
if (${SHADER_FORMAT} STREQUAL "GLSL")
  add_definitions(-DUSE_GLSL_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "CG")
  add_definitions(-DUSE_CG_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "GXP")
  add_definitions(-DUSE_GXP_SHADERS)
else()
  message(FATAL_ERROR "SHADER_FORMAT option can only be one of: 'GLSL', 'CG', 'GXP'")
endif()

set(valid_dump_formats GLSL CG)
option(DUMP_COMPILED_SHADERS "When using GLSL or CG shader format, cache compiled shaders on disk" OFF)
if (DUMP_COMPILED_SHADERS)
  if (SHADER_FORMAT IN_LIST valid_dump_formats)
    add_definitions(-DDUMP_COMPILED_SHADERS)
  else()
    message(WARNING "DUMP_COMPILED_SHADERS has no effect for the chosen shader format ${SHADER_FORMAT}")
  endif()
endif()

# Unique debug definition for loader's logging to not interfere with anything
# Profilers and diagnostics for debug builds, all off by default because they
# slow the game down. See docs/DEBUGGING.md for what each one logs.
option(SSX_GLPROF "Debug: time spent in each GL function ([PERF] lines; ~1 us per GL call)" OFF)
option(SSX_FNPROF "Debug: time spent in the game functions of source/ssx/fnprof_list.h (costly)" OFF)
option(SSX_ALLOCPROF "Debug: who allocates through MemoryFramework, and what it costs" OFF)
option(SSX_RENDERPROF "Debug: which code holds the most memory in the RENDER_GEN heap" OFF)
option(SSX_ASPROF "Debug: which ActionScript functions long UI frames run, and member reads" OFF)
option(SSX_POOL_WALK "Debug: walk the UI memory pool every 30 s ([POOL] lines; holds its lock)" OFF)
option(SSX_DIAG "Debug: verbose logs and the diagnostics of past investigations (file traces, vitaGL statistics...)" OFF)
foreach(opt SSX_GLPROF SSX_FNPROF SSX_ALLOCPROF SSX_RENDERPROF SSX_ASPROF SSX_POOL_WALK SSX_DIAG)
  if (${opt})
    add_definitions(-D${opt}=1)
  else()
    add_definitions(-D${opt}=0)
  endif()
endforeach()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_SOLOADER -DLOG_TO_FILE)
  # Uncomment this for verbose FalsoJNI logging:
  #add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
  # Uncomment this for OpenGL logging:
  #add_definitions(-DDEBUG_OPENGL)
  # Uncomment this for so_util logging:
  #add_definitions(-DSO_UTIL_VERBOSE=1)
  # Uncomment this for verbose FalsoNDK logging (separate things):
  #add_definitions(-DDEBUG_POLL_AND_WAKE=1)
  #add_definitions(-DDEBUG_EPOLL=1)
  #add_definitions(-DDEBUG_PIPEFD=1)
  #add_definitions(-DDEBUG_CALLBACKS=1)
  # TEXTURES_SPEEDHACK: update textures in place. Without it vitaGL copies a
  # whole texture on every glTexSubImage2D to one in use; Scaleform updates its
  # 1024x1024 glyph cache per character, which filled VRAM with pending copies.
  # Space-separated flags passed to vitaGL
  # HAVE_PROFILING (with SSX_DIAG): vitaGL logs every 30 frames where draw time
  # goes (draws, vertex program patching), ~9 lines a second
  set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 TEST=1 LOG_ERRORS=1 HAVE_SHADER_CACHE=1 TEXTURES_SPEEDHACK=1 DRAW_SPEEDHACK=1")
  if (SSX_DIAG)
    set(VITAGL_MAKE_FLAGS "${VITAGL_MAKE_FLAGS} HAVE_PROFILING=1")
  endif()
else()
  # Space-separated flags passed to vitaGL
  set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1 TEXTURES_SPEEDHACK=1 DRAW_SPEEDHACK=1")
endif()

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu11 -Wno-deprecated")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu++20 -Wno-write-strings -Wno-psabi")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O1 -g3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1 -g3")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g0 -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g0 -ffast-math")
endif()

set(SOURCES
    source/main.c
    source/dynlib.c
    source/java.c
    source/jni/jni_egl.c
    source/jni/jni_audio.c
    source/jni/jni_io.c
    source/jni/jni_system.c
    source/jni/jni_video.c
    source/ssx/lifecycle.c
    source/ssx/stubs.c
    source/ssx/assets.c
    source/ssx/memreport.c
    source/ssx/texmem.c
    source/ssx/spinwait.c
    source/ssx/bufmem.c
    source/ssx/scopecache.c
    source/ssx/shaderbind.c
    source/ssx/terraincull.c
    source/ssx/vaocache.c
    source/ssx/boardpress.c
    source/ssx/settings.c
    source/ssx/core3.c
    source/ssx/jobprof.c
    source/ssx/sfmesh.c
    source/ssx/hwskin.c
    source/ssx/audioguard.c
    source/ssx/sfgc.c
    source/ssx/terrainmerge.c
    source/ssx/texload.c
    source/ssx/allocprof.c
    source/ssx/gfxcache.c
    source/ssx/asprof.c
    source/ssx/filecopy.c
    source/ssx/luagc.c
    source/ssx/displaylist.c
    source/ssx/movielife.c
    source/ssx/fpscap.c
    source/ssx/assetunload.c
    source/ssx/globeload.c
    source/ssx/unlockwait.c
    source/ssx/trace.c
    source/ssx/fbprobe.c
    source/ssx/renderprof.c
    source/ssx/perf.c
    source/ssx/glprof.c
    source/ssx/overlay.c
    source/ssx/watchdog.c
    source/patch.c
    source/reimpl/bits/_ctype.c
    source/reimpl/errno.c
    source/reimpl/fastmath.c
    source/reimpl/io.c
    source/reimpl/sparse_save.c
    source/reimpl/log.c
    source/reimpl/mem.c
    source/reimpl/pthr.c
    source/reimpl/sys.c
    source/reimpl/time64.c
    source/utils/dialog.c
    source/utils/glutil.c
    source/utils/init.c
    source/utils/logger.c
    source/utils/settings.c
    source/utils/utils.c
    lib/falso_jni/FalsoJNI.c
    lib/falso_jni/FalsoJNI_ImplBridge.c
    lib/falso_jni/FalsoJNI_Logger.c
    lib/falso_jni/converter.c
    lib/sha1/sha1.c
    lib/fios/fios.c
    lib/so_util/so_util.c)

if(NOT NDK_PORT)
list(APPEND SOURCES
  source/reimpl/asset_manager.cpp
  source/reimpl/controls.c
  source/reimpl/controls.h)
endif()

add_executable(${CMAKE_PROJECT_NAME} ${SOURCES})

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)

if(NDK_PORT)
  add_subdirectory(lib/falso_ndk)
  add_dependencies(${CMAKE_PROJECT_NAME} FalsoNDK)
endif()

# VitaGL built from lib/vitagl via its own Makefile.
# Made this way so that testing quick adjustments to VitaGL is possible,
# as well as changing build flags in a fast and efficient way.
separate_arguments(VITAGL_MAKE_ARGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS}")

set(VITAGL_DIR ${CMAKE_SOURCE_DIR}/lib/vitagl)
set(VITAGL_LIB ${VITAGL_DIR}/libvitaGL.a)

include(ProcessorCount)
ProcessorCount(VITAGL_NPROC)
if (VITAGL_NPROC EQUAL 0)
  set(VITAGL_NPROC 4)
endif()

# vitaGL is rebuilt whole when its flags differ from its last build's (see
# cmake/vitagl_flags.cmake)
add_custom_target(vitaGL_build
                  COMMAND ${CMAKE_COMMAND} -DVITAGL_DIR=${VITAGL_DIR} "-DVITAGL_MAKE_FLAGS=${VITAGL_MAKE_FLAGS}"
                          -P ${CMAKE_SOURCE_DIR}/cmake/vitagl_flags.cmake
                  COMMAND make -C ${VITAGL_DIR} -j${VITAGL_NPROC} ${VITAGL_MAKE_ARGS}
                  BYPRODUCTS ${VITAGL_LIB}
                  COMMENT "Building VitaGL"
                  VERBATIM)

add_library(vitaGL STATIC IMPORTED)
set_target_properties(vitaGL PROPERTIES
                      IMPORTED_LOCATION ${VITAGL_LIB}
                      INTERFACE_INCLUDE_DIRECTORIES ${VITAGL_DIR}/source)
add_dependencies(${CMAKE_PROJECT_NAME} vitaGL_build)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                           )

if(NDK_PORT)
  target_link_libraries(${CMAKE_PROJECT_NAME}
                        FalsoNDK
                        )
endif()

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      OpenSLES
                      sndfile
                      mp3lame
                      mpg123
                      opus
                      vorbis
                      ogg
                      vorbisenc
                      vorbisfile
                      FLAC
                      stdc++
                      vitaGL
                      vitashark
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      z
                      m
                      ScePgf_stub
                      mathneon
                      kubridge_stub
                      ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceAvPlayer_stub
                      SceSysmodule_stub
                      SceAudioIn_stub
                      SceCommonDialog_stub
                      SceDisplay_stub
                      SceKernelDmacMgr_stub
                      SceFios2_stub
                      SceCtrl_stub
                      SceGxm_stub
                      ScePower_stub
                      SceTouch_stub
                      SceVshBridge_stub
                      )

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME ${VITA_APP_NAME}
                FILE ${CMAKE_SOURCE_DIR}/extras/cpuinfo cpuinfo
                ${CMAKE_SOURCE_DIR}/extras/meminfo meminfo
                ${CMAKE_SOURCE_DIR}/extras/urandom urandom
                ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
                )

add_custom_target(send
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ${VITA_TITLEID} | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(send_kvdb
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ux0:/app/${VITA_TITLEID}/eboot.bin | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(reboot
                  COMMAND echo reboot | nc ${PSVITAIP} 1338
                  )
