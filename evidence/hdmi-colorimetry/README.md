# HDMI sources: the colorimetry is the device's, not this file's (2026-10-20)

The field report: **HDMI sources never previewed**, and the only thing the log had to say about the
audio side was a device that could not be found.

## What was wrong

`build_capture_pipeline()` capped the `v4l2src` leg with the colorimetry this application had decided
the unit's samples were in - `make_video_caps_string(unit_range_for(mode))`, i.e. `bt709` for a
measured-limited leg, `1:3:5:1` for a full-range leg, `0:3:5:1` for an unmeasured one.

v4l2src validates the colorimetry named on its sink caps against the one the device reports and
**refuses the stream** when the two disagree (GStreamer 1.28.7 + the units on this rig):

```
ERROR: Device '/dev/video0' does not support 1:3:5:1 colorimetry
  gst_v4l2_object_set_format_full(): Device wants 1:4:5:1 colorimetry
streaming stopped, reason not-negotiated (-4)
```

A pinned value here is therefore not a re-tagging of the samples, it stops the camera: the producer
pipeline posts a bus error at PLAYING, so the camera gets no preview tile **and no recording either**.
Both HDMI units are affected - it is not one bad unit - and every `RangeMode` fails, because each one
names a colorimetry the device does not have.

The two units state, for the `1920x1080` `YUY2` this recorder opens them with:

| unit | USB id | `api.v4l2.path` | colour it states |
|---|---|---|---|
| UGREEN HDMI Capture (MS2131) | `345f:2131` | `/dev/video0` | `2:4:5:1` |
| UGREEN 65518 | `2b89:5518` | `/dev/video2` | `2:4:7:1` |

## The second half, which only shows once the first is fixed

With the device's own colour on the wire, `osrrange` derives its src caps from its sink caps (it
keeps the coding and rewrites the range). A preview tile that pinned `bt709` therefore could not
intersect the producer - `not-negotiated` at `intersrc_v0-appsrc` - exactly the failure
`preview-negotiation/` documents for a range mismatch, and for the same textual-caps reason.

So the fix is not only "stop pinning on the capture leg": every leg of a device camera's chain has to
declare what the chain carries, which is the device's own statement carrying the range that leg's
conversion produced (`declared_caps_for_camera()`, and the tile's converted leg).

## Evidence

Every run below is on the capture box (`192.168.1.235`), GStreamer 1.28.7 with the application's own
`libgstosrrange.so` on `GST_PLUGIN_PATH`, against the two real units.

**The camera is refused**

| file | chain | result |
|---|---|---|
| `before-ms2131-pinned-1_3_5_1.txt` | the capture leg as it was, MS2131's default mode | refused, `exit=1` |
| `before-65518-pinned-0_3_5_1.txt` | the capture leg as it was, passthrough unit | refused, `exit=1` |
| `before-tile-pins-bt709.txt` | device colour in, tile pinning `bt709` | `not-negotiated` at `intersrc_v0-appsrc`, `exit=1` |

**The fixed chain carries frames**

The in-process `intersink`/`intersrc` pair never forwards EOS (GStreamer says so itself: *"Intersink
with appsink ... should not share the same toplevel bin"*), so a clean exit proves nothing here.
Frames are counted in bytes instead: the compositor's output is scaled to `320x180` `YUY2`
(`115200` bytes/frame) and written for a fixed wall-clock window.

| file | chain | frames |
|---|---|---|
| `b-inter-compositor-scale.txt` | fixed producer + tile for the MS2131 | **238 in 8 s** (~30 fps) |
| `d-65518-inter-compositor.txt` | fixed producer + tile for the 65518 | **235 in 8 s** (~30 fps) |
| `a-device-compositor-scale.txt` | control: device -> compositor -> scale | 146 in 5 s |
| `c-device-only.txt` | control: device -> file | 84 in 3 s |

`after-ms2131.txt` and `after-65518.txt` are the same fixed chains ending in a `fakesink`: no error
of any kind, the run ends on the harness timeout because of the EOS behaviour above.

**Not verified here:** a take recorded by the application itself over HDMI. The recorder running on
the box has to be restarted to load a fixed binary, and that instance was in use.
