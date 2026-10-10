# Local GStreamer patches carried by this repo

## `0001-mxfmux-seed-timecode-from-videotimecodemeta.patch`

**What it fixes.** GStreamer's `mxfmux` hardcodes `component->start_timecode = 0` for
both timecode tracks (Material + Source package) and never reads buffer timecode
metadata. Every MXF we record therefore carries a timecode track that starts at
`00:00:00:00`, regardless of configuration — there is no property to change it.

**What the patch does** (26 added lines, 3 sites, verified by build + A/B test):

1. `gst/mxf/mxfmux.c`: include `<gst/video/gstvideometa.h>`.
2. In `create_metadata()`'s per-pad loop — where a buffer is already peeked via
   `gst_aggregator_pad_peek_buffer()` — read `gst_buffer_get_video_time_code_meta()`
   and record `gst_video_time_code_frames_since_daily_jam()` plus the drop-frame flag
   on the mux object.
3. At both timecode-component creation sites, seed `component->start_timecode` from
   that value and set `component->drop_frame` (replacing upstream's `/* TODO: drop frame */`).

`gst/mxf/mxfmux.h`: three fields (`have_start_timecode`, `start_timecode`, `drop_frame`).

The data definition is deliberately left at `TIMECODE_12M_INACTIVE`: a non-zero start on
an *inactive* track means "timecode starts here and counts at the edit rate", which is
what a session origin wants. Switching to `_ACTIVE` asserts real-time jam and is a
separate compliance decision.

**Verified result** (same pipeline, `timecodestamper source=rtc` before the encoder):

| plugin | `ffmpeg -i` reads |
|---|---|
| stock 1.28.6 | `timecode : 00:00:00:00` |
| patched | `timecode : 06:26:42:13` (wall clock 06:26:41) |

Read MXF timecode with `ffmpeg -i` — **`ffprobe` cannot display it** and will report nothing.

## Applicability

Applies to gst-plugins-bad **1.28.6**, **1.28.7** and current `main` (1.29.x): the
surrounding code is identical in all of them. Apply with:

```
git apply -p1 < patches/0001-mxfmux-seed-timecode-from-videotimecodemeta.patch
# or: patch -p1 < ...   (from the gst-plugins-bad source root)
```

**Pin the version.** The built plugin must match the installed GStreamer runtime
version exactly — rebuild it whenever the runtime is upgraded.

## Building

* **Linux**: `tools/build-patched-gstmxf-linux.sh [<ver>]` — the counterpart of the Windows
  script. Fetches the matching `gst-plugins-bad-<ver>.tar.xz`, applies this patch (idempotently),
  builds only the mxf plugin (`-Dauto_features=disabled -Dmxf=enabled`, ~161 targets, seconds)
  into `build/patched-gst-plugins/`, then **self-checks** that `gst-inspect-1.0 mxfmux` resolves to
  the plugin it just built and writes a `manifest.json` with the patch/plugin SHA-256s. Needs the
  version-matched `-dev` headers, meson, ninja and git. Ship the `.so` to `<exe dir>/gst-plugins/`.
  Verified on 1.28.6 (both copies byte-identical): stock MXF reads `timecode : 00:00:00:00`, the
  patched one reads the wall clock.
* **Windows** (the capture machine): `tools/build-patched-gstmxf-windows.ps1`. Requires the
  official GStreamer installer installed with `/TYPE=devel` (Runtime + development headers),
  Visual Studio 2022, Python + meson/ninja, and Git for Windows (for `git apply`).

## How the application finds the patched plugin

The app ships its GStreamer customisations in a `gst-plugins/` directory **next to the
executable** and points GStreamer at it before `gst_init()` (`osr::configure_gst_plugin_path()`
in `source/main.cpp`). `OSR_GST_PLUGIN_DIR` overrides the location for field diagnostics and CI.

```
<app dir>/OpenStudioRecorder      (or .exe)
<app dir>/gst-plugins/gstmxf.dll  (libgstmxf.so on Linux)
```

Verified behaviour (gst 1.28.6):

* GStreamer catalogues plugins from `GST_PLUGIN_PATH` before its system plugin directory, and a
  same-named plugin found there **replaces** the installed one — including when the registry
  cache is warm. In-process `gst_registry_scan_path()` does **not** override, so the path must be
  set before `gst_init()`.
* If the copy is missing — or was built for an *older* core than the installed runtime — GStreamer
  refuses it (`gst_plugin_check_version()`: rejected only when `major != GST_VERSION_MAJOR ||
  minor > GST_VERSION_MINOR`, logged as "has incompatible version ... not loading") and the stock
  plugin is used. Start-up then logs:
  `gst: WARNING - the stock mxfmux is active, MXF timecode will start at 00:00:00:00`.
* A plugin built against 1.28 keeps loading on any *newer* 1.2x/1.3x runtime.

The plugin's reported version is printed at start-up alongside the runtime version, so drift is
visible in the log without any tooling.

## Getting the plugin without a toolchain (CI)

`.github/workflows/patched-gstreamer.yml` builds `gstmxf.dll` on a `windows-2022` runner and
publishes it as an artifact — the capture machine needs no Visual Studio, Python or meson:

1. Run the workflow (**Actions → Patched GStreamer plugin (Windows) → Run workflow**); set
   `gst_version` to the runtime's minor series (`1.28.6` or `1.28.7` — any 1.28.x build loads on
   any 1.28+ runtime, but a plugin built for a *newer minor* than the runtime is refused).
2. Download the `gstmxf-<version>-windows-x86_64` artifact.
3. Copy `gstmxf.dll` into `<app dir>\gst-plugins\` next to `OpenStudioRecorder.exe`.

The pipeline runs `tools/build-patched-gstmxf-windows.ps1` verbatim, so CI and a workstation follow
the same path; it also asserts that GStreamer resolves `mxfmux` to the built DLL, records a short
DNxHR test clip through the muxer, and (with `verify_timecode` enabled) installs ffmpeg and fails
if the clip's timecode is all zeros. Each artifact carries `manifest.json` with the source version,
patch SHA-256 and DLL SHA-256.

Authenticode signing is wired up but **inactive** until a code-signing certificate exists: set the
`WINDOWS_SIGN_PFX_BASE64` / `WINDOWS_SIGN_PFX_PASSWORD` repository secrets and the signing step
runs automatically (`signtool`, SHA-256, DigiCert timestamp). Without them the artifact is
unsigned — the published hashes still let anyone verify integrity, but Windows SmartScreen will
warn on first run.

## Upstream status (checked 2026-10-02)

Not fixed in `1.28` or `main` (1.29.2.1-dev); **no MR and no issue exist** for it —
`mxfmux.c` has 67 commits and not one has ever touched timecode. Nearest prior art:
`!10533` (draft, qtdemux timecode read side), `!10335` (timecode meta serialization, merged).
An issue draft and the MXF metadata proposal live in
`~/workspace/video/research/2026-10-02-mxfmux-timecode-upstream-issue-draft.md` and
`...-mxf-metadata-upstream-proposal.md`.

## Shipping it (Cerbero)

For a distributable patched runtime rather than a locally-rebuilt plugin, Cerbero is the
right tool (it is what produces the official binaries). Carry this patch as a recipe patch:
add the file under `recipes/gst-plugins-bad-1.0/patches/` and list it in the recipe's
`patches = [...]`, which `cerbero/build/source.py` applies after unpacking. Cerbero has no
MSVC cross-build from Linux — Windows builds are done natively on Windows with Visual Studio.
