# The libavcodec encoders this app wraps: what is real, what is not

The recorder encodes through GStreamer's `avenc_*` elements, which wrap **libavcodec** — the
clean-room C implementations, not the Avid or GoPro SDKs. That distinction is real and worth
recording, but several specific claims about it that have been raised do not survive measurement.
This note is the audit, so the same questions do not get re-litigated from scratch. Everything
below was measured on ffmpeg n9.0.1 / GStreamer 1.28.6, 2026-10.

## Claims that are real, and what this app does about them

| Claim | Measured reality | Position in this app |
|---|---|---|
| dnxd has no 12-bit encode | True. `ffmpeg -h encoder=dnxhd` offers `yuv422p yuv422p10le yuv444p10le gbrp10le` — no 12-bit, no alpha. | **Out of scope by evidence, and already documented at `get_setting_for_quality()`:** the captures are 8-bit YUY2, so a 10-bit profile adds no information. DNxHR therefore uses the 8-bit profiles (lb/sq/hq) deliberately. |
| DNxHR is strictly CBR | True and confirmed: 30 frames of the same content encoded at `dnxhr_hqx` from 8-bit and from 10-bit input produced **byte-identical** files (4,598,829 each). | Fine for us — predictable file sizes. It is also why DNxHR is the right choice when bit-rate must not vary. |
| CineForm's bit-rate varies with content | True, and it is the format's **design**, not a libavcodec defect — GoPro's own README: *"CineForm is a constant quality design, bit-rates will vary as needed for the scene."* | Choose DNxHR when you need CBR; size the disk for CineForm's variance rather than trying to "fix" it. |
| MXF colour metadata can go missing | True, and it is measurable in our DNxHR output: the CDCI picture essence descriptor is written with `video-line-map-0=0`, `video-line-map-1=0` and **`component-depth=0`**, and no colour-coding fields. `ffprobe` then falls back to guessing (it reported `bt709` for a file that carries nothing). | **Open item.** The muxer is GStreamer's `mxfmux`, which we already ship a patch for (timecode) — extending that patch to fill the descriptor is the in-pattern fix. |
| Colour tags in the container | True, and it is wrong today: with our caps (`colorimetry=2:4:0:1`) the MOV path writes `color_range=unknown` and `color_space=smpte170m` (**BT.601**) for a 1080p feed. HD content is BT.709, and our own capture characterisation calls the MS2131 unit "Rec.709-limited". | **Open item.** Fix the caps to describe HD, and add the colour tags to the acceptance check for a recording. |
| cfhd wants dimensions that are multiples of 8/16 | Plausible, not reproduced — and unreachable here: resolutions come from negotiated device modes, so they are always standard. | No action. |

## Claims that are not real

| Claim | Why it is wrong |
|---|---|
| "Pass `-mbd rd` to dnxhd for rate-distortion macroblock decisions" | **There is no such option.** `ffmpeg -h encoder=dnxhd` lists exactly three AVOptions: `nitris_compat`, `ibias`, `profile`. `-mbd` belongs to MPEG-style encoders and this is an intra VC-3 codec. Passing it does nothing. |
| "Use `cfhd -quality film1` or higher to bypass the missing companding curve" | **Inverted.** The enum runs best-to-worst as the number rises: 0 = `film3+` (the top tier, and libavcodec's default), 12 = `low`. Measured file sizes for 30 x 1080p frames: **film3+ 15,371,787 · film1.5 13,425,707 · film1 12,119,527 · low 9,012,303**. Following that advice would have *lowered* quality below the default. |
| "dnxhd only accepts 10-bit formats" | It also accepts 8-bit `yuv422p`. What it does *not* accept is 12-bit or alpha. |
| "cfhd is limited by the missing GoPro SDK" / "the official SDK is AVX2-optimised and faster" | GoPro's own SDK benchmark (`TestCFHD`, in their README) encodes 1080p at **9.3–13.7 ms/frame**. libavcodec's `cfhd`, measured here: **9.8 ms/frame** (0.294 s wall for 30 frames; 0.573 s user + 0.420 s sys). Same order — and the SDK README describes its SIMD as **SSE2**, not AVX2. |
| "12-bit 4:4:4 is the reason to use the SDK" | `avenc_cfhd` already accepts `gbrp12le` and `gbrap12le`, i.e. 12-bit is available in libavcodec today — and there is no 12-bit source in this rig to feed it. |
| "FFmpeg's mxfenc misses Avid descriptors" | We do not use FFmpeg's muxer at all: the MXF path is GStreamer `mxfmux`, which we already override with our own patch. The symptom class is real (see above); the stated mechanism is not ours. |

## Verdict on adopting the GoPro CineForm SDK

**Not recommended.** The licence is fine (dual Apache-2.0 / MIT), but:

1. It buys no measurable speed: 9.8 ms/frame measured here vs 9.3–13.7 ms/frame from GoPro's own
   benchmark. There is no headroom to win.
2. Its headline quality advantage (a non-linear companding curve in the 10-bit path) is unreachable:
   our captures are 8-bit, and libavcodec's `cfhd` compresses 8-bit 4:2:2 **as 10-bit** anyway —
   exactly as the SDK README describes its own behaviour.
3. Variable bit-rate is the CineForm format's design, not a libavcodec deviation, so the SDK does not
   remove it either.
4. It is a ~160k-line third-party tree, last committed **February 2022**, tested by its authors against
   Ubuntu 16.04, and it would need a new GStreamer encoder element written around it. That is a large,
   permanent dependency to solve problems we have measured as absent.

**What to do instead:** raise CineForm's top tier (done: `High` now maps to `film3+` = 0, which is
free quality), and fix the colour metadata, which is the one real quality defect found — a wrong
matrix tag is visible in an NLE in a way 0.5 dB of PSNR never is.

## Acceptance checks worth keeping

```bash
# colour tags the file actually carries (per container)
ffprobe -hide_banner -v error -show_entries \
  stream=codec_name,pix_fmt,color_range,color_space,color_transfer,color_primaries \
  -of default=noprint_wrappers=1 take.mov

# what GStreamer itself parses out of the container (shows MXF descriptor fields)
gst-discoverer-1.0 -v take.mxf | grep -A2 descriptor
```

A recording passes when the tags describe the source honestly (limited range, BT.709 for HD) and the
MXF descriptor carries a real `component-depth` and line map rather than zeros.
