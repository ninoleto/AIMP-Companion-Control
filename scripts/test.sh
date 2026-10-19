#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Isrc -isystem third_party tests/protocol_tests.cpp src/protocol.cpp -o build/protocol-tests
ASAN_OPTIONS=detect_leaks=0 ./build/protocol-tests
${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Isrc -isystem third_party tests/audio_controls_tests.cpp -o build/audio-controls-tests
ASAN_OPTIONS=detect_leaks=0 ./build/audio-controls-tests
for name in library_tag_validation media_controls music_library_validation; do
    ${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
        -Isrc -isystem third_party "tests/${name}_tests.cpp" -o "build/${name}-tests"
    ASAN_OPTIONS=detect_leaks=0 "./build/${name}-tests"
done
python3 tests/preset_management_tests.py
python3 tests/focus_resolution_tests.py
