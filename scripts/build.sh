#!/usr/bin/env bash
# Use the pinned LLVM-MinGW toolchain documented in BUILD.md. No packers or post-link rewriting.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${AIMP_TOOLCHAIN:?Set AIMP_TOOLCHAIN to the extracted LLVM-MinGW directory}"
mkdir -p build dist
sources=(src/globals.cpp src/utils.cpp src/network.cpp src/focus_sync.cpp src/settings.cpp
         src/player_api.cpp src/commands.cpp src/audio_controls.cpp src/player_extensions.cpp src/library_controls.cpp
         src/media_controls.cpp src/music_library_controls.cpp src/app_controls.cpp src/skin_controls.cpp
         src/protocol.cpp src/ws_server.cpp src/options_frame.cpp src/plugin.cpp)
for arch in x86_64 i686; do
    compiler="$AIMP_TOOLCHAIN/bin/$arch-w64-mingw32-clang++"
    resource="$AIMP_TOOLCHAIN/bin/$arch-w64-mingw32-windres"
    exports=()
    if [[ "$arch" == i686 ]]; then exports=(src/plugin.def); fi
    "$resource" src/version.rc "build/version-$arch.o"
    "$compiler" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-braces \
        -Wno-delete-non-abstract-non-virtual-dtor -DUNICODE -D_UNICODE -DNOMINMAX \
        -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -ffile-prefix-map="$PWD"=. \
        -Isrc -isystem sdk -isystem third_party "${sources[@]}" "build/version-$arch.o" \
        "${exports[@]}" -shared -static -static-libstdc++ \
        -Wl,--no-insert-timestamp,--dynamicbase,--nxcompat \
        -lws2_32 -liphlpapi -luuid -lkernel32 -luser32 -lgdi32 -lbcrypt -loleaut32 \
        -o "dist/AimpCompanionControl-$arch.dll"
done
python3 scripts/package.py
