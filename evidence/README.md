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
