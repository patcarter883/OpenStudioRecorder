#!/usr/bin/env bash
# Builds a patched libgstmxf.so (MXF timecode seeding) for a Linux capture machine.
#
# Rebuilds ONLY the mxf plugin of gst-plugins-bad from the matching upstream source tarball,
# with patches/0001-mxfmux-seed-timecode-from-videotimecodemeta.patch applied. The resulting
# libgstmxf.so is dropped in --plugin-out-dir and loaded via GST_PLUGIN_PATH (the app points
# GStreamer at <exe dir>/gst-plugins before gst_init()), so the system GStreamer is untouched.
#
# Linux counterpart of tools/build-patched-gstmxf-windows.ps1 - keep the two in step.
#
# Prerequisites:
#   * GStreamer runtime + development headers for the SAME version (pkg-config gstreamer-1.0,
#     gstreamer-base-1.0, gstreamer-video-1.0, gstreamer-audio-1.0, gstreamer-tag-1.0)
#   * meson, ninja, git, curl, tar
#
# Usage:
#   tools/build-patched-gstmxf-linux.sh [GStreamerVersion] [PluginOutDir]
#
# Note: the plugin must be built from the same version's source as the installed runtime
# (GStreamer refuses a plugin built for a newer minor). Rebuild it on every runtime upgrade.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
# Every patch in patches/, applied in name order. 0001 seeds the timecode tracks from
# GstVideoTimeCodeMeta; 0002 fills the CDCI descriptor fields the plugin otherwise leaves at
# zero (component depth, subsampling, video line map, signal standard).
PATCH_DIR="$REPO_DIR/patches"

# Default the version to the installed runtime: a mismatch always fails at load time, never here.
detect_version() {
  gst-launch-1.0 --version 2>/dev/null | sed -n 's/^GStreamer \([0-9][0-9.]*\).*/\1/p' | head -1
}

GST_VERSION="${1:-$(detect_version)}"
PLUGIN_OUT_DIR="${2:-$REPO_DIR/build/patched-gst-plugins}"
WORK_DIR="${WORK_DIR:-${TMPDIR:-/tmp}/gst-mxf-patch}"

if [ -z "$GST_VERSION" ]; then
  echo "error: could not detect the installed GStreamer version; pass it explicitly." >&2
  exit 1
fi

# --- 1. sanity ------------------------------------------------------------------
echo "GStreamer version  : $GST_VERSION"
echo "Plugin output dir  : $PLUGIN_OUT_DIR"
echo "Work dir           : $WORK_DIR"

PATCHES=("$PATCH_DIR"/*.patch)
[ -e "${PATCHES[0]}" ] || { echo "error: no patch files found in $PATCH_DIR" >&2; exit 1; }

for tool in meson ninja git curl tar; do
  command -v "$tool" >/dev/null 2>&1 || { echo "error: $tool not found on PATH." >&2; exit 1; }
done

for pkg in gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0; do
  have="$(pkg-config --modversion "$pkg" 2>/dev/null || true)"
  [ -n "$have" ] || { echo "error: development headers for $pkg not found (install the -dev/-devel package)." >&2; exit 1; }
  if [ "$have" != "$GST_VERSION" ]; then
    echo "error: headers for $pkg are $have but the runtime is $GST_VERSION - they must match." >&2
    exit 1
  fi
done

# --- 2. fetch + unpack the matching source tarball -------------------------------
mkdir -p "$WORK_DIR"
SRC_DIR="$WORK_DIR/gst-plugins-bad-$GST_VERSION"
TARBALL="$WORK_DIR/gst-plugins-bad-$GST_VERSION.tar.xz"
if [ ! -d "$SRC_DIR" ]; then
  if [ ! -f "$TARBALL" ]; then
    URL="https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-$GST_VERSION.tar.xz"
    echo "Downloading $URL"
    curl -sSL -o "$TARBALL" "$URL"
  fi
  tar -xf "$TARBALL" -C "$WORK_DIR"
fi

# --- 3. apply the patch (fail fast if the base version is wrong) -----------------
cd "$SRC_DIR"
[ -d .git ] || git init -q .

# A tarball has no git state; initialise + commit the pristine tree so `git apply` behaves
# exactly as it does on a developer machine, and so "already applied" is detectable.
if ! git rev-parse --verify -q HEAD >/dev/null 2>&1; then
  git add -A >/dev/null 2>&1
  git -c user.email=build@local -c user.name=build commit -qm "pristine $GST_VERSION" >/dev/null 2>&1 || true
fi

for PATCH in "${PATCHES[@]}"; do
  if git apply -p1 --check --reverse "$PATCH" >/dev/null 2>&1; then
    echo "Already applied: $(basename "$PATCH")"
  else
    git apply -p1 --check "$PATCH"
    git apply -p1 "$PATCH"
    echo "Applied: $(basename "$PATCH")"
  fi
done

# --- 4. configure + build just the mxf plugin ------------------------------------
# A configure that failed leaves a build/ with no build.ninja; re-running ninja there fails with
# a confusing error, so wipe and reconfigure unless the directory is a complete configure.
if [ ! -f build/build.ninja ]; then
  rm -rf build
  meson setup build --prefix="$WORK_DIR/prefix" -Dauto_features=disabled -Dmxf=enabled
fi
ninja -C build

# --- 5. collect the plugin --------------------------------------------------------
SO="$(find "$SRC_DIR/build" -name 'libgstmxf.so' -print -quit)"
[ -n "$SO" ] || { echo "error: libgstmxf.so not found under $SRC_DIR/build - did the build fail?" >&2; exit 1; }

mkdir -p "$PLUGIN_OUT_DIR"
cp "$SO" "$PLUGIN_OUT_DIR/"
echo "Built plugin: $PLUGIN_OUT_DIR/libgstmxf.so"

# --- 6. self-check: GStreamer must resolve mxfmux to the plugin we just built ----
# Without this the artifact could silently be useless (e.g. if the plugin were refused for
# being built for a different core than the installed runtime).
RESOLVED="$(GST_PLUGIN_PATH="$PLUGIN_OUT_DIR" gst-inspect-1.0 mxfmux 2>/dev/null \
  | sed -n 's/^ *Filename *\(.*\)$/\1/p' | head -1)"
[ -n "$RESOLVED" ] || { echo "error: gst-inspect-1.0 could not resolve mxfmux after the build." >&2; exit 1; }
case "$RESOLVED" in
  "$PLUGIN_OUT_DIR"/*) echo "Self-check OK: mxfmux resolves to $RESOLVED" ;;
  *) echo "error: GStreamer resolved mxfmux to '$RESOLVED' instead of the built plugin under '$PLUGIN_OUT_DIR'." >&2; exit 1 ;;
esac

# --- 7. manifest: make the artifact self-describing ------------------------------
sha() { command -v sha256sum >/dev/null 2>&1 && sha256sum "$1" | cut -d' ' -f1 || shasum -a 256 "$1" | cut -d' ' -f1; }
cat > "$PLUGIN_OUT_DIR/manifest.json" <<EOF
{
  "gstreamer_source_version": "$GST_VERSION",
  "plugin": "libgstmxf.so",
  "built_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "patch_files": "$(cd "$PATCH_DIR" && ls *.patch | tr '\n' ' ')",
  "patches_sha256": "$(cat "$PATCH_DIR"/*.patch | sha256sum | cut -d' ' -f1)",
  "plugin_sha256": "$(sha "$PLUGIN_OUT_DIR/libgstmxf.so")",
  "gstreamer_pkgconfig_version": "$(pkg-config --modversion gstreamer-1.0)"
}
EOF
echo "Manifest: $PLUGIN_OUT_DIR/manifest.json"

cat <<EOF

Patched plugin : $PLUGIN_OUT_DIR/libgstmxf.so
Ship it next to the application (the app points GStreamer at <exe dir>/gst-plugins):
    cp $PLUGIN_OUT_DIR/libgstmxf.so <app dir>/gst-plugins/

Verify which plugin is loaded (Filename must be the one above):
    GST_PLUGIN_PATH=<app dir>/gst-plugins gst-inspect-1.0 mxfmux

End-to-end check (timecode must NOT be 00:00:00:00):
    GST_PLUGIN_PATH=<app dir>/gst-plugins gst-launch-1.0 -e videotestsrc num-buffers=50 \\
      ! video/x-raw,format=YV12,width=1920,height=1080,framerate=25/1 ! timecodestamper source=rtc \\
      ! videoconvert ! avenc_dnxhd profile=2 threads=1 ! mxfmux ! filesink location=test.mxf
    ffmpeg -i test.mxf     # use ffmpeg, NOT ffprobe - ffprobe cannot show MXF timecode
EOF
