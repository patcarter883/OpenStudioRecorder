#!/usr/bin/env bash
# The gate a build has to pass before it is deployed: configure with the test feature, build, run the
# suite, and only then let the app's own binary stand. A build that skips the tests is how an
# unverified binary reaches a show - and until this existed the deployed build was configured with no
# tests at all (ctest reported 0), so "it builds" was the only thing anyone had checked.
#
#   tools/build-and-test.sh [build-dir]        default: build/gate
#
# Tests need BOTH switches: BUILD_TESTING, and the vcpkg manifest feature that supplies Catch2.
set -euo pipefail

BUILD_DIR="${1:-build/gate}"

# vcpkg is wherever THIS checkout last used it: read the path out of an existing build cache instead of
# assuming $HOME/vcpkg. (Assuming it is how the first run of this gate failed on the capture box, whose
# vcpkg is not in $HOME.)
# A set-but-wrong VCPKG_ROOT is a hint, not gospel: fall through to discovery rather than refusing.
if [ -n "${VCPKG_ROOT:-}" ] && [ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
  echo "gate: VCPKG_ROOT=$VCPKG_ROOT has no scripts/buildsystems/vcpkg.cmake - searching instead" >&2
  VCPKG_ROOT=""
fi
if [ -z "${VCPKG_ROOT:-}" ]; then
  for candidate in "$HOME/vcpkg" "$HOME/code/vcpkg" /opt/vcpkg /usr/local/vcpkg; do
    if [ -f "$candidate/scripts/buildsystems/vcpkg.cmake" ]; then
      VCPKG_ROOT="$candidate"
      break
    fi
  done
fi
if [ -z "${VCPKG_ROOT:-}" ] && [ -f build/vcpkg/CMakeCache.txt ]; then
  VCPKG_ROOT="$(sed -n 's|^CMAKE_TOOLCHAIN_FILE:[^=]*=\(.*\)/scripts/buildsystems/vcpkg\.cmake$|\1|p' \
    build/vcpkg/CMakeCache.txt | head -1)"
fi
if [ -z "${VCPKG_ROOT:-}" ] || [ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
  echo "error: cannot find vcpkg - set VCPKG_ROOT to the vcpkg checkout" >&2
  exit 2
fi
export VCPKG_ROOT
echo "gate: vcpkg=$VCPKG_ROOT"

# Reuse an existing vcpkg tree when there is one (a manifest install from scratch builds FLTK from
# source), unless the caller pinned its own.
if [ -z "${VCPKG_INSTALLED_DIR:-}" ] && [ -d build/vcpkg/vcpkg_installed ]; then
  export VCPKG_INSTALLED_DIR="$PWD/build/vcpkg/vcpkg_installed"
fi

cmake -B "$BUILD_DIR" -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DVCPKG_MANIFEST_FEATURES=test \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  ${VCPKG_INSTALLED_DIR:+-DVCPKG_INSTALLED_DIR="$VCPKG_INSTALLED_DIR"}

cmake --build "$BUILD_DIR" -j"$(nproc)"

# The gate itself. --output-on-failure so a failure in a deploy log is readable.
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "GATE PASSED: $BUILD_DIR built, tests green - deploy from $BUILD_DIR/OpenStudioRecorder"
