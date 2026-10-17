#!/usr/bin/env bash
# Builds libgstosrrange.so for a Linux capture machine.
#
# Unlike the patched-mxf plugin we do not patch an upstream tree here - this element is ours - so
# the build is a plain gcc compile against the installed GStreamer development headers. Those
# headers must match the installed runtime's minor version: GStreamer refuses to load a plugin
# built for a newer core, and it fails at load time rather than here.
#
# Windows counterpart: tools/build-osrrange-plugin-windows.ps1 - keep the two in step.
#
# Usage:
#   tools/build-osrrange-plugin-linux.sh [PluginOutDir]
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
SRC_FILE="$REPO_DIR/source/gst-plugin/gstosrrange.c"
PLUGIN_OUT_DIR="${1:-$REPO_DIR/build/patched-gst-plugins}"
PKGS="gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0"

# --- 1. sanity ------------------------------------------------------------------
[ -f "$SRC_FILE" ] || { echo "error: plugin source not found at $SRC_FILE" >&2; exit 1; }

command -v gcc >/dev/null 2>&1 || { echo "error: gcc not found on PATH." >&2; exit 1; }

RUNTIME_VERSION="$(gst-launch-1.0 --version 2>/dev/null | sed -n 's/^GStreamer \([0-9][0-9.]*\).*/\1/p' | head -1)"
[ -n "$RUNTIME_VERSION" ] || { echo "error: could not detect the installed GStreamer runtime version." >&2; exit 1; }

for pkg in $PKGS; do
  have="$(pkg-config --modversion "$pkg" 2>/dev/null || true)"
  [ -n "$have" ] || { echo "error: development headers for $pkg not found (install the -dev/-devel package)." >&2; exit 1; }
  if [ "$have" != "$RUNTIME_VERSION" ]; then
    echo "error: headers for $pkg are $have but the runtime is $RUNTIME_VERSION - they must match." >&2
    exit 1
  fi
done

echo "GStreamer runtime  : $RUNTIME_VERSION"
echo "Source             : $SRC_FILE"
echo "Plugin output dir  : $PLUGIN_OUT_DIR"

# --- 2. compile -----------------------------------------------------------------
mkdir -p "$PLUGIN_OUT_DIR"
# shellcheck disable=SC2086
gcc -shared -fPIC -O2 -Wall -Wextra \
  -Wno-missing-field-initializers \
  $(pkg-config --cflags $PKGS) \
  -o "$PLUGIN_OUT_DIR/libgstosrrange.so" "$SRC_FILE" \
  $(pkg-config --libs $PKGS)

echo "Built plugin: $PLUGIN_OUT_DIR/libgstosrrange.so"

# --- 3. self-check: the element must resolve to the plugin we just built ---------
RESOLVED="$(GST_PLUGIN_PATH="$PLUGIN_OUT_DIR" gst-inspect-1.0 osrrange 2>/dev/null \
  | sed -n 's/^ *Filename *\(.*\)$/\1/p' | head -1)"
[ -n "$RESOLVED" ] || { echo "error: gst-inspect-1.0 could not resolve 'osrrange' after the build." >&2; exit 1; }
case "$RESOLVED" in
  "$PLUGIN_OUT_DIR"/*) echo "Self-check OK: osrrange resolves to $RESOLVED" ;;
  *) echo "error: GStreamer resolved osrrange to '$RESOLVED' instead of the built plugin under '$PLUGIN_OUT_DIR'." >&2; exit 1 ;;
esac

# --- 4. self-check: it must actually convert, and pass through when asked --------
# Samples we know the answer for: Y=0 must become 16, Y=255 must become 235. A plugin that loads
# but does nothing would otherwise look fine here and silently leave video unconverted.
PROBE_IN="$PLUGIN_OUT_DIR/osrrange-probe.yuv"
PROBE_OUT="$PLUGIN_OUT_DIR/osrrange-out.yuv"

python3 - "$PROBE_IN" <<'PY'
import sys
W = H = 64
buf = bytearray()
for y in range(H):
    Y = 0 if y < H // 2 else 255          # top half black, bottom half white
    row = bytearray()
    for _ in range(W // 2):
        row += bytes((Y, 128, Y, 128))
    buf += row
open(sys.argv[1], 'wb').write(bytes(buf))
PY

check() {
  local mode="$1" want_black="$2" want_white="$3"
  rm -f "$PROBE_OUT"
  # blocksize must be a whole frame: filesrc defaults to 4096, which splits a 64x64 YUY2 frame
  # (8192 bytes) in half. The element correctly refuses a short buffer, so the harness has to
  # hand it whole frames - the same thing v4l2src does in the real pipeline.
  GST_PLUGIN_PATH="$PLUGIN_OUT_DIR" gst-launch-1.0 -q -e \
    filesrc location="$PROBE_IN" blocksize=8192 \
    ! video/x-raw,format=YUY2,width=64,height=64,framerate=30/1 \
    ! osrrange "mode=$mode" \
    ! filesink location="$PROBE_OUT"
  python3 - "$PROBE_OUT" "$mode" "$want_black" "$want_white" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
Y = d[0::2]
black = sorted(Y[:32*64])[1600]
white = sorted(Y[32*64:])[1600]
ok = (black == int(sys.argv[3])) and (white == int(sys.argv[4]))
print("  mode=%-16s Y=0 -> %3d   Y=255 -> %3d   %s"
      % (sys.argv[2], black, white, "OK" if ok else "MISMATCH (want %s/%s)" % (sys.argv[3], sys.argv[4])))
sys.exit(0 if ok else 1)
PY
}

echo "Functional self-check:"
check "full-to-limited" 16 235
check "passthrough" 0 255

rm -f "$PROBE_IN" "$PROBE_OUT"

# --- 5. manifest: make the artifact self-describing -----------------------------
sha() { command -v sha256sum >/dev/null 2>&1 && sha256sum "$1" | cut -d' ' -f1 || shasum -a 256 "$1" | cut -d' ' -f1; }
cat > "$PLUGIN_OUT_DIR/manifest-osrrange.json" <<EOF
{
  "gstreamer_runtime_version": "$RUNTIME_VERSION",
  "plugin": "libgstosrrange.so",
  "element": "osrrange",
  "built_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "source": "source/gst-plugin/gstosrrange.c",
  "source_sha256": "$(sha "$SRC_FILE")",
  "plugin_sha256": "$(sha "$PLUGIN_OUT_DIR/libgstosrrange.so")"
}
EOF
echo "Manifest: $PLUGIN_OUT_DIR/manifest-osrrange.json"

cat <<EOF

Ship it next to the application (the app points GStreamer at <exe dir>/gst-plugins):
    cp $PLUGIN_OUT_DIR/libgstosrrange.so <app dir>/gst-plugins/

Verify which plugin is loaded (Filename must be the one above):
    GST_PLUGIN_PATH=<app dir>/gst-plugins gst-inspect-1.0 osrrange
EOF
