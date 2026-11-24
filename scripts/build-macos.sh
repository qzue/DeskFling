#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
if [ -d .tools/bin ]; then PATH="$project_dir/.tools/bin:$PATH"; export PATH; fi
cmake --preset macos
cmake --build --preset macos-release --parallel
ctest --preset macos-release
cmake --install build/macos --config Release --prefix "$project_dir/dist/macos-dev"
