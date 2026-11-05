#!/bin/sh
# Give the next build a new stamp (see CMakeLists.txt: BUILD_STAMP_SRC), so it
# links to a different executable. Usage: tools/bump_stamp.sh [build dir]
dir=${1:-build}
printf 'extern "C" const char helsari_build_stamp[] = "%s";\n' "$(date +%Y%m%d-%H%M%S)-$$" > "$dir/helsari_build_stamp.cpp"
