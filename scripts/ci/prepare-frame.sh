#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++ python3 pkg-config make meson nasm \
  libopenxr-dev libvulkan-dev libavcodec-dev libavutil-dev libavformat-dev libswscale-dev \
  libpulse-dev libgtk-3-dev libjson-c-dev libssl-dev libxml2-dev spirv-tools
mkdir -p build/deps build/tools build/tmp
# Use the exact compiler that generated the checked-in shader arrays.
git clone --depth 1 --branch 15.1.0 https://github.com/KhronosGroup/glslang.git build/deps/glslang
test "$(git -C build/deps/glslang rev-parse HEAD)" = 1062752a891c95b2bfeed9e356562d88f9df84ac
cmake -S build/deps/glslang -B build/tools/glslang -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=ON \
  -DGLSLANG_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$PWD/build/tools/glslang-installed"
cmake --build build/tools/glslang --parallel 4
cmake --install build/tools/glslang
echo "$PWD/build/tools/glslang-installed/bin" >> "$GITHUB_PATH"
git clone --depth 1 --branch release-1.1.36 https://github.com/KhronosGroup/OpenXR-SDK.git build/deps/openxr-sdk
test "$(git -C build/deps/openxr-sdk rev-parse HEAD)" = 8899a91c17ce9618f565f42408b47db1d6e9ccc7
