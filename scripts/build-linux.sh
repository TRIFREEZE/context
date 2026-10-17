#!/usr/bin/env bash
# Developer build on Linux (VST3 + Standalone + tests). Not a release target, but it
# compiles and tests the same code as Windows.
#   sudo apt install build-essential cmake ninja-build libasound2-dev libx11-dev libxext-dev \
#        libxinerama-dev libxrandr-dev libxcursor-dev libfreetype-dev libfontconfig1-dev libcurl4-openssl-dev libgl1-mesa-dev
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
"$repo_root/scripts/fetch-juce.sh"
cmake -S "$repo_root" -B "$repo_root/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DFETCHCONTENT_SOURCE_DIR_JUCE="$repo_root/.deps/JUCE" -DCONTEXT_BUILD_TESTS=ON
cmake --build "$repo_root/build" --parallel
ctest --test-dir "$repo_root/build" --output-on-failure
echo "Built: $repo_root/build/Context_artefacts/Release/VST3/Context.vst3"
