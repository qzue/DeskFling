#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
benchmark=${1:-build/macos/benchmarks/Release/dpp_benchmark}
for count in 50 200 500; do
    for scenario in fall stack throw sleep pause switch; do
        "$benchmark" "$count" 36000 "$scenario"
    done
done
