cmake_minimum_required(VERSION 3.20)
# Clang targeting MSVC otherwise defaults to Debug (checked STL) and the DLL runtime.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
project(helsari C CXX)
# Optimisation levels are set per target below.
set(CMAKE_C_FLAGS_RELEASE "")
set(CMAKE_CXX_FLAGS_RELEASE "")

# Everything game-specific is generated on the user's machine from their own
# dump. Nothing derived from the game is part of this repository.
set(HELSARI_DUMP "" CACHE PATH "Game disc dump folder (contains PS3_GAME)")
set(RPCS3_EXE "" CACHE FILEPATH "rpcs3.exe, used once to decrypt EBOOT.BIN")

if(NOT HELSARI_DUMP)
  message(FATAL_ERROR "Pass your dump: cmake -B build -DHELSARI_DUMP=\"E:/PS3 Dumps/YourDump\" -DRPCS3_EXE=\"E:/RPCS3/rpcs3.exe\"")
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

# The game is compiled on the machine that plays it, so it can use all of that CPU: AVX-512's
# 32 vector registers cut register spills in SPU code by nearly half. Needs at least AVX2.
set(HELSARI_MARCH native CACHE STRING "Target CPU (native, or e.g. x86-64-v3 for a binary that runs elsewhere)")

if(NOT MINGW)
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES _CRT_NONSTDC_NO_WARNINGS)
endif()

set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
message(STATUS "Generating C from ${HELSARI_DUMP} ...")
execute_process(
  COMMAND ${Python3_EXECUTABLE} -m recomp --dump ${HELSARI_DUMP} --rpcs3 ${RPCS3_EXE} --out ${GEN_DIR}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools
  RESULT_VARIABLE gen_result)
if(NOT gen_result EQUAL 0)
  message(FATAL_ERROR "recompiler failed (see output above)")
endif()
# Regenerate when the recompiler changes.
file(GLOB_RECURSE RECOMP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/recomp/*.py)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_SOURCES})

file(STRINGS ${GEN_DIR}/sources.txt GENERATED_SOURCES)
file(STRINGS ${GEN_DIR}/spu_sources.txt GENERATED_SPU_SOURCES)

# SPU code the game loads from its data files is captured by the runtime into
# build/spu_cache/ (spu_capture.cpp) and compiled here on every build; the
# generator only rewrites files whose content changed.
set(SPU_CACHE_DIR ${CMAKE_BINARY_DIR}/spu_cache)
set(SPU_CACHE_CMD ${Python3_EXECUTABLE} -m recomp.spu_cache --cache ${SPU_CACHE_DIR} --out ${GEN_DIR}
    --elf ${GEN_DIR}/decrypt/EBOOT.elf)
execute_process(COMMAND ${SPU_CACHE_CMD} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools)
set(SPU_RT_SOURCES ${GEN_DIR}/spu_rt/spu_rt_modules.cpp)
foreach(i RANGE 0 15)
  string(LENGTH "${i}" _len)
  if(_len EQUAL 1)
    set(i "0${i}")
  endif()
  list(APPEND SPU_RT_SOURCES ${GEN_DIR}/spu_rt/spu_rt_${i}.cpp)
endforeach()
add_custom_target(spu_cache_gen
  COMMAND ${SPU_CACHE_CMD}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools
  BYPRODUCTS ${SPU_RT_SOURCES} ${GENERATED_SPU_SOURCES}
  COMMENT "Compiling captured SPU code")

# Generated code compiles in large, memory-hungry units: cap how many build at
# once so a parallel build cannot exhaust RAM (each pool slot can take a few GB).
set_property(GLOBAL APPEND PROPERTY JOB_POOLS ppu_pool=12 spu_pool=4)

# Translated PPU code: plain C, compiled for exact PowerPC semantics.
add_library(helsari_ppu STATIC ${GENERATED_SOURCES})
set_property(TARGET helsari_ppu PROPERTY JOB_POOL_COMPILE ppu_pool)
target_include_directories(helsari_ppu PRIVATE runtime/include ${GEN_DIR})
target_compile_options(helsari_ppu PRIVATE
  -O1 -fwrapv -fno-strict-aliasing -ffp-contract=off -fexceptions
  -Wno-unused-label -Wno-unused-but-set-variable -Wno-parentheses-equality -w)

# Build stamp: an ID logged at startup, kept in the build directory. Changing
# it (tools/bump_stamp.sh) produces a new executable without touching sources -
# e.g. when Windows Smart App Control blocks a fresh build.
set(BUILD_STAMP_SRC ${CMAKE_BINARY_DIR}/helsari_build_stamp.cpp)
if(NOT EXISTS ${BUILD_STAMP_SRC})
  file(WRITE ${BUILD_STAMP_SRC} "extern \"C\" const char helsari_build_stamp[] = \"0\";
")
endif()

add_executable(helsari
  ${BUILD_STAMP_SRC}
  runtime/main.cpp
  runtime/core.cpp
  runtime/memory/memory.cpp
  runtime/ppu/thread.cpp
  runtime/ppu/fibers.cpp
  runtime/diag/debug_hooks.cpp
  runtime/syscalls/kernel.cpp
  runtime/syscalls/syscalls.cpp
  runtime/syscalls/fs.cpp
  port/menu_patch.cpp
  port/pc_options.cpp
  port/mouse.cpp
  port/keybinds.cpp
  libs/system/hle_sys.cpp
  libs/video/hle_gcm.cpp
  libs/input/hle_io.cpp
  libs/input/dualsense.cpp
  libs/spurs/spurs.cpp
  libs/spurs/spurs_kernel.cpp
  libs/spurs/spurs_tasks.cpp
  runtime/syscalls/sync.cpp
  libs/system/savedata.cpp
  runtime/diag/profiler.cpp
  runtime/spu/spu_interp.cpp
  runtime/spu/spu_mfc.cpp
  runtime/spu/spu_native.cpp
  runtime/spu/spu_capture.cpp
  libs/codec/inflate.cpp
  libs/codec/vdec.cpp
  libs/codec/adec.cpp
  libs/video/rsx/vp.cpp
  libs/video/rsx/fp.cpp
  libs/video/rsx/d3d.cpp)
set_target_properties(helsari PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON OUTPUT_NAME helsari2)

# Recompiled SPU code (EBOOT modules and the runtime-captured cache).
add_library(helsari_spu STATIC ${GENERATED_SPU_SOURCES} ${SPU_RT_SOURCES})
add_dependencies(helsari_spu spu_cache_gen)
set_target_properties(helsari_spu PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON JOB_POOL_COMPILE spu_pool)
target_include_directories(helsari_spu PRIVATE runtime/include runtime runtime/spu runtime/syscalls runtime/ppu runtime/memory runtime/diag libs libs/video libs/video/rsx libs/spurs libs/input libs/system libs/codec port ${GEN_DIR})
target_compile_options(helsari_spu PRIVATE -O2 -march=${HELSARI_MARCH} -Wno-unused-label)
target_include_directories(helsari PRIVATE runtime/include runtime runtime/spu runtime/syscalls runtime/ppu runtime/memory runtime/diag libs libs/video libs/video/rsx libs/spurs libs/input libs/system libs/codec port ${GEN_DIR})
cmake_path(SET _dump NORMALIZE "${HELSARI_DUMP}")
target_compile_definitions(helsari PRIVATE
  HELSARI_DEFAULT_DUMP="${_dump}"
  HELSARI_DEFAULT_ELF="${GEN_DIR}/decrypt/EBOOT.elf")
target_compile_options(helsari PRIVATE -O2 -g -march=${HELSARI_MARCH})
target_link_libraries(helsari PRIVATE helsari_spu helsari_ppu xinput hid setupapi gdi32 user32 d3d11 dxgi d3dcompiler dxguid winmm ole32 synchronization mfplat mfuuid)
if(MINGW)
  target_link_options(helsari PRIVATE -static)
else()
  # Microsoft's signed Clang (Visual Studio Build Tools): allowed by Smart App Control.
  # 128-bit division comes from compiler-rt; the same 2 MB default stack as MinGW.
  get_filename_component(_llvm_bin ${CMAKE_CXX_COMPILER} DIRECTORY)
  file(GLOB _rt "${_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib")
  target_link_libraries(helsari PRIVATE ${_rt})
  target_link_options(helsari PRIVATE -fuse-ld=lld -g -Wl,/STACK:2097152)
endif()
