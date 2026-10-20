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

### The second half: "not measured" was reading as "measured limited"

`g_jxs_declared_range[]` is a file-scope `std::atomic<int>` array, so every slot started at 0 - and
`JXS_RANGE_LIMITED` is 0. An unmeasured JPEG XS camera therefore read as MEASURED LIMITED:
`range_mode_for_camera()` returned `PassthroughLimited`, the preview tile declared `bt709`, while
that camera's producer declared the range the probe actually read off the wire (`1:3:5:1` for both
nodes on the rig). Those cannot intersect - caps fields are compared textually - so the preview died
at its first buffer, with the reason in GStreamer's debug info rather than its message:

```
Preview error from intersrc_v0-appsrc: Internal data stream error.
streaming stopped, reason not-negotiated (-4)
```

Caught on the capture box (`192.168.1.235`) because the preview now says what each tile decided:

```
CAM 0: preview tile: range measured, declaring bt709 -> no conversion element
CAM 1: preview tile: range measured, declaring bt709 -> no conversion element
CAM 0: JXS stream declares video_full_range=1 (H.273 1/1/1) from the PMT JXS_video_descriptor
```

"range measured" before a byte of the stream had been parsed, and the producer declaring FULL. A slot
now starts at `JXS_RANGE_UNKNOWN`, and every slot is reset when the camera set moves (removal shifts
later cameras into different slots). With "not measured" no longer reading as a measurement, the
bounded wait in `build_preview_pipeline()` has something real to wait for, and the tile is built from
the measured range with the conversion spliced in - the link that only exists because of the element
fix above:

```
CAM 0: JXS range wait -> measured, tile built from the declaration
CAM 0: preview tile: range measured, declaring 1:3:5:1 -> osrrange mode=full-to-limited, declaring bt709
```

The same zero-default had a recording's leg declare `bt709` for a camera whose producer declared
`1:3:5:1`, so a take from these cameras could not negotiate either;
`declared_video_caps_string()` makes an unmeasured leg declare no range at all rather than a
fictitious one, which is what "not measured" means.

Verified live on the capture box after the fix, in one session: both tiles show live pictures
("CAM 0 OK" / "CAM 1 OK", timestamps advancing), no `Preview error` line, and a 15 s take wrote two
DNxHD MXFs that `ffprobe` reads as `color_range=pc`, `color_space=bt709`, BT.709 - what a full-range
stream must be tagged as.
