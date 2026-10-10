#!/usr/bin/env bash
# Full Linux compile gate for OpenStudioRecorder: vcpkg builds FLTK from source,
# then the app (main.cpp + UserInterface.cxx) compiles against it.
set -x
export TMPDIR=/home/pat/.hermes/cache/scratch
export VCPKG_ROOT=/home/pat/vcpkg
cd /home/pat/workspace/video/OpenStudioRecorder || exit 9
rm -rf build/vcpkg
cmake --preset=vcpkg > build-vcpkg.log 2>&1
echo "CONFIGURE_RC=$?" >> build-vcpkg.log
if [ "$(grep -c CONFIGURE_RC=0 build-vcpkg.log)" = 1 ]; then
  cmake --build build/vcpkg -j8 > build-vcpkg.log 2>&1
  echo "BUILD_RC=$?" >> build-vcpkg.log
fi
# Re-install the patched mxf plugin next to the binary. This script wipes build/vcpkg, and the
# app replaces the system plugin with <exe dir>/gst-plugins/libgstmxf.so - without this the app
# silently falls back to STOCK mxfmux and every MXF timecode track starts at 00:00:00:00.
if [ -f build/patched-gst-plugins/libgstmxf.so ]; then
  mkdir -p build/vcpkg/gst-plugins
  cp build/patched-gst-plugins/libgstmxf.so build/vcpkg/gst-plugins/
  echo "INSTALLED_PATCHED_MXF_PLUGIN=build/vcpkg/gst-plugins/libgstmxf.so" >> build-vcpkg.log
else
  echo "WARNING: build/patched-gst-plugins/libgstmxf.so is missing - the app will use STOCK mxfmux" \
       "(MXF timecode will read 00:00:00:00). Build it: tools/build-patched-gstmxf-linux.sh" | tee -a build-vcpkg.log
fi

tail -30 build-vcpkg.log
echo "OSR_BUILD_JOB_DONE"
