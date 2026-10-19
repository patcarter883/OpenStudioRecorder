# Zoom control: a CAPABILITY plus a VALUE

A JPEG XS camera node exposes a `Zoom` control alongside exposure, AF and the rest.
It is deliberately **two things**, not one number:

* a **VALUE** - a normalised factor, `JC_ZOOM_MIN` (1.0 = full frame) to the
  device's own advertised maximum, `JC_ZOOM_MAX` (4.0) being the protocol ceiling;
* a **CAPABILITY** - what that factor actually drives on THIS device:
  `none`, `digital` (a crop: no glass moves), `optical` (a real zoom lens), or
  `optical+digital` (the lens to its limit, then a crop beyond it).

Without the capability an operator reads a bare factor and assumes a lens. That is
the defect this contract exists to prevent: **a digital crop must never be
presentable as optical zoom.**

## The capability is discovered from the DEVICE, never hardcoded

The node probes its own camera and reports what it finds:

| node | source | what it yields |
|---|---|---|
| Pi (libcamera, IMX708) | `camera->controls()` advertises `ScalerCrop` | rectangular crop space, driver min/max rectangle -> `digital` |
| Android (Camera2) | `SCALER_AVAILABLE_MAX_DIGITAL_ZOOM`, `CONTROL_ZOOM_RATIO_RANGE`, `LENS_INFO_AVAILABLE_FOCAL_LENGTHS`, `SENSOR_INFO_ACTIVE_ARRAY_SIZE` | `digital`; a lens is inferred only from genuinely multi-focal hardware or a zoom-ratio range reaching below 1.0 |
| UVC / USB camera | `V4L2_CID_ZOOM_ABSOLUTE` | an optical/motorised zoom -> `optical` |
| PTZ / IP camera | its own pan/zoom API | `optical` |
| a device with neither | - | `none`: the control is **not advertised at all** |

Nothing anywhere hardcodes "digital". A real zoom lens that arrives later is
honoured by the existing code paths, not by a code change.

## How it is carried (non-breaking)

The capability rides the `Zoom` control's OWN descriptor in `GET /api/controls` as
extra JSON keys - `zoom_type` ("unknown"|"none"|"digital"|"optical"|
"optical+digital") and `zoom_type_source` (e.g. `libcamera ScalerCrop`):

```json
{"name":"Zoom","type":"Float","array":false,"min":1,"max":4,"default":1,
 "zoom_type":"digital","zoom_type_source":"libcamera ScalerCrop"}
```

That is a **non-breaking extension**: the JSON reader and the advertised-set
mapper in an older consumer ignore keys they do not know, so an old pane still
renders and POSTs the plain float. A node that reports no capability parses as
`unknown` - which is **not** the same as `digital`.

The value is set like any other hot control: `POST /api/controls {"Zoom": 2.5}`,
applied to the next request, never a service restart.

## One mapping, one decision

* `jc_zoom_crop_rect()` in `jxs_controls.{c,h}` is the **only** factor->rectangle
  mapping. The Pi (sensor-space `ScalerCrop`) and the Android node (active-array
  `SCALER_CROP_REGION`) both use it, so two different node families crop
  identically for the same factor.
* Mechanism and precedence live in **one** place, the node's `jxs_zoom_policy.h`
  (`decide()`), mirroring `jxs_af_policy.h`: optical wins when the device has a
  lens, a digital-only device says so, an optical-only device never silently falls
  back to a crop, and an unadvertised `ScalerCrop` is never set (libcamera
  rejects the WHOLE request for an unsupported control, which stops frames).
* The digital ceiling is **device-derived**: `JC_ZOOM_MAX` capped by the driver's
  own rectangle bounds, and - for a crop-only device - by a 2x interpolation cap on
  the ISP's per-output-pixel source scale, so a "4x" that is really 4x of mush is
  not offered.

## Where it is visible

* the node's API: `GET /api/controls` (descriptor + capability) and the `zoom`
  block in the status read-back - `type`, `max_factor`, `desired`, `applied`,
  `mechanism` and `crop`, the last being the rectangle **libcamera reported back**
  in the request metadata, i.e. the device's own answer rather than our echo;
* the node's web UI: the control card carries the capability in words
  ("DIGITAL zoom - crops the sensor, no lens moves");
* the OBS plugin's source properties: the Zoom row's label carries the same words;
* OpenStudioRecorder's control pane: likewise.

## How to verify it took effect

A control that is plumbed but does nothing is the failure mode to guard against, so
check BOTH:

1. the applied value read back from the node - `zoom.applied`, `zoom.crop` and
   `zoom.crop_reported` from the status file / `GET /api/controls`;
2. the image actually changing. A crop narrows the field of view: decode a frame at
   two factors and check that the larger factor's frame matches the **centre** of
   the smaller factor's frame (measured on this rig at 1.0 vs 3.0: NCC 0.98 at the
   expected offset, against 0.07 and -0.05 for the wrong thirds of the frame).
