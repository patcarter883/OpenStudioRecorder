#pragma once

// How a camera's quantisation range is handled, and what each leg of its chain then DECLARES.
//
// This header exists so the mapping can be unit-tested without linking GStreamer, FLTK or the
// whole application: it is included by source/main.cpp (which uses the mapping) and by the test
// that asserts a measured full-range stream is declared full on BOTH the unit leg and the
// encoded leg. One definition, two consumers.

namespace osr
{

// What the recorder has to do about a camera's quantisation range. Capture units differ: some pass
// the source's range through unchanged, others convert it in hardware. Converting twice compresses
// the picture, so this is decided per camera - never globally.
//
// The last two entries describe a source whose range was MEASURED rather than assumed (the JPEG XS
// network cameras, which sign their range on the wire - see source/jxs_range_probe.c). They exist
// because "Passthrough" cannot express "measured full, and nothing converts it": its declared range
// is Unknown by design, since an unmeasured unit's range must not be claimed. A stream that declared
// full range and is passed through verbatim is a different, knowable case - and the difference is
// exactly what stops a player assuming limited range and crushing the blacks.
//
// New entries are APPENDED: the numeric values of the existing ones are part of the config/UI
// surface and must not move.
enum class RangeMode
{
  Passthrough,        // range NOT measured; add no element, declare it unknown
  FullToLimited,      // the unit passed a full-range source through; convert it here
  LimitedToFull,      // the unit passed a limited-range source through; convert it here
  PassthroughFull,    // MEASURED full at the source; nothing converts it (full in, full out)
  PassthroughLimited  // MEASURED limited at the source; nothing converts it (limited in, out)
};

}  // namespace osr

// The range a pipeline leg DECLARES, as distinct from the RangeMode that says what the chain does
// about it. The two are not the same thing: a unit that passes a full-range source through has a
// full-range INPUT and, after its conversion element, a limited-range OUTPUT.
enum class ColorRange
{
  Limited,       // the rig's working range
  Full,          // declared only where a conversion has actually produced full-range samples
  Unspecified    // a unit whose range has not been measured - declared honestly as unknown
};

static inline const char* colorimetry_for(ColorRange range)
{
  switch (range)
  {
    case ColorRange::Limited:
      return "bt709";  // named shorthand = limited range + BT.709 matrix/transfer/primaries
    case ColorRange::Full:
      // range 1 (full) : matrix 3 : transfer 5 : primaries 1, all BT.709. Numeric because no
      // named shorthand expresses full range, and the transfer value caught us out before:
      // GstVideoTransferFunction runs UNKNOWN, GAMMA10, GAMMA18, GAMMA20, GAMMA22, BT709 (5).
      return "1:3:5:1";
    case ColorRange::Unspecified:
    default:
      return "0:3:5:1";  // range 0 = unknown; nothing downstream may then claim it is limited
  }
}

// What the capture unit hands us, before any conversion element of ours has run.
static inline ColorRange unit_range_for(osr::RangeMode mode)
{
  switch (mode)
  {
    case osr::RangeMode::FullToLimited:
      return ColorRange::Full;
    case osr::RangeMode::LimitedToFull:
      return ColorRange::Limited;
    case osr::RangeMode::PassthroughFull:
      // The stream signed full range and no element of ours converts it: the unit leg is full.
      return ColorRange::Full;
    case osr::RangeMode::PassthroughLimited:
      // The stream signed limited range and no element of ours converts it: the unit leg is limited.
      return ColorRange::Limited;
    case osr::RangeMode::Passthrough:
    default:
      return ColorRange::Unspecified;
  }
}

// What actually reaches the encoder, and therefore what the file's samples are.
static inline ColorRange encoded_range_for(osr::RangeMode mode)
{
  switch (mode)
  {
    case osr::RangeMode::FullToLimited:
      return ColorRange::Limited;
    case osr::RangeMode::LimitedToFull:
      return ColorRange::Full;
    case osr::RangeMode::PassthroughFull:
      // Full in, full out: no conversion element sits on the chain, so the samples are still full
      // when they reach the encoder and the muxer. This is the mapping the old model could not
      // express (Passthrough always meant "unknown" to it), and the reason a recording made from a
      // full-range JPEG XS node was tagged range-unknown.
      return ColorRange::Full;
    case osr::RangeMode::PassthroughLimited:
      return ColorRange::Limited;
    case osr::RangeMode::Passthrough:
    default:
      return ColorRange::Unspecified;
  }
}

// The element that performs a conversion, or nullptr when the leg needs none - so a camera that
// needs nothing gets no extra element and its pipeline string is exactly what it was. The two
// MEASURED pass-through modes are the point of this being here: a stream that signed its range and
// is already in it must gain NO element, or its samples are converted a second time.
static inline const char* range_element_for(osr::RangeMode mode)
{
  switch (mode)
  {
    case osr::RangeMode::FullToLimited:
      return "osrrange mode=full-to-limited";
    case osr::RangeMode::LimitedToFull:
      return "osrrange mode=limited-to-full";
    case osr::RangeMode::PassthroughFull:
    case osr::RangeMode::PassthroughLimited:
      // Measured, and measured to be ALREADY the range the samples are in: a pass-through must not
      // gain a conversion element, or the picture is compressed (or washed out) a second time.
      return nullptr;
    case osr::RangeMode::Passthrough:
    default:
      return nullptr;
  }
}
