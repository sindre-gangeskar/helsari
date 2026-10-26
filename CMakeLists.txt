cmake_minimum_required(VERSION 3.20)
project(kz2recomp C CXX)

# Everything game-specific is generated on the user's machine from their own
# dump. Nothing derived from the game is part of this repository.
set(KZ2_DUMP "" CACHE PATH "Killzone 2 disc dump folder (contains PS3_GAME)")
set(RPCS3_EXE "" CACHE FILEPATH "rpcs3.exe, used once to decrypt EBOOT.BIN")

if(NOT KZ2_DUMP)
  message(FATAL_ERROR "Pass your dump: cmake -B build -DKZ2_DUMP=\"E:/PS3 Dumps/Killzone 2\" -DRPCS3_EXE=\"E:/RPCS3/rpcs3.exe\"")
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

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

# Translated PPU code: plain C, compiled for exact PowerPC semantics.
add_library(kz2_ppu STATIC ${GENERATED_SOURCES})
target_include_directories(kz2_ppu PRIVATE runtime/include ${GEN_DIR})
target_compile_options(kz2_ppu PRIVATE
  -O1 -fwrapv -fno-strict-aliasing -ffp-contract=off -fexceptions
  -Wno-unused-label -Wno-unused-but-set-variable -Wno-parentheses-equality -w)

add_executable(kz2
  runtime/src/main.cpp
  runtime/src/core.cpp
  runtime/src/memory.cpp
  runtime/src/thread.cpp
  runtime/src/fibers.cpp
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
  runtime/src/spu/spu_interp.cpp
  runtime/src/spu/spu_mfc.cpp
  runtime/src/inflate.cpp
  runtime/src/rsx/vp.cpp
  runtime/src/rsx/fp.cpp
  runtime/src/rsx/d3d.cpp)
set_target_properties(kz2 PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(kz2 PRIVATE runtime/include runtime/src ${GEN_DIR})
cmake_path(SET _dump NORMALIZE "${KZ2_DUMP}")
target_compile_definitions(kz2 PRIVATE
  KZ2_DEFAULT_DUMP="${_dump}"
  KZ2_DEFAULT_ELF="${GEN_DIR}/decrypt/EBOOT.elf")
target_compile_options(kz2 PRIVATE -O2 -g)
target_link_libraries(kz2 PRIVATE kz2_ppu xinput hid setupapi gdi32 user32 d3d11 dxgi d3dcompiler dxguid)
target_link_options(kz2 PRIVATE -static)
