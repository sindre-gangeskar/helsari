cmake_minimum_required(VERSION 3.20)
# Clang targeting MSVC otherwise defaults to Debug (checked STL) and the DLL runtime.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
project(kz2recomp C CXX)
# Optimisation levels are set per target below.
set(CMAKE_C_FLAGS_RELEASE "")
set(CMAKE_CXX_FLAGS_RELEASE "")

# Everything game-specific is generated on the user's machine from their own
# dump. Nothing derived from the game is part of this repository.
set(KZ2_DUMP "" CACHE PATH "Killzone 2 disc dump folder (contains PS3_GAME)")
set(RPCS3_EXE "" CACHE FILEPATH "rpcs3.exe, used once to decrypt EBOOT.BIN")

if(NOT KZ2_DUMP)
  message(FATAL_ERROR "Pass your dump: cmake -B build -DKZ2_DUMP=\"E:/PS3 Dumps/Killzone 2\" -DRPCS3_EXE=\"E:/RPCS3/rpcs3.exe\"")
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

if(NOT MINGW)
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES _CRT_NONSTDC_NO_WARNINGS)
endif()

set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
message(STATUS "Generating C from ${KZ2_DUMP} ...")
execute_process(
  COMMAND ${Python3_EXECUTABLE} -m recomp --dump ${KZ2_DUMP} --rpcs3 ${RPCS3_EXE} --out ${GEN_DIR}
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
add_library(kz2_ppu STATIC ${GENERATED_SOURCES})
set_property(TARGET kz2_ppu PROPERTY JOB_POOL_COMPILE ppu_pool)
target_include_directories(kz2_ppu PRIVATE runtime/include ${GEN_DIR})
target_compile_options(kz2_ppu PRIVATE
  -O1 -fwrapv -fno-strict-aliasing -ffp-contract=off -fexceptions
  -Wno-unused-label -Wno-unused-but-set-variable -Wno-parentheses-equality -w)

# Build stamp: an ID logged at startup, kept in the build directory. Changing
# it (tools/bump_stamp.sh) produces a new executable without touching sources -
# e.g. when Windows Smart App Control blocks a fresh build.
set(BUILD_STAMP_SRC ${CMAKE_BINARY_DIR}/kz2_build_stamp.cpp)
if(NOT EXISTS ${BUILD_STAMP_SRC})
  file(WRITE ${BUILD_STAMP_SRC} "extern \"C\" const char kz2_build_stamp[] = \"0\";
")
endif()

add_executable(kz2
  ${BUILD_STAMP_SRC}
  runtime/src/main.cpp
  runtime/src/core.cpp
  runtime/src/memory.cpp
  runtime/src/thread.cpp
  runtime/src/fibers.cpp
  runtime/src/debug_hooks.cpp
  runtime/src/kernel.cpp
  runtime/src/syscalls.cpp
  runtime/src/fs.cpp
  runtime/src/hle_sys.cpp
  runtime/src/hle_gcm.cpp
  runtime/src/hle_io.cpp
  runtime/src/dualsense.cpp
  runtime/src/spurs.cpp
  runtime/src/spurs_kernel.cpp
  runtime/src/spurs_tasks.cpp
  runtime/src/sync.cpp
  runtime/src/savedata.cpp
  runtime/src/profiler.cpp
  runtime/src/spu/spu_interp.cpp
  runtime/src/spu/spu_mfc.cpp
  runtime/src/spu/spu_native.cpp
  runtime/src/spu/spu_capture.cpp
  runtime/src/inflate.cpp
  runtime/src/rsx/vp.cpp
  runtime/src/rsx/fp.cpp
  runtime/src/rsx/d3d.cpp)
set_target_properties(kz2 PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# Recompiled SPU code (EBOOT modules and the runtime-captured cache).
add_library(kz2_spu STATIC ${GENERATED_SPU_SOURCES} ${SPU_RT_SOURCES})
add_dependencies(kz2_spu spu_cache_gen)
set_target_properties(kz2_spu PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON JOB_POOL_COMPILE spu_pool)
target_include_directories(kz2_spu PRIVATE runtime/include runtime/src ${GEN_DIR})
target_compile_options(kz2_spu PRIVATE -O2 -march=x86-64-v3 -Wno-unused-label)
target_include_directories(kz2 PRIVATE runtime/include runtime/src ${GEN_DIR})
cmake_path(SET _dump NORMALIZE "${KZ2_DUMP}")
target_compile_definitions(kz2 PRIVATE
  KZ2_DEFAULT_DUMP="${_dump}"
  KZ2_DEFAULT_ELF="${GEN_DIR}/decrypt/EBOOT.elf")
target_compile_options(kz2 PRIVATE -O2 -g -march=x86-64-v3)
target_link_libraries(kz2 PRIVATE kz2_spu kz2_ppu xinput hid setupapi gdi32 user32 d3d11 dxgi d3dcompiler dxguid winmm ole32 synchronization)
if(MINGW)
  target_link_options(kz2 PRIVATE -static)
else()
  # Microsoft's signed Clang (Visual Studio Build Tools): allowed by Smart App Control.
  # 128-bit division comes from compiler-rt; the same 2 MB default stack as MinGW.
  get_filename_component(_llvm_bin ${CMAKE_CXX_COMPILER} DIRECTORY)
  file(GLOB _rt "${_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib")
  target_link_libraries(kz2 PRIVATE ${_rt})
  target_link_options(kz2 PRIVATE -fuse-ld=lld -g -Wl,/STACK:2097152)
endif()
