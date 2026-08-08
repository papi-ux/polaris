#!/bin/bash
# Finds which translation units, if any, trigger an internal compiler error
# (ICE) on the current C++ compiler at real optimization, beyond the files
# already quarantined at -O0 in cmake/targets/common.cmake.
#
# Run this after bumping the minimum/tested GCC version, or when evaluating
# a new distro's default compiler, to find out whether the quarantine list
# needs to grow. It does NOT modify the quarantine itself - it only reports.
#
# Usage:
#   scripts/bisect-gcc-ice.sh [build-dir]
#
# CC/CXX (or -DCMAKE_C_COMPILER/-DCMAKE_CXX_COMPILER via
# POLARIS_BISECT_CMAKE_ARGS) select the compiler under test, same as a
# normal cmake invocation.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
root_dir="$(cd "$script_dir/.." && pwd)"
build_dir="${1:-${root_dir}/build-bisect}"

# shellcheck disable=SC2206 # intentional word-splitting of user-provided extra args
extra_cmake_args=(${POLARIS_BISECT_CMAKE_ARGS:-})

echo "Bisecting ICE-prone translation units in: ${root_dir}"
echo "Compiler: ${CXX:-$(command -v c++)}"
echo "Build directory: ${build_dir}"
echo

cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPOLARIS_ENABLE_CUDA=OFF \
  -DPOLARIS_ALLOW_CUDA_DISABLED_ON_NVIDIA=ON \
  -DBOOST_USE_STATIC=OFF \
  -DBUILD_TESTS=OFF \
  "${extra_cmake_args[@]}"

log_file="${build_dir}/bisect-ice.log"

# -k 0: keep going past failures so every ICE in this run surfaces, not just
# the first one ninja happens to hit.
set +e
ninja -C "${build_dir}" -k 0 2>&1 | tee "${log_file}"
build_status=${PIPESTATUS[0]}
set -e

echo
echo "=== Bisect result ==="

# GCC reports an ICE as "internal compiler error: ..." on its own line,
# preceded by the offending source file on the "n: In ..." context line or
# the ninja "Building CXX object .../<file>.o" line immediately above it.
mapfile -t ice_files < <(
  awk '
    /Building (CXX|C) object/ { last_target = $0 }
    /internal compiler error/ { print last_target }
  ' "${log_file}" | sed -E 's#.*CMakeFiles/[^/]+\.dir/##; s#\.o$##' | sort -u
)

if [ "${#ice_files[@]}" -eq 0 ]; then
  if [ "${build_status}" -eq 0 ]; then
    echo "No internal compiler errors. Every translation unit compiled clean at real optimization."
    exit 0
  else
    echo "Build failed, but not with an ICE pattern this script recognizes."
    echo "Check ${log_file} directly - this may be an unrelated build break, not a compiler bug."
    exit 1
  fi
fi

echo "Internal compiler error(s) found in:"
for f in "${ice_files[@]}"; do
  echo "  - ${f}"
done
echo
echo "If any of these are new, quarantine them at -O0 alongside process.cpp/ai_optimizer.cpp"
echo "in the per-file block in cmake/targets/common.cmake (search for GCC_RELEASE_ICE_WORKAROUND_FLAGS)."
echo "Do not restore the old target-wide block - quarantine only the files that actually ICE."
exit 1
