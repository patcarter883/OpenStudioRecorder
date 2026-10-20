# Evidence: the range declaration the recorder now reads, declares and writes

Three independent checks. None of them was produced by reading a value back from the recorder's own
data structures - each reads a different artefact (hand-built wire bytes, a real capture, a recorded
container).

## 1. Hand-built wire bytes (`ctest -R jxs-ts-colour`)

`test/tools/jxs_ts_colour_test.c` builds the MPEG-TS bytes by hand - the PMT's
`JXS_video_descriptor` and the per-frame `jxes` box - and drives them through the recorder's probe
(`source/jxs_range_probe.c`) over the **vendored** parser (`vendor/jxs/jxs_ts.c`). It asserts the
reported range **follows the signed value** in both carriage points, including:

* PMT full / box absent -> FULL; PMT limited / box absent -> limited;
* box overrides the PMT when they disagree;
* **mutation**: the same code path reports limited when the byte says limited (`video_full_range=0`);
* nothing signed anywhere -> `present=0`, range UNKNOWN (never assumed limited).

## 2. Real captures (`captures.txt`)

`test/tools/jxs_range_probe_capture` (not a test; a field tool) fed the actual captured transport
streams under `~/workspace/jxs-range2/` through the same probe and parser:

| capture | declared | reported |
|---|---|---|
| `full.ts` | full | FULL (`1:3:5:1`) |
| `lim2.ts` | limited | limited (`bt709`) |
| `full2.ts`, `auto.ts`, `lim.ts` | full | FULL (`1:3:5:1`) |

`lim2.ts` is the mutation case **on real bytes**: the same binary reports limited because the file
says limited. (`lim.ts` declares full despite its name - recorded before the node was switched.)

## 3. The recorded container (`ffprobe.txt`)

The MXF's colour coding and range come from the `colorimetry` on the caps the recorder puts on the
chain (`patches/0003-mxfmux-write-colour-coding-and-range.patch`, read at buffer time). Running the
recorder's own encode chain with the caps each leg now declares:

| chain caps `colorimetry` | what it means | ffprobe reads |
|---|---|---|
| `1:3:5:1` | measured FULL stream | `color_range=pc`, BT.709 |
| `bt709` | measured limited stream | `color_range=tv`, BT.709 |
| `0:3:5:1` | nothing measured | `color_range=unknown` |

`plugin.txt` records the patched `libgstmxf.so` GStreamer actually resolved (sha256 matches
`build/patched-gst-plugins/manifest.json`), so the plugin under test is the repo's.

## 4. End to end from real camera bytes (`e2e/ffprobe.txt`)

A **real captured stream** decoded, declared and muxed the way the recorder now does it:
`tsdemux ! svtjpegxsdec ! videoconvert ! capsfilter(<the caps the mapping declares>) ! videoconvert !
avenc_dnxhd ! mxfmux`, then read back.

| capture | declares | recorder declares | recorded MXF reads |
|---|---|---|---|
| `full2.ts` | full | `1:3:5:1` | `color_range=pc`, BT.709 |
| `auto.ts` | full | `1:3:5:1` | `color_range=pc`, BT.709 |
| `lim2.ts` | limited | `bt709` | `color_range=tv`, BT.709 |

No range-conversion element (`osrrange`) appears in the chain for either measured pass-through - the
samples are decoded and encoded as they are, which is what "full in, full out" means (pinned by the
`range_element_for` unit test). The MXF files (~55 MB each) are not committed; the chain is in the
evidence file.

**Not verified here:** a recording made from a live JXS camera on the capture box (`192.168.1.235`).
The recorder is running there and must not be restarted, so the deploy is outstanding - see the
commit message.

## 5. The conversion element's own negotiation (`preview-negotiation/`)

The preview tile converts a full-range tile into the limited compositor with
`osrrange mode=full-to-limited ! capsfilter(colorimetry=bt709)`. That link did not exist before this
fix: `osrrange` inherited `GstBaseTransform`'s **identity** `transform_caps`, so its src pad answered
with the sink caps (full range) and full-range caps do not intersect limited-range caps.

Reproduced and fixed with the **real captured stream** (`full2.ts`, which signs FULL), the producer
chain byte-for-byte as `build_jxs_capture_pipeline()` builds it, and the consumer chain byte-for-byte
as `build_preview_pipeline()` builds it (inter hop, `timeoverlay`, `textoverlay`, `compositor`):

| | `gst-launch` result |
|---|---|
| before (`master` plugin) | `erroneous pipeline: could not link osrrange0 to capsfilter2` (rc=1) |
| after (this fix) | runs clean, rc=0 |
| rolled-back control (no `osrrange` in the tile) | runs clean, rc=0 |

The caps the element negotiates, named (`-v` on the minimal link):

```
capsfilter0.src  colorimetry=(string)1:3:5:1      # full, as declared by the tile's first filter
osrrange0.sink   colorimetry=(string)1:3:5:1
osrrange0.src    colorimetry=(string)bt709        # <- what the element now advertises
capsfilter1.sink colorimetry=(string)bt709
```

and the samples are converted exactly once, on the real capture (`preview-negotiation/luma.txt`):
`full2.ts` decodes to Y 8..241 as captured and to Y 23..223 after the element - 23 and 223 are what
the LUT gives for 8 and 241.

Two properties of caps decide the fix and are pinned in `preview-negotiation/caps-textual.txt`:
caps fields are compared **textually** (`colorimetry=bt709` does not intersect
`colorimetry=2:3:5:1`, although they are the same colorimetry), and `always_in_place` never
constrained which caps may be advertised.

### The second half: the tile must be built from the range the producer will declare

The element fix alone is not enough on a live box. `btn_preview_cb` starts the session (which
starts the camera thread that builds the capture pipeline) and then builds the preview
immediately, so on the first Preview after cameras were added the JXS range has not been measured
yet: the tile is built as "range not measured" (`0:3:5:1`) while the producer's branch caps - decided
when tsdemux announces its video pad, from the measured value - declare `1:3:5:1` a moment later.
Those cannot intersect, and the preview dies with the same message:

```
Preview error from intersrc_v0-appsrc: Internal data stream error.
```

Observed on the capture box (192.168.1.235) with the element fix deployed, 2026-10-20:
`CAM n: JXS stream declares video_full_range=1` appears in the log **after** the preview pipeline
line was already logged, and `/proc/<pid>/maps` shows `libgstosrrange.so` was never loaded - so the
tile really was built without the conversion element. `build_preview_pipeline()` now waits in
bounded steps (4 s, 100 ms) for a JXS camera that has not announced its range yet, so the tile is
built from the same value the producer uses; a camera that never announces itself stays declared
unknown and shows as a dead tile rather than blocking the preview.
